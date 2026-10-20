/*
 * Copyright 2026 Wii-DS bridge contributors
 * Native Wii-DS radio transport, derived from the tested Wii-DS experiment.
 * Licensed under GPL-3.0-or-later, like melonDS.
 * No cached client ROM, synthetic game reply, or CPU patch is used here.
 */
#include "WiiDSBridge.h"
#include "WiiDSMPConfig.h"
#include "Platform.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace melonDS::WiiDSBridge
{
namespace
{
constexpr u16 FirstPort = 7065, LastPort = 7080;
constexpr char Hello[] = "WIIDSBRIDGE/1 DOLPHIN HELLO";
constexpr char Ack[] = "WIIDSBRIDGE/1 MELONDS ACK";
constexpr std::array<u8, 8> Magic{'W','D','S','B','0','0','0','1'};
constexpr std::array<u8, 6> WiiMAC{0x00,0x17,0xab,0x4d,0xb9,0xe0};
constexpr u8 CommandMAC[6] = {0x03,0x09,0xbf,0x00,0x00,0x00};
constexpr u8 AckMAC[6] = {0x03,0x09,0xbf,0x00,0x00,0x03};
constexpr u8 ReplyMAC[6] = {0x03,0x09,0xbf,0x00,0x00,0x10};
constexpr size_t MaxRadioPacket = 2048;
constexpr size_t MaxQueuedPackets = 4096;
constexpr long MaxDiagnosticBytes = 8 * 1024 * 1024;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket InvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket InvalidSocket = -1;
void CloseSocket(Socket s) { close(s); }
#endif
std::mutex Mutex;
struct QueuedFrame
{
    std::vector<u8> Data;
    u32 Round = 0;
    bool Ack = false;
};
struct Context
{
explicit Context(void* owner) : Owner(owner) {}
~Context() { if (Listener != InvalidSocket) CloseSocket(Listener); }
void* Owner;
bool Diagnostics = false;
bool Automatic = false, Selected = false;
bool HostDisconnectPending = false;
Socket Listener = InvalidSocket;
sockaddr_in Peer{};
bool HavePeer = false, HaveBeacon = false, Associated = false;
bool CommandAllowed = true, HaveClientMAC = false;
u8 Channel = 1, ExpectedChannel = 0, DTIM = 0;
u16 RadioSequence = 1, BeaconSequence = 0, CommandSequence = 0;
u16 ExpectedAID = 0, RoundClients = 0, RoundReplies = 0;
u64 LastHello = 0, LastBind = 0, LastBeacon = 0, SessionGeneration = 1;
u64 BindNonce = 0;
u16 BoundPort = 0;
u32 CurrentRound = 0;
bool MultiRound = false;
unsigned HotCount = 0;
std::array<u8, 2> BeaconTimestamp{};
std::array<u8, 128> BeaconData{};
std::array<u8, 6> ClientMAC{};
std::deque<std::vector<u8>> RadioFrames;
std::deque<QueuedFrame> HostFrames;
// A round remains known until its command and correlated ACK reach this core.
// The bool prevents duplicate ACK datagrams from completing a round twice.
std::unordered_map<u32, bool> KnownRounds;

void Log(const char* event, unsigned value = 0)
{
    if (!Diagnostics) return;
    const bool hotEvent = !std::strcmp(event, "DS_FRAME_SENT") ||
        !std::strcmp(event, "COMMAND_QUEUED") ||
        !std::strcmp(event, "HOST_FRAME_DELIVERED") ||
        !std::strcmp(event, "RADIO_FRAME_DELIVERED") ||
        !std::strcmp(event, "MP_ACK_FAILURE_MASK");
    if (hotEvent && ++HotCount > 16 && (HotCount & 255)) return;
    if (std::FILE* file = std::fopen("wii-ds-bridge.log", "a"))
    {
        if (std::fseek(file, 0, SEEK_END) != 0 || std::ftell(file) >= MaxDiagnosticBytes)
        { std::fclose(file); return; }
        std::fprintf(file, "WII_DS us=%llu event=%s value=%u generation=%llu port=%u\n",
                     static_cast<unsigned long long>(Platform::GetUSCount()), event,
                     value, static_cast<unsigned long long>(SessionGeneration), BoundPort);
        std::fclose(file);
    }
}
void ClearRadio(bool clearBeacon)
{
    if (Automatic) Selected = false;
    RadioFrames.clear(); HostFrames.clear();
    KnownRounds.clear(); CurrentRound = 0; MultiRound = false;
    Associated = false; HaveClientMAC = false;
    ExpectedAID = RoundClients = RoundReplies = 0;
    ExpectedChannel = 0;
    CommandAllowed = true;
    Channel = 1;
    ++SessionGeneration;
    if (clearBeacon)
    {
        HaveBeacon = false; BeaconData.fill(0); BeaconTimestamp.fill(0);
        RadioSequence = 1; BeaconSequence = CommandSequence = 0; DTIM = 0;
    }
    LastBeacon = 0;
}
void ClearPeer(const char* reason)
{
    const bool lostSelectedHost = Automatic && Selected;
    ClearRadio(true); HavePeer = false; Peer = {}; LastHello = 0;
    // Keep this until the core observes host loss, even if another HELLO
    // arrives first. Discovery is not a replacement for the old guest host.
    if (lostSelectedHost) HostDisconnectPending = true;
    Log(reason);
}
bool SamePeer(const sockaddr_in& sender)
{
    return HavePeer && sender.sin_addr.s_addr == Peer.sin_addr.s_addr &&
           sender.sin_port == Peer.sin_port;
}
void EnsureSocket()
{
    if (Listener != InvalidSocket) return;
    const auto now = Platform::GetMSCount();
    if (LastBind && now - LastBind < 1000) return;
    LastBind = now;
#ifdef _WIN32
    static const bool socketsReady = [] {
        WSADATA data{}; return WSAStartup(MAKEWORD(2,2), &data) == 0;
    }();
    if (!socketsReady) return;
#endif
    for (u16 port = FirstPort; port <= LastPort; ++port)
    {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == InvalidSocket) return;
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        CloseSocket(socket); continue;
    }
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) { CloseSocket(socket); return; }
#else
    if (fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK) < 0)
    { CloseSocket(socket); return; }
#endif
    Listener = socket;
    BoundPort = port;
    // The exclusive port distinguishes simultaneous processes even if they
    // happen to bind at the same clock tick with identical virtual addresses.
    BindNonce = (u64(BoundPort) << 48) |
        ((Platform::GetUSCount() ^ static_cast<u64>(reinterpret_cast<uintptr_t>(Owner))) &
         0x0000ffffffffffffULL);
    Log("LISTENING", BoundPort);
    return;
    }
    Log("BIND_RETRY");
}
int SendEnvelope(u8 type, const u8* body, size_t size)
{
    if (!HavePeer || Listener == InvalidSocket || size > MaxRadioPacket + 20) return 0;
    std::vector<u8> packet(11 + size);
    std::copy(Magic.begin(), Magic.end(), packet.begin());
    packet[8] = type; packet[9] = static_cast<u8>(size >> 8); packet[10] = static_cast<u8>(size);
    if (size) std::memcpy(packet.data() + 11, body, size);
    return sendto(Listener, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                  reinterpret_cast<const sockaddr*>(&Peer), sizeof(Peer));
}
void QueueBeacon(u8 channel)
{
    if (RadioFrames.size() >= 6) return;
    std::vector<u8> packet(12, 0);
    packet[8] = 0x0a; packet[9] = channel;
    const auto append = [&](std::initializer_list<u8> bytes) { packet.insert(packet.end(), bytes); };
    append({0x80,0,0,0}); append({0xff,0xff,0xff,0xff,0xff,0xff});
    packet.insert(packet.end(), WiiMAC.begin(), WiiMAC.end());
    packet.insert(packet.end(), WiiMAC.begin(), WiiMAC.end());
    append({u8(BeaconSequence << 4),u8(BeaconSequence >> 4)});
    BeaconSequence = (BeaconSequence + 1) & 0xfff;
    const auto time = Platform::GetUSCount();
    for (unsigned i = 0; i < 8; ++i) packet.push_back(u8(time >> (i*8)));
    append({0xd7,0,0x21,0x04});
    append({0x01,0x08,0x82,0x84,0x0b,0x16,0x24,0x30,0x48,0x6c});
    append({0x03,0x01,channel}); append({0x05,0x04,DTIM,0x02,0,0}); DTIM ^= 1;
    append({0x2a,0x01,0,0x2f,0x01,0,0x32,0x04,0x0c,0x12,0x18,0x60});
    append({0xdd,0x88,0,0x09,0xbf,0,0xff,0xff});
    packet.insert(packet.end(), BeaconTimestamp.begin(), BeaconTimestamp.end());
    packet.insert(packet.end(), BeaconData.begin(), BeaconData.end());
    append({0xdd,0x09,0,0x10,0x18,0x02,0,0xf0,0,0,0,0,0,0,0});
    const auto length = packet.size() - 12;
    packet[10] = u8(length); packet[11] = u8(length >> 8);
    RadioFrames.emplace_back(std::move(packet));
}
std::vector<u8> MakeAck(u16 failure = 0)
{
    std::vector<u8> ack(12 + 32, 0);
    ack[8] = 0x14; ack[9] = Channel; ack[10] = 32;
    auto* frame = ack.data() + 12;
    frame[0] = 0x18; frame[1] = 0x02;
    std::memcpy(frame + 4, AckMAC, 6);
    std::memcpy(frame + 10, WiiMAC.data(), 6); std::memcpy(frame + 16, WiiMAC.data(), 6);
    frame[22] = u8(CommandSequence << 4); frame[23] = u8(CommandSequence >> 4);
    CommandSequence = (CommandSequence + 1) & 0xfff;
    frame[24] = 0x28; // Existing capture-validated bridge ACK countdown, not SDK deadline.
    frame[26] = u8(failure); frame[27] = u8(failure >> 8);
    return ack;
}
void QueueCommand(const u8* application, size_t length, const WiiDSMPSettings& config, u32 round = 0)
{
    if (length < 1 || 12 + 24 + 4 + length + 4 > MaxRadioPacket) return;
    if (round && KnownRounds.count(round)) return;
    if (round) MultiRound = true;
    if (HostFrames.size() + 2 > MaxQueuedPackets)
    {
        // Never silently discard one member of a CMD/ACK pair.
        ClearPeer("QUEUE_OVERFLOW"); return;
    }
    const size_t frameLength = 24 + 4 + length + 4;
    std::vector<u8> radio(12 + frameLength, 0);
    radio[8] = 0x14; radio[9] = Channel; radio[10] = u8(frameLength); radio[11] = u8(frameLength >> 8);
    auto* frame = radio.data() + 12;
    frame[0] = 0x28; frame[1] = 0x02; frame[2] = u8(config.Duration); frame[3] = u8(config.Duration >> 8);
    std::memcpy(frame + 4, CommandMAC, 6);
    std::memcpy(frame + 10, WiiMAC.data(), 6); std::memcpy(frame + 16, WiiMAC.data(), 6);
    frame[22] = u8(CommandSequence << 4); frame[23] = u8(CommandSequence >> 4);
    CommandSequence = (CommandSequence + 1) & 0xfff;
    frame[24] = u8(config.ReplyTime); frame[25] = u8(config.ReplyTime >> 8);
    frame[26] = u8(config.ClientMask); frame[27] = u8(config.ClientMask >> 8);
    std::memcpy(frame + 28, application, length);
    // IOS swaps the application buffer's first halfword onto the radio.
    if (length >= 2) std::swap(frame[28], frame[29]);
    HostFrames.push_back({std::move(radio), round, false});
    if (round) KnownRounds.emplace(round, false);
    else HostFrames.push_back({MakeAck(), 0, true});
    Log("COMMAND_QUEUED", static_cast<unsigned>(length));
}
void QueueRoundAck(u32 round, u16 failure)
{
    const auto known = KnownRounds.find(round);
    if (!round || known == KnownRounds.end() || known->second) return;
    if (HostFrames.size() >= MaxQueuedPackets) { ClearPeer("QUEUE_OVERFLOW"); return; }
    const auto command = std::find_if(HostFrames.begin(), HostFrames.end(), [round](const auto& queued) {
        return queued.Round == round && !queued.Ack;
    });
    if (command == HostFrames.end() && CurrentRound != round) return;
    known->second = true;
    // An ACK may arrive before a non-target core has consumed its CMD. Keep
    // the pair together and never let another command overtake this ACK.
    const auto position = command == HostFrames.end() ? HostFrames.begin() : std::next(command);
    HostFrames.insert(position, {MakeAck(failure), round, true});
    Log("ROUND_ACK_QUEUED", round);
}
void Pump()
{
    EnsureSocket();
    if (Listener == InvalidSocket) return;
    auto now = Platform::GetMSCount();
    if (HavePeer && now - LastHello > 3000) ClearPeer("PEER_EXPIRED");
    for (unsigned datagram = 0; datagram < 32; ++datagram)
    {
        std::array<u8,4096> packet{}; sockaddr_in sender{};
#ifdef _WIN32
        int size = sizeof(sender);
#else
        socklen_t size = sizeof(sender);
#endif
        const int received = recvfrom(Listener, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &size);
        if (received <= 0) break;
        if (sender.sin_family != AF_INET || sender.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
        if (received == static_cast<int>(sizeof(Hello)-1) && !std::memcmp(packet.data(), Hello, sizeof(Hello)-1))
        {
            if (HavePeer && !SamePeer(sender)) continue; // A second Dolphin cannot silently take over.
            if (!HavePeer) { ClearRadio(true); Peer = sender; HavePeer = true; Log("PEER_HELLO"); }
            LastHello = now;
            char response[96]; const auto length = std::snprintf(response, sizeof(response), "%s %016llX M2", Ack,
                                         static_cast<unsigned long long>(BindNonce));
            sendto(Listener, response, length, 0, reinterpret_cast<const sockaddr*>(&sender), size);
            continue;
        }
        if (!SamePeer(sender) || received < 11 || std::memcmp(packet.data(), Magic.data(), Magic.size())) continue;
        const size_t length = (u16(packet[9]) << 8) | packet[10];
        if (static_cast<size_t>(received) != 11 + length) continue;
        const auto* body = packet.data() + 11;
        if (packet[8] == 8 && length == 0) { ClearPeer("PEER_STOP"); continue; }
        if (packet[8] == 2 && length == 130)
        {
            // Only discard old beacon copies; in-flight native radio work stays intact.
            RadioFrames.erase(std::remove_if(RadioFrames.begin(), RadioFrames.end(), [](const auto& p) {
                return p.size() > 12 && p[12] == 0x80;
            }), RadioFrames.end());
            std::memcpy(BeaconTimestamp.data(), body, 2); std::memcpy(BeaconData.data(), body + 2, 128);
            HaveBeacon = true; LastBeacon = 0; Log("BEACON_CONFIG");
        }
        else if (packet[8] == 7 && length > 16 && length <= 16 + MaxRadioPacket)
        {
            std::array<u8,16> raw{}; std::memcpy(raw.data(), body, raw.size());
            const auto config = DecodeWiiDSMPConfig(raw);
            if (config) QueueCommand(body + 16, length - 16, *config);
            else Log("UNSUPPORTED_MP_CONFIG");
        }
        else if (packet[8] == 9 && length > 20 && length <= 20 + MaxRadioPacket)
        {
            const u32 round = (u32(body[0]) << 24) | (u32(body[1]) << 16) |
                              (u32(body[2]) << 8) | body[3];
            std::array<u8,16> raw{}; std::memcpy(raw.data(), body + 4, raw.size());
            const auto config = DecodeWiiDSMPConfig(raw);
            if (round && config) QueueCommand(body + 20, length - 20, *config, round);
            else Log("UNSUPPORTED_MP_CONFIG");
        }
        else if (packet[8] == 11 && length == 6)
        {
            const u32 round = (u32(body[0]) << 24) | (u32(body[1]) << 16) |
                              (u32(body[2]) << 8) | body[3];
            QueueRoundAck(round, (u16(body[4]) << 8) | body[5]);
        }
        else if (packet[8] == 5 && length >= 12 + 28 && length <= MaxRadioPacket &&
                 static_cast<size_t>(u16(body[10]) | (u16(body[11]) << 8)) == length - 12)
        {
            if (RadioFrames.size() >= MaxQueuedPackets) { ClearPeer("QUEUE_OVERFLOW"); continue; }
            RadioFrames.emplace_front(body, body + length);
            Log("MANAGEMENT_QUEUED", body[12]);
        }
        // Legacy type4 guessing and synthetic replies intentionally are unsupported.
    }
    if (HavePeer && HaveBeacon && (LastBeacon == 0 || now - LastBeacon >= 200))
    {
        if (Associated) QueueBeacon(Channel);
        else { QueueBeacon(1); QueueBeacon(7); QueueBeacon(13); }
        LastBeacon = now;
    }
}
bool SelectForTransmit(const u8* data, int length);
bool ConsumeHostDisconnect();
int SendPacket(const u8* data, int length);
int ReceivePacket(u8* data, u64* timestamp, bool host);
void NotifyAssociationResponse(u16 aid, u8 channel);
void NotifyAID(u16 aid, u8 channel);
};
std::unordered_map<void*, std::unique_ptr<Context>> Contexts;
Context* Find(void* instance)
{
    const auto found = Contexts.find(instance);
    return found == Contexts.end() ? nullptr : found->second.get();
}
}

bool Configure(void* instance, bool enabled, bool diagnostics)
{
    std::lock_guard<std::mutex> lock(Mutex);
    if (!instance) return false;
    if (!enabled)
    {
        if (auto* context = Find(instance)) context->ClearPeer("DISABLED");
        Contexts.erase(instance);
        return true;
    }
    auto& entry = Contexts[instance];
    if (!entry) entry = std::make_unique<Context>(instance);
    auto& context = *entry;
    if (context.Automatic) context.ClearRadio(false);
    context.Diagnostics = diagnostics; context.Automatic = false; context.Selected = false;
    context.HostDisconnectPending = false;
    return true;
}
bool ConfigureAutomatic(void* instance, bool enabled, bool diagnostics)
{
    if (!enabled) return Configure(instance, false, diagnostics);
    std::lock_guard<std::mutex> lock(Mutex);
    if (!instance) return false;
    auto& entry = Contexts[instance];
    if (!entry) entry = std::make_unique<Context>(instance);
    auto& context = *entry;
    if (!context.Automatic)
    { context.ClearRadio(false); context.HostDisconnectPending = false; }
    context.Diagnostics = diagnostics; context.Automatic = true;
    return true;
}
void Release(void* instance) { Configure(instance, false, false); }
bool IsDiscoverable(void* instance) { std::lock_guard<std::mutex> lock(Mutex); return Find(instance) != nullptr; }
bool IsAutomatic(void* instance) { std::lock_guard<std::mutex> lock(Mutex); auto* c = Find(instance); return c && c->Automatic; }
bool IsEnabled(void* instance) { std::lock_guard<std::mutex> lock(Mutex); auto* c = Find(instance); return c && (!c->Automatic || c->Selected); }
bool IsActive(void* instance) { std::lock_guard<std::mutex> lock(Mutex); auto* c = Find(instance); return c && c->HavePeer; }
bool SelectForTransmit(void* instance, const u8* data, int length)
{
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    return context && context->SelectForTransmit(data, length);
}
bool Context::SelectForTransmit(const u8* data, int length)
{
    Pump();
    if (!Automatic || Selected) return true;
    if (!HavePeer || !HaveBeacon || !data || length < 12 + 24 + 4 + 4 ||
        static_cast<size_t>(length) > MaxRadioPacket ||
        (u16(data[10]) | (u16(data[11]) << 8)) != length - 12) return false;
    const u8* frame = data + 12;
    if ((frame[1] & 0x43) || std::memcmp(frame + 4, WiiMAC.data(), 6) ||
        std::memcmp(frame + 16, WiiMAC.data(), 6) || (frame[10] & 1)) return false;
    constexpr u8 empty[6]{};
    if (!std::memcmp(frame + 10, empty, 6) || !std::memcmp(frame + 10, WiiMAC.data(), 6)) return false;
    const bool association = frame[0] == 0x00;
    const bool authentication = frame[0] == 0xb0 && length >= 12 + 24 + 6 + 4 &&
        frame[24] == 0 && frame[25] == 0 && frame[26] == 1 && frame[27] == 0 &&
        frame[28] == 0 && frame[29] == 0;
    if (!association && !authentication) return false;
    Selected = true;
    HostDisconnectPending = false;
    Log(authentication ? "AUTO_AUTH_SELECTED" : "AUTO_ASSOC_SELECTED");
    return true;
}
bool IsWiiFrame(void* instance, const u8* data, int length)
{
    std::lock_guard<std::mutex> lock(Mutex);
    return Find(instance) && data && length >= 12 + 24 &&
        !std::memcmp(data + 12 + 10, WiiMAC.data(), 6) &&
        !std::memcmp(data + 12 + 16, WiiMAC.data(), 6);
}
bool ConsumeHostDisconnect(void* instance)
{
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    return context && context->ConsumeHostDisconnect();
}
bool Context::ConsumeHostDisconnect()
{
    if (!Automatic) return false;
    Pump();
    if (!HostDisconnectPending) return false;
    HostDisconnectPending = false;
    Log("HOST_LOST_DELIVERED");
    return true;
}
void Process(void* instance) { std::lock_guard<std::mutex> lock(Mutex); if (auto* c = Find(instance)) c->Pump(); }
void ResetRadio(void* instance) { std::lock_guard<std::mutex> lock(Mutex); if (auto* c = Find(instance)) { c->ClearRadio(false); c->HostDisconnectPending = false; c->Log("RADIO_RESET"); } }
u64 Generation(void* instance) { std::lock_guard<std::mutex> lock(Mutex); auto* c = Find(instance); return c ? c->SessionGeneration : 0; }
unsigned IdentitySlot(void* instance)
{
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    if (!context) return 0;
    context->EnsureSocket();
    return context->Listener != InvalidSocket ? context->BoundPort - FirstPort : 0;
}
std::array<u8,6> ParentBSSID() { return WiiMAC; }
void SetCommandAllowed(void* instance, bool allowed)
{ std::lock_guard<std::mutex> lock(Mutex); if (auto* c = Find(instance)) c->CommandAllowed = allowed; }

int Send(void* instance, const u8* data, int length)
{
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    return context ? context->SendPacket(data, length) : 0;
}
int Context::SendPacket(const u8* data, int length)
{
    if ((Automatic && !Selected) || !data || length < 12 + 24 || static_cast<size_t>(length) > MaxRadioPacket) return 0;
    Pump();
    if (!HavePeer || (Automatic && !Selected)) return 0;
    const auto* frame = data + 12;
    const bool reply = Associated && HaveClientMAC && data[9] == Channel && length >= 12 + 24 + 4 &&
        (frame[0] == 0x18 || frame[0] == 0x58) && (frame[1] & 3) == 1 &&
        !std::memcmp(frame + 4, WiiMAC.data(), 6) && !std::memcmp(frame + 10, ClientMAC.data(), 6) &&
        !std::memcmp(frame + 16, ReplyMAC, 6) && ExpectedAID > 0 && ExpectedAID < 16;
    if (reply && MultiRound && !CurrentRound) return 0;
    if (reply && CurrentRound)
    {
        std::vector<u8> body(4 + length);
        body[0] = u8(CurrentRound >> 24); body[1] = u8(CurrentRound >> 16);
        body[2] = u8(CurrentRound >> 8); body[3] = u8(CurrentRound);
        std::memcpy(body.data() + 4, data, length);
        if (SendEnvelope(10, body.data(), body.size()) != 11 + static_cast<int>(body.size())) return 0;
    }
    else if (SendEnvelope(3, data, length) != 11 + length) return 0;
    if (!Associated && length >= 12 + 24 + 4 + 4 && frame[0] == 0 && (frame[1] & 3) == 0 &&
        !std::memcmp(frame + 4, WiiMAC.data(), 6) && !std::memcmp(frame + 16, WiiMAC.data(), 6) &&
        !(frame[10] & 1))
    {
        constexpr u8 empty[6]{};
        if (std::memcmp(frame + 10, empty, 6))
        { std::memcpy(ClientMAC.data(), frame + 10, 6); HaveClientMAC = true; Log("CLIENT_ASSOC_TX"); }
    }
    if (reply)
        RoundReplies |= u16(1u << ExpectedAID) & RoundClients;
    Log("DS_FRAME_SENT", length);
    return length;
}
int Receive(void* instance, u8* data, u64* timestamp, bool host)
{
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    return context ? context->ReceivePacket(data, timestamp, host) : 0;
}
int Context::ReceivePacket(u8* data, u64* timestamp, bool host)
{
    if (!data || !timestamp) return 0;
    Pump();
    if (host && Automatic && !Selected) return 0;
    if (host && (HostFrames.empty() || !Associated)) return 0;
    if (!host && RadioFrames.empty()) return 0;
    std::vector<u8> packet;
    u32 round = 0;
    if (host)
    {
        const auto& next = HostFrames.front();
        if (!next.Ack && (!CommandAllowed || CurrentRound)) return 0;
        if (next.Ack && next.Round && CurrentRound != next.Round) return 0;
        round = next.Round;
        if (round)
        {
            if (next.Ack) { CurrentRound = 0; KnownRounds.erase(round); }
            else CurrentRound = round;
        }
        packet = std::move(HostFrames.front().Data); HostFrames.pop_front();
    }
    else { packet = std::move(RadioFrames.front()); RadioFrames.pop_front(); }
    if (host && packet.size() >= 12 + 28)
    {
        if (packet[12] == 0x28 && packet[13] == 0x02)
        { RoundClients = u16(packet[38]) | (u16(packet[39]) << 8); RoundReplies = 0; }
        else if (!round && packet[12] == 0x18 && packet[13] == 0x02)
        {
            const auto failure = RoundClients & ~RoundReplies;
            packet[38] = u8(failure); packet[39] = u8(failure >> 8);
            Log("MP_ACK_FAILURE_MASK", failure);
        }
    }
    if (packet.size() >= 12 + 24 && !std::memcmp(packet.data() + 22, WiiMAC.data(), 6))
    {
        packet[34] = (packet[34] & 0xf) | u8(RadioSequence << 4);
        packet[35] = u8(RadioSequence >> 4); RadioSequence = (RadioSequence + 1) & 0xfff;
    }
    std::memcpy(data, packet.data(), packet.size());
    *timestamp = host || Associated ? 0 : Platform::GetUSCount();
    if (!host && packet.size() >= 12 + 36 && packet[12] == 0x80 && packet[13] == 0)
        SendEnvelope(6, nullptr, 0);
    Log(host ? "HOST_FRAME_DELIVERED" : "RADIO_FRAME_DELIVERED", static_cast<unsigned>(packet.size()));
    return static_cast<int>(packet.size());
}
void NotifyAssociationResponse(void* instance, u16 aid, u8 channel)
{
    std::lock_guard<std::mutex> lock(Mutex);
    if (auto* c = Find(instance)) c->NotifyAssociationResponse(aid, channel);
}
void Context::NotifyAssociationResponse(u16 aid, u8 channel)
{
    ExpectedAID = aid & 0x7ff;
    ExpectedChannel = channel;
    if (ExpectedAID) { Channel = channel; for (auto& p : HostFrames) p.Data[9] = channel; }
    Log("ASSOCIATION_RESPONSE", ExpectedAID);
}
void NotifyAID(void* instance, u16 aid, u8 channel)
{
    std::lock_guard<std::mutex> lock(Mutex);
    if (auto* c = Find(instance)) c->NotifyAID(aid, channel);
}
void Context::NotifyAID(u16 aid, u8 channel)
{
    const u16 acceptedAID = aid & 0x7ff;
    const bool associated = acceptedAID != 0;
    if (!associated)
    {
        HaveClientMAC = false; ClientMAC.fill(0);
        ExpectedAID = 0; ExpectedChannel = 0;
    }
    // The guest self-tests AID registers with nonzero patterns at boot. A
    // write establishes association only after a matching real response.
    if (associated && (!ExpectedAID || acceptedAID != ExpectedAID || channel != ExpectedChannel))
        return;
    if (associated == Associated) return;
    if (associated)
    { Channel = channel; for (auto& p : HostFrames) p.Data[9] = channel; Associated = true; }
    else
    { HostFrames.clear(); KnownRounds.clear(); CurrentRound = 0; Associated = false; CommandAllowed = true; RoundClients = RoundReplies = 0; ++SessionGeneration; }
    Log("GUEST_AID", acceptedAID);
}
}
