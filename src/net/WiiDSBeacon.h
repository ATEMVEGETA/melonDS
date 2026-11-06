/*
 * Copyright 2026 Wii-DS bridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WIIDS_BEACON_H
#define WIIDS_BEACON_H

#include <array>
#include <cstddef>
#include "../types.h"

namespace melonDS::WiiDSBridge
{
// WD provides a fixed-size storage buffer, not a fixed-size radio record.
// Nintendo version-1 GameInfo has a 16-byte header and an explicit user-data
// length at offset 0x0A. Advertising its unused storage padding makes strict
// BSS descriptor validation fail after a host changes advertisement type.
// Preserve the existing opaque-buffer behavior for unknown/malformed formats.
constexpr std::size_t NintendoGameInfoLength(const std::array<u8, 128>& data)
{
    constexpr std::size_t HeaderSize = 16;
    if (data[0] != 1 || data[1] != 0 || data[0x0A] > data.size() - HeaderSize)
        return data.size();
    return HeaderSize + data[0x0A];
}
}

#endif
