// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#include "test_util.h"

#include "rtmp/flv.h"

using namespace rtmp;

static void test_avc_video()
{
	// Keyframe, AVC sequence header (avcC).
	const uint8_t seq[] = {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00, 0x1F};
	VideoPacket p;
	CHECK(parse_video_tag(seq, sizeof(seq), 1000, p) == ParseStatus::Ok);
	CHECK(p.codec == VideoCodec::H264);
	CHECK(p.config);
	CHECK(p.keyframe);
	CHECK_EQ(p.dts_ms, int64_t(1000));
	CHECK_EQ(p.size, size_t(4));
	CHECK_EQ(p.data[0], uint8_t(0x01));

	// Inter frame with a positive composition time.
	const uint8_t inter[] = {0x27, 0x01, 0x00, 0x00, 0x42, 0xAA, 0xBB};
	CHECK(parse_video_tag(inter, sizeof(inter), 2000, p) == ParseStatus::Ok);
	CHECK(!p.config);
	CHECK(!p.keyframe);
	CHECK_EQ(p.cts_ms, 0x42);
	CHECK_EQ(p.size, size_t(2));

	// Negative composition time (24-bit two's complement).
	const uint8_t negative[] = {0x27, 0x01, 0xFF, 0xFF, 0xFE, 0x01};
	CHECK(parse_video_tag(negative, sizeof(negative), 0, p) == ParseStatus::Ok);
	CHECK_EQ(p.cts_ms, -2);

	// End of sequence.
	const uint8_t eos[] = {0x17, 0x02, 0x00, 0x00, 0x00};
	CHECK(parse_video_tag(eos, sizeof(eos), 0, p) == ParseStatus::Ok);
	CHECK(p.end_of_sequence);
}

static void test_legacy_hevc_and_skips()
{
	// Codec id 12 is HEVC with the AVC tag layout.
	const uint8_t hevc[] = {0x1C, 0x00, 0x00, 0x00, 0x00, 0xAA};
	VideoPacket p;
	CHECK(parse_video_tag(hevc, sizeof(hevc), 0, p) == ParseStatus::Ok);
	CHECK(p.codec == VideoCodec::HEVC);
	CHECK(p.config);

	// Sorenson H.263 (codec 2) is not supported.
	const uint8_t h263[] = {0x12, 0x00, 0x00, 0x00, 0x00};
	CHECK(parse_video_tag(h263, sizeof(h263), 0, p) == ParseStatus::Unsupported);

	// Video info/command frame.
	const uint8_t command[] = {0x57, 0x01};
	CHECK(parse_video_tag(command, sizeof(command), 0, p) == ParseStatus::Skip);

	// Truncated.
	CHECK(parse_video_tag(hevc, 0, 0, p) == ParseStatus::Invalid);
	const uint8_t cut[] = {0x17, 0x01, 0x00};
	CHECK(parse_video_tag(cut, sizeof(cut), 0, p) == ParseStatus::Invalid);

	// Unknown AVC packet type.
	const uint8_t bad[] = {0x17, 0x09, 0x00, 0x00, 0x00};
	CHECK(parse_video_tag(bad, sizeof(bad), 0, p) == ParseStatus::Invalid);
}

static void test_enhanced_video()
{
	VideoPacket p;

	// IsExHeader | keyframe | SequenceStart, 'hvc1'
	const uint8_t hvc_seq[] = {0x90, 'h', 'v', 'c', '1', 0x01, 0x02};
	CHECK(parse_video_tag(hvc_seq, sizeof(hvc_seq), 0, p) == ParseStatus::Ok);
	CHECK(p.codec == VideoCodec::HEVC);
	CHECK(p.config);
	CHECK(p.keyframe);
	CHECK_EQ(p.size, size_t(2));

	// CodedFrames carry a composition time for hvc1.
	const uint8_t hvc_frame[] = {0xA1, 'h', 'v', 'c', '1', 0x00, 0x00, 0x21, 0xCC};
	CHECK(parse_video_tag(hvc_frame, sizeof(hvc_frame), 0, p) == ParseStatus::Ok);
	CHECK(!p.keyframe);
	CHECK_EQ(p.cts_ms, 0x21);
	CHECK_EQ(p.size, size_t(1));

	// CodedFramesX has none, and AV1 never has one.
	const uint8_t hvc_x[] = {0x93, 'h', 'v', 'c', '1', 0xCC};
	CHECK(parse_video_tag(hvc_x, sizeof(hvc_x), 0, p) == ParseStatus::Ok);
	CHECK_EQ(p.cts_ms, 0);
	CHECK_EQ(p.size, size_t(1));

	const uint8_t av1[] = {0x91, 'a', 'v', '0', '1', 0xDD, 0xEE};
	CHECK(parse_video_tag(av1, sizeof(av1), 0, p) == ParseStatus::Ok);
	CHECK(p.codec == VideoCodec::AV1);
	CHECK_EQ(p.size, size_t(2));

	// Unknown FourCC and metadata packets are skipped, not errors.
	const uint8_t unknown[] = {0x91, 'x', 'x', 'x', 'x', 0x00};
	CHECK(parse_video_tag(unknown, sizeof(unknown), 0, p) == ParseStatus::Unsupported);
	const uint8_t metadata[] = {0x94, 'h', 'v', 'c', '1', 0x00};
	CHECK(parse_video_tag(metadata, sizeof(metadata), 0, p) == ParseStatus::Skip);

	// Too short for a FourCC.
	const uint8_t cut[] = {0x91, 'h', 'v'};
	CHECK(parse_video_tag(cut, sizeof(cut), 0, p) == ParseStatus::Invalid);
}

static void test_audio()
{
	AudioPacket p;

	const uint8_t aac_cfg[] = {0xAF, 0x00, 0x12, 0x10};
	CHECK(parse_audio_tag(aac_cfg, sizeof(aac_cfg), 5, p) == ParseStatus::Ok);
	CHECK(p.codec == AudioCodec::AAC);
	CHECK(p.config);
	CHECK_EQ(p.size, size_t(2));
	CHECK_EQ(p.dts_ms, int64_t(5));

	const uint8_t aac_raw[] = {0xAF, 0x01, 0x21, 0x22, 0x23};
	CHECK(parse_audio_tag(aac_raw, sizeof(aac_raw), 0, p) == ParseStatus::Ok);
	CHECK(!p.config);
	CHECK_EQ(p.size, size_t(3));

	// MP3, 44.1 kHz stereo.
	const uint8_t mp3[] = {0x2F, 0xFF, 0xFB, 0x90};
	CHECK(parse_audio_tag(mp3, sizeof(mp3), 0, p) == ParseStatus::Ok);
	CHECK(p.codec == AudioCodec::MP3);
	CHECK_EQ(p.sample_rate_hint, uint32_t(44100));
	CHECK_EQ(p.channels_hint, uint8_t(2));
	CHECK_EQ(p.size, size_t(3));

	// Enhanced RTMP Opus: sequence start, then coded frames.
	const uint8_t opus_cfg[] = {0x90, 'O', 'p', 'u', 's', 0x01};
	CHECK(parse_audio_tag(opus_cfg, sizeof(opus_cfg), 0, p) == ParseStatus::Ok);
	CHECK(p.codec == AudioCodec::Opus);
	CHECK(p.config);
	const uint8_t opus_frame[] = {0x91, 'O', 'p', 'u', 's', 0x01, 0x02};
	CHECK(parse_audio_tag(opus_frame, sizeof(opus_frame), 0, p) == ParseStatus::Ok);
	CHECK(!p.config);
	CHECK_EQ(p.size, size_t(2));

	// Nellymoser (6) and an unknown enhanced FourCC are unsupported; a sequence end is just skipped.
	const uint8_t nelly[] = {0x6E, 0x00};
	CHECK(parse_audio_tag(nelly, sizeof(nelly), 0, p) == ParseStatus::Unsupported);
	const uint8_t flac[] = {0x91, 'f', 'L', 'a', 'C', 0x00};
	CHECK(parse_audio_tag(flac, sizeof(flac), 0, p) == ParseStatus::Unsupported);
	const uint8_t end[] = {0x92, 'O', 'p', 'u', 's'};
	CHECK(parse_audio_tag(end, sizeof(end), 0, p) == ParseStatus::Skip);

	// Truncated.
	CHECK(parse_audio_tag(aac_cfg, 0, 0, p) == ParseStatus::Invalid);
	const uint8_t cut[] = {0xAF};
	CHECK(parse_audio_tag(cut, sizeof(cut), 0, p) == ParseStatus::Invalid);
}

int main()
{
	test_avc_video();
	test_legacy_hevc_and_skips();
	test_enhanced_video();
	test_audio();
	return finish_tests("flv");
}
