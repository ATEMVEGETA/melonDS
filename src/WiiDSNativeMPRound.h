/*
 * Copyright 2026 Wii-DS bridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WIIDS_NATIVE_MP_ROUND_H
#define WIIDS_NATIVE_MP_ROUND_H
#include <cstdint>
namespace melonDS
{
// Transport ownership only: never synthesize guest SDK completion flags.
class WiiDSNativeMPRound
{
public:
    void Reset() { RoundPending = false; AckPending = false; AckEndCursor = 0; }
    void CommandCommitted() { RoundPending = true; AckPending = false; }
    void AckCommitted(std::uint16_t endCursor)
    {
        if (RoundPending && !AckPending) { AckPending = true; AckEndCursor = endCursor; }
    }
    void CursorAdvanced(std::uint16_t oldCursor, std::uint16_t newCursor,
                        std::uint16_t begin, std::uint16_t end)
    {
        if (!AckPending || begin >= end || oldCursor == newCursor ||
            oldCursor < begin || oldCursor >= end || newCursor < begin ||
            newCursor >= end || AckEndCursor < begin || AckEndCursor >= end)
            return;
        const std::uint32_t size = end - begin;
        const auto distance = [oldCursor, size](std::uint16_t cursor) {
            return cursor >= oldCursor ? std::uint32_t(cursor - oldCursor)
                                       : size - std::uint32_t(oldCursor - cursor);
        };
        const auto consumed = distance(AckEndCursor);
        if (consumed && consumed <= distance(newCursor)) Reset();
    }
    bool CanReceiveCommand(bool replyTXBusy) const { return !RoundPending && !replyTXBusy; }
    bool Pending() const { return RoundPending; }
    bool AwaitingAckConsumption() const { return AckPending; }
private:
    bool RoundPending = false, AckPending = false;
    std::uint16_t AckEndCursor = 0;
};
}
#endif
