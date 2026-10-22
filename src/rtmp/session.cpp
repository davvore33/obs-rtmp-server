// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#include "rtmp/session.h"

#include "rtmp/bytes.h"
#include "rtmp/flv.h"
#include "rtmp/log.h"

#include <algorithm>
#include <random>

namespace rtmp {

namespace {

constexpr size_t kHandshakeSize = 1536;
constexpr uint8_t kRtmpVersion = 3;

constexpr uint32_t kWindowAckSize = 2500000;
constexpr uint32_t kControlChunkStream = 2;
constexpr uint32_t kCommandChunkStream = 3;
constexpr uint32_t kStreamChunkStream = 5;

constexpr uint16_t kUserControlStreamBegin = 0;
constexpr uint16_t kUserControlPingRequest = 6;
constexpr uint16_t kUserControlPingResponse = 7;

std::string trim_slashes(std::string s)
{
	size_t first = s.find_first_not_of('/');
	if (first == std::string::npos)
		return std::string();
	size_t last = s.find_last_not_of('/');
	return s.substr(first, last - first + 1);
}

} // namespace

Session::Session(Router &router, SendFn send, std::string peer)
	: router_(router),
	  send_(std::move(send)),
	  peer_(std::move(peer))
{
}

Session::~Session()
{
	finish("connection closed");
}

void Session::finish(const char *reason)
{
	if (!sink_)
		return;
	std::shared_ptr<StreamSink> sink = std::move(sink_);
	log(LogLevel::Info, "%s: stopped publishing (%s)", peer_.c_str(), reason);
	sink->on_publish_end(reason);
}

bool Session::on_data(const uint8_t *data, size_t size)
{
	bool ok = process(data, size);
	// Flush even on failure so a rejection reaches the publisher before we hang up.
	bool sent = flush();
	return ok && sent;
}

bool Session::process(const uint8_t *data, size_t size)
{
	if (handshake_state_ != HandshakeState::Done) {
		handshake_buffer_.insert(handshake_buffer_.end(), data, data + size);
		if (!handshake(handshake_buffer_))
			return false;
		if (handshake_state_ != HandshakeState::Done)
			return true;
		// Chunk data may have followed C2 in the same read.
		std::vector<uint8_t> rest = std::move(handshake_buffer_);
		handshake_buffer_.clear();
		if (rest.empty())
			return true;
		return process(rest.data(), rest.size());
	}

	stats_.bytes_received += size;
	bool ok = reader_.feed(data, size, [this](Message &m) { return handle_message(m); });

	// Acknowledge received bytes as the peer asked (window acknowledgement size).
	if (ok && peer_window_ > 0 && stats_.bytes_received - acked_bytes_ >= peer_window_) {
		std::vector<uint8_t> payload;
		wr32(payload, static_cast<uint32_t>(stats_.bytes_received));
		send_control(kMsgAcknowledgement, payload);
		acked_bytes_ = stats_.bytes_received;
	}
	return ok;
}

// Plain (non-digest) handshake: C0 C1 -> S0 S1 S2, then C2. Every mainstream
// encoder (OBS, FFmpeg, Larix, vMix, hardware encoders) accepts this.
bool Session::handshake(std::vector<uint8_t> &input)
{
	if (handshake_state_ == HandshakeState::WaitC0C1) {
		if (input.size() < 1 + kHandshakeSize)
			return true;

		if (input[0] != kRtmpVersion) {
			log(LogLevel::Warning, "%s: unsupported RTMP version %u (encrypted RTMPE is not supported)",
			    peer_.c_str(), input[0]);
			return false;
		}

		std::vector<uint8_t> reply;
		reply.reserve(1 + 2 * kHandshakeSize);
		reply.push_back(kRtmpVersion);

		// S1: time (0), zero, 1528 random bytes.
		static thread_local std::mt19937 rng{std::random_device{}()};
		wr32(reply, 0);
		wr32(reply, 0);
		for (size_t i = 8; i < kHandshakeSize; i++)
			reply.push_back(static_cast<uint8_t>(rng()));

		// S2: echo of C1.
		reply.insert(reply.end(), input.begin() + 1, input.begin() + 1 + kHandshakeSize);

		out_.insert(out_.end(), reply.begin(), reply.end());
		input.erase(input.begin(), input.begin() + 1 + static_cast<std::ptrdiff_t>(kHandshakeSize));
		handshake_state_ = HandshakeState::WaitC2;
	}

	if (handshake_state_ == HandshakeState::WaitC2) {
		if (input.size() < kHandshakeSize)
			return true;
		input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(kHandshakeSize));
		handshake_state_ = HandshakeState::Done;
	}
	return true;
}

bool Session::handle_message(Message &m)
{
	switch (m.type) {
	case kMsgSetChunkSize: {
		if (m.payload.size() < 4)
			return false;
		uint32_t size = rd32(m.payload.data()) & 0x7FFFFFFF;
		if (!reader_.set_chunk_size(size)) {
			log(LogLevel::Warning, "%s: rejected chunk size %u", peer_.c_str(), size);
			return false;
		}
		return true;
	}
	case kMsgAbort:
		if (m.payload.size() >= 4)
			reader_.abort_message(rd32(m.payload.data()));
		return true;
	case kMsgWindowAckSize:
		if (m.payload.size() >= 4)
			peer_window_ = rd32(m.payload.data());
		return true;
	case kMsgUserControl:
		return handle_user_control(m);
	case kMsgVideo:
		return handle_video(m);
	case kMsgAudio:
		return handle_audio(m);
	case kMsgDataAmf0:
		return handle_data(m, false);
	case kMsgDataAmf3:
		return handle_data(m, true);
	case kMsgCommandAmf0:
		return handle_command(m, false);
	case kMsgCommandAmf3:
		return handle_command(m, true);
	default:
		// Acknowledgement, peer bandwidth, aggregate, shared objects: not needed for ingest.
		return true;
	}
}

bool Session::handle_user_control(const Message &m)
{
	if (m.payload.size() < 6)
		return true;
	uint16_t event = rd16(m.payload.data());
	if (event == kUserControlPingRequest)
		send_user_control(kUserControlPingResponse, rd32(m.payload.data() + 2));
	return true;
}

int64_t Session::unwrap_timestamp(uint32_t timestamp)
{
	constexpr int64_t kWrap = int64_t(1) << 32;
	int64_t t = timestamp_epoch_ + timestamp;
	// 32-bit RTMP timestamps wrap after ~49 days; a big jump backwards is a wrap.
	if (t + kWrap / 2 < last_timestamp_) {
		timestamp_epoch_ += kWrap;
		t += kWrap;
	}
	last_timestamp_ = t;
	return t;
}

bool Session::handle_video(const Message &m)
{
	if (!sink_)
		return true;

	VideoPacket packet;
	switch (parse_video_tag(m.payload.data(), m.payload.size(), unwrap_timestamp(m.timestamp), packet)) {
	case ParseStatus::Ok:
		stats_.video_packets++;
		sink_->on_video(packet);
		break;
	case ParseStatus::Skip:
		break;
	case ParseStatus::Unsupported:
		if (!warned_video_ && !m.payload.empty()) {
			warned_video_ = true;
			log(LogLevel::Warning, "%s: ignoring unsupported video packets (first byte 0x%02x)",
			    peer_.c_str(), m.payload[0]);
		}
		break;
	case ParseStatus::Invalid:
		log(LogLevel::Debug, "%s: malformed video packet", peer_.c_str());
		break;
	}
	return !sink_->should_disconnect();
}

bool Session::handle_audio(const Message &m)
{
	if (!sink_)
		return true;

	AudioPacket packet;
	switch (parse_audio_tag(m.payload.data(), m.payload.size(), unwrap_timestamp(m.timestamp), packet)) {
	case ParseStatus::Ok:
		stats_.audio_packets++;
		sink_->on_audio(packet);
		break;
	case ParseStatus::Skip:
		break;
	case ParseStatus::Unsupported:
		if (!warned_audio_ && !m.payload.empty()) {
			warned_audio_ = true;
			log(LogLevel::Warning, "%s: ignoring unsupported audio packets (first byte 0x%02x)",
			    peer_.c_str(), m.payload[0]);
		}
		break;
	case ParseStatus::Invalid:
		log(LogLevel::Debug, "%s: malformed audio packet", peer_.c_str());
		break;
	}
	return !sink_->should_disconnect();
}

bool Session::handle_data(const Message &m, bool amf3)
{
	if (!sink_)
		return true;

	const uint8_t *p = m.payload.data();
	size_t n = m.payload.size();
	if (amf3) {
		if (n == 0 || p[0] != 0)
			return true;
		p++;
		n--;
	}

	std::vector<amf0::Value> values;
	amf0::decode_all(p, n, values);

	// ["@setDataFrame", "onMetaData", {...}] or ["onMetaData", {...}]
	size_t i = 0;
	if (i < values.size() && values[i].is_string() && values[i].string == "@setDataFrame")
		i++;
	if (i + 1 >= values.size() || !values[i].is_string() || values[i].string != "onMetaData")
		return true;

	const amf0::Value &info = values[i + 1];
	if (info.type != amf0::Type::Object && info.type != amf0::Type::EcmaArray)
		return true;

	auto number = [&](const char *key) {
		const amf0::Value *v = info.find(key);
		return v ? v->as_number() : 0.0;
	};

	Metadata md;
	md.width = number("width");
	md.height = number("height");
	md.frame_rate = number("framerate");
	md.video_bitrate_kbps = number("videodatarate");
	md.audio_bitrate_kbps = number("audiodatarate");
	md.audio_sample_rate = number("audiosamplerate");
	md.audio_channels = static_cast<int>(number("audiochannels"));
	if (md.audio_channels == 0) {
		if (const amf0::Value *stereo = info.find("stereo"))
			md.audio_channels = stereo->as_number(1) != 0 ? 2 : 1;
	}
	if (const amf0::Value *encoder = info.find("encoder"))
		md.encoder = encoder->as_string();

	sink_->on_metadata(md);
	return true;
}

bool Session::handle_command(const Message &m, bool amf3)
{
	const uint8_t *p = m.payload.data();
	size_t n = m.payload.size();
	if (amf3) {
		if (n == 0 || p[0] != 0)
			return true;
		p++;
		n--;
	}

	std::vector<amf0::Value> args;
	amf0::decode_all(p, n, args); // keep whatever parsed; trailing junk is tolerated
	if (args.size() < 2 || !args[0].is_string())
		return true;

	const std::string &name = args[0].string;
	const double transaction = args[1].as_number();

	if (name == "connect")
		return on_connect(transaction, args);
	if (name == "createStream")
		return on_create_stream(transaction);
	if (name == "publish")
		return on_publish(m, args);
	if (name == "deleteStream" || name == "closeStream" || name == "FCUnpublish") {
		on_unpublish(m, "NetStream.Unpublish.Success");
		return true;
	}
	if (name == "releaseStream" || name == "FCPublish") {
		// Encoders send these before publish; they only need the call answered.
		if (transaction > 0)
			send_result(transaction, amf0::Value::null(), amf0::Value::undefined());
		return true;
	}
	if (name == "play" || name == "play2") {
		send_status(m.stream_id, "error", "NetStream.Play.Failed", "This server only accepts published streams.");
		return false;
	}
	// _checkbandwidth, getStreamLength, ping...: nothing to do for ingest.
	return true;
}

bool Session::on_connect(double transaction, const std::vector<amf0::Value> &args)
{
	if (args.size() > 2 && args[2].type == amf0::Type::Object) {
		const amf0::Value &obj = args[2];
		if (const amf0::Value *v = obj.find("app"))
			app_ = v->as_string();
		if (const amf0::Value *v = obj.find("tcUrl"))
			tc_url_ = v->as_string();
		if (const amf0::Value *v = obj.find("flashVer"))
			flash_ver_ = v->as_string();
	}
	size_t query = app_.find('?');
	if (query != std::string::npos)
		app_.resize(query);
	app_ = trim_slashes(app_);

	{
		std::vector<uint8_t> payload;
		wr32(payload, kWindowAckSize);
		send_control(kMsgWindowAckSize, payload);
	}
	{
		std::vector<uint8_t> payload;
		wr32(payload, kWindowAckSize);
		wr8(payload, 2); // dynamic
		send_control(kMsgSetPeerBandwidth, payload);
	}
	send_user_control(kUserControlStreamBegin, 0);

	amf0::Value properties = amf0::Value::object();
	properties.set("fmsVer", "FMS/3,0,1,123").set("capabilities", 31.0).set("mode", 1.0);

	amf0::Value info = amf0::Value::object();
	info.set("level", "status")
		.set("code", "NetConnection.Connect.Success")
		.set("description", "Connection succeeded.")
		.set("objectEncoding", 0.0);

	send_result(transaction, properties, info);
	log(LogLevel::Debug, "%s: connect app='%s' flashVer='%s'", peer_.c_str(), app_.c_str(), flash_ver_.c_str());
	return true;
}

bool Session::on_create_stream(double transaction)
{
	send_result(transaction, amf0::Value::null(), amf0::Value::from_number(next_stream_id_++));
	return true;
}

bool Session::on_publish(const Message &m, const std::vector<amf0::Value> &args)
{
	if (sink_) {
		send_status(m.stream_id, "error", "NetStream.Publish.BadName", "Already publishing on this connection.");
		return false;
	}

	// ["publish", txn, null, "<name>?<query>", "live"]
	std::string name = args.size() > 3 ? args[3].as_string() : std::string();
	PublishInfo info;
	size_t query = name.find('?');
	if (query != std::string::npos) {
		info.query = name.substr(query + 1);
		name.resize(query);
	}
	info.stream_key = trim_slashes(name);
	info.app = app_;
	info.peer = peer_;
	info.tc_url = tc_url_;
	info.flash_ver = flash_ver_;

	std::string reason;
	std::shared_ptr<StreamSink> sink = router_.open_stream(info, reason);
	if (!sink) {
		log(LogLevel::Warning, "%s: publish rejected for %s/%s (%s)", peer_.c_str(), info.app.c_str(),
		    info.stream_key.c_str(), reason.c_str());
		send_status(m.stream_id, "error", "NetStream.Publish.BadName", reason);
		return false;
	}

	sink_ = std::move(sink);
	publish_stream_id_ = m.stream_id;
	send_user_control(kUserControlStreamBegin, m.stream_id);
	send_status(m.stream_id, "status", "NetStream.Publish.Start", "Publishing " + info.stream_key + ".");

	log(LogLevel::Info, "%s: publishing %s/%s", peer_.c_str(), info.app.c_str(), info.stream_key.c_str());
	sink_->on_publish_start(info);
	return true;
}

void Session::on_unpublish(const Message &m, const char *code)
{
	if (!sink_)
		return;
	send_status(m.stream_id ? m.stream_id : publish_stream_id_, "status", code, "Stream stopped.");
	finish("publisher stopped the stream");
}

void Session::send_message(uint32_t csid, uint8_t type, uint32_t stream_id, const std::vector<uint8_t> &payload)
{
	writer_.write(out_, csid, type, stream_id, 0, payload.data(), payload.size());
}

void Session::send_control(uint8_t type, const std::vector<uint8_t> &payload)
{
	send_message(kControlChunkStream, type, 0, payload);
}

void Session::send_user_control(uint16_t event, uint32_t value)
{
	std::vector<uint8_t> payload;
	wr16(payload, event);
	wr32(payload, value);
	send_control(kMsgUserControl, payload);
}

void Session::send_command(uint32_t stream_id, const std::vector<amf0::Value> &values)
{
	std::vector<uint8_t> payload;
	for (const amf0::Value &v : values)
		amf0::encode(v, payload);
	send_message(stream_id == 0 ? kCommandChunkStream : kStreamChunkStream, kMsgCommandAmf0, stream_id, payload);
}

void Session::send_result(double transaction, const amf0::Value &properties, const amf0::Value &info)
{
	send_command(0, {amf0::Value::from_string("_result"), amf0::Value::from_number(transaction), properties, info});
}

void Session::send_status(uint32_t stream_id, const char *level, const char *code, const std::string &description)
{
	amf0::Value info = amf0::Value::object();
	info.set("level", level).set("code", code).set("description", description);
	send_command(stream_id, {amf0::Value::from_string("onStatus"), amf0::Value::from_number(0), amf0::Value::null(),
				 info});
}

bool Session::flush()
{
	if (out_.empty())
		return true;
	bool ok = send_(out_.data(), out_.size());
	out_.clear();
	return ok;
}

} // namespace rtmp
