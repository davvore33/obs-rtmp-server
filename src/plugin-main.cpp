// SPDX-License-Identifier: GPL-2.0-or-later
/*
OBS RTMP Server
Copyright (C) 2026 obs-rtmp-server contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "rtmp/log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

void register_rtmp_server_source();

namespace {

void forward_log(rtmp::LogLevel level, const char *message)
{
	int obs_level = LOG_DEBUG;
	switch (level) {
	case rtmp::LogLevel::Error:
		obs_level = LOG_ERROR;
		break;
	case rtmp::LogLevel::Warning:
		obs_level = LOG_WARNING;
		break;
	case rtmp::LogLevel::Info:
		obs_level = LOG_INFO;
		break;
	case rtmp::LogLevel::Debug:
		obs_level = LOG_DEBUG;
		break;
	}
	obs_log(obs_level, "%s", message);
}

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("RtmpServer.Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS RTMP Server";
}

bool obs_module_load(void)
{
	rtmp::set_log_callback(forward_log);
	register_rtmp_server_source();
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	rtmp::set_log_callback(nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
