// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rtmp {

constexpr uint32_t kDefaultChunkSize = 128;
constexpr size_t kMaxMessageSize = 16u * 1024 * 1024;

enum MessageType : uint8_t {
	kMsgSetChunkSize = 1,
	kMsgAbort = 2,
	kMsgAcknowledgement = 3,
	kMsgUserControl = 4,
	kMsgWindowAckSize = 5,
	kMsgSetPeerBandwidth = 6,
	kMsgAudio = 8,
	kMsgVideo = 9,
	kMsgDataAmf3 = 15,
	kMsgCommandAmf3 = 17,
	kMsgDataAmf0 = 18,
	kMsgCommandAmf0 = 20,
	kMsgAggregate = 22,
};

struct Message {
	uint8_t type = 0;
	uint32_t stream_id = 0;
	uint32_t timestamp = 0; // absolute, milliseconds, wraps at 2^32
	std::vector<uint8_t> payload;
};

// Reassembles messages from the chunk stream. Sans-IO: feed it whatever bytes
// arrived, it calls the handler once per completed message.
class ChunkReader {
public:
	// Return false to stop parsing (e.g. the connection should be dropped).
	using MessageHandler = std::function<bool(Message &)>;

	explicit ChunkReader(size_t max_message_size = kMaxMessageSize);

	// Takes effect on the next chunk, so a handler may call it from inside feed().
	bool set_chunk_size(uint32_t size);
	uint32_t chunk_size() const { return chunk_size_; }

	// Drops a partially received message (RTMP "Abort Message").
	void abort_message(uint32_t chunk_stream_id);

	// Returns false on a protocol violation or when the handler returned false.
	bool feed(const uint8_t *data, size_t size, const MessageHandler &handler);

private:
	struct ChunkStream {
		bool initialized = false;
		bool has_extended_timestamp = false;
		uint32_t timestamp = 0;
		uint32_t timestamp_delta = 0;
		uint32_t length = 0;
		uint8_t type = 0;
		uint32_t stream_id = 0;
		uint32_t received = 0;
		std::vector<uint8_t> payload;
	};

	enum class ParseResult { Done, NeedMore, Error, Stopped };

	ParseResult parse_chunk(const uint8_t *p, size_t available, size_t &consumed, const MessageHandler &handler);

	size_t max_message_size_;
	uint32_t chunk_size_ = kDefaultChunkSize;
	size_t buffered_partial_ = 0;
	std::vector<uint8_t> buffer_;
	std::unordered_map<uint32_t, ChunkStream> streams_;
};

// Splits outgoing messages into chunks.
class ChunkWriter {
public:
	void set_chunk_size(uint32_t size) { chunk_size_ = size ? size : kDefaultChunkSize; }
	uint32_t chunk_size() const { return chunk_size_; }

	// Appends the chunked encoding of one message to `out`.
	void write(std::vector<uint8_t> &out, uint32_t chunk_stream_id, uint8_t type, uint32_t stream_id,
		   uint32_t timestamp, const uint8_t *payload, size_t length) const;

private:
	uint32_t chunk_size_ = kDefaultChunkSize;
};

} // namespace rtmp
