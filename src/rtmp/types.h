// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rtmp {

enum class VideoCodec { Unknown, H264, HEVC, AV1, VP9 };
enum class AudioCodec { Unknown, AAC, MP3, Opus };

const char *codec_name(VideoCodec codec);
const char *codec_name(AudioCodec codec);

// A demuxed video access unit. `data` is only valid during the callback.
struct VideoPacket {
	VideoCodec codec = VideoCodec::Unknown;
	// `data` is a decoder configuration record (avcC / hvcC / av1C / vpcC),
	// not coded video.
	bool config = false;
	bool keyframe = false;
	bool end_of_sequence = false;
	int64_t dts_ms = 0;
	int32_t cts_ms = 0; // pts = dts + cts
	const uint8_t *data = nullptr;
	size_t size = 0;
};

// A demuxed audio frame. `data` is only valid during the callback.
struct AudioPacket {
	AudioCodec codec = AudioCodec::Unknown;
	// `data` is a decoder configuration (AudioSpecificConfig, OpusHead...).
	bool config = false;
	int64_t dts_ms = 0;
	// What the FLV tag header claims; 0 when the codec carries it in-band.
	uint32_t sample_rate_hint = 0;
	uint8_t channels_hint = 0;
	const uint8_t *data = nullptr;
	size_t size = 0;
};

// Fields of the publisher's onMetaData. Missing values are 0.
struct Metadata {
	double width = 0;
	double height = 0;
	double frame_rate = 0;
	double video_bitrate_kbps = 0;
	double audio_bitrate_kbps = 0;
	double audio_sample_rate = 0;
	int audio_channels = 0;
	std::string encoder;
};

struct PublishInfo {
	std::string app;        // from the connect command, e.g. "live"
	std::string stream_key; // from the publish command, query string removed
	std::string query;      // everything after '?' in the publish name
	std::string peer;       // "ip:port" of the publisher
	std::string tc_url;
	std::string flash_ver;
};

// Receives one publisher's media. All callbacks run on the connection's
// network thread; implementations must be quick and thread-safe against their
// own teardown.
class StreamSink {
public:
	virtual ~StreamSink() = default;

	virtual void on_publish_start(const PublishInfo &info) { (void)info; }
	virtual void on_metadata(const Metadata &metadata) { (void)metadata; }
	virtual void on_video(const VideoPacket &packet) = 0;
	virtual void on_audio(const AudioPacket &packet) = 0;
	virtual void on_publish_end(const char *reason) { (void)reason; }

	// Polled by the connection loop; return true to drop the publisher.
	virtual bool should_disconnect() const { return false; }
};

// Decides who may publish. Returns nullptr to reject and fills `reject_reason`.
class Router {
public:
	virtual ~Router() = default;
	virtual std::shared_ptr<StreamSink> open_stream(const PublishInfo &info, std::string &reject_reason) = 0;
};

} // namespace rtmp
