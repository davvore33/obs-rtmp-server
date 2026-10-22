// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include "rtmp/types.h"

#include <cstddef>
#include <cstdint>

namespace rtmp {

enum class ParseStatus {
	Ok,          // `out` is filled in
	Skip,        // well formed, but nothing to deliver (command frame, sequence end...)
	Unsupported, // a codec this server does not demux
	Invalid,     // truncated or malformed
};

// Parses the payload of an RTMP video message (an FLV video tag body). The
// returned packet points into `data`.
//
// Handles the classic tag layout (H.264, plus HEVC sent as codec id 12) and
// Enhanced RTMP FourCC tags (avc1, hvc1, av01, vp09).
ParseStatus parse_video_tag(const uint8_t *data, size_t size, int64_t dts_ms, VideoPacket &out);

// Parses the payload of an RTMP audio message (an FLV audio tag body):
// AAC, MP3 and Enhanced RTMP mp4a / Opus.
ParseStatus parse_audio_tag(const uint8_t *data, size_t size, int64_t dts_ms, AudioPacket &out);

} // namespace rtmp
