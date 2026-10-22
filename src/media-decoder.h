// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#pragma once

#include "rtmp/types.h"

#include <media-io/audio-io.h>
#include <media-io/video-io.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace rtmpsrc {

// A decoded picture in a format OBS ingests as-is. The plane pointers are only
// valid during the callback.
struct DecodedVideo {
	const uint8_t *data[MAX_AV_PLANES] = {};
	uint32_t linesize[MAX_AV_PLANES] = {};
	uint32_t width = 0;
	uint32_t height = 0;
	video_format format = VIDEO_FORMAT_NONE;
	video_range_type range = VIDEO_RANGE_PARTIAL;
	video_colorspace colorspace = VIDEO_CS_709;
	video_trc trc = VIDEO_TRC_DEFAULT;
	int64_t pts_ms = 0;
};

class VideoDecoder {
public:
	using FrameCallback = std::function<void(const DecodedVideo &)>;

	VideoDecoder();
	~VideoDecoder();

	VideoDecoder(const VideoDecoder &) = delete;
	VideoDecoder &operator=(const VideoDecoder &) = delete;

	// `config` is the avcC / hvcC / av1C record from the sequence header.
	bool open(rtmp::VideoCodec codec, const uint8_t *config, size_t config_size, std::string &error);
	void close();
	bool is_open() const { return ctx_ != nullptr; }
	// True if the decoder was opened for exactly this codec and configuration.
	bool matches(rtmp::VideoCodec codec, const uint8_t *config, size_t config_size) const;

	// Decodes one access unit and reports every picture that comes out.
	// Returns false when the stream looks broken enough to reopen the decoder.
	bool decode(const rtmp::VideoPacket &packet, const FrameCallback &on_frame);

	rtmp::VideoCodec codec() const { return codec_; }

private:
	void emit(const AVFrame &frame, const FrameCallback &on_frame);

	AVCodecContext *ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVFrame *converted_ = nullptr;
	SwsContext *sws_ = nullptr;
	rtmp::VideoCodec codec_ = rtmp::VideoCodec::Unknown;
	std::vector<uint8_t> config_;
	int consecutive_errors_ = 0;
};

// Planar float PCM, laid out the way obs_source_output_audio wants it. The
// plane pointers are only valid during the callback.
struct DecodedAudio {
	const float *planes[MAX_AV_PLANES] = {};
	uint32_t frames = 0;
	uint32_t sample_rate = 0;
	speaker_layout layout = SPEAKERS_STEREO;
	int64_t pts_ms = 0;
};

class AudioDecoder {
public:
	using FrameCallback = std::function<void(const DecodedAudio &)>;

	AudioDecoder();
	~AudioDecoder();

	AudioDecoder(const AudioDecoder &) = delete;
	AudioDecoder &operator=(const AudioDecoder &) = delete;

	// `config` is the AudioSpecificConfig / OpusHead (may be empty for MP3).
	bool open(rtmp::AudioCodec codec, const uint8_t *config, size_t config_size, uint32_t sample_rate_hint,
		  int channels_hint, std::string &error);
	void close();
	bool is_open() const { return ctx_ != nullptr; }
	bool matches(rtmp::AudioCodec codec, const uint8_t *config, size_t config_size) const;

	bool decode(const rtmp::AudioPacket &packet, const FrameCallback &on_frame);

	rtmp::AudioCodec codec() const { return codec_; }

private:
	bool emit(const AVFrame &frame, int64_t pts_ms, const FrameCallback &on_frame);

	AVCodecContext *ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
	SwrContext *swr_ = nullptr;
	std::vector<std::vector<float>> converted_;
	int swr_in_rate_ = 0;
	int swr_in_format_ = -1;
	int swr_in_channels_ = 0;
	rtmp::AudioCodec codec_ = rtmp::AudioCodec::Unknown;
	std::vector<uint8_t> config_;
	int consecutive_errors_ = 0;
};

} // namespace rtmpsrc
