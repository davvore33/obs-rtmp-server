// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#pragma once

#include "rtmp/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rtmp {

// Process-wide registry that lets several consumers (OBS sources) share one
// listening port: each registers an (app, stream key) route, and the hub
// starts a server per port on demand and stops it with the last route.
class Hub {
public:
	struct RouteSpec {
		std::string bind_address = "127.0.0.1";
		uint16_t port = 1935;
		std::string app = "live";
		// Empty accepts any stream key under `app`.
		std::string key;
		// A new publisher may take the route over from one that has sent
		// nothing for this long (a camera whose Wi-Fi dropped and came back).
		// 0 disables takeover: the newcomer is always refused while busy.
		int takeover_after_ms = 5000;
	};

	// Keeps a route registered. Destroying it removes the route, disconnects
	// its publisher and stops the port's server if no routes remain.
	class Registration {
	public:
		~Registration();

		Registration(const Registration &) = delete;
		Registration &operator=(const Registration &) = delete;

		bool publisher_connected() const;
		std::string publisher_peer() const;

	private:
		friend class Hub;
		struct Impl;
		explicit Registration(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> impl_;
	};

	static Hub &instance();

	// Returns null and sets `error` on failure (port in use, key taken, ...).
	// `sink` receives the publisher's media for as long as the route is live.
	std::unique_ptr<Registration> add_route(const RouteSpec &spec, std::shared_ptr<StreamSink> sink,
						std::string &error);

private:
	Hub();
	~Hub();
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace rtmp
