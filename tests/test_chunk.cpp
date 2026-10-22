// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "test_util.h"

#include "rtmp/bytes.h"

using namespace rtmp;

static std::vector<uint8_t> pattern(size_t n, uint8_t seed = 1)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; i++)
		v[i] = static_cast<uint8_t>(seed + i * 31);
	return v;
}

static bool feed_all(ChunkReader &reader, const std::vector<uint8_t> &bytes, std::vector<Message> &out,
		     size_t step = 0)
{
	auto handler = [&](Message &m) {
		out.push_back(m);
		return true;
	};
	if (step == 0)
		return reader.feed(bytes.data(), bytes.size(), handler);
	for (size_t i = 0; i < bytes.size(); i += step) {
		if (!reader.feed(bytes.data() + i, std::min(step, bytes.size() - i), handler))
			return false;
	}
	return true;
}

static void test_single_chunk()
{
	ChunkWriter writer;
	std::vector<uint8_t> bytes;
	auto payload = pattern(50);
	writer.write(bytes, 3, kMsgVideo, 1, 1234, payload.data(), payload.size());

	ChunkReader reader;
	std::vector<Message> out;
	CHECK(feed_all(reader, bytes, out));
	CHECK_EQ(out.size(), size_t(1));
	CHECK_EQ(out[0].type, uint8_t(kMsgVideo));
	CHECK_EQ(out[0].stream_id, uint32_t(1));
	CHECK_EQ(out[0].timestamp, uint32_t(1234));
	CHECK(out[0].payload == payload);
}

static void test_multi_chunk_and_byte_at_a_time()
{
	ChunkWriter writer;
	std::vector<uint8_t> bytes;
	auto a = pattern(1000, 3);
	auto b = pattern(300, 9);
	writer.write(bytes, 6, kMsgVideo, 1, 40, a.data(), a.size());
	writer.write(bytes, 4, kMsgAudio, 1, 41, b.data(), b.size());

	for (size_t step : {size_t(0), size_t(1), size_t(7), size_t(129)}) {
		ChunkReader reader;
		std::vector<Message> out;
		CHECK(feed_all(reader, bytes, out, step));
		CHECK_EQ(out.size(), size_t(2));
		if (out.size() == 2) {
			CHECK(out[0].payload == a);
			CHECK(out[1].payload == b);
			CHECK_EQ(out[1].timestamp, uint32_t(41));
		}
	}
}

static void test_interleaved_chunk_streams()
{
	// Build two messages by hand and interleave their chunks (chunk size 128).
	ChunkWriter w;
	std::vector<uint8_t> v1, v2;
	auto a = pattern(200, 1);
	auto b = pattern(200, 2);
	w.write(v1, 6, kMsgVideo, 1, 10, a.data(), a.size());
	w.write(v2, 4, kMsgAudio, 1, 11, b.data(), b.size());

	// First chunk of each is 12 + 128 bytes, second is 1 + 72.
	const size_t first = 12 + 128;
	std::vector<uint8_t> mixed;
	mixed.insert(mixed.end(), v1.begin(), v1.begin() + first);
	mixed.insert(mixed.end(), v2.begin(), v2.begin() + first);
	mixed.insert(mixed.end(), v2.begin() + first, v2.end());
	mixed.insert(mixed.end(), v1.begin() + first, v1.end());

	ChunkReader reader;
	std::vector<Message> out;
	CHECK(feed_all(reader, mixed, out));
	CHECK_EQ(out.size(), size_t(2));
	if (out.size() == 2) {
		CHECK_EQ(out[0].type, uint8_t(kMsgAudio)); // finished first
		CHECK(out[0].payload == b);
		CHECK(out[1].payload == a);
	}
}

static void test_chunk_size_change_mid_buffer()
{
	// SetChunkSize followed in the same buffer by a message using the new size.
	ChunkWriter w;
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> size_payload;
	wr32(size_payload, 4096);
	w.write(bytes, 2, kMsgSetChunkSize, 0, 0, size_payload.data(), size_payload.size());

	ChunkWriter big;
	big.set_chunk_size(4096);
	auto payload = pattern(3000);
	big.write(bytes, 6, kMsgVideo, 1, 5, payload.data(), payload.size());

	ChunkReader reader;
	std::vector<Message> out;
	auto handler = [&](Message &m) {
		out.push_back(m);
		if (m.type == kMsgSetChunkSize)
			reader.set_chunk_size(rd32(m.payload.data()));
		return true;
	};
	CHECK(reader.feed(bytes.data(), bytes.size(), handler));
	CHECK_EQ(out.size(), size_t(2));
	if (out.size() == 2)
		CHECK(out[1].payload == payload);
}

static void test_extended_timestamp()
{
	ChunkWriter writer;
	std::vector<uint8_t> bytes;
	auto payload = pattern(300);
	const uint32_t ts = 0x01234567;
	writer.write(bytes, 5, kMsgVideo, 1, ts, payload.data(), payload.size());

	ChunkReader reader;
	std::vector<Message> out;
	CHECK(feed_all(reader, bytes, out, 5));
	CHECK_EQ(out.size(), size_t(1));
	if (!out.empty()) {
		CHECK_EQ(out[0].timestamp, ts);
		CHECK(out[0].payload == payload);
	}
}

static void test_large_chunk_stream_ids()
{
	for (uint32_t csid : {uint32_t(2), uint32_t(63), uint32_t(64), uint32_t(319), uint32_t(320), uint32_t(1000)}) {
		ChunkWriter writer;
		std::vector<uint8_t> bytes;
		auto payload = pattern(150);
		writer.write(bytes, csid, kMsgAudio, 1, 7, payload.data(), payload.size());
		ChunkReader reader;
		std::vector<Message> out;
		CHECK(feed_all(reader, bytes, out, 3));
		CHECK_EQ(out.size(), size_t(1));
		if (!out.empty())
			CHECK(out[0].payload == payload);
	}
}

static void test_compressed_headers_and_deltas()
{
	// fmt 0 (ts 100) then fmt 1 (delta 20), fmt 2 (delta 30), fmt 3 (repeat delta 30).
	std::vector<uint8_t> bytes;
	auto add = [&](std::initializer_list<uint8_t> header, size_t payload_len) {
		bytes.insert(bytes.end(), header.begin(), header.end());
		for (size_t i = 0; i < payload_len; i++)
			bytes.push_back(static_cast<uint8_t>(i));
	};
	// fmt0 csid4: ts=100 len=10 type=9 stream=1
	add({0x04, 0, 0, 100, 0, 0, 10, 9, 1, 0, 0, 0}, 10);
	// fmt1 csid4: delta=20 len=6 type=8
	add({0x44, 0, 0, 20, 0, 0, 6, 8}, 6);
	// fmt2 csid4: delta=30 (len/type inherited)
	add({0x84, 0, 0, 30}, 6);
	// fmt3 csid4: new message, same delta
	add({0xC4}, 6);

	ChunkReader reader;
	std::vector<Message> out;
	CHECK(feed_all(reader, bytes, out));
	CHECK_EQ(out.size(), size_t(4));
	if (out.size() == 4) {
		CHECK_EQ(out[0].timestamp, uint32_t(100));
		CHECK_EQ(out[1].timestamp, uint32_t(120));
		CHECK_EQ(out[1].type, uint8_t(8));
		CHECK_EQ(out[2].timestamp, uint32_t(150));
		CHECK_EQ(out[2].payload.size(), size_t(6));
		CHECK_EQ(out[3].timestamp, uint32_t(180));
		CHECK_EQ(out[3].stream_id, uint32_t(1));
	}
}

static void test_abort()
{
	ChunkWriter writer;
	std::vector<uint8_t> bytes;
	auto payload = pattern(300);
	writer.write(bytes, 6, kMsgVideo, 1, 1, payload.data(), payload.size());
	// Only the first chunk, then abort, then a fresh complete message.
	std::vector<uint8_t> first(bytes.begin(), bytes.begin() + 12 + 128);

	ChunkReader reader;
	std::vector<Message> out;
	CHECK(feed_all(reader, first, out));
	CHECK(out.empty());
	reader.abort_message(6);

	std::vector<uint8_t> fresh;
	auto small = pattern(20);
	writer.write(fresh, 6, kMsgVideo, 1, 2, small.data(), small.size());
	CHECK(feed_all(reader, fresh, out));
	CHECK_EQ(out.size(), size_t(1));
	if (!out.empty())
		CHECK(out[0].payload == small);
}

static void test_protocol_errors()
{
	std::vector<Message> out;

	{ // header type 1 on a chunk stream that never had a full header
		ChunkReader reader;
		const std::vector<uint8_t> bytes = {0x44, 0, 0, 1, 0, 0, 1, 8, 0};
		CHECK(!feed_all(reader, bytes, out));
	}
	{ // message larger than the limit
		ChunkReader reader(1000);
		const std::vector<uint8_t> bytes = {0x04, 0, 0, 0, 0x00, 0x10, 0x00, 9, 1, 0, 0, 0};
		CHECK(!feed_all(reader, bytes, out));
	}
	{ // too many chunk streams
		ChunkReader reader;
		bool failed = false;
		for (uint32_t csid = 64; csid < 300 && !failed; csid++) {
			// fmt 0, two-byte chunk stream id, one-byte message
			std::vector<uint8_t> bytes = {0x00, static_cast<uint8_t>(csid - 64), 0, 0, 0, 0, 0, 1, 9, 1, 0, 0, 0, 0xAA};
			failed = !feed_all(reader, bytes, out);
		}
		CHECK(failed);
	}
	{ // handler asks to stop
		ChunkReader reader;
		ChunkWriter writer;
		std::vector<uint8_t> bytes;
		auto payload = pattern(10);
		writer.write(bytes, 3, kMsgAudio, 1, 0, payload.data(), payload.size());
		CHECK(!reader.feed(bytes.data(), bytes.size(), [](Message &) { return false; }));
	}
	{ // invalid chunk sizes
		ChunkReader reader;
		CHECK(!reader.set_chunk_size(0));
		CHECK(!reader.set_chunk_size(0x7FFFFFFF));
		CHECK(reader.set_chunk_size(4096));
	}
}

int main()
{
	test_single_chunk();
	test_multi_chunk_and_byte_at_a_time();
	test_interleaved_chunk_streams();
	test_chunk_size_change_mid_buffer();
	test_extended_timestamp();
	test_large_chunk_stream_ids();
	test_compressed_headers_and_deltas();
	test_abort();
	test_protocol_errors();
	return finish_tests("chunk");
}
