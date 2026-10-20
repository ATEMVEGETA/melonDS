/*
 * Copyright 2026 Wii-DS bridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WIIDS_MP_CONFIG_H
#define WIIDS_MP_CONFIG_H
#include <array>
#include <cstdint>
#include <optional>
namespace melonDS
{
struct WiiDSMPSettings
{
    std::uint16_t ReplyTime, ClientMask, Duration, SDKDeadline;
};
// WD vector1 is big-endian guest data. Do not infer framing from title or
// application length. AID zero is the parent; child bits one through fifteen
// each reserve a reply slot in the same multiplayer command round.
inline std::optional<WiiDSMPSettings> DecodeWiiDSMPConfig(
    const std::array<std::uint8_t, 16>& config)
{
    const auto be16 = [&config](unsigned offset) {
        return (std::uint32_t(config[offset]) << 8) | config[offset + 1];
    };
    if (be16(0) || be16(2) || be16(4) || be16(12) || be16(14))
        return std::nullopt;
    const auto child = be16(6), mask = be16(8);
    const auto time = (child & 0x8000) ? (child & 0x7fff) : 214 + 4 * child;
    if ((mask & 1) || time < 214 || time > 0xffff)
        return std::nullopt;
    unsigned clients = 0;
    for (auto bits = mask; bits; bits >>= 1) clients += bits & 1;
    const auto duration = 234 + clients * (time + 10);
    if (duration > 0xffff) return std::nullopt;
    return WiiDSMPSettings{std::uint16_t(time), std::uint16_t(mask),
                          std::uint16_t(duration),
                          std::uint16_t(be16(10))};
}
}
#endif
