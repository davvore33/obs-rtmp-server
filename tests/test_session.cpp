// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "test_util.h"

#include "rtmp/bytes.h"

#include "rtmp/session.h"

using namespace rtmp;

// Router that accepts one app/key and hands out a RecordingSink.
class TestRouter : public Router {
public:
	std::shared_ptr<StreamSink> open_stream(const PublishInfo &info, std::string &reason) override
	{
		last_info = info;
		if (info.app != "live" || info.stream_key != "secret") {
			reason = "nope";
			return nullptr;
		}
		return sink;
	}
	std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
	PublishInfo last_info;
};

struct Harness {
	TestRouter router;
	FakeClient client;
	std::vector<uint8_t> sent; // everything the session wrote
	Session session;

	Harness()
		: session(router, [this](const uint8_t *d, size_t n) {
			  sent.insert(sent.end(), d, d + n);
			  return true;
		  }, "test:1")
	{
	}

	// Deliver bytes (optionally in small pieces) and hand the replies to the client.
	bool deliver(const std::vector<uint8_t> &bytes, size_t step = 0)
	{
		bool ok = true;
		size_t n = step ? step : bytes.size();
		for (size_t i = 0; i < bytes.size() && ok; i += n)
			ok = session.on_data(bytes.data() + i, std::min(n, bytes.size() - i));
		client.receive(sent.data(), sent.size());
		sent.clear();
		return ok;
	}
};

static std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
{
	std::vector<uint8_t> out;
	for (const auto &p : parts)
		out.insert(out.end(), p.begin(), p.end());
	return out;
}

static void test_connect_and_publish(size_t step)
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live")}), step));

	// Handshake reply: S0 + S1 + S2 (S2 echoes C1), then protocol control + _result.
	std::vector<amf0::Value> result;
	CHECK(h.client.find_command("_result", result));
	if (result.size() >= 4) {
		CHECK_EQ(result[1].as_number(), 1.0); // transaction id echoed
		CHECK_EQ(result[3].find("code")->as_string(), std::string("NetConnection.Connect.Success"));
	}
	bool saw_window = false;
	bool saw_bandwidth = false;
	for (const auto &m : h.client.received) {
		saw_window |= m.type == kMsgWindowAckSize;
		saw_bandwidth |= m.type == kMsgSetPeerBandwidth;
	}
	CHECK(saw_window);
	CHECK(saw_bandwidth);

	CHECK(h.deliver(h.client.create_stream(), step));
	CHECK(h.client.find_command("_result", result));
	CHECK(!h.session.publishing());

	CHECK(h.deliver(h.client.publish("secret?token=abc"), step));
	CHECK(h.session.publishing());
	CHECK_EQ(h.client.status_code(), std::string("NetStream.Publish.Start"));
	CHECK(h.router.sink->started);
	CHECK_EQ(h.router.sink->info.app, std::string("live"));
	CHECK_EQ(h.router.sink->info.stream_key, std::string("secret"));
	CHECK_EQ(h.router.sink->info.query, std::string("token=abc"));
	CHECK_EQ(h.router.sink->info.peer, std::string("test:1"));
	CHECK_EQ(h.router.sink->info.flash_ver, std::string("FMLE/3.0"));

	// Metadata via @setDataFrame.
	amf0::Value md = amf0::Value::object();
	md.set("width", 1280.0).set("height", 720.0).set("framerate", 30.0).set("encoder", "test");
	md.set("audiosamplerate", 48000.0).set("stereo", amf0::Value::from_bool(true));
	std::vector<uint8_t> md_payload;
	amf0::encode(amf0::Value::from_string("@setDataFrame"), md_payload);
	amf0::encode(amf0::Value::from_string("onMetaData"), md_payload);
	amf0::encode(md, md_payload);
	CHECK(h.deliver(h.client.message(4, kMsgDataAmf0, 1, 0, md_payload), step));
	CHECK(h.router.sink->got_metadata);
	CHECK_EQ(h.router.sink->metadata.width, 1280.0);
	CHECK_EQ(h.router.sink->metadata.frame_rate, 30.0);
	CHECK_EQ(h.router.sink->metadata.audio_channels, 2);
	CHECK_EQ(h.router.sink->metadata.encoder, std::string("test"));

	// Media. 3000 bytes forces several chunks at the default chunk size.
	std::vector<uint8_t> avc_seq = {0x17, 0, 0, 0, 0, 1, 2, 3};
	std::vector<uint8_t> keyframe = {0x17, 1, 0, 0, 0x28};
	keyframe.resize(3000, 0x5A);
	std::vector<uint8_t> aac = {0xAF, 1, 9, 9, 9};
	CHECK(h.deliver(concat({h.client.video(0, avc_seq), h.client.video(33, keyframe), h.client.audio(23, aac)}),
			step));

	auto &sink = *h.router.sink;
	CHECK_EQ(sink.video_packets.size(), size_t(2));
	CHECK_EQ(sink.audio_packets.size(), size_t(1));
	if (sink.video_packets.size() == 2) {
		CHECK(sink.video_packets[0].config);
		CHECK(!sink.video_packets[1].config);
		CHECK(sink.video_packets[1].keyframe);
		CHECK_EQ(sink.video_packets[1].dts_ms, int64_t(33));
		CHECK_EQ(sink.video_packets[1].cts_ms, 0x28);
		CHECK_EQ(sink.video_bytes[1].size(), size_t(2995));
	}
	if (!sink.audio_packets.empty())
		CHECK_EQ(sink.audio_packets[0].dts_ms, int64_t(23));

	// Publisher stops the stream.
	using amf0::Value;
	CHECK(h.deliver(h.client.command(1, {Value::from_string("FCUnpublish"), Value::from_number(0), Value::null(),
					     Value::from_string("secret")}),
			step));
	CHECK(sink.ended);
	CHECK(!h.session.publishing());
}

static void test_rejected_publish()
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"), h.client.create_stream()})));
	// Wrong key: the session reports failure so the connection is closed...
	CHECK(!h.deliver(h.client.publish("wrong")));
	// ...after telling the encoder why.
	CHECK_EQ(h.client.status_code(), std::string("NetStream.Publish.BadName"));
	CHECK(!h.session.publishing());
	CHECK(!h.router.sink->started);
	CHECK_EQ(h.router.last_info.stream_key, std::string("wrong"));

	// Wrong app.
	Harness other;
	CHECK(other.deliver(concat({other.client.handshake(), other.client.connect("elsewhere"), other.client.create_stream()})));
	CHECK(!other.deliver(other.client.publish("secret")));
}

static void test_bad_handshake_version()
{
	Harness h;
	std::vector<uint8_t> hs = h.client.handshake();
	hs[0] = 6; // RTMPE
	CHECK(!h.deliver(hs));
}

static void test_media_before_publish_is_ignored()
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"), h.client.create_stream()})));
	std::vector<uint8_t> frame = {0x17, 1, 0, 0, 0, 1, 2, 3};
	CHECK(h.deliver(h.client.video(0, frame)));
	CHECK(h.router.sink->video_packets.empty());
}

static void test_play_is_refused()
{
	using amf0::Value;
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"), h.client.create_stream()})));
	CHECK(!h.deliver(h.client.command(1, {Value::from_string("play"), Value::from_number(0), Value::null(),
					      Value::from_string("secret")})));
	CHECK_EQ(h.client.status_code(), std::string("NetStream.Play.Failed"));
}

static void test_release_stream_and_fcpublish_are_answered()
{
	using amf0::Value;
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"),
				h.client.command(0, {Value::from_string("releaseStream"), Value::from_number(2), Value::null(),
						     Value::from_string("secret")}),
				h.client.command(0, {Value::from_string("FCPublish"), Value::from_number(3), Value::null(),
						     Value::from_string("secret")})})));
	size_t results = 0;
	for (const auto &m : h.client.received) {
		if (m.type != kMsgCommandAmf0)
			continue;
		std::vector<Value> v;
		amf0::decode_all(m.payload.data(), m.payload.size(), v);
		if (v.size() >= 2 && v[0].string == "_result" && v[1].as_number() >= 2)
			results++;
	}
	CHECK_EQ(results, size_t(2));
}

static void test_ping_and_chunk_size()
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live")})));
	h.client.received.clear();

	// PingRequest(event 6, timestamp) must be answered with PingResponse(7).
	std::vector<uint8_t> ping;
	wr16(ping, 6);
	wr32(ping, 0xDEADBEEF);
	CHECK(h.deliver(h.client.message(2, kMsgUserControl, 0, 0, ping)));
	bool pong = false;
	for (const auto &m : h.client.received) {
		if (m.type == kMsgUserControl && m.payload.size() >= 6 && rd16(m.payload.data()) == 7 &&
		    rd32(m.payload.data() + 2) == 0xDEADBEEF)
			pong = true;
	}
	CHECK(pong);

	// The encoder switches to 4096-byte chunks, then publishes.
	std::vector<uint8_t> size;
	wr32(size, 4096);
	CHECK(h.deliver(h.client.message(2, kMsgSetChunkSize, 0, 0, size)));
	FakeClient big;
	big.writer.set_chunk_size(4096);
	CHECK(h.deliver(concat({big.create_stream(), big.publish("secret")})));
	CHECK(h.session.publishing());
	std::vector<uint8_t> frame = {0x17, 1, 0, 0, 0};
	frame.resize(10000, 7);
	CHECK(h.deliver(big.video(1, frame)));
	CHECK_EQ(h.router.sink->video_packets.size(), size_t(1));

	// An absurd chunk size is a protocol error.
	std::vector<uint8_t> huge;
	wr32(huge, 0x7FFFFFFF);
	CHECK(!h.deliver(h.client.message(2, kMsgSetChunkSize, 0, 0, huge)));
}

static void test_acknowledgements()
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"), h.client.create_stream(),
				h.client.publish("secret")})));
	h.client.received.clear();

	// The encoder asks to be acknowledged every 1000 bytes.
	std::vector<uint8_t> window;
	wr32(window, 1000);
	CHECK(h.deliver(h.client.message(2, kMsgWindowAckSize, 0, 0, window)));

	std::vector<uint8_t> frame = {0x27, 1, 0, 0, 0};
	frame.resize(1500, 1);
	CHECK(h.deliver(h.client.video(1, frame)));
	bool acked = false;
	for (const auto &m : h.client.received)
		acked |= m.type == kMsgAcknowledgement;
	CHECK(acked);
}

static void test_timestamp_wraparound()
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"), h.client.create_stream(),
				h.client.publish("secret")})));
	std::vector<uint8_t> frame = {0x27, 1, 0, 0, 0, 1};
	CHECK(h.deliver(h.client.video(0xFFFFFF00u, frame)));
	CHECK(h.deliver(h.client.video(0x00000100u, frame))); // wrapped past 2^32
	auto &v = h.router.sink->video_packets;
	CHECK_EQ(v.size(), size_t(2));
	if (v.size() == 2) {
		CHECK_EQ(v[0].dts_ms, int64_t(0xFFFFFF00u));
		CHECK_EQ(v[1].dts_ms, (int64_t(1) << 32) + 0x100);
	}
}

static void test_kick_and_disconnect()
{
	Harness h;
	CHECK(h.deliver(concat({h.client.handshake(), h.client.connect("live"), h.client.create_stream(),
				h.client.publish("secret")})));
	CHECK(!h.session.should_disconnect());
	h.router.sink->kick = true;
	CHECK(h.session.should_disconnect());

	// Abrupt disconnect ends the stream.
	h.session.finish("peer vanished");
	CHECK(h.router.sink->ended);
	CHECK_EQ(h.router.sink->end_reason, std::string("peer vanished"));
}

int main()
{
	test_connect_and_publish(0);
	test_connect_and_publish(1); // one byte at a time
	test_connect_and_publish(97);
	test_rejected_publish();
	test_bad_handshake_version();
	test_media_before_publish_is_ignored();
	test_play_is_refused();
	test_release_stream_and_fcpublish_are_answered();
	test_ping_and_chunk_size();
	test_acknowledgements();
	test_timestamp_wraparound();
	test_kick_and_disconnect();
	return finish_tests("session");
}
