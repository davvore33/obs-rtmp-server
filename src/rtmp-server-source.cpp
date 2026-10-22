// SPDX-License-Identifier: GPL-2.0-or-later
/*
RTMP Server Source
Copyright (C) 2026 rtmp-server-source contributors

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

// The "RTMP Server" source: listens for an RTMP publisher (OBS, ffmpeg, a
// phone app...) and shows what it sends as an async video + audio source.

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "media-decoder.h"
#include "rtmp/hub.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace {

constexpr const char *kBindAddress = "bind_address";
constexpr const char *kPort = "port";
constexpr const char *kApp = "app";
constexpr const char *kStreamKey = "stream_key";
constexpr const char *kServerUrl = "server_url";
constexpr const char *kLatency = "latency";

constexpr const char *kBindLocal = "127.0.0.1";
constexpr const char *kBindAll = "0.0.0.0";
constexpr const char *kLatencyNormal = "normal";
constexpr const char *kLatencyLow = "low";

constexpr int kDefaultPort = 1935;

const char *text(const char *key)
{
	return obs_module_text(key);
}

std::string trim(std::string s)
{
	auto not_space = [](unsigned char c) { return !std::isspace(c) && c != '/'; };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
	s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
	return s;
}

// First non-loopback IPv4 address of this machine, for the "listen on all
// interfaces" case. Empty if unknown.
std::string lan_address()
{
#ifdef _WIN32
	return std::string();
#else
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return std::string();

	std::string found;
	for (ifaddrs *a = list; a && found.empty(); a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
			continue;
		char buf[INET_ADDRSTRLEN];
		auto *in = reinterpret_cast<sockaddr_in *>(a->ifa_addr);
		if (inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf)))
			found = buf;
	}
	freeifaddrs(list);
	return found;
#endif
}

std::string server_url_for(const std::string &bind, int port, const std::string &app)
{
	std::string host = bind;
	if (bind == kBindAll || bind.empty()) {
		host = lan_address();
		if (host.empty())
			host = "<this-computer-ip>";
	}
	std::string url = "rtmp://" + host;
	if (port != 1935)
		url += ":" + std::to_string(port);
	url += "/" + app;
	return url;
}

// ---------------------------------------------------------------------------
// Ingest: receives one publisher's packets on the network thread, decodes them
// and hands pictures and PCM to the OBS source.
//
// It outlives the OBS source if the hub still holds it, so everything that
// touches the source goes through `mutex_` and detach() cuts the link.
// ---------------------------------------------------------------------------
class Ingest : public rtmp::StreamSink {
public:
	struct Status {
		bool connected = false;
		std::string peer;
		std::string video;
		std::string audio;
	};

	explicit Ingest(obs_source_t *source) : source_(source) {}

	// The source is being destroyed: wait for any callback in flight, then
	// make all later ones no-ops.
	void detach()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		clear_output();
		source_ = nullptr;
		video_decoder_.close();
		audio_decoder_.close();
	}

	Status status() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Status s;
		s.connected = connected_;
		s.peer = peer_;
		s.video = video_info_;
		s.audio = audio_info_;
		return s;
	}

	void on_publish_start(const rtmp::PublishInfo &info) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		reset_stream_state();
		connected_ = true;
		peer_ = info.peer;
		obs_log(LOG_INFO, "publisher connected: %s", info.peer.c_str());
	}

	void on_metadata(const rtmp::Metadata &md) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (md.width > 0 && md.height > 0)
			obs_log(LOG_INFO, "stream metadata: %.0fx%.0f @ %.2f fps (%s)", md.width, md.height, md.frame_rate,
				md.encoder.c_str());
	}

	void on_video(const rtmp::VideoPacket &packet) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!source_)
			return;

		if (packet.config) {
			if (video_decoder_.matches(packet.codec, packet.data, packet.size))
				return;
			video_codec_ = packet.codec;
			video_config_.assign(packet.data, packet.data + packet.size);
			open_video_decoder();
			return;
		}
		if (packet.end_of_sequence || !video_decoder_.is_open())
			return;
		if (waiting_for_keyframe_) {
			if (!packet.keyframe)
				return;
			waiting_for_keyframe_ = false;
		}

		const bool ok = video_decoder_.decode(packet, [this](const rtmpsrc::DecodedVideo &v) { output_video(v); });
		if (!ok) {
			// Too many undecodable packets: start over at the next keyframe.
			obs_log(LOG_WARNING, "video decoder is failing, restarting it");
			open_video_decoder();
		}
	}

	void on_audio(const rtmp::AudioPacket &packet) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!source_)
			return;

		if (packet.config || (!audio_decoder_.is_open() && packet.codec == rtmp::AudioCodec::MP3)) {
			// MP3 has no config record: open on the first frame.
			const uint8_t *config = packet.config ? packet.data : nullptr;
			const size_t size = packet.config ? packet.size : 0;
			if (audio_decoder_.matches(packet.codec, config, size))
				return;
			audio_codec_ = packet.codec;
			audio_config_.assign(config, config + size);
			audio_rate_hint_ = packet.sample_rate_hint;
			audio_channels_hint_ = packet.channels_hint;
			if (!open_audio_decoder() || packet.config)
				return;
		}
		if (!audio_decoder_.is_open())
			return;

		if (!audio_decoder_.decode(packet, [this](const rtmpsrc::DecodedAudio &a) { output_audio(a); })) {
			obs_log(LOG_WARNING, "audio decoder is failing, restarting it");
			open_audio_decoder();
		}
	}

	void on_publish_end(const char *reason) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		obs_log(LOG_INFO, "publisher disconnected (%s)", reason);
		connected_ = false;
		reset_stream_state();
		clear_output();
	}

private:
	// Called with mutex_ held.
	void open_video_decoder()
	{
		std::string error;
		waiting_for_keyframe_ = true;
		if (!video_decoder_.open(video_codec_, video_config_.data(), video_config_.size(), error))
			obs_log(LOG_WARNING, "video: %s", error.c_str());
	}

	bool open_audio_decoder()
	{
		std::string error;
		if (audio_decoder_.open(audio_codec_, audio_config_.data(), audio_config_.size(), audio_rate_hint_,
					audio_channels_hint_, error))
			return true;
		obs_log(LOG_WARNING, "audio: %s", error.c_str());
		return false;
	}

	void reset_stream_state()
	{
		video_decoder_.close();
		audio_decoder_.close();
		video_config_.clear();
		audio_config_.clear();
		waiting_for_keyframe_ = true;
		timing_set_ = false;
		base_ns_ = 0;
		peer_.clear();
		video_info_.clear();
		audio_info_.clear();
	}

	void clear_output()
	{
		if (source_)
			obs_source_output_video2(source_, nullptr);
	}

	// RTMP timestamps are the publisher's own clock. Anchor the first one to
	// the local clock so audio and video keep their relative timing and OBS
	// buffers from there.
	uint64_t to_obs_ns(int64_t ms)
	{
		if (!timing_set_) {
			base_ns_ = static_cast<int64_t>(os_gettime_ns()) - ms * 1000000;
			timing_set_ = true;
		}
		int64_t ns = base_ns_ + ms * 1000000;
		return ns < 0 ? 0 : static_cast<uint64_t>(ns);
	}

	void output_video(const rtmpsrc::DecodedVideo &v)
	{
		obs_source_frame2 frame = {};
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			frame.data[i] = const_cast<uint8_t *>(v.data[i]);
			frame.linesize[i] = v.linesize[i];
		}
		frame.width = v.width;
		frame.height = v.height;
		frame.timestamp = to_obs_ns(v.pts_ms);
		frame.format = v.format;
		frame.range = v.range;
		frame.trc = static_cast<uint8_t>(v.trc);
		if (!video_format_get_parameters_for_format(v.colorspace, v.range, v.format, frame.color_matrix,
							    frame.color_range_min, frame.color_range_max))
			return;

		if (video_info_.empty() || v.width != info_width_ || v.height != info_height_) {
			info_width_ = v.width;
			info_height_ = v.height;
			video_info_ = std::to_string(v.width) + "x" + std::to_string(v.height) + " " +
				      rtmp::codec_name(video_decoder_.codec());
			obs_log(LOG_INFO, "receiving video: %s", video_info_.c_str());
		}
		obs_source_output_video2(source_, &frame);
	}

	void output_audio(const rtmpsrc::DecodedAudio &a)
	{
		obs_source_audio audio = {};
		const uint32_t channels = get_audio_channels(a.layout);
		for (uint32_t ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++)
			audio.data[ch] = reinterpret_cast<const uint8_t *>(a.planes[ch]);
		audio.frames = a.frames;
		audio.speakers = a.layout;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = a.sample_rate;
		audio.timestamp = to_obs_ns(a.pts_ms);

		if (audio_info_.empty()) {
			char rate[32];
			snprintf(rate, sizeof(rate), "%.1f kHz", a.sample_rate / 1000.0);
			audio_info_ = std::string(rtmp::codec_name(audio_decoder_.codec())) + " " + rate + " " +
				      (channels == 1 ? "mono" : channels == 2 ? "stereo" : std::to_string(channels) + " ch");
			obs_log(LOG_INFO, "receiving audio: %s", audio_info_.c_str());
		}
		obs_source_output_audio(source_, &audio);
	}

	mutable std::mutex mutex_;
	obs_source_t *source_;

	rtmpsrc::VideoDecoder video_decoder_;
	rtmpsrc::AudioDecoder audio_decoder_;
	bool waiting_for_keyframe_ = true;

	// Last decoder configuration seen, kept so a failed decoder can be reopened.
	rtmp::VideoCodec video_codec_ = rtmp::VideoCodec::Unknown;
	std::vector<uint8_t> video_config_;
	rtmp::AudioCodec audio_codec_ = rtmp::AudioCodec::Unknown;
	std::vector<uint8_t> audio_config_;
	uint32_t audio_rate_hint_ = 0;
	int audio_channels_hint_ = 0;

	bool timing_set_ = false;
	int64_t base_ns_ = 0;

	bool connected_ = false;
	std::string peer_;
	std::string video_info_;
	std::string audio_info_;
	uint32_t info_width_ = 0;
	uint32_t info_height_ = 0;
};

// ---------------------------------------------------------------------------
// The OBS source object
// ---------------------------------------------------------------------------
struct RtmpServerSource {
	obs_source_t *source = nullptr;
	std::shared_ptr<Ingest> ingest;
	std::unique_ptr<rtmp::Hub::Registration> registration;

	// What `registration` was created for, and why the last attempt failed.
	rtmp::Hub::RouteSpec active_spec;
	std::string error;
};

const char *source_get_name(void *)
{
	return text("RtmpServerSource");
}

bool same_route(const rtmp::Hub::RouteSpec &a, const rtmp::Hub::RouteSpec &b)
{
	return a.bind_address == b.bind_address && a.port == b.port && a.app == b.app && a.key == b.key;
}

void source_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<RtmpServerSource *>(data);

	const bool low_latency = std::string(obs_data_get_string(settings, kLatency)) == kLatencyLow;
	obs_source_set_async_unbuffered(s->source, low_latency);
	obs_source_set_async_decoupled(s->source, low_latency);

	rtmp::Hub::RouteSpec spec;
	spec.bind_address = obs_data_get_string(settings, kBindAddress);
	spec.port = static_cast<uint16_t>(std::clamp<long long>(obs_data_get_int(settings, kPort), 1, 65535));
	spec.app = trim(obs_data_get_string(settings, kApp));
	spec.key = trim(obs_data_get_string(settings, kStreamKey));

	obs_data_set_string(settings, kServerUrl, server_url_for(spec.bind_address, spec.port, spec.app).c_str());

	if (s->registration && same_route(spec, s->active_spec))
		return;

	// Register the new route before dropping the old one, so a change of key
	// or app on the same port does not restart the server.
	std::string error;
	auto fresh = rtmp::Hub::instance().add_route(spec, s->ingest, error);
	if (!fresh && s->registration && s->active_spec.port == spec.port) {
		// Probably conflicts with our own previous route (new bind address).
		s->registration.reset();
		error.clear();
		fresh = rtmp::Hub::instance().add_route(spec, s->ingest, error);
	}

	s->registration = std::move(fresh);
	if (s->registration) {
		s->active_spec = spec;
		s->error.clear();
		obs_log(LOG_INFO, "listening: %s/%s", server_url_for(spec.bind_address, spec.port, spec.app).c_str(),
			spec.key.c_str());
	} else {
		s->error = error;
		obs_log(LOG_WARNING, "cannot listen on %s:%u: %s", spec.bind_address.c_str(), spec.port, error.c_str());
	}
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new RtmpServerSource();
	s->source = source;
	s->ingest = std::make_shared<Ingest>(source);
	source_update(s, settings);
	return s;
}

void source_destroy(void *data)
{
	auto *s = static_cast<RtmpServerSource *>(data);
	// Cut the link to OBS first so a packet in flight cannot reach a dead source.
	s->ingest->detach();
	s->registration.reset();
	delete s;
}

void source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kBindAddress, kBindLocal);
	obs_data_set_default_int(settings, kPort, kDefaultPort);
	obs_data_set_default_string(settings, kApp, "live");
	obs_data_set_default_string(settings, kStreamKey, "stream");
	obs_data_set_default_string(settings, kLatency, kLatencyNormal);
}

// Keeps the read-only "Server URL" field in step with the fields it is built from.
bool on_route_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	std::string bind = obs_data_get_string(settings, kBindAddress);
	int port = static_cast<int>(obs_data_get_int(settings, kPort));
	std::string app = trim(obs_data_get_string(settings, kApp));
	obs_data_set_string(settings, kServerUrl, server_url_for(bind, port, app).c_str());
	return true;
}

bool on_refresh_clicked(obs_properties_t *, obs_property_t *, void *)
{
	return true; // makes OBS rebuild the properties, which re-reads the status
}

obs_properties_t *source_get_properties(void *data)
{
	auto *s = static_cast<RtmpServerSource *>(data);
	obs_properties_t *props = obs_properties_create();

	// Status line.
	std::string status;
	obs_text_info_type status_type = OBS_TEXT_INFO_NORMAL;
	if (!s->error.empty()) {
		status = std::string(text("RtmpServer.Status.Error")) + " " + s->error;
		status_type = OBS_TEXT_INFO_ERROR;
	} else {
		Ingest::Status st = s->ingest->status();
		if (st.connected) {
			status = std::string(text("RtmpServer.Status.Receiving")) + " " + st.peer;
			if (!st.video.empty())
				status += "\n" + st.video;
			if (!st.audio.empty())
				status += (st.video.empty() ? "\n" : " + ") + st.audio;
		} else {
			status = text("RtmpServer.Status.Waiting");
		}
	}
	obs_property_t *status_prop = obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_type(status_prop, status_type);
	obs_property_text_set_info_word_wrap(status_prop, true);

	obs_property_t *bind = obs_properties_add_list(props, kBindAddress, text("RtmpServer.Bind"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(bind, text("RtmpServer.Bind.Local"), kBindLocal);
	obs_property_list_add_string(bind, text("RtmpServer.Bind.All"), kBindAll);
	obs_property_set_long_description(bind, text("RtmpServer.Bind.Help"));
	obs_property_set_modified_callback(bind, on_route_changed);

	obs_property_t *port = obs_properties_add_int(props, kPort, text("RtmpServer.Port"), 1, 65535, 1);
	obs_property_set_modified_callback(port, on_route_changed);

	obs_property_t *app = obs_properties_add_text(props, kApp, text("RtmpServer.App"), OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback(app, on_route_changed);

	obs_properties_add_text(props, kStreamKey, text("RtmpServer.StreamKey"), OBS_TEXT_DEFAULT);

	obs_property_t *url = obs_properties_add_text(props, kServerUrl, text("RtmpServer.ServerUrl"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(url, text("RtmpServer.ServerUrl.Help"));

	obs_property_t *latency = obs_properties_add_list(props, kLatency, text("RtmpServer.Latency"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(latency, text("RtmpServer.Latency.Normal"), kLatencyNormal);
	obs_property_list_add_string(latency, text("RtmpServer.Latency.Low"), kLatencyLow);
	obs_property_set_long_description(latency, text("RtmpServer.Latency.Help"));

	obs_properties_add_button2(props, "refresh", text("RtmpServer.Refresh"), on_refresh_clicked, s);

	return props;
}

} // namespace

void register_rtmp_server_source()
{
	obs_source_info info = {};
	info.id = "rtmp_server_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = source_get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_defaults = source_get_defaults;
	info.get_properties = source_get_properties;
	obs_register_source(&info);
}
