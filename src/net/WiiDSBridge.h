/*
 * Copyright 2026 Wii-DS bridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WIIDS_BRIDGE_H
#define WIIDS_BRIDGE_H

#include <array>
#include "../types.h"

namespace melonDS
{
// A separate radio backend. Automatic discovery leaves LocalMP/LAN intact
// until the guest explicitly authenticates/associates with the Wii parent.
namespace WiiDSBridge
{
bool Configure(void* instance, bool enabled, bool diagnostics);
bool ConfigureAutomatic(void* instance, bool enabled, bool diagnostics);
void Release(void* instance);
bool IsDiscoverable(void* instance);
bool IsAutomatic(void* instance);
bool IsEnabled(void* instance);
bool IsActive(void* instance);
bool SelectForTransmit(void* instance, const u8* data, int length);
bool IsWiiFrame(void* instance, const u8* data, int length);
// A lost selected Wii parent must not fall through to blocking LocalMP host
// reception. Report it once through the core's existing "host gone" path.
bool ConsumeHostDisconnect(void* instance);
void Process(void* instance);
void ResetRadio(void* instance);
// Opt-in observation only: never changes guest RAM, execution or radio state.
bool DiagnosticsEnabled();
void ObserveARM9Jump(void* instance, u32 address, const u8* ramHeader);
enum class WifiDiagnosticEvent : u8
{
    WaitMPReady, WaitPower, WaitRXEnabled, WaitRXRing,
    DropShortFrame, DropFrameLength, DropChannel, DropNotMP,
    HostLost, Deauthenticated, ReceiveRejected, ReceiveCommitted, RingFull,
    Count
};
void TraceWifiEvent(void* instance, WifiDiagnosticEvent event, u32 value0 = 0,
                   u32 value1 = 0, u32 value2 = 0, u32 value3 = 0,
                   const u8* radio = nullptr, u32 radioLength = 0);
u64 Generation(void* instance);
// A context retains its exclusively bound radio port for its lifetime. The
// slot also distinguishes guest MACs across separately launched processes.
unsigned IdentitySlot(void* instance);
int Send(void* instance, const u8* data, int length);
int Receive(void* instance, u8* data, u64* timestamp, bool host);
void SetCommandAllowed(void* instance, bool allowed);
void NotifyAssociationResponse(void* instance, u16 aid, u8 channel);
void NotifyAID(void* instance, u16 aid, u8 channel);
// The selected Dolphin's identity is per radio, not a globally fixed address.
std::array<u8, 6> ParentBSSID(void* instance);
}
}
#endif
