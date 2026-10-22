// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 rtmp-server-source contributors
// Listens like the OBS source does and reports what a publisher sends, so the
// server can be tried with a real encoder without running OBS.
//
//   rtmp-probe [--bind 127.0.0.1] [--port 1935] [--app live] [--key stream]
//
// With --decode the media is also run through the same decoders the OBS
// plugin uses, and --selftest turns that into a pass/fail check (used by
// tests/e2e.sh together with test-publisher):
//
//   rtmp-probe --decode --selftest --port 19350 --min-video-frames 60 \
//              --min-audio-seconds 2 --expect-size 640x360 --expect-luma 200,50

#include "media-decoder.h"
#include "rtmp/hub.h"
#include "rtmp/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

// media-decoder logs through the plugin's obs_log; here it just prints.
extern "C" void obs_log(int level, const char *format, ...)
{
	if (level > 300) // LOG_INFO and chattier
		return;
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[decoder] ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	va_end(args);
}

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int)
{
	g_stop = true;
}

struct Expectations {
	int min_video_frames = 0;
	double min_audio_seconds = 0;
	int width = 0;
	int height = 0;
	bool check_luma = false;
	double top_luma = 0;
	double bottom_luma = 0;
	double luma_tolerance = 12;
	double min_audio_rms = 0;
	double max_audio_rms = 1;
};

class ProbeSink : public rtmp::StreamSink {
public:
	explicit ProbeSink(bool decode) : decode_(decode) {}

	void on_publish_start(const rtmp::PublishInfo &info) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		printf("publisher connected: %s (app=%s key=%s flashVer=%s)\n", info.peer.c_str(), info.app.c_str(),
		       info.stream_key.c_str(), info.flash_ver.c_str());
		video_packets_ = audio_packets_ = keyframes_ = 0;
		video_bytes_ = audio_bytes_ = last_video_bytes_ = last_audio_bytes_ = 0;
		ever_connected_ = true;
	}

	void on_metadata(const rtmp::Metadata &md) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		printf("metadata: %.0fx%.0f @ %.2f fps, video %.0f kbps, audio %.0f kbps %.0f Hz %d ch, encoder '%s'\n",
		       md.width, md.height, md.frame_rate, md.video_bitrate_kbps, md.audio_bitrate_kbps,
		       md.audio_sample_rate, md.audio_channels, md.encoder.c_str());
	}

	void on_video(const rtmp::VideoPacket &p) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		video_codec_ = p.codec;
		last_dts_ = p.dts_ms;
		if (p.config) {
			printf("video config: %s, %zu bytes\n", rtmp::codec_name(p.codec), p.size);
			if (decode_ && !video_decoder_.matches(p.codec, p.data, p.size)) {
				std::string error;
				if (!video_decoder_.open(p.codec, p.data, p.size, error))
					printf("video decoder: %s\n", error.c_str());
				waiting_for_keyframe_ = true;
			}
			return;
		}
		video_packets_++;
		video_bytes_ += p.size;
		if (p.keyframe)
			keyframes_++;

		if (!decode_ || !video_decoder_.is_open())
			return;
		if (waiting_for_keyframe_) {
			if (!p.keyframe)
				return;
			waiting_for_keyframe_ = false;
		}
		video_decoder_.decode(p, [this](const rtmpsrc::DecodedVideo &v) { analyze_video(v); });
	}

	void on_audio(const rtmp::AudioPacket &p) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		audio_codec_ = p.codec;
		last_dts_ = p.dts_ms;
		if (p.config) {
			printf("audio config: %s, %zu bytes\n", rtmp::codec_name(p.codec), p.size);
		} else {
			audio_packets_++;
			audio_bytes_ += p.size;
		}
		if (!decode_)
			return;

		if (p.config || (!audio_decoder_.is_open() && p.codec == rtmp::AudioCodec::MP3)) {
			const uint8_t *config = p.config ? p.data : nullptr;
			const size_t size = p.config ? p.size : 0;
			if (!audio_decoder_.matches(p.codec, config, size)) {
				std::string error;
				if (!audio_decoder_.open(p.codec, config, size, p.sample_rate_hint, p.channels_hint, error))
					printf("audio decoder: %s\n", error.c_str());
			}
			if (p.config)
				return;
		}
		if (audio_decoder_.is_open())
			audio_decoder_.decode(p, [this](const rtmpsrc::DecodedAudio &a) { analyze_audio(a); });
	}

	void on_publish_end(const char *reason) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		printf("publisher gone: %s\n", reason);
		publisher_gone_ = true;
	}

	bool ever_connected() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return ever_connected_;
	}

	void print_stats()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (video_packets_ == 0 && audio_packets_ == 0)
			return;
		printf("  %s: %llu pkts (%llu key) %.0f kbps | %s: %llu pkts %.0f kbps | dts %.1fs", rtmp::codec_name(video_codec_),
		       (unsigned long long)video_packets_, (unsigned long long)keyframes_,
		       (video_bytes_ - last_video_bytes_) * 8 / 1000.0, rtmp::codec_name(audio_codec_),
		       (unsigned long long)audio_packets_, (audio_bytes_ - last_audio_bytes_) * 8 / 1000.0,
		       last_dts_ / 1000.0);
		if (decode_)
			printf(" | decoded %llu frames %ux%u, %.2fs audio", (unsigned long long)decoded_frames_, width_,
			       height_, decoded_audio_samples_ / (audio_rate_ ? audio_rate_ : 48000.0));
		printf("\n");
		last_video_bytes_ = video_bytes_;
		last_audio_bytes_ = audio_bytes_;
	}

	// Returns true when every expectation holds; otherwise `why` says what is missing.
	bool check(const Expectations &e, std::string &why) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		char buf[256];
		if (decoded_frames_ < static_cast<uint64_t>(e.min_video_frames)) {
			snprintf(buf, sizeof(buf), "decoded %llu video frames, wanted %d", (unsigned long long)decoded_frames_,
				 e.min_video_frames);
			why = buf;
			return false;
		}
		const double audio_seconds = decoded_audio_samples_ / (audio_rate_ ? audio_rate_ : 48000.0);
		if (audio_seconds < e.min_audio_seconds) {
			snprintf(buf, sizeof(buf), "decoded %.2fs of audio, wanted %.2fs", audio_seconds, e.min_audio_seconds);
			why = buf;
			return false;
		}
		if (e.width && (width_ != static_cast<uint32_t>(e.width) || height_ != static_cast<uint32_t>(e.height))) {
			snprintf(buf, sizeof(buf), "picture is %ux%u, wanted %dx%d", width_, height_, e.width, e.height);
			why = buf;
			return false;
		}
		if (e.check_luma && (std::fabs(top_luma_ - e.top_luma) > e.luma_tolerance ||
				     std::fabs(bottom_luma_ - e.bottom_luma) > e.luma_tolerance)) {
			snprintf(buf, sizeof(buf), "luma top/bottom is %.1f/%.1f, wanted %.0f/%.0f +-%.0f", top_luma_,
				 bottom_luma_, e.top_luma, e.bottom_luma, e.luma_tolerance);
			why = buf;
			return false;
		}
		if (e.min_audio_rms > 0) {
			const double rms = audio_samples_seen_ ? std::sqrt(audio_square_sum_ / audio_samples_seen_) : 0;
			if (rms < e.min_audio_rms || rms > e.max_audio_rms) {
				snprintf(buf, sizeof(buf), "audio RMS is %.3f, wanted %.3f..%.3f", rms, e.min_audio_rms,
					 e.max_audio_rms);
				why = buf;
				return false;
			}
		}
		return true;
	}

	std::string summary() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const double rms = audio_samples_seen_ ? std::sqrt(audio_square_sum_ / audio_samples_seen_) : 0;
		char buf[256];
		snprintf(buf, sizeof(buf), "%s %ux%u (format %d), luma top/bottom %.1f/%.1f, %llu frames; audio %.2fs RMS %.3f",
			 rtmp::codec_name(video_codec_), width_, height_, (int)pixel_format_, top_luma_, bottom_luma_,
			 (unsigned long long)decoded_frames_, decoded_audio_samples_ / (audio_rate_ ? audio_rate_ : 48000.0),
			 rms);
		return buf;
	}

	void dump_last_frame(const std::string &path) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (last_luma_.empty())
			return;
		if (FILE *f = fopen(path.c_str(), "wb")) {
			fprintf(f, "P5\n%u %u\n255\n", width_, height_);
			fwrite(last_luma_.data(), 1, last_luma_.size(), f);
			fclose(f);
			printf("wrote %s\n", path.c_str());
		}
	}

private:
	void analyze_video(const rtmpsrc::DecodedVideo &v)
	{
		decoded_frames_++;
		width_ = v.width;
		height_ = v.height;
		pixel_format_ = v.format;
		if (v.format != VIDEO_FORMAT_I420 && v.format != VIDEO_FORMAT_NV12 && v.format != VIDEO_FORMAT_I422 &&
		    v.format != VIDEO_FORMAT_I444)
			return;

		// Average the 8-bit luma plane over the top and bottom halves.
		double top = 0, bottom = 0;
		last_luma_.resize(static_cast<size_t>(v.width) * v.height);
		for (uint32_t y = 0; y < v.height; y++) {
			const uint8_t *row = v.data[0] + static_cast<size_t>(y) * v.linesize[0];
			uint64_t sum = 0;
			for (uint32_t x = 0; x < v.width; x++)
				sum += row[x];
			(y < v.height / 2 ? top : bottom) += static_cast<double>(sum);
			std::memcpy(&last_luma_[static_cast<size_t>(y) * v.width], row, v.width);
		}
		const double half = static_cast<double>(v.width) * (v.height / 2);
		top_luma_ = top / half;
		bottom_luma_ = bottom / half;
	}

	void analyze_audio(const rtmpsrc::DecodedAudio &a)
	{
		decoded_audio_samples_ += a.frames;
		audio_rate_ = a.sample_rate;
		for (uint32_t i = 0; i < a.frames; i++) {
			audio_square_sum_ += static_cast<double>(a.planes[0][i]) * a.planes[0][i];
			audio_samples_seen_++;
		}
	}

	const bool decode_;
	mutable std::mutex mutex_;
	rtmpsrc::VideoDecoder video_decoder_;
	rtmpsrc::AudioDecoder audio_decoder_;
	bool waiting_for_keyframe_ = true;

	rtmp::VideoCodec video_codec_ = rtmp::VideoCodec::Unknown;
	rtmp::AudioCodec audio_codec_ = rtmp::AudioCodec::Unknown;
	uint64_t video_packets_ = 0, audio_packets_ = 0, keyframes_ = 0;
	uint64_t video_bytes_ = 0, audio_bytes_ = 0, last_video_bytes_ = 0, last_audio_bytes_ = 0;
	int64_t last_dts_ = 0;
	bool ever_connected_ = false;
	bool publisher_gone_ = false;

	uint64_t decoded_frames_ = 0;
	uint32_t width_ = 0, height_ = 0;
	video_format pixel_format_ = VIDEO_FORMAT_NONE;
	double top_luma_ = 0, bottom_luma_ = 0;
	std::vector<uint8_t> last_luma_;

	uint64_t decoded_audio_samples_ = 0;
	uint32_t audio_rate_ = 0;
	double audio_square_sum_ = 0;
	uint64_t audio_samples_seen_ = 0;
};

} // namespace

int main(int argc, char **argv)
{
	rtmp::Hub::RouteSpec spec;
	spec.key = "stream";
	bool decode = false;
	bool selftest = false;
	int timeout_s = 30;
	std::string dump_path;
	Expectations expect;

	for (int i = 1; i < argc; i++) {
		auto value = [&]() -> const char * {
			if (i + 1 >= argc) {
				fprintf(stderr, "missing value for %s\n", argv[i]);
				exit(2);
			}
			return argv[++i];
		};
		const std::string a = argv[i];
		if (a == "--bind")
			spec.bind_address = value();
		else if (a == "--port")
			spec.port = static_cast<uint16_t>(atoi(value()));
		else if (a == "--app")
			spec.app = value();
		else if (a == "--key")
			spec.key = value();
		else if (a == "--decode")
			decode = true;
		else if (a == "--selftest")
			selftest = decode = true;
		else if (a == "--timeout")
			timeout_s = atoi(value());
		else if (a == "--dump-pgm")
			dump_path = value();
		else if (a == "--min-video-frames")
			expect.min_video_frames = atoi(value());
		else if (a == "--min-audio-seconds")
			expect.min_audio_seconds = atof(value());
		else if (a == "--expect-size") {
			if (sscanf(value(), "%dx%d", &expect.width, &expect.height) != 2)
				return 2;
		} else if (a == "--expect-luma") {
			if (sscanf(value(), "%lf,%lf", &expect.top_luma, &expect.bottom_luma) != 2)
				return 2;
			expect.check_luma = true;
		} else if (a == "--expect-audio-rms") {
			if (sscanf(value(), "%lf,%lf", &expect.min_audio_rms, &expect.max_audio_rms) != 2)
				return 2;
		} else {
			fprintf(stderr,
				"usage: %s [--bind ADDR] [--port N] [--app NAME] [--key KEY] [--decode] [--dump-pgm FILE]\n"
				"          [--selftest --timeout S --min-video-frames N --min-audio-seconds S\n"
				"           --expect-size WxH --expect-luma TOP,BOTTOM --expect-audio-rms MIN,MAX]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	auto sink = std::make_shared<ProbeSink>(decode);
	std::string error;
	auto registration = rtmp::Hub::instance().add_route(spec, sink, error);
	if (!registration) {
		fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}

	printf("waiting for a publisher at rtmp://%s:%u/%s/%s\n", spec.bind_address.c_str(), spec.port,
	       spec.app.c_str(), spec.key.c_str());
	fflush(stdout);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
	std::string why = "no publisher connected";
	int exit_code = 0;
	while (!g_stop) {
		std::this_thread::sleep_for(std::chrono::milliseconds(selftest ? 250 : 1000));
		static int ticks = 0;
		if (!selftest || ++ticks % 4 == 0)
			sink->print_stats();
		fflush(stdout);

		if (selftest) {
			if (sink->ever_connected() && sink->check(expect, why)) {
				printf("SELFTEST OK: %s\n", sink->summary().c_str());
				break;
			}
			if (std::chrono::steady_clock::now() > deadline) {
				printf("SELFTEST FAILED: %s\n", why.c_str());
				printf("  last state: %s\n", sink->summary().c_str());
				exit_code = 1;
				break;
			}
		}
	}
	if (!dump_path.empty())
		sink->dump_last_frame(dump_path);
	printf("shutting down\n");
	return exit_code;
}
