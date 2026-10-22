// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <basetsd.h>
#endif

namespace rtmp::net {

#ifdef _WIN32
using socket_t = UINT_PTR;
constexpr socket_t kInvalidSocket = static_cast<socket_t>(~0);
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

// Starts the platform socket layer (Winsock). No-op elsewhere.
bool init();

// Human readable description of the last socket error.
std::string last_error();

// Owning socket handle.
class Socket {
public:
	Socket() = default;
	explicit Socket(socket_t fd) : fd_(fd) {}
	~Socket() { reset(); }

	Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidSocket; }
	Socket &operator=(Socket &&other) noexcept
	{
		if (this != &other) {
			reset();
			fd_ = other.fd_;
			other.fd_ = kInvalidSocket;
		}
		return *this;
	}
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;

	socket_t get() const { return fd_; }
	bool valid() const { return fd_ != kInvalidSocket; }
	void reset();

private:
	socket_t fd_ = kInvalidSocket;
};

// A level-triggered "stop now" signal that wait_readable() can watch, so
// shutting a server down never has to wait for a poll timeout.
class WakePipe {
public:
	WakePipe();
	~WakePipe();
	WakePipe(const WakePipe &) = delete;
	WakePipe &operator=(const WakePipe &) = delete;

	// Once signalled it stays readable.
	void signal();
	socket_t read_fd() const { return fds_[0]; }

private:
	socket_t fds_[2] = {kInvalidSocket, kInvalidSocket};
};

enum class WaitResult { Readable, Wake, Timeout, Error };

// Blocks until `fd` is readable, `wake_fd` (optional) is readable, or the
// timeout expires.
WaitResult wait_readable(socket_t fd, socket_t wake_fd, int timeout_ms);

// Binds and listens on `address:port`. `address` may be an IPv4/IPv6 literal.
bool listen_on(const std::string &address, uint16_t port, Socket &out, std::string &error);

// Accepts one pending connection. `peer` is "ip:port".
bool accept_connection(socket_t listener, Socket &out, std::string &peer);

// Disables Nagle, SIGPIPE and sets a send timeout.
void configure_client(socket_t fd);

// Writes everything or fails. Returns false on error/timeout.
bool send_all(socket_t fd, const uint8_t *data, size_t size);

// Returns bytes read, 0 on orderly close, negative on error.
long recv_some(socket_t fd, uint8_t *buffer, size_t size);

} // namespace rtmp::net
