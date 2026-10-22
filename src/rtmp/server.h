// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include "rtmp/socket.h"
#include "rtmp/types.h"

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rtmp {

// A TCP listener that runs one thread per RTMP connection and hands publishers
// to a Router. Meant for a handful of ingest connections, not a CDN.
class Server {
public:
	struct Config {
		std::string bind_address = "127.0.0.1";
		uint16_t port = 1935;
		size_t max_connections = 8;
		// Drop a connection that sends nothing for this long.
		int idle_timeout_ms = 30000;
		// Drop a connection that has not started publishing after this long.
		int publish_deadline_ms = 20000;
	};

	Server(Config config, Router &router);
	~Server();

	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	bool start(std::string &error);
	// Stops accepting, disconnects everyone and joins all threads. Idempotent.
	void stop();

	bool running() const { return running_; }
	const Config &config() const { return config_; }

private:
	struct Connection {
		net::Socket socket;
		std::string peer;
		std::thread thread;
		std::atomic<bool> finished{false};
	};

	void accept_loop();
	void serve(Connection *connection);
	void reap_finished();

	Config config_;
	Router &router_;

	std::atomic<bool> running_{false};
	net::Socket listener_;
	std::unique_ptr<net::WakePipe> wake_;
	std::thread accept_thread_;

	std::mutex connections_mutex_;
	std::list<std::unique_ptr<Connection>> connections_;
};

} // namespace rtmp
