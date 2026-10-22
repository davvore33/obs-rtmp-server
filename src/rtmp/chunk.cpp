// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#include "rtmp/chunk.h"

#include "rtmp/bytes.h"
#include "rtmp/log.h"

#include <algorithm>

namespace rtmp {

namespace {

constexpr uint32_t kMaxChunkSize = 16u * 1024 * 1024;
constexpr size_t kMaxChunkStreams = 64;
// Cap on bytes held in half-received messages across all chunk streams.
constexpr size_t kMaxBufferedPartial = 64u * 1024 * 1024;
constexpr uint32_t kExtendedTimestamp = 0xFFFFFF;

} // namespace

ChunkReader::ChunkReader(size_t max_message_size) : max_message_size_(max_message_size) {}

bool ChunkReader::set_chunk_size(uint32_t size)
{
	if (size == 0 || size > kMaxChunkSize)
		return false;
	chunk_size_ = size;
	return true;
}

void ChunkReader::abort_message(uint32_t chunk_stream_id)
{
	auto it = streams_.find(chunk_stream_id);
	if (it == streams_.end())
		return;
	buffered_partial_ -= it->second.payload.size();
	it->second.payload.clear();
	it->second.received = 0;
}

bool ChunkReader::feed(const uint8_t *data, size_t size, const MessageHandler &handler)
{
	buffer_.insert(buffer_.end(), data, data + size);

	size_t pos = 0;
	bool ok = true;
	for (;;) {
		size_t consumed = 0;
		ParseResult r = parse_chunk(buffer_.data() + pos, buffer_.size() - pos, consumed, handler);
		if (r == ParseResult::NeedMore)
			break;
		pos += consumed;
		if (r == ParseResult::Error || r == ParseResult::Stopped) {
			ok = false;
			break;
		}
	}

	buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
	return ok;
}

ChunkReader::ParseResult ChunkReader::parse_chunk(const uint8_t *p, size_t available, size_t &consumed,
						  const MessageHandler &handler)
{
	// Nothing below may modify reader state until the whole chunk is known to be
	// present: on NeedMore the same bytes are parsed again from the start.
	if (available < 1)
		return ParseResult::NeedMore;

	const unsigned fmt = p[0] >> 6;
	uint32_t csid = p[0] & 0x3F;
	size_t off = 1;
	if (csid == 0) {
		if (available < 2)
			return ParseResult::NeedMore;
		csid = static_cast<uint32_t>(p[1]) + 64;
		off = 2;
	} else if (csid == 1) {
		if (available < 3)
			return ParseResult::NeedMore;
		csid = (static_cast<uint32_t>(p[2]) << 8) + p[1] + 64;
		off = 3;
	}

	static const size_t kHeaderSize[4] = {11, 7, 3, 0};
	const size_t header_size = kHeaderSize[fmt];
	if (available < off + header_size)
		return ParseResult::NeedMore;

	auto found = streams_.find(csid);
	if (found == streams_.end()) {
		if (fmt != 0) {
			log(LogLevel::Warning, "chunk: first chunk on stream %u uses header type %u", csid, fmt);
			return ParseResult::Error;
		}
		if (streams_.size() >= kMaxChunkStreams) {
			log(LogLevel::Warning, "chunk: too many chunk streams");
			return ParseResult::Error;
		}
		found = streams_.emplace(csid, ChunkStream()).first;
	}
	ChunkStream &cs = found->second;
	if (fmt != 0 && !cs.initialized) {
		log(LogLevel::Warning, "chunk: header type %u on uninitialised chunk stream %u", fmt, csid);
		return ParseResult::Error;
	}

	uint32_t ts_field = 0;
	uint32_t length = cs.length;
	uint8_t type = cs.type;
	uint32_t stream_id = cs.stream_id;
	if (fmt <= 2)
		ts_field = rd24(p + off);
	if (fmt <= 1) {
		length = rd24(p + off + 3);
		type = p[off + 6];
	}
	if (fmt == 0)
		stream_id = rd32le(p + off + 7);

	size_t pos = off + header_size;

	// A type 3 chunk repeats the extended timestamp field only if the chunk
	// stream's last full header had one.
	const bool extended = fmt <= 2 ? ts_field == kExtendedTimestamp : cs.has_extended_timestamp;
	uint32_t extended_value = 0;
	if (extended) {
		if (available < pos + 4)
			return ParseResult::NeedMore;
		extended_value = rd32(p + pos);
		pos += 4;
	}

	// Type 3 either continues a partly received message or starts a new one
	// that reuses the previous header.
	const bool continuation = fmt == 3 && cs.received > 0;
	const uint32_t message_length = continuation ? cs.length : length;
	if (!continuation && message_length > max_message_size_) {
		log(LogLevel::Warning, "chunk: message of %u bytes exceeds limit", message_length);
		return ParseResult::Error;
	}

	const uint32_t already = continuation ? cs.received : 0;
	const uint32_t to_read = std::min<uint32_t>(message_length - already, chunk_size_);
	if (available < pos + to_read)
		return ParseResult::NeedMore;
	if (buffered_partial_ + to_read > kMaxBufferedPartial) {
		log(LogLevel::Warning, "chunk: too much partial message data buffered");
		return ParseResult::Error;
	}

	// The chunk is complete: commit.
	if (!continuation) {
		// Any half-received message on this chunk stream is abandoned.
		buffered_partial_ -= cs.payload.size();
		cs.payload.clear();
		cs.received = 0;

		const uint32_t value = extended ? extended_value : ts_field;
		if (fmt == 0) {
			cs.timestamp = value;
			cs.timestamp_delta = 0;
		} else if (fmt == 3) {
			cs.timestamp += cs.timestamp_delta;
		} else {
			cs.timestamp_delta = value;
			cs.timestamp += value;
		}
		if (fmt <= 2)
			cs.has_extended_timestamp = extended;
		cs.length = length;
		cs.type = type;
		cs.stream_id = stream_id;
		cs.initialized = true;
	}

	cs.payload.insert(cs.payload.end(), p + pos, p + pos + to_read);
	cs.received += to_read;
	buffered_partial_ += to_read;
	consumed = pos + to_read;

	if (cs.received < cs.length)
		return ParseResult::Done;

	Message message;
	message.type = cs.type;
	message.stream_id = cs.stream_id;
	message.timestamp = cs.timestamp;
	message.payload = std::move(cs.payload);
	buffered_partial_ -= message.payload.size();
	cs.payload.clear();
	cs.received = 0;

	return handler(message) ? ParseResult::Done : ParseResult::Stopped;
}

void ChunkWriter::write(std::vector<uint8_t> &out, uint32_t csid, uint8_t type, uint32_t stream_id,
			uint32_t timestamp, const uint8_t *payload, size_t length) const
{
	auto basic_header = [&](unsigned fmt) {
		if (csid < 64) {
			wr8(out, static_cast<uint8_t>((fmt << 6) | csid));
		} else if (csid < 320) {
			wr8(out, static_cast<uint8_t>(fmt << 6));
			wr8(out, static_cast<uint8_t>(csid - 64));
		} else {
			wr8(out, static_cast<uint8_t>((fmt << 6) | 1));
			wr16(out, static_cast<uint16_t>(((csid - 64) >> 8) | ((csid - 64) << 8)));
		}
	};

	const bool extended = timestamp >= kExtendedTimestamp;

	basic_header(0);
	wr24(out, extended ? kExtendedTimestamp : timestamp);
	wr24(out, static_cast<uint32_t>(length));
	wr8(out, type);
	wr32le(out, stream_id);
	if (extended)
		wr32(out, timestamp);

	size_t sent = 0;
	do {
		if (sent > 0) {
			basic_header(3);
			if (extended)
				wr32(out, timestamp);
		}
		const size_t n = std::min<size_t>(length - sent, chunk_size_);
		out.insert(out.end(), payload + sent, payload + sent + n);
		sent += n;
	} while (sent < length);
}

} // namespace rtmp
