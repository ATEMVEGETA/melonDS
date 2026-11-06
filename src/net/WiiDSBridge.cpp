/*
 * Copyright 2026 Wii-DS bridge contributors
 * Native Wii-DS radio transport, derived from the tested Wii-DS experiment.
 * Licensed under GPL-3.0-or-later, like melonDS.
 * No cached client ROM, synthetic game reply, or CPU patch is used here.
 */
#include "WiiDSBridge.h"
#include "WiiDSBeacon.h"
#include "WiiDSMPConfig.h"
#include "Platform.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
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
#include <io.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace melonDS::WiiDSBridge
{
namespace
{
constexpr u16 FirstPort = 7065, LastPort = 7080;
constexpr char Hello[] = "WIIDSBRIDGE/1 DOLPHIN HELLO";
constexpr char Ack[] = "WIIDSBRIDGE/1 MELONDS ACK";
constexpr std::array<u8, 8> Magic{'W','D','S','B','0','0','0','1'};
constexpr std::array<u8, 6> LegacyWiiMAC{0x00,0x17,0xab,0x4d,0xb9,0xe0};
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
std::atomic<bool> AnyDiagnostics{false};
u64 NextContextID = 0;
struct DiagnosticSink
{
    std::FILE* File = nullptr;
    std::array<char, 64 * 1024> StdioBuffer{};
    std::array<char, 256 * 1024> Pending{};
    size_t PendingBytes = 0;
    u64 KnownBytes = 0, LastFlushMS = 0, DroppedRecords = 0;
    bool Broken = false, Full = false;

    explicit DiagnosticSink(u64 now) : LastFlushMS(now)
    {
        File = std::fopen("wii-ds-bridge.log", "ab");
        if (!File) { Broken = true; return; }
        if (std::setvbuf(File, StdioBuffer.data(), _IOFBF, StdioBuffer.size()) != 0 ||
            std::fseek(File, 0, SEEK_END) != 0)
        { Broken = true; Close(); return; }
        const auto size = std::ftell(File);
        if (size < 0) { Broken = true; Close(); return; }
        KnownBytes = static_cast<u64>(size);
        if (KnownBytes >= static_cast<u64>(MaxDiagnosticBytes)) Full = true;
    }
    ~DiagnosticSink() { Close(); }

    bool Lock(bool wait)
    {
#ifdef _WIN32
        OVERLAPPED position{};
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(File)));
        if (handle == INVALID_HANDLE_VALUE) { Broken = true; return false; }
        if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY),
                       0, MAXDWORD, MAXDWORD, &position)) return true;
        if (GetLastError() != ERROR_LOCK_VIOLATION && GetLastError() != ERROR_IO_PENDING) Broken = true;
#else
        if (flock(fileno(File), LOCK_EX | (wait ? 0 : LOCK_NB)) == 0) return true;
        if (errno != EWOULDBLOCK && errno != EAGAIN) Broken = true;
#endif
        return false;
    }
    void Unlock()
    {
#ifdef _WIN32
        OVERLAPPED position{};
        UnlockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(File))), 0,
                     MAXDWORD, MAXDWORD, &position);
#else
        flock(fileno(File), LOCK_UN);
#endif
    }
    bool Size(u64& size)
    {
#ifdef _WIN32
        LARGE_INTEGER value{};
        if (!GetFileSizeEx(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(File))), &value) || value.QuadPart < 0)
            return false;
        size = static_cast<u64>(value.QuadPart);
#else
        struct stat value{};
        if (fstat(fileno(File), &value) != 0 || value.st_size < 0) return false;
        size = static_cast<u64>(value.st_size);
#endif
        return true;
    }
    void Flush(u64 now, bool force = false, bool wait = false)
    {
        if (!File || Broken || !PendingBytes) return;
        if (!force && now >= LastFlushMS && now - LastFlushMS < 1000) return;
        LastFlushMS = now; // A busy/erroring sink cannot turn a silent poll into an I/O loop.
        if (!Lock(wait)) return;
        u64 size = 0;
        if (!Size(size)) { Broken = true; Unlock(); return; }
        const auto remaining = size < static_cast<u64>(MaxDiagnosticBytes)
            ? static_cast<size_t>(MaxDiagnosticBytes - size) : 0;
        size_t count = std::min(PendingBytes, remaining);
        if (count < PendingBytes)
        {
            while (count && Pending[count - 1] != '\n') --count;
            Full = true; // Keep only whole bounded records at the shared file cap.
        }
        if (count)
        {
            const auto written = std::fwrite(Pending.data(), 1, count, File);
            if (written != count || std::fflush(File) != 0) Broken = true;
            KnownBytes = size + written;
        }
        else KnownBytes = size;
        PendingBytes = 0;
        if (KnownBytes >= static_cast<u64>(MaxDiagnosticBytes)) Full = true;
        Unlock();
    }
    void Append(const char* data, size_t length, u64 now)
    {
        if (!File || Broken || Full) return;
        Flush(now);
        if (Broken || Full) return;
        if (KnownBytes + PendingBytes + length > static_cast<u64>(MaxDiagnosticBytes))
        {
            Flush(now, true);
            if (KnownBytes + length > static_cast<u64>(MaxDiagnosticBytes)) { Full = true; return; }
        }
        if (PendingBytes + length > Pending.size()) { ++DroppedRecords; return; }
        std::memcpy(Pending.data() + PendingBytes, data, length);
        PendingBytes += length;
    }
    void Close()
    {
        if (!File) return;
        Flush(Platform::GetMSCount(), true, true);
        std::fclose(File);
        File = nullptr;
    }
};
struct QueuedFrame
{
    std::vector<u8> Data;
    u32 Round = 0;
    bool Ack = false;
};
struct Context
{
explicit Context(void* owner) : Owner(owner), ContextID(++NextContextID) {}
~Context() { CloseDiagnostic(); if (Listener != InvalidSocket) CloseSocket(Listener); }
void* Owner;
u64 ContextID;
bool Diagnostics = false;
bool DiagnosticFileFull = false;
std::unique_ptr<DiagnosticSink> DiagnosticFile;
u64 DiagnosticEvent = 0;
std::array<u64, 3> FrameTraceCount{};
u64 DropTraceCount = 0;
u32 ObservedHeaderHash = 0, ObservedEntryHash = 0;
struct WifiTraceState { u64 Count = 0, LastMS = 0; };
std::array<WifiTraceState, static_cast<size_t>(WifiDiagnosticEvent::Count)> WifiTrace{};
bool Automatic = false, Selected = false;
bool HostDisconnectPending = false;
Socket Listener = InvalidSocket;
sockaddr_in Peer{};
bool HavePeer = false, HaveBeacon = false, Associated = false;
bool HaveParentIdentity = false;
std::array<u8, 6> ParentMAC = LegacyWiiMAC;
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

void WriteDiagnostic(const char* event, unsigned value, const char* detail = "")
{
    if (!Diagnostics || DiagnosticFileFull) return;
    const auto now = Platform::GetMSCount();
    if (!DiagnosticFile) DiagnosticFile = std::make_unique<DiagnosticSink>(now);
    if (DiagnosticFile->Broken || DiagnosticFile->Full) { DiagnosticFileFull = true; return; }
    char record[1536];
    const int length = std::snprintf(record, sizeof(record), "WII_DS us=%llu event=%s value=%u generation=%llu port=%u instance=%llu trace=%llu mono_ms=%llu selected=%u associated=%u aid=%u channel=%u radio_q=%zu host_q=%zu round=%u diag_dropped=%llu %s\n",
                     static_cast<unsigned long long>(Platform::GetUSCount()), event,
                     value, static_cast<unsigned long long>(SessionGeneration), BoundPort,
                     static_cast<unsigned long long>(ContextID),
                     static_cast<unsigned long long>(++DiagnosticEvent),
                     static_cast<unsigned long long>(now),
                     unsigned(Selected), unsigned(Associated), unsigned(ExpectedAID), unsigned(Channel),
                     RadioFrames.size(), HostFrames.size(), CurrentRound,
                     static_cast<unsigned long long>(DiagnosticFile->DroppedRecords), detail);
    if (length > 0)
    {
        const auto count = std::min<size_t>(static_cast<size_t>(length), sizeof(record) - 1);
        record[count - 1] = '\n';
        DiagnosticFile->Append(record, count, now);
    }
    DiagnosticFileFull = DiagnosticFile->Broken || DiagnosticFile->Full;
}
void FlushDiagnostic()
{
    if (!DiagnosticFile) return;
    DiagnosticFile->Flush(Platform::GetMSCount());
    DiagnosticFileFull = DiagnosticFile->Broken || DiagnosticFile->Full;
}
void CloseDiagnostic()
{
    if (!DiagnosticFile) return;
    DiagnosticFile->Close();
    DiagnosticFile.reset();
}
void Trace(const char* event, const char* format, ...)
{
    if (!Diagnostics || DiagnosticFileFull) return;
    char detail[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    WriteDiagnostic(event, 0, detail);
}
void TraceDrop(const char* event, unsigned value = 0)
{
    if (!Diagnostics) return;
    if (++DropTraceCount > 32 && (DropTraceCount & 63)) return;
    WriteDiagnostic(event, value, "drop/rejection; sampled after first32");
}
void TraceFrame(const char* event, const u8* radio, size_t length, u32 round, unsigned stream)
{
    if (!Diagnostics || !radio || length < 12 + 24 || stream >= FrameTraceCount.size()) return;
    const auto count = ++FrameTraceCount[stream];
    // Keep initial handshakes complete without unbounded packet-file traffic.
    if (count > 512 && (count & 63)) return;
    const auto* frame = radio + 12;
    const unsigned control = unsigned(frame[0]) | (unsigned(frame[1]) << 8);
    const unsigned sequence = (unsigned(frame[22]) | (unsigned(frame[23]) << 8)) >> 4;
    size_t nativeOffset = 24;
    if ((control & 0x00fc) == 0x28 || (control & 0x00fc) == 0x18 || (control & 0x00fc) == 0x58)
        nativeOffset += 4;
    const auto available = length - 12;
    const size_t headLength = nativeOffset < available ? std::min<size_t>(8, available - nativeOffset) : 0;
    char head[17]{};
    for (size_t i = 0; i < headLength; ++i)
        std::snprintf(head + 2 * i, sizeof(head) - 2 * i, "%02X", unsigned(frame[nativeOffset + i]));
    Trace(event,
          "frame_count=%llu packet_round=%u length=%zu fc=%04X type=%u subtype=%u seq=%u rx_channel=%u dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X bssid=%02X:%02X:%02X:%02X:%02X:%02X native_header=%s header_bytes=%zu",
          static_cast<unsigned long long>(count), round, length, control, (control >> 2) & 3, (control >> 4) & 15,
          sequence, unsigned(radio[9]), frame[4], frame[5], frame[6], frame[7], frame[8], frame[9],
          frame[10], frame[11], frame[12], frame[13], frame[14], frame[15],
          frame[16], frame[17], frame[18], frame[19], frame[20], frame[21], head, headLength);
}
void Log(const char* event, unsigned value = 0)
{
    if (!Diagnostics) return;
    const bool hotEvent = !std::strcmp(event, "DS_FRAME_SENT") ||
        !std::strcmp(event, "COMMAND_QUEUED") ||
        !std::strcmp(event, "HOST_FRAME_DELIVERED") ||
        !std::strcmp(event, "RADIO_FRAME_DELIVERED") ||
        !std::strcmp(event, "MP_ACK_FAILURE_MASK");
    if (hotEvent && ++HotCount > 16 && (HotCount & 255)) return;
    WriteDiagnostic(event, value);
}
void ClearRadio(bool clearBeacon)
{
    Trace("RADIO_CLEAR", "clear_beacon=%u expected_channel=%u known_rounds=%zu", unsigned(clearBeacon),
          unsigned(ExpectedChannel), KnownRounds.size());
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
        HaveParentIdentity = false; ParentMAC = LegacyWiiMAC;
        HaveBeacon = false; BeaconData.fill(0); BeaconTimestamp.fill(0);
        RadioSequence = 1; BeaconSequence = CommandSequence = 0; DTIM = 0;
    }
    LastBeacon = 0;
    // Diagnostic histories are per radio generation, not guest state.
    if (Diagnostics)
    {
        FrameTraceCount.fill(0); DropTraceCount = 0;
        ObservedHeaderHash = ObservedEntryHash = 0;
        WifiTrace.fill({});
    }
}
void ClearPeer(const char* reason)
{
    if (Diagnostics)
    {
        const auto now = Platform::GetMSCount();
        Trace("PEER_CLEAR", "reason=%s peer_age_ms=%llu have_peer=%u have_beacon=%u", reason,
              static_cast<unsigned long long>(LastHello && now >= LastHello ? now - LastHello : 0),
              unsigned(HavePeer), unsigned(HaveBeacon));
    }
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
    const int sent = sendto(Listener, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                            reinterpret_cast<const sockaddr*>(&Peer), sizeof(Peer));
    if (Diagnostics && sent != static_cast<int>(packet.size()))
    {
#ifdef _WIN32
        const int error = WSAGetLastError();
#else
        const int error = errno;
#endif
        Trace("UDP_SEND_FAILED", "envelope_type=%u expected=%zu actual=%d socket_error=%d", unsigned(type), packet.size(), sent, error);
    }
    return sent;
}
void QueueBeacon(u8 channel)
{
    if (RadioFrames.size() >= 6) return;
    std::vector<u8> packet(12, 0);
    packet[8] = 0x0a; packet[9] = channel;
    const auto append = [&](std::initializer_list<u8> bytes) { packet.insert(packet.end(), bytes); };
    append({0x80,0,0,0}); append({0xff,0xff,0xff,0xff,0xff,0xff});
    packet.insert(packet.end(), ParentMAC.begin(), ParentMAC.end());
    packet.insert(packet.end(), ParentMAC.begin(), ParentMAC.end());
    append({u8(BeaconSequence << 4),u8(BeaconSequence >> 4)});
    BeaconSequence = (BeaconSequence + 1) & 0xfff;
    const auto time = Platform::GetUSCount();
    for (unsigned i = 0; i < 8; ++i) packet.push_back(u8(time >> (i*8)));
    append({0xd7,0,0x21,0x04});
    append({0x01,0x08,0x82,0x84,0x0b,0x16,0x24,0x30,0x48,0x6c});
    append({0x03,0x01,channel}); append({0x05,0x04,DTIM,0x02,0,0}); DTIM ^= 1;
    append({0x2a,0x01,0,0x2f,0x01,0,0x32,0x04,0x0c,0x12,0x18,0x60});
    const auto gameInfoLength = NintendoGameInfoLength(BeaconData);
    append({0xdd,u8(8 + gameInfoLength),0,0x09,0xbf,0,0xff,0xff});
    packet.insert(packet.end(), BeaconTimestamp.begin(), BeaconTimestamp.end());
    packet.insert(packet.end(), BeaconData.begin(), BeaconData.begin() + gameInfoLength);
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
    std::memcpy(frame + 10, ParentMAC.data(), 6); std::memcpy(frame + 16, ParentMAC.data(), 6);
    frame[22] = u8(CommandSequence << 4); frame[23] = u8(CommandSequence >> 4);
    CommandSequence = (CommandSequence + 1) & 0xfff;
    frame[24] = 0x28; // Existing capture-validated bridge ACK countdown, not SDK deadline.
    frame[26] = u8(failure); frame[27] = u8(failure >> 8);
    return ack;
}
void QueueCommand(const u8* application, size_t length, const WiiDSMPSettings& config, u32 round = 0)
{
    if (length < 1 || 12 + 24 + 4 + length + 4 > MaxRadioPacket) { TraceDrop("DROP_COMMAND_LENGTH", static_cast<unsigned>(length)); return; }
    if (round && KnownRounds.count(round)) { TraceDrop("DROP_DUPLICATE_ROUND", round); return; }
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
    std::memcpy(frame + 10, ParentMAC.data(), 6); std::memcpy(frame + 16, ParentMAC.data(), 6);
    frame[22] = u8(CommandSequence << 4); frame[23] = u8(CommandSequence >> 4);
    CommandSequence = (CommandSequence + 1) & 0xfff;
    frame[24] = u8(config.ReplyTime); frame[25] = u8(config.ReplyTime >> 8);
    frame[26] = u8(config.ClientMask); frame[27] = u8(config.ClientMask >> 8);
    std::memcpy(frame + 28, application, length);
    // IOS swaps the application buffer's first halfword onto the radio.
    if (length >= 2) std::swap(frame[28], frame[29]);
    TraceFrame("COMMAND_HEADER_QUEUED", radio.data(), radio.size(), round, 1);
    HostFrames.push_back({std::move(radio), round, false});
    if (round) KnownRounds.emplace(round, false);
    else HostFrames.push_back({MakeAck(), 0, true});
    Log("COMMAND_QUEUED", static_cast<unsigned>(length));
}
void QueueRoundAck(u32 round, u16 failure)
{
    const auto known = KnownRounds.find(round);
    if (!round || known == KnownRounds.end() || known->second) { TraceDrop("DROP_UNKNOWN_OR_DUPLICATE_ACK", round); return; }
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
    FlushDiagnostic();
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
        if (received <= 0)
        {
            if (Diagnostics && received < 0)
            {
#ifdef _WIN32
                const int error = WSAGetLastError();
                const bool noData = error == WSAEWOULDBLOCK;
#else
                const int error = errno;
                const bool noData = error == EWOULDBLOCK || error == EAGAIN;
#endif
                if (!noData) TraceDrop("UDP_RECV_FAILED", unsigned(error));
            }
            break;
        }
        if (sender.sin_family != AF_INET || sender.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
        if (received == static_cast<int>(sizeof(Hello)-1) && !std::memcmp(packet.data(), Hello, sizeof(Hello)-1))
        {
            if (HavePeer && !SamePeer(sender)) continue; // A second Dolphin cannot silently take over.
            if (!HavePeer) { ClearRadio(true); Peer = sender; HavePeer = true; Log("PEER_HELLO"); }
            LastHello = now;
            char response[96]; const auto length = std::snprintf(response, sizeof(response), "%s %016llX P1 M2", Ack,
                                         static_cast<unsigned long long>(BindNonce));
            sendto(Listener, response, length, 0, reinterpret_cast<const sockaddr*>(&sender), size);
            continue;
        }
        if (!SamePeer(sender)) { TraceDrop("DROP_FOREIGN_PEER", unsigned(received)); continue; }
        if (received < 11 || std::memcmp(packet.data(), Magic.data(), Magic.size()))
        { TraceDrop("DROP_ENVELOPE_HEADER", unsigned(received)); continue; }
        const size_t length = (u16(packet[9]) << 8) | packet[10];
        if (static_cast<size_t>(received) != 11 + length)
        { TraceDrop("DROP_ENVELOPE_LENGTH", unsigned(length)); continue; }
        const auto* body = packet.data() + 11;
        if (packet[8] == 8 && length == 0) { ClearPeer("PEER_STOP"); continue; }
        if ((packet[8] == 2 && length == 130 && !HaveParentIdentity) ||
            (packet[8] == 12 && length == 136))
        {
            // P1 carries the parent identity and beacon atomically. The Wii
            // guest uses this same normal MAC through WD and NCD; never guess
            // it from game payloads or rewrite a user's networking settings.
            if (packet[8] == 12)
            {
                std::array<u8, 6> parent{};
                std::memcpy(parent.data(), body, parent.size());
                if ((parent[0] & 1) || std::all_of(parent.begin(), parent.end(), [](u8 b) { return b == 0; }))
                { Log("INVALID_PARENT_IDENTITY"); continue; }
                // The parent MAC is immutable during one WD session. Without
                // a wire epoch, accepting an in-place change would let an old
                // duplicated beacon switch the active radio back again.
                if (HaveParentIdentity && parent != ParentMAC)
                { Log("PARENT_CHANGE_REQUIRES_SESSION"); continue; }
                if (HaveBeacon && parent != ParentMAC)
                {
                    const bool lostSelectedHost = Automatic && Selected;
                    ClearRadio(true);
                    if (lostSelectedHost) HostDisconnectPending = true;
                }
                const bool changed = !HaveParentIdentity || parent != ParentMAC;
                ParentMAC = parent; HaveParentIdentity = true;
                if (changed) Log("PARENT_IDENTITY");
                body += parent.size();
            }
            // Only discard old beacon copies; in-flight native radio work stays intact.
            RadioFrames.erase(std::remove_if(RadioFrames.begin(), RadioFrames.end(), [](const auto& p) {
                return p.size() > 12 && p[12] == 0x80;
            }), RadioFrames.end());
            const bool metadataChanged = Diagnostics && (!HaveBeacon || std::memcmp(BeaconData.data(), body + 2, 128));
            std::memcpy(BeaconTimestamp.data(), body, 2); std::memcpy(BeaconData.data(), body + 2, 128);
            HaveBeacon = true; LastBeacon = 0; Log("BEACON_CONFIG");
            if (metadataChanged)
            {
                char hex[257];
                u32 hash = 2166136261u;
                for (size_t i = 0; i < BeaconData.size(); ++i)
                {
                    hash = (hash ^ BeaconData[i]) * 16777619u;
                    std::snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02X", unsigned(BeaconData[i]));
                }
                Trace("BEACON_NATIVE_METADATA", "timestamp=%02X%02X native_fnv1a32=%08X native_bytes=128 advertised_native_bytes=%zu native_hex=%s",
                      unsigned(BeaconTimestamp[0]), unsigned(BeaconTimestamp[1]), hash,
                      NintendoGameInfoLength(BeaconData), hex);
            }
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
            // A late management response from a previous parent must not
            // associate the core with that old BSSID after a fresh session.
            if (HaveParentIdentity && (body[12] & 0x0c) == 0 &&
                (std::memcmp(body + 22, ParentMAC.data(), 6) ||
                 std::memcmp(body + 28, ParentMAC.data(), 6)))
            { Log("FOREIGN_PARENT_MANAGEMENT"); continue; }
            if (RadioFrames.size() >= MaxQueuedPackets) { ClearPeer("QUEUE_OVERFLOW"); continue; }
            RadioFrames.emplace_front(body, body + length);
            TraceFrame("MANAGEMENT_HEADER_QUEUED", body, length, 0, 1);
            Log("MANAGEMENT_QUEUED", body[12]);
        }
        else TraceDrop("DROP_UNSUPPORTED_ENVELOPE", packet[8]);
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
void RefreshDiagnostics()
{
    AnyDiagnostics.store(std::any_of(Contexts.begin(), Contexts.end(), [](const auto& entry) {
        return entry.second->Diagnostics;
    }), std::memory_order_relaxed);
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
        RefreshDiagnostics();
        return true;
    }
    auto& entry = Contexts[instance];
    if (!entry) entry = std::make_unique<Context>(instance);
    auto& context = *entry;
    if (context.Automatic) context.ClearRadio(false);
    const bool newlyEnabled = diagnostics && !context.Diagnostics;
    if (context.Diagnostics && !diagnostics) context.CloseDiagnostic();
    context.Diagnostics = diagnostics; context.Automatic = false; context.Selected = false;
    context.HostDisconnectPending = false;
    if (newlyEnabled) { context.DiagnosticFileFull = false; context.Log("DIAGNOSTICS_ENABLED"); }
    RefreshDiagnostics();
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
    const bool newlyEnabled = diagnostics && !context.Diagnostics;
    if (context.Diagnostics && !diagnostics) context.CloseDiagnostic();
    context.Diagnostics = diagnostics; context.Automatic = true;
    if (newlyEnabled) { context.DiagnosticFileFull = false; context.Log("DIAGNOSTICS_ENABLED"); }
    RefreshDiagnostics();
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
    if ((frame[1] & 0x43) || std::memcmp(frame + 4, ParentMAC.data(), 6) ||
        std::memcmp(frame + 16, ParentMAC.data(), 6) || (frame[10] & 1)) return false;
    constexpr u8 empty[6]{};
    if (!std::memcmp(frame + 10, empty, 6) || !std::memcmp(frame + 10, ParentMAC.data(), 6)) return false;
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
    const auto* context = Find(instance);
    return context && data && length >= 12 + 24 &&
        !std::memcmp(data + 12 + 10, context->ParentMAC.data(), 6) &&
        !std::memcmp(data + 12 + 16, context->ParentMAC.data(), 6);
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
bool DiagnosticsEnabled() { return AnyDiagnostics.load(std::memory_order_relaxed); }
void ObserveARM9Jump(void* instance, u32 address, const u8* ramHeader)
{
    if (!DiagnosticsEnabled() || !ramHeader) return;
    const auto read32 = [ramHeader](size_t offset) {
        return u32(ramHeader[offset]) | (u32(ramHeader[offset + 1]) << 8) |
               (u32(ramHeader[offset + 2]) << 16) | (u32(ramHeader[offset + 3]) << 24);
    };
    // Reject ordinary branches with one header-word read, before locks/hash.
    // This hook is not a per-instruction logger and is not exhaustive in JIT.
    const u32 entry = read32(0x24);
    if ((address & ~1u) != (entry & ~1u)) return;
    const u32 load = read32(0x28), size = read32(0x2c);
    if (load < 0x02000000 || size == 0 || u64(load) + size > 0x02400000 ||
        (entry & ~1u) < load || (entry & ~1u) >= u64(load) + size) return;
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    if (!context || !context->Diagnostics) return;
    u32 hash = 2166136261u;
    for (size_t i = 0; i < 0x40; ++i) hash = (hash ^ ramHeader[i]) * 16777619u;
    if (hash != context->ObservedHeaderHash)
    {
        context->ObservedHeaderHash = hash;
        context->Trace("RAM_BOOT_HEADER_OBSERVED", "header_hash=%08X arm9_entry=%08X arm9_load=%08X arm9_size=%u arm7_entry=%08X arm7_load=%08X arm7_size=%u note=header_presence_is_not_transfer_completion",
                       hash, entry, load, size, read32(0x34), read32(0x38), read32(0x3c));
    }
    if ((address & ~1u) == (entry & ~1u) && context->ObservedEntryHash != hash)
    {
        context->ObservedEntryHash = hash;
        context->Trace("RAM_ENTRY_JUMP_OBSERVED", "pc=%08X header_hash=%08X arm9_size=%u note=observed_ARM9_jump_only_not_application_connection_success", address, hash, size);
    }
}
void TraceWifiEvent(void* instance, WifiDiagnosticEvent event, u32 value0, u32 value1,
                   u32 value2, u32 value3, const u8* radio, u32 radioLength)
{
    if (!DiagnosticsEnabled()) return;
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    const auto index = static_cast<size_t>(event);
    if (!context || !context->Diagnostics || context->DiagnosticFileFull || index >= context->WifiTrace.size()) return;
    constexpr const char* names[] = {
        "CORE_WAIT_MP_READY", "CORE_WAIT_POWER", "CORE_WAIT_RX_ENABLED", "CORE_WAIT_RX_RING",
        "CORE_DROP_SHORT_FRAME", "CORE_DROP_FRAME_LENGTH", "CORE_DROP_CHANNEL", "CORE_DROP_NOT_MP",
        "CORE_HOST_LOST", "CORE_DEAUTHENTICATED", "CORE_RX_REJECTED", "CORE_RX_COMMITTED", "CORE_RX_RING_FULL"
    };
    auto& state = context->WifiTrace[index];
    const auto count = ++state.Count;
    const auto now = Platform::GetMSCount();
    const bool quietGate = index <= static_cast<size_t>(WifiDiagnosticEvent::WaitRXRing);
    // Silent polling can be 1500Hz: at most one file record/second/reason.
    // Do not log every differing cursor/counter value as a state change.
    if (quietGate && count > 1 && now >= state.LastMS && now - state.LastMS < 1000) return;
    if (!quietGate && count > 512 && (count & 63)) return;
    state.LastMS = now;
    context->Trace(names[index], "event_count=%llu v0=%08X v1=%08X v2=%08X v3=%08X",
                   static_cast<unsigned long long>(count), value0, value1, value2, value3);
    if (radio && radioLength >= 12 + 24 && radioLength <= MaxRadioPacket)
        context->TraceFrame("CORE_RX_HEADER", radio, radioLength, context->CurrentRound, 2);
}
u64 Generation(void* instance) { std::lock_guard<std::mutex> lock(Mutex); auto* c = Find(instance); return c ? c->SessionGeneration : 0; }
unsigned IdentitySlot(void* instance)
{
    std::lock_guard<std::mutex> lock(Mutex);
    auto* context = Find(instance);
    if (!context) return 0;
    context->EnsureSocket();
    return context->Listener != InvalidSocket ? context->BoundPort - FirstPort : 0;
}
std::array<u8,6> ParentBSSID(void* instance)
{
    std::lock_guard<std::mutex> lock(Mutex);
    const auto* context = Find(instance);
    return context ? context->ParentMAC : LegacyWiiMAC;
}
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
    if ((Automatic && !Selected) || !data || length < 12 + 24 || static_cast<size_t>(length) > MaxRadioPacket)
    { TraceDrop("DROP_TX_NOT_SELECTED_OR_LENGTH", unsigned(length)); return 0; }
    Pump();
    if (!HavePeer || (Automatic && !Selected)) { TraceDrop("DROP_TX_NO_PEER", unsigned(length)); return 0; }
    const auto* frame = data + 12;
    const bool reply = Associated && HaveClientMAC && data[9] == Channel && length >= 12 + 24 + 4 &&
        (frame[0] == 0x18 || frame[0] == 0x58) && (frame[1] & 3) == 1 &&
        !std::memcmp(frame + 4, ParentMAC.data(), 6) && !std::memcmp(frame + 10, ClientMAC.data(), 6) &&
        !std::memcmp(frame + 16, ReplyMAC, 6) && ExpectedAID > 0 && ExpectedAID < 16;
    if (reply && MultiRound && !CurrentRound) { TraceDrop("DROP_REPLY_WITHOUT_ROUND", unsigned(length)); return 0; }
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
        !std::memcmp(frame + 4, ParentMAC.data(), 6) && !std::memcmp(frame + 16, ParentMAC.data(), 6) &&
        !(frame[10] & 1))
    {
        constexpr u8 empty[6]{};
        if (std::memcmp(frame + 10, empty, 6))
        { std::memcpy(ClientMAC.data(), frame + 10, 6); HaveClientMAC = true; Log("CLIENT_ASSOC_TX"); }
    }
    if (reply)
        RoundReplies |= u16(1u << ExpectedAID) & RoundClients;
    TraceFrame("DS_HEADER_TRANSMITTED", data, static_cast<size_t>(length), reply ? CurrentRound : 0, 0);
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
    if (host && (HostFrames.empty() || !Associated))
    {
        if (!HostFrames.empty() && !Associated) TraceDrop("HOST_DELIVERY_WAIT_ASSOCIATION", unsigned(HostFrames.size()));
        return 0;
    }
    if (!host && RadioFrames.empty()) return 0;
    std::vector<u8> packet;
    u32 round = 0;
    if (host)
    {
        const auto& next = HostFrames.front();
        if (!next.Ack && (!CommandAllowed || CurrentRound))
        { TraceDrop("HOST_DELIVERY_WAIT_COMMAND_GATE", next.Round); return 0; }
        if (next.Ack && next.Round && CurrentRound != next.Round)
        { TraceDrop("HOST_DELIVERY_WAIT_ACK_ROUND", next.Round); return 0; }
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
    if (packet.size() >= 12 + 24 && !std::memcmp(packet.data() + 22, ParentMAC.data(), 6))
    {
        packet[34] = (packet[34] & 0xf) | u8(RadioSequence << 4);
        packet[35] = u8(RadioSequence >> 4); RadioSequence = (RadioSequence + 1) & 0xfff;
    }
    std::memcpy(data, packet.data(), packet.size());
    *timestamp = host || Associated ? 0 : Platform::GetUSCount();
    if (!host && packet.size() >= 12 + 36 && packet[12] == 0x80 && packet[13] == 0)
        SendEnvelope(6, nullptr, 0);
    TraceFrame(host ? "HOST_HEADER_DELIVERED" : "RADIO_HEADER_DELIVERED", packet.data(), packet.size(), round, 2);
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
    {
        if (Diagnostics)
            Trace("GUEST_AID_REJECTED", "observed_aid=%u observed_channel=%u expected_aid=%u expected_channel=%u", unsigned(acceptedAID), unsigned(channel), unsigned(ExpectedAID), unsigned(ExpectedChannel));
        return;
    }
    if (associated == Associated) return;
    if (associated)
    { Channel = channel; for (auto& p : HostFrames) p.Data[9] = channel; Associated = true; }
    else
    { HostFrames.clear(); KnownRounds.clear(); CurrentRound = 0; Associated = false; CommandAllowed = true; RoundClients = RoundReplies = 0; ++SessionGeneration; }
    Log("GUEST_AID", acceptedAID);
}
#ifdef WII_DS_DIAGNOSTIC_TESTING
// Test-only seams: absent from emulator builds and never affect the protocol.
void FlushDiagnosticsForTest()
{
    std::lock_guard<std::mutex> lock(Mutex);
    for (auto& entry : Contexts)
        if (entry.second->DiagnosticFile)
            entry.second->DiagnosticFile->Flush(Platform::GetMSCount(), true, true);
}
void EmitDiagnosticsForTest(void* instance, unsigned count)
{
    std::lock_guard<std::mutex> lock(Mutex);
    if (auto* context = Find(instance))
        for (unsigned i = 0; i < count; ++i)
            context->Trace("DIAGNOSTIC_WRITER_FIXTURE", "record=%u synthetic_metadata_only=1", i);
}
#endif
}
