// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#include "rtmp/flv.h"

#include "rtmp/bytes.h"

namespace rtmp {

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
	return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
	       (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

VideoCodec video_codec_from_fourcc(uint32_t cc)
{
	if (cc == fourcc('a', 'v', 'c', '1'))
		return VideoCodec::H264;
	if (cc == fourcc('h', 'v', 'c', '1'))
		return VideoCodec::HEVC;
	if (cc == fourcc('a', 'v', '0', '1'))
		return VideoCodec::AV1;
	if (cc == fourcc('v', 'p', '0', '9'))
		return VideoCodec::VP9;
	return VideoCodec::Unknown;
}

AudioCodec audio_codec_from_fourcc(uint32_t cc)
{
	if (cc == fourcc('m', 'p', '4', 'a'))
		return AudioCodec::AAC;
	if (cc == fourcc('O', 'p', 'u', 's'))
		return AudioCodec::Opus;
	return AudioCodec::Unknown;
}

int32_t read_composition_time(const uint8_t *p)
{
	int32_t v = static_cast<int32_t>(rd24(p));
	if (v & 0x800000)
		v -= 0x1000000;
	return v;
}

constexpr uint8_t kFrameKey = 1;
constexpr uint8_t kFrameGeneratedKey = 4;
constexpr uint8_t kFrameCommand = 5;

} // namespace

const char *codec_name(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::H264:
		return "H.264";
	case VideoCodec::HEVC:
		return "HEVC";
	case VideoCodec::AV1:
		return "AV1";
	case VideoCodec::VP9:
		return "VP9";
	default:
		return "unknown";
	}
}

const char *codec_name(AudioCodec codec)
{
	switch (codec) {
	case AudioCodec::AAC:
		return "AAC";
	case AudioCodec::MP3:
		return "MP3";
	case AudioCodec::Opus:
		return "Opus";
	default:
		return "unknown";
	}
}

ParseStatus parse_video_tag(const uint8_t *d, size_t n, int64_t dts_ms, VideoPacket &out)
{
	out = VideoPacket();
	if (n < 1)
		return ParseStatus::Invalid;
	out.dts_ms = dts_ms;

	const uint8_t b0 = d[0];

	if (b0 & 0x80) {
		// Enhanced RTMP: | 1 | frame type (3) | packet type (4) | FourCC | ...
		const uint8_t frame_type = (b0 >> 4) & 0x07;
		const uint8_t packet_type = b0 & 0x0F;
		if (frame_type == kFrameCommand)
			return ParseStatus::Skip;
		if (n < 5)
			return ParseStatus::Invalid;

		out.codec = video_codec_from_fourcc(rd32(d + 1));
		if (out.codec == VideoCodec::Unknown)
			return ParseStatus::Unsupported;

		size_t off = 5;
		switch (packet_type) {
		case 0: // sequence start
			out.config = true;
			break;
		case 1: // coded frames, with a composition time for AVC / HEVC
			if (out.codec == VideoCodec::H264 || out.codec == VideoCodec::HEVC) {
				if (n < off + 3)
					return ParseStatus::Invalid;
				out.cts_ms = read_composition_time(d + off);
				off += 3;
			}
			break;
		case 2: // sequence end
			out.end_of_sequence = true;
			break;
		case 3: // coded frames without composition time
			break;
		default: // metadata, MPEG-2 TS config, multitrack, ModEx
			return ParseStatus::Skip;
		}

		out.keyframe = frame_type == kFrameKey || frame_type == kFrameGeneratedKey;
		out.data = d + off;
		out.size = n - off;
		return ParseStatus::Ok;
	}

	// Classic layout: | frame type (4) | codec id (4) | AVC packet type | cts (3) | ...
	const uint8_t frame_type = b0 >> 4;
	const uint8_t codec_id = b0 & 0x0F;
	if (frame_type == kFrameCommand)
		return ParseStatus::Skip;
	if (codec_id != 7 && codec_id != 12)
		return ParseStatus::Unsupported;
	if (n < 5)
		return ParseStatus::Invalid;

	out.codec = codec_id == 7 ? VideoCodec::H264 : VideoCodec::HEVC;
	out.keyframe = frame_type == kFrameKey;
	out.cts_ms = read_composition_time(d + 2);
	switch (d[1]) {
	case 0:
		out.config = true;
		break;
	case 1:
		break;
	case 2:
		out.end_of_sequence = true;
		break;
	default:
		return ParseStatus::Invalid;
	}
	out.data = d + 5;
	out.size = n - 5;
	return ParseStatus::Ok;
}

ParseStatus parse_audio_tag(const uint8_t *d, size_t n, int64_t dts_ms, AudioPacket &out)
{
	out = AudioPacket();
	if (n < 1)
		return ParseStatus::Invalid;
	out.dts_ms = dts_ms;

	const uint8_t format = d[0] >> 4;

	switch (format) {
	case 10: // AAC: | 0xAF | packet type | data |
		if (n < 2)
			return ParseStatus::Invalid;
		if (d[1] > 1)
			return ParseStatus::Invalid;
		out.codec = AudioCodec::AAC;
		out.config = d[1] == 0;
		out.data = d + 2;
		out.size = n - 2;
		return ParseStatus::Ok;

	case 2:  // MP3
	case 14: { // MP3 8 kHz
		static const uint32_t kRates[4] = {5512, 11025, 22050, 44100};
		out.codec = AudioCodec::MP3;
		out.sample_rate_hint = format == 14 ? 8000 : kRates[(d[0] >> 2) & 3];
		out.channels_hint = static_cast<uint8_t>((d[0] & 1) + 1);
		out.data = d + 1;
		out.size = n - 1;
		return ParseStatus::Ok;
	}

	case 9: { // Enhanced RTMP: | 9 | packet type (4) | FourCC | ...
		if (n < 5)
			return ParseStatus::Invalid;
		out.codec = audio_codec_from_fourcc(rd32(d + 1));
		if (out.codec == AudioCodec::Unknown)
			return ParseStatus::Unsupported;
		const uint8_t packet_type = d[0] & 0x0F;
		if (packet_type == 0)
			out.config = true;
		else if (packet_type != 1)
			return ParseStatus::Skip; // sequence end, multichannel config, multitrack, ModEx
		out.data = d + 5;
		out.size = n - 5;
		return ParseStatus::Ok;
	}

	default:
		return ParseStatus::Unsupported;
	}
}

} // namespace rtmp
