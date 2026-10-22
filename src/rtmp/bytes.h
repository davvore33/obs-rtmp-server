// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtmp {

// Big-endian readers/writers used throughout the protocol code.

inline uint16_t rd16(const uint8_t *p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t rd24(const uint8_t *p)
{
	return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

inline uint32_t rd32(const uint8_t *p)
{
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// RTMP message headers carry the message stream id little-endian.
inline uint32_t rd32le(const uint8_t *p)
{
	return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
	       (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

inline void wr8(std::vector<uint8_t> &out, uint8_t v)
{
	out.push_back(v);
}

inline void wr16(std::vector<uint8_t> &out, uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

inline void wr24(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

inline void wr32(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

inline void wr32le(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 24));
}

} // namespace rtmp
