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
u64 Generation(void* instance);
// A context retains its exclusively bound radio port for its lifetime. The
// slot also distinguishes guest MACs across separately launched processes.
unsigned IdentitySlot(void* instance);
int Send(void* instance, const u8* data, int length);
int Receive(void* instance, u8* data, u64* timestamp, bool host);
void SetCommandAllowed(void* instance, bool allowed);
void NotifyAssociationResponse(void* instance, u16 aid, u8 channel);
void NotifyAID(void* instance, u16 aid, u8 channel);
std::array<u8, 6> ParentBSSID();
}
}
#endif
