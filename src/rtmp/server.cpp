// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "rtmp/server.h"

#include "rtmp/log.h"
#include "rtmp/session.h"

#include <chrono>
#include <vector>

namespace rtmp {

namespace {

constexpr int kPollIntervalMs = 200;

} // namespace

Server::Server(Config config, Router &router) : config_(std::move(config)), router_(router) {}

Server::~Server()
{
	stop();
}

bool Server::start(std::string &error)
{
	if (running_)
		return true;

	net::Socket listener;
	if (!net::listen_on(config_.bind_address, config_.port, listener, error))
		return false;

	listener_ = std::move(listener);
	wake_ = std::make_unique<net::WakePipe>();
	running_ = true;

	try {
		accept_thread_ = std::thread([this] { accept_loop(); });
	} catch (const std::exception &e) {
		running_ = false;
		listener_.reset();
		error = std::string("cannot start server thread: ") + e.what();
		return false;
	}

	log(LogLevel::Info, "listening on %s:%u", config_.bind_address.c_str(), config_.port);
	return true;
}

void Server::stop()
{
	if (!running_.exchange(false))
		return;

	wake_->signal();
	if (accept_thread_.joinable())
		accept_thread_.join();

	std::list<std::unique_ptr<Connection>> connections;
	{
		std::lock_guard<std::mutex> lock(connections_mutex_);
		connections.swap(connections_);
	}
	for (auto &c : connections) {
		if (c->thread.joinable())
			c->thread.join();
	}

	listener_.reset();
	log(LogLevel::Info, "stopped listening on %s:%u", config_.bind_address.c_str(), config_.port);
}

void Server::reap_finished()
{
	std::list<std::unique_ptr<Connection>> done;
	{
		std::lock_guard<std::mutex> lock(connections_mutex_);
		for (auto it = connections_.begin(); it != connections_.end();) {
			if ((*it)->finished) {
				done.push_back(std::move(*it));
				it = connections_.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto &c : done) {
		if (c->thread.joinable())
			c->thread.join();
	}
}

void Server::accept_loop()
{
	const net::socket_t wake_fd = wake_->read_fd();

	while (running_) {
		net::WaitResult w = net::wait_readable(listener_.get(), wake_fd, kPollIntervalMs);
		reap_finished();
		if (w == net::WaitResult::Timeout)
			continue;
		if (w != net::WaitResult::Readable)
			break;

		net::Socket client;
		std::string peer;
		if (!net::accept_connection(listener_.get(), client, peer))
			continue;

		std::unique_lock<std::mutex> lock(connections_mutex_);
		if (connections_.size() >= config_.max_connections) {
			lock.unlock();
			log(LogLevel::Warning, "rejecting %s: too many connections", peer.c_str());
			continue; // the socket closes as `client` goes out of scope
		}

		net::configure_client(client.get());
		auto connection = std::make_unique<Connection>();
		connection->socket = std::move(client);
		connection->peer = peer;
		Connection *raw = connection.get();
		try {
			connection->thread = std::thread([this, raw] { serve(raw); });
		} catch (const std::exception &e) {
			log(LogLevel::Error, "cannot start connection thread: %s", e.what());
			continue;
		}
		connections_.push_back(std::move(connection));
	}
}

void Server::serve(Connection *connection)
{
	const net::socket_t fd = connection->socket.get();
	const net::socket_t wake_fd = wake_->read_fd();
	const auto started = std::chrono::steady_clock::now();

	log(LogLevel::Debug, "%s: connected", connection->peer.c_str());

	Session session(
		router_, [fd](const uint8_t *data, size_t size) { return net::send_all(fd, data, size); },
		connection->peer);

	std::vector<uint8_t> buffer(64 * 1024);
	int idle_ms = 0;
	const char *reason = "connection closed by peer";

	while (running_) {
		net::WaitResult w = net::wait_readable(fd, wake_fd, kPollIntervalMs);

		if (w == net::WaitResult::Wake) {
			reason = "server stopped";
			break;
		}
		if (w == net::WaitResult::Error) {
			reason = "socket error";
			break;
		}
		if (w == net::WaitResult::Timeout) {
			idle_ms += kPollIntervalMs;
			if (session.should_disconnect()) {
				reason = "stream was removed";
				break;
			}
			if (idle_ms >= config_.idle_timeout_ms) {
				reason = "timed out waiting for data";
				break;
			}
		} else {
			long n = net::recv_some(fd, buffer.data(), buffer.size());
			if (n <= 0)
				break;
			idle_ms = 0;
			if (!session.on_data(buffer.data(), static_cast<size_t>(n))) {
				reason = "closed by server";
				break;
			}
			if (session.should_disconnect()) {
				reason = "stream was removed";
				break;
			}
		}

		if (!session.publishing()) {
			auto elapsed = std::chrono::steady_clock::now() - started;
			if (elapsed > std::chrono::milliseconds(config_.publish_deadline_ms)) {
				reason = "did not start publishing in time";
				break;
			}
		}
	}

	session.finish(reason);
	log(LogLevel::Debug, "%s: disconnected (%s)", connection->peer.c_str(), reason);
	connection->finished = true;
}

} // namespace rtmp
