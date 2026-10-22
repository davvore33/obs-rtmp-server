// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rtmp-server-source contributors
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace rtmp {

enum class LogLevel { Debug, Info, Warning, Error };

using LogFn = void (*)(LogLevel level, const char *message);

inline std::atomic<LogFn> &log_callback()
{
	static std::atomic<LogFn> fn{nullptr};
	return fn;
}

// Route the library's log output (the OBS plugin points this at blog()).
inline void set_log_callback(LogFn fn)
{
	log_callback().store(fn);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
inline void log(LogLevel level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (LogFn fn = log_callback().load())
		fn(level, buf);
	else if (level != LogLevel::Debug)
		fprintf(stderr, "[rtmp] %s\n", buf);
}

} // namespace rtmp
