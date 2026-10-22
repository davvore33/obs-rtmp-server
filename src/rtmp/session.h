// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include "rtmp/amf0.h"
#include "rtmp/chunk.h"
#include "rtmp/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rtmp {

// One RTMP connection from a publisher. Sans-IO: the owner feeds received
// bytes to on_data() and the session writes replies through the send callback.
// Only publishing (ingest) is supported; play requests are refused.
class Session {
public:
	// Writes bytes to the peer; returns false if the connection is broken.
	using SendFn = std::function<bool(const uint8_t *, size_t)>;

	struct Stats {
		uint64_t bytes_received = 0;
		uint64_t video_packets = 0;
		uint64_t audio_packets = 0;
	};

	Session(Router &router, SendFn send, std::string peer);
	~Session();

	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	// Consumes bytes from the peer. Returns false when the connection must be
	// closed (protocol error, rejected publish, publisher left).
	bool on_data(const uint8_t *data, size_t size);

	// Ends the publish (if any) and tells the sink why. Safe to call twice.
	void finish(const char *reason);

	bool publishing() const { return sink_ != nullptr; }
	// True once the sink wants the publisher gone (e.g. its source was removed).
	bool should_disconnect() const { return sink_ && sink_->should_disconnect(); }
	const Stats &stats() const { return stats_; }

private:
	enum class HandshakeState { WaitC0C1, WaitC2, Done };

	bool process(const uint8_t *data, size_t size);
	bool handshake(std::vector<uint8_t> &input);
	bool handle_message(Message &message);
	bool handle_command(const Message &message, bool amf3);
	bool handle_data(const Message &message, bool amf3);
	bool handle_user_control(const Message &message);
	bool handle_video(const Message &message);
	bool handle_audio(const Message &message);

	bool on_connect(double transaction, const std::vector<amf0::Value> &args);
	bool on_create_stream(double transaction);
	bool on_publish(const Message &message, const std::vector<amf0::Value> &args);
	void on_unpublish(const Message &message, const char *code);

	void send_message(uint32_t csid, uint8_t type, uint32_t stream_id, const std::vector<uint8_t> &payload);
	void send_control(uint8_t type, const std::vector<uint8_t> &payload);
	void send_user_control(uint16_t event, uint32_t value);
	void send_command(uint32_t stream_id, const std::vector<amf0::Value> &values);
	void send_result(double transaction, const amf0::Value &properties, const amf0::Value &info);
	void send_status(uint32_t stream_id, const char *level, const char *code, const std::string &description);
	bool flush();

	int64_t unwrap_timestamp(uint32_t timestamp);

	Router &router_;
	SendFn send_;
	std::string peer_;

	HandshakeState handshake_state_ = HandshakeState::WaitC0C1;
	std::vector<uint8_t> handshake_buffer_;

	ChunkReader reader_;
	ChunkWriter writer_;
	std::vector<uint8_t> out_;

	uint32_t peer_window_ = 0;
	uint64_t acked_bytes_ = 0;
	Stats stats_;

	std::string app_;
	std::string tc_url_;
	std::string flash_ver_;
	uint32_t next_stream_id_ = 1;

	std::shared_ptr<StreamSink> sink_;
	uint32_t publish_stream_id_ = 0;

	int64_t timestamp_epoch_ = 0;
	int64_t last_timestamp_ = 0;

	bool warned_video_ = false;
	bool warned_audio_ = false;
};

} // namespace rtmp
