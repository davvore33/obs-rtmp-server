// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#include "rtmp/hub.h"

#include "rtmp/log.h"
#include "rtmp/server.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

namespace rtmp {

namespace {

int64_t now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class RouteSink;

struct Route {
	std::string app;
	std::string key;
	std::shared_ptr<StreamSink> sink;
	int takeover_after_ms = 0;
	std::atomic<bool> removed{false};
	// When the current publisher last sent anything.
	std::atomic<int64_t> last_activity_ms{0};
	// Guarded by the owning Endpoint's mutex.
	bool busy = false;
	std::string peer;
	RouteSink *owner = nullptr; // the publisher currently holding the route
};

// One listening port: a Server plus the routes registered on it.
class Endpoint : public Router {
public:
	explicit Endpoint(Server::Config config) : server(std::move(config), *this) {}
	~Endpoint() override { server.stop(); }

	std::shared_ptr<StreamSink> open_stream(const PublishInfo &info, std::string &reject_reason) override;
	// Frees the route if `who` still holds it (a takeover may have passed it on).
	void release(Route &route, RouteSink *who);

	Server server;
	std::mutex mutex;
	std::vector<std::shared_ptr<Route>> routes;
};

// Wraps a route's sink for the lifetime of one publisher: forwards media,
// frees the route when the publisher goes away, and lets the route kick it.
//
// Holds a raw Endpoint pointer on purpose. A sink only lives inside a session
// on one of the endpoint's connection threads, and ~Endpoint joins those
// threads first, so the endpoint always outlives every RouteSink.
class RouteSink : public StreamSink {
public:
	RouteSink(Endpoint *endpoint, std::shared_ptr<Route> route) : endpoint_(endpoint), route_(std::move(route)) {}

	// A newer publisher took the route over: stop forwarding and get dropped.
	void detach() { detached_ = true; }

	void on_publish_start(const PublishInfo &info) override
	{
		if (detached_)
			return;
		touch();
		route_->sink->on_publish_start(info);
	}
	void on_metadata(const Metadata &metadata) override
	{
		if (detached_)
			return;
		touch();
		route_->sink->on_metadata(metadata);
	}
	void on_video(const VideoPacket &packet) override
	{
		if (detached_)
			return;
		touch();
		route_->sink->on_video(packet);
	}
	void on_audio(const AudioPacket &packet) override
	{
		if (detached_)
			return;
		touch();
		route_->sink->on_audio(packet);
	}
	bool should_disconnect() const override { return detached_ || route_->removed; }

	void on_publish_end(const char *reason) override
	{
		if (detached_)
			return; // the route already belongs to someone else
		route_->sink->on_publish_end(reason);
		endpoint_->release(*route_, this);
	}

private:
	void touch() { route_->last_activity_ms = now_ms(); }

	Endpoint *endpoint_;
	std::shared_ptr<Route> route_;
	std::atomic<bool> detached_{false};
};

std::shared_ptr<StreamSink> Endpoint::open_stream(const PublishInfo &info, std::string &reject_reason)
{
	std::lock_guard<std::mutex> lock(mutex);

	std::shared_ptr<Route> match;
	for (const auto &route : routes) {
		if (route->removed || route->app != info.app)
			continue;
		if (route->key == info.stream_key) {
			match = route;
			break;
		}
		if (route->key.empty() && !match)
			match = route; // wildcard route, an exact match still wins
	}

	if (!match) {
		reject_reason = "No stream is listening for this application and stream key.";
		return nullptr;
	}
	if (match->busy) {
		// A publisher whose network dropped without a goodbye looks alive until
		// its connection times out. If it has gone quiet, let the newcomer in
		// (typically the same camera reconnecting) instead of locking it out.
		const int64_t quiet_ms = now_ms() - match->last_activity_ms;
		if (match->takeover_after_ms <= 0 || quiet_ms < match->takeover_after_ms) {
			reject_reason = "This stream already has a publisher.";
			return nullptr;
		}
		log(LogLevel::Info, "%s: taking over %s/%s from %s, silent for %lld ms", info.peer.c_str(),
		    info.app.c_str(), info.stream_key.c_str(), match->peer.c_str(), static_cast<long long>(quiet_ms));
		if (match->owner)
			match->owner->detach();
	}

	auto sink = std::make_shared<RouteSink>(this, match);
	match->busy = true;
	match->peer = info.peer;
	match->owner = sink.get();
	match->last_activity_ms = now_ms();
	return sink;
}

void Endpoint::release(Route &route, RouteSink *who)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (route.owner != who)
		return;
	route.busy = false;
	route.peer.clear();
	route.owner = nullptr;
}

} // namespace

struct Hub::State {
	std::mutex mutex; // guards `endpoints`; taken before any Endpoint::mutex
	std::map<uint16_t, std::shared_ptr<Endpoint>> endpoints;
};

struct Hub::Registration::Impl {
	std::shared_ptr<Endpoint> endpoint;
	std::shared_ptr<Route> route;
};

Hub::Hub() : state_(std::make_unique<State>()) {}
Hub::~Hub() = default;

Hub &Hub::instance()
{
	// Never destroyed: threads and registrations may outlive static teardown.
	static Hub *hub = new Hub();
	return *hub;
}

Hub::Registration::Registration(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Hub::Registration::~Registration()
{
	Hub::State &state = *Hub::instance().state_;
	std::shared_ptr<Endpoint> doomed;
	{
		std::lock_guard<std::mutex> hub_lock(state.mutex);
		Endpoint &endpoint = *impl_->endpoint;

		bool empty;
		{
			std::lock_guard<std::mutex> lock(endpoint.mutex);
			impl_->route->removed = true;
			auto &routes = endpoint.routes;
			for (auto it = routes.begin(); it != routes.end(); ++it) {
				if (*it == impl_->route) {
					routes.erase(it);
					break;
				}
			}
			empty = routes.empty();
		}

		if (empty) {
			auto it = state.endpoints.find(endpoint.server.config().port);
			if (it != state.endpoints.end() && it->second == impl_->endpoint)
				state.endpoints.erase(it);
			doomed = impl_->endpoint;
		}
	}

	impl_->endpoint.reset();
	// Stop the server outside the hub lock: this joins the connection threads.
	doomed.reset();
}

bool Hub::Registration::publisher_connected() const
{
	std::lock_guard<std::mutex> lock(impl_->endpoint->mutex);
	return impl_->route->busy;
}

std::string Hub::Registration::publisher_peer() const
{
	std::lock_guard<std::mutex> lock(impl_->endpoint->mutex);
	return impl_->route->peer;
}

std::unique_ptr<Hub::Registration> Hub::add_route(const RouteSpec &spec, std::shared_ptr<StreamSink> sink,
						   std::string &error)
{
	std::lock_guard<std::mutex> hub_lock(state_->mutex);

	std::shared_ptr<Endpoint> endpoint;
	bool created = false;

	auto it = state_->endpoints.find(spec.port);
	if (it != state_->endpoints.end()) {
		endpoint = it->second;
		if (endpoint->server.config().bind_address != spec.bind_address) {
			error = "Port " + std::to_string(spec.port) + " is already used by another RTMP source listening on " +
				endpoint->server.config().bind_address + ".";
			return nullptr;
		}
	} else {
		Server::Config config;
		config.bind_address = spec.bind_address;
		config.port = spec.port;
		endpoint = std::make_shared<Endpoint>(config);
		if (!endpoint->server.start(error))
			return nullptr;
		created = true;
	}

	auto route = std::make_shared<Route>();
	route->app = spec.app;
	route->key = spec.key;
	route->sink = std::move(sink);
	route->takeover_after_ms = spec.takeover_after_ms;

	{
		std::lock_guard<std::mutex> lock(endpoint->mutex);
		for (const auto &other : endpoint->routes) {
			if (other->app == spec.app && other->key == spec.key) {
				error = "Another RTMP source already uses this application and stream key on port " +
					std::to_string(spec.port) + ".";
				return nullptr;
			}
		}
		endpoint->routes.push_back(route);
	}

	if (created)
		state_->endpoints[spec.port] = endpoint;

	auto impl = std::make_unique<Registration::Impl>();
	impl->endpoint = std::move(endpoint);
	impl->route = std::move(route);
	return std::unique_ptr<Registration>(new Registration(std::move(impl)));
}

} // namespace rtmp
