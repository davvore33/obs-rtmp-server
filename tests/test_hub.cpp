// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
// End-to-end over real TCP sockets: Hub + Server + Session with a fake encoder.
#include "test_util.h"

#include "rtmp/bytes.h"
#include "rtmp/hub.h"

#ifdef _WIN32
int main()
{
	return 0; // socket test is POSIX only for now
}
#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace rtmp;

// A blocking TCP client that speaks just enough RTMP to publish.
class Encoder {
public:
	explicit Encoder(uint16_t port)
	{
		fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		connected_ = ::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		timeval tv{0, 100000};
		setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	~Encoder()
	{
		if (fd_ >= 0)
			::close(fd_);
	}

	bool connected() const { return connected_; }

	bool send(const std::vector<uint8_t> &bytes)
	{
		return ::send(fd_, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size());
	}

	// Reads until `done()` is true, the peer closes, or ~timeout_ms passes.
	// Returns false if the peer closed the connection.
	template <class Pred> bool read_until(Pred done, int timeout_ms = 3000)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		uint8_t buf[4096];
		while (std::chrono::steady_clock::now() < deadline) {
			if (done())
				return true;
			ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
			if (n == 0)
				return false;
			if (n > 0)
				client.receive(buf, static_cast<size_t>(n));
		}
		return done();
	}

	bool publish(const std::string &key)
	{
		if (!send(client.handshake()) || !send(client.connect("live")) || !send(client.create_stream()))
			return false;
		if (!read_until([&] {
			    std::vector<amf0::Value> v;
			    return client.find_command("_result", v) && client.received.size() >= 4;
		    }))
			return false;
		return send(client.publish(key));
	}

	FakeClient client;

private:
	int fd_ = -1;
	bool connected_ = false;
};

static std::vector<uint8_t> keyframe_tag()
{
	std::vector<uint8_t> tag = {0x17, 1, 0, 0, 0, 0, 0, 0, 4, 1, 2, 3, 4};
	return tag;
}

static void test_shared_port_routes()
{
	Hub &hub = Hub::instance();
	uint16_t port = 0;
	std::string error;

	auto sink1 = std::make_shared<RecordingSink>();
	auto sink2 = std::make_shared<RecordingSink>();
	std::unique_ptr<Hub::Registration> reg1, reg2;

	// Find a free port (other test runs or services may hold some).
	for (uint16_t candidate = 47935; candidate < 47990 && !reg1; candidate++) {
		Hub::RouteSpec spec;
		spec.port = candidate;
		spec.app = "live";
		spec.key = "cam1";
		reg1 = hub.add_route(spec, sink1, error);
		port = candidate;
	}
	CHECK(reg1 != nullptr);
	if (!reg1)
		return;

	Hub::RouteSpec spec2;
	spec2.port = port;
	spec2.app = "live";
	spec2.key = "cam2";
	reg2 = hub.add_route(spec2, sink2, error);
	CHECK(reg2 != nullptr);

	// The same app/key twice on one port is refused, as is a different bind address.
	Hub::RouteSpec dup = spec2;
	CHECK(hub.add_route(dup, std::make_shared<RecordingSink>(), error) == nullptr);
	CHECK(!error.empty());
	Hub::RouteSpec other_bind = spec2;
	other_bind.key = "cam3";
	other_bind.bind_address = "0.0.0.0";
	CHECK(hub.add_route(other_bind, std::make_shared<RecordingSink>(), error) == nullptr);

	// Two encoders publish to the two routes at the same time.
	Encoder enc1(port), enc2(port);
	CHECK(enc1.connected());
	CHECK(enc2.connected());
	CHECK(enc1.publish("cam1"));
	CHECK(enc2.publish("cam2"));
	CHECK(enc1.read_until([&] { return enc1.client.status_code() == "NetStream.Publish.Start"; }));
	CHECK(enc2.read_until([&] { return enc2.client.status_code() == "NetStream.Publish.Start"; }));
	CHECK(reg1->publisher_connected());
	CHECK(reg2->publisher_connected());

	CHECK(enc1.send(enc1.client.video(0, {0x17, 0, 0, 0, 0, 1, 2, 3})));
	CHECK(enc1.send(enc1.client.video(40, keyframe_tag())));
	CHECK(enc2.send(enc2.client.audio(0, {0xAF, 0, 0x12, 0x10})));
	CHECK(sink1->wait_for([&] { return sink1->video_packets.size() == 2; }));
	CHECK(sink2->wait_for([&] { return sink2->audio_packets.size() == 1; }));
	CHECK(sink1->audio_packets.empty());
	CHECK(sink2->video_packets.empty());
	CHECK(!reg1->publisher_peer().empty());

	// A second encoder on a busy key, and one on an unknown key, are turned away.
	Encoder busy(port), unknown(port);
	CHECK(busy.publish("cam1"));
	CHECK(busy.read_until([&] { return busy.client.status_code() == "NetStream.Publish.BadName"; }));
	CHECK(unknown.publish("nope"));
	CHECK(unknown.read_until([&] { return unknown.client.status_code() == "NetStream.Publish.BadName"; }));
	// The legitimate publisher was not disturbed.
	CHECK(enc1.send(enc1.client.video(80, keyframe_tag())));
	CHECK(sink1->wait_for([&] { return sink1->video_packets.size() == 3; }));

	// Removing a route disconnects its publisher; the other keeps going.
	reg1.reset();
	CHECK(sink1->wait_for([&] { return sink1->ended; }));
	CHECK(!enc1.read_until([] { return false; }, 2000)); // peer closed the connection
	CHECK(enc2.send(enc2.client.audio(20, {0xAF, 1, 1, 2, 3})));
	CHECK(sink2->wait_for([&] { return sink2->audio_packets.size() == 2; }));

	// Removing the last route stops the server and frees the port.
	reg2.reset();
	CHECK(sink2->wait_for([&] { return sink2->ended; }));
	Encoder refused(port);
	CHECK(!refused.connected());
}

static void test_publisher_can_reconnect()
{
	Hub &hub = Hub::instance();
	std::string error;
	auto sink = std::make_shared<RecordingSink>();
	std::unique_ptr<Hub::Registration> reg;
	uint16_t port = 0;
	for (uint16_t candidate = 47991; candidate < 48040 && !reg; candidate++) {
		Hub::RouteSpec spec;
		spec.port = candidate;
		spec.key = "again";
		reg = hub.add_route(spec, sink, error);
		port = candidate;
	}
	CHECK(reg != nullptr);
	if (!reg)
		return;

	for (int attempt = 0; attempt < 3; attempt++) {
		{
			Encoder enc(port);
			CHECK(enc.publish("again"));
			CHECK(enc.read_until([&] { return enc.client.status_code() == "NetStream.Publish.Start"; }));
		} // abrupt disconnect
		CHECK(sink->wait_for([&] { return sink->ended; }));
		for (int i = 0; i < 50 && reg->publisher_connected(); i++)
			usleep(20000);
		CHECK(!reg->publisher_connected());
		{
			std::lock_guard<std::mutex> lock(sink->mutex);
			sink->ended = false;
		}
	}
}

static void test_takeover_of_silent_publisher()
{
	Hub &hub = Hub::instance();
	std::string error;
	auto sink = std::make_shared<RecordingSink>();
	std::unique_ptr<Hub::Registration> reg;
	uint16_t port = 0;
	for (uint16_t candidate = 48041; candidate < 48090 && !reg; candidate++) {
		Hub::RouteSpec spec;
		spec.port = candidate;
		spec.key = "cam";
		spec.takeover_after_ms = 400;
		reg = hub.add_route(spec, sink, error);
		port = candidate;
	}
	CHECK(reg != nullptr);
	if (!reg)
		return;

	// A camera publishes, then its Wi-Fi drops: the socket stays open but silent.
	Encoder dropped(port);
	CHECK(dropped.publish("cam"));
	CHECK(dropped.read_until([&] { return dropped.client.status_code() == "NetStream.Publish.Start"; }));
	CHECK(dropped.send(dropped.client.video(0, {0x17, 0, 0, 0, 0, 1, 2, 3})));
	CHECK(sink->wait_for([&] { return sink->video_packets.size() == 1; }));

	// Straight away it still counts as alive, so a second sender is refused.
	{
		Encoder too_early(port);
		CHECK(too_early.publish("cam"));
		CHECK(too_early.read_until([&] { return too_early.client.status_code() == "NetStream.Publish.BadName"; }));
	}

	// Once it has been quiet long enough, the camera reconnecting takes over.
	usleep(700000);
	Encoder camera(port);
	CHECK(camera.publish("cam"));
	CHECK(camera.read_until([&] { return camera.client.status_code() == "NetStream.Publish.Start"; }));
	CHECK(camera.send(camera.client.video(0, {0x17, 0, 0, 0, 0, 4, 5, 6})));
	CHECK(sink->wait_for([&] { return sink->video_packets.size() == 2; }));
	CHECK(sink->wait_for([&] { return sink->starts == 2; }));

	// The stale connection is dropped, and its end must not be reported: the
	// sink would otherwise think the new stream ended.
	CHECK(!dropped.read_until([] { return false; }, 2000));
	usleep(200000);
	CHECK_EQ(sink->ends, 0);
	CHECK(reg->publisher_connected());
	CHECK(camera.send(camera.client.video(40, keyframe_tag())));
	CHECK(sink->wait_for([&] { return sink->video_packets.size() == 3; }));
}

int main()
{
	test_shared_port_routes();
	test_publisher_can_reconnect();
	test_takeover_of_silent_publisher();
	return finish_tests("hub");
}

#endif
