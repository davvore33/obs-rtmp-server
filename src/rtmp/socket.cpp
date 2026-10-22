// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "rtmp/socket.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace rtmp::net {

namespace {

#ifdef _WIN32
constexpr int kSendFlags = 0;
void close_socket(socket_t fd)
{
	closesocket(fd);
}
int socket_errno()
{
	return WSAGetLastError();
}
#else
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
void close_socket(socket_t fd)
{
	::close(fd);
}
int socket_errno()
{
	return errno;
}
#endif

constexpr int kSendTimeoutMs = 5000;

std::string describe_peer(const sockaddr *addr, socklen_t len)
{
	char host[NI_MAXHOST];
	char port[NI_MAXSERV];
	if (getnameinfo(addr, len, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		return "unknown";
	if (addr->sa_family == AF_INET6)
		return std::string("[") + host + "]:" + port;
	return std::string(host) + ":" + port;
}

} // namespace

bool init()
{
#ifdef _WIN32
	static const bool ok = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	return ok;
#else
	return true;
#endif
}

std::string last_error()
{
#ifdef _WIN32
	return "socket error " + std::to_string(WSAGetLastError());
#else
	return std::strerror(errno);
#endif
}

void Socket::reset()
{
	if (fd_ != kInvalidSocket) {
		close_socket(fd_);
		fd_ = kInvalidSocket;
	}
}

WakePipe::WakePipe()
{
#ifndef _WIN32
	int fds[2];
	if (::pipe(fds) == 0) {
		fds_[0] = fds[0];
		fds_[1] = fds[1];
	}
#endif
}

WakePipe::~WakePipe()
{
#ifndef _WIN32
	for (socket_t fd : fds_) {
		if (fd != kInvalidSocket)
			::close(fd);
	}
#endif
}

void WakePipe::signal()
{
#ifndef _WIN32
	if (fds_[1] != kInvalidSocket) {
		const char byte = 1;
		ssize_t ignored = ::write(fds_[1], &byte, 1);
		(void)ignored;
	}
#endif
}

WaitResult wait_readable(socket_t fd, socket_t wake_fd, int timeout_ms)
{
#ifdef _WIN32
	// No wake pipe on Windows: callers fall back to their poll timeout.
	(void)wake_fd;
	WSAPOLLFD pfd = {fd, POLLRDNORM, 0};
	int r = WSAPoll(&pfd, 1, timeout_ms);
	if (r == 0)
		return WaitResult::Timeout;
	if (r < 0)
		return WaitResult::Error;
	if (pfd.revents & (POLLRDNORM | POLLHUP))
		return WaitResult::Readable;
	return WaitResult::Error;
#else
	pollfd fds[2];
	fds[0] = {fd, POLLIN, 0};
	nfds_t count = 1;
	if (wake_fd != kInvalidSocket) {
		fds[1] = {wake_fd, POLLIN, 0};
		count = 2;
	}
	int r;
	do {
		r = ::poll(fds, count, timeout_ms);
	} while (r < 0 && errno == EINTR);

	if (r == 0)
		return WaitResult::Timeout;
	if (r < 0)
		return WaitResult::Error;
	if (count == 2 && (fds[1].revents & POLLIN))
		return WaitResult::Wake;
	if (fds[0].revents & (POLLIN | POLLHUP))
		return WaitResult::Readable;
	return WaitResult::Error;
#endif
}

bool listen_on(const std::string &address, uint16_t port, Socket &out, std::string &error)
{
	if (!init()) {
		error = "network layer failed to start";
		return false;
	}

	addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

	addrinfo *results = nullptr;
	const std::string port_text = std::to_string(port);
	const char *host = address.empty() ? nullptr : address.c_str();
	int rc = getaddrinfo(host, port_text.c_str(), &hints, &results);
	if (rc != 0) {
		error = "invalid listen address '" + address + "': " + gai_strerror(rc);
		return false;
	}

	std::string last_failure = "no usable address";
	for (addrinfo *ai = results; ai; ai = ai->ai_next) {
		Socket sock(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
		if (!sock.valid()) {
			last_failure = last_error();
			continue;
		}

		int on = 1;
		setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
		setsockopt(sock.get(), SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&on), sizeof(on));
#endif
		if (ai->ai_family == AF_INET6) {
			// "::" should accept IPv4 too.
			int off = 0;
			setsockopt(sock.get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off),
				   sizeof(off));
		}

		if (::bind(sock.get(), ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
			last_failure = last_error();
			continue;
		}
		if (::listen(sock.get(), 16) != 0) {
			last_failure = last_error();
			continue;
		}

		out = std::move(sock);
		freeaddrinfo(results);
		return true;
	}

	freeaddrinfo(results);
	error = "cannot listen on " + address + ":" + port_text + " (" + last_failure + ")";
	return false;
}

bool accept_connection(socket_t listener, Socket &out, std::string &peer)
{
	sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	socket_t fd = ::accept(listener, reinterpret_cast<sockaddr *>(&addr), &len);
	if (fd == kInvalidSocket)
		return false;
	out = Socket(fd);
	peer = describe_peer(reinterpret_cast<sockaddr *>(&addr), len);
	return true;
}

void configure_client(socket_t fd)
{
	int on = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&on), sizeof(on));
#endif
#ifdef _WIN32
	DWORD timeout = kSendTimeoutMs;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
	timeval tv;
	tv.tv_sec = kSendTimeoutMs / 1000;
	tv.tv_usec = (kSendTimeoutMs % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool send_all(socket_t fd, const uint8_t *data, size_t size)
{
	while (size > 0) {
#ifdef _WIN32
		int n = ::send(fd, reinterpret_cast<const char *>(data), static_cast<int>(size), kSendFlags);
#else
		ssize_t n = ::send(fd, data, size, kSendFlags);
#endif
		if (n < 0) {
			if (socket_errno() == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		data += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

long recv_some(socket_t fd, uint8_t *buffer, size_t size)
{
	for (;;) {
#ifdef _WIN32
		int n = ::recv(fd, reinterpret_cast<char *>(buffer), static_cast<int>(size), 0);
#else
		ssize_t n = ::recv(fd, buffer, size, 0);
#endif
		if (n < 0 && socket_errno() == EINTR)
			continue;
		return static_cast<long>(n);
	}
}

} // namespace rtmp::net
