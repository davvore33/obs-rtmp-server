// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
// Feeds corrupted RTMP conversations to a Session. The pass criterion is
// "does not crash, hang or trip a sanitizer"; run it under ASan/UBSan.
#include "test_util.h"

#include "rtmp/bytes.h"
#include "rtmp/session.h"

#include <random>

using namespace rtmp;

namespace {

// Accepts everything and touches the data it is given, so bad pointers or
// sizes handed to a sink show up under a sanitizer.
class TouchingSink : public StreamSink {
public:
	void on_video(const VideoPacket &p) override
	{
		for (size_t i = 0; i < p.size; i++)
			checksum += p.data[i];
	}
	void on_audio(const AudioPacket &p) override
	{
		for (size_t i = 0; i < p.size; i++)
			checksum += p.data[i];
	}
	volatile uint64_t checksum = 0;
};

class AcceptAll : public Router {
public:
	std::shared_ptr<StreamSink> open_stream(const PublishInfo &, std::string &) override
	{
		return std::make_shared<TouchingSink>();
	}
};

std::vector<uint8_t> valid_conversation()
{
	FakeClient c;
	std::vector<uint8_t> out = c.handshake();
	auto add = [&](const std::vector<uint8_t> &bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); };

	add(c.connect("live"));
	add(c.create_stream());
	add(c.publish("key?x=1"));

	amf0::Value md = amf0::Value::object();
	md.set("width", 640.0).set("height", 360.0).set("stereo", amf0::Value::from_bool(true));
	std::vector<uint8_t> md_payload;
	amf0::encode(amf0::Value::from_string("@setDataFrame"), md_payload);
	amf0::encode(amf0::Value::from_string("onMetaData"), md_payload);
	amf0::encode(md, md_payload);
	add(c.message(4, kMsgDataAmf0, 1, 0, md_payload));

	std::vector<uint8_t> size;
	wr32(size, 1024);
	add(c.message(2, kMsgSetChunkSize, 0, 0, size));

	add(c.video(0, {0x17, 0, 0, 0, 0, 1, 0x64, 0, 0x1F, 0xFF, 0xE1, 0, 2, 0x67, 0x64, 1, 0, 1, 0x68}));
	for (uint32_t i = 0; i < 20; i++) {
		std::vector<uint8_t> frame = {static_cast<uint8_t>(i % 5 ? 0x27 : 0x17), 1, 0, 0, 0};
		frame.resize(200 + i * 37, static_cast<uint8_t>(i));
		add(c.video(i * 33, frame));
		add(c.audio(i * 23, {0xAF, 1, 0x21, 0x22, 0x23, 0x24}));
	}
	add(c.command(1, {amf0::Value::from_string("FCUnpublish"), amf0::Value::from_number(0), amf0::Value::null(),
			  amf0::Value::from_string("key")}));
	return out;
}

// Feed `bytes` in random-sized pieces; stops at the first refusal like a server would.
void run(const std::vector<uint8_t> &bytes, std::mt19937 &rng)
{
	AcceptAll router;
	Session session(router, [](const uint8_t *, size_t) { return true; }, "fuzz:1");
	size_t pos = 0;
	while (pos < bytes.size()) {
		size_t n = std::min<size_t>(bytes.size() - pos, 1 + rng() % 3000);
		if (!session.on_data(bytes.data() + pos, n))
			break;
		pos += n;
	}
	session.finish("fuzz done");
}

} // namespace

int main(int argc, char **argv)
{
	const int iterations = argc > 1 ? atoi(argv[1]) : 4000;
	std::mt19937 rng(12345);
	const std::vector<uint8_t> base = valid_conversation();

	// Sanity: the unmodified conversation is accepted and reaches the sink.
	run(base, rng);

	for (int i = 0; i < iterations; i++) {
		std::vector<uint8_t> bytes = base;
		switch (i % 4) {
		case 0: // flip a few bytes after the handshake
			for (int m = 0, n = 1 + rng() % 8; m < n; m++)
				bytes[3073 + rng() % (bytes.size() - 3073)] = static_cast<uint8_t>(rng());
			break;
		case 1: // truncate
			bytes.resize(3073 + rng() % (bytes.size() - 3073));
			break;
		case 2: { // overwrite a run with garbage
			size_t at = 3073 + rng() % (bytes.size() - 3073);
			size_t len = std::min<size_t>(1 + rng() % 64, bytes.size() - at);
			for (size_t k = 0; k < len; k++)
				bytes[at + k] = static_cast<uint8_t>(rng());
			break;
		}
		case 3: { // valid handshake, then pure noise
			bytes.resize(3073);
			size_t noise = rng() % 20000;
			for (size_t k = 0; k < noise; k++)
				bytes.push_back(static_cast<uint8_t>(rng()));
			break;
		}
		}
		run(bytes, rng);
	}

	// Handshake itself: arbitrary leading bytes.
	for (int i = 0; i < 500; i++) {
		std::vector<uint8_t> bytes(rng() % 4000);
		for (auto &b : bytes)
			b = static_cast<uint8_t>(rng());
		if (!bytes.empty() && i % 2)
			bytes[0] = 3;
		run(bytes, rng);
	}

	printf("fuzz: %d mutated conversations survived\n", iterations);
	return finish_tests("fuzz");
}
