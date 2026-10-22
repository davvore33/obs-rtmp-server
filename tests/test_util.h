// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include "rtmp/amf0.h"
#include "rtmp/chunk.h"
#include "rtmp/types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

// Minimal test harness: CHECK records a failure and carries on, main returns
// the failure count.
inline int &failure_count()
{
	static int failures = 0;
	return failures;
}

#define CHECK(cond)                                                                   \
	do {                                                                          \
		if (!(cond)) {                                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failure_count()++;                                            \
		}                                                                     \
	} while (0)

#define CHECK_EQ(a, b)                                                                \
	do {                                                                          \
		auto va = (a);                                                        \
		auto vb = (b);                                                        \
		if (!(va == vb)) {                                                    \
			fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); \
			failure_count()++;                                            \
		}                                                                     \
	} while (0)

inline int finish_tests(const char *name)
{
	if (failure_count() == 0) {
		printf("%s: ok\n", name);
		return 0;
	}
	printf("%s: %d failure(s)\n", name, failure_count());
	return 1;
}

// Plays the publisher's side of an RTMP connection by producing the bytes a
// real encoder would send and decoding what the server answers.
class FakeClient {
public:
	FakeClient() : reader_(rtmp::kMaxMessageSize) {}

	// C0 + C1 (+ C2 when `with_c2`). C1/C2 content is arbitrary.
	std::vector<uint8_t> handshake(bool with_c2 = true) const
	{
		std::vector<uint8_t> out;
		out.push_back(3);
		for (size_t i = 0; i < 1536; i++)
			out.push_back(static_cast<uint8_t>(i * 7));
		if (with_c2) {
			for (size_t i = 0; i < 1536; i++)
				out.push_back(static_cast<uint8_t>(i * 3));
		}
		return out;
	}

	std::vector<uint8_t> message(uint32_t csid, uint8_t type, uint32_t stream_id, uint32_t timestamp,
				     const std::vector<uint8_t> &payload) const
	{
		std::vector<uint8_t> out;
		writer.write(out, csid, type, stream_id, timestamp, payload.data(), payload.size());
		return out;
	}

	std::vector<uint8_t> command(uint32_t stream_id, const std::vector<rtmp::amf0::Value> &values) const
	{
		std::vector<uint8_t> payload;
		for (const auto &v : values)
			rtmp::amf0::encode(v, payload);
		return message(3, rtmp::kMsgCommandAmf0, stream_id, 0, payload);
	}

	std::vector<uint8_t> connect(const std::string &app) const
	{
		using rtmp::amf0::Value;
		Value obj = Value::object();
		obj.set("app", app).set("flashVer", "FMLE/3.0").set("tcUrl", "rtmp://localhost/" + app);
		return command(0, {Value::from_string("connect"), Value::from_number(1), obj});
	}

	std::vector<uint8_t> create_stream() const
	{
		using rtmp::amf0::Value;
		return command(0, {Value::from_string("createStream"), Value::from_number(2), Value::null()});
	}

	std::vector<uint8_t> publish(const std::string &key) const
	{
		using rtmp::amf0::Value;
		return command(1, {Value::from_string("publish"), Value::from_number(0), Value::null(),
				   Value::from_string(key), Value::from_string("live")});
	}

	std::vector<uint8_t> video(uint32_t timestamp, const std::vector<uint8_t> &tag) const
	{
		return message(6, rtmp::kMsgVideo, 1, timestamp, tag);
	}

	std::vector<uint8_t> audio(uint32_t timestamp, const std::vector<uint8_t> &tag) const
	{
		return message(4, rtmp::kMsgAudio, 1, timestamp, tag);
	}

	// Feed bytes the server wrote. The first 3073 bytes are S0+S1+S2.
	bool receive(const uint8_t *data, size_t size)
	{
		if (handshake_skipped_ < 1 + 2 * 1536) {
			size_t skip = std::min(size, 1 + 2 * 1536 - handshake_skipped_);
			handshake_skipped_ += skip;
			data += skip;
			size -= skip;
		}
		if (size == 0)
			return true;
		return reader_.feed(data, size, [this](rtmp::Message &m) {
			received.push_back(m);
			return true;
		});
	}

	// First received command whose name matches, decoded.
	bool find_command(const std::string &name, std::vector<rtmp::amf0::Value> &out) const
	{
		for (const auto &m : received) {
			if (m.type != rtmp::kMsgCommandAmf0)
				continue;
			std::vector<rtmp::amf0::Value> values;
			rtmp::amf0::decode_all(m.payload.data(), m.payload.size(), values);
			if (!values.empty() && values[0].is_string() && values[0].string == name) {
				out = values;
				return true;
			}
		}
		return false;
	}

	// code of the n-th onStatus
	std::string status_code(size_t index = 0) const
	{
		size_t seen = 0;
		for (const auto &m : received) {
			if (m.type != rtmp::kMsgCommandAmf0)
				continue;
			std::vector<rtmp::amf0::Value> values;
			rtmp::amf0::decode_all(m.payload.data(), m.payload.size(), values);
			if (values.size() >= 4 && values[0].is_string() && values[0].string == "onStatus") {
				if (seen++ == index) {
					const rtmp::amf0::Value *code = values[3].find("code");
					return code ? code->as_string() : std::string();
				}
			}
		}
		return std::string();
	}

	rtmp::ChunkWriter writer;
	std::vector<rtmp::Message> received;

private:
	rtmp::ChunkReader reader_;
	size_t handshake_skipped_ = 0;
};

// Records everything a publisher sends; thread-safe so socket tests can wait.
class RecordingSink : public rtmp::StreamSink {
public:
	void on_publish_start(const rtmp::PublishInfo &i) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		info = i;
		started = true;
		starts++;
		cv.notify_all();
	}
	void on_metadata(const rtmp::Metadata &m) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		metadata = m;
		got_metadata = true;
		cv.notify_all();
	}
	void on_video(const rtmp::VideoPacket &p) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		video_packets.push_back(p);
		video_bytes.emplace_back(p.data, p.data + p.size);
		cv.notify_all();
	}
	void on_audio(const rtmp::AudioPacket &p) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		audio_packets.push_back(p);
		cv.notify_all();
	}
	void on_publish_end(const char *r) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		ended = true;
		ends++;
		end_reason = r;
		cv.notify_all();
	}
	bool should_disconnect() const override { return kick; }

	template <class Pred> bool wait_for(Pred pred, int timeout_ms = 3000)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
	}

	std::mutex mutex;
	std::condition_variable cv;
	rtmp::PublishInfo info;
	rtmp::Metadata metadata;
	std::vector<rtmp::VideoPacket> video_packets;
	std::vector<std::vector<uint8_t>> video_bytes;
	std::vector<rtmp::AudioPacket> audio_packets;
	bool started = false;
	bool got_metadata = false;
	bool ended = false;
	int starts = 0;
	int ends = 0;
	std::string end_reason;
	std::atomic<bool> kick{false};
};
