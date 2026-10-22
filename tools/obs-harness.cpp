// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
// Loads the built plugin into a real libobs (the one from an installed OBS),
// creates an "RTMP Server" source and checks what OBS itself ends up with:
// the async video frame the source exposes and the audio it delivers.
//
// It does not start the OBS app or touch its configuration. Run it against a
// publisher such as test-publisher (see tests/e2e.sh).
//
//   obs-harness --plugin <path>/obs-rtmp-server.plugin --port 19360 --key harness \
//               --expect-size 640x360 --expect-luma 200,50 --min-audio-seconds 2 [--passes 2]
//
// With --passes N the check must succeed N times in a row, each time with a
// new publisher: after a pass the harness waits for the picture to stop (the
// caller kills the publisher) and then for a fresh stream to arrive.

#include <obs.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct AudioProbe {
	std::mutex mutex;
	uint64_t frames = 0;
	double square_sum = 0;
};

void on_audio(void *param, obs_source_t *, const struct audio_data *data, bool muted)
{
	auto *probe = static_cast<AudioProbe *>(param);
	if (muted || !data->data[0])
		return;
	std::lock_guard<std::mutex> lock(probe->mutex);
	const float *samples = reinterpret_cast<const float *>(data->data[0]);
	for (uint32_t i = 0; i < data->frames; i++)
		probe->square_sum += static_cast<double>(samples[i]) * samples[i];
	probe->frames += data->frames;
}

void log_handler(int level, const char *format, va_list args, void *)
{
	if (level > LOG_INFO)
		return;
	char buf[2048];
	vsnprintf(buf, sizeof(buf), format, args);
	printf("[obs] %s\n", buf);
}

} // namespace

int main(int argc, char **argv)
{
	std::string plugin;
	std::string key = "harness";
	int port = 19360;
	int timeout_s = 30;
	int expect_w = 0, expect_h = 0;
	double expect_top = -1, expect_bottom = -1;
	double min_audio_seconds = 0;
	int passes = 1;
	const char *graphics = "libobs-metal.dylib";

	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		auto value = [&]() -> const char * {
			if (i + 1 >= argc)
				exit(2);
			return argv[++i];
		};
		if (a == "--plugin")
			plugin = value();
		else if (a == "--port")
			port = atoi(value());
		else if (a == "--key")
			key = value();
		else if (a == "--timeout")
			timeout_s = atoi(value());
		else if (a == "--graphics")
			graphics = value();
		else if (a == "--expect-size")
			sscanf(value(), "%dx%d", &expect_w, &expect_h);
		else if (a == "--expect-luma")
			sscanf(value(), "%lf,%lf", &expect_top, &expect_bottom);
		else if (a == "--min-audio-seconds")
			min_audio_seconds = atof(value());
		else if (a == "--passes")
			passes = atoi(value());
		else {
			fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}
	if (plugin.empty()) {
		fprintf(stderr, "usage: %s --plugin <bundle.plugin> [--port N] [--key K] [--graphics libobs-metal.dylib|libobs-opengl.dylib]\n"
				"          [--expect-size WxH] [--expect-luma TOP,BOTTOM] [--min-audio-seconds S] [--timeout S]\n",
			argv[0]);
		return 2;
	}

	base_set_log_handler(log_handler, nullptr);
	if (!obs_startup("en-US", nullptr, nullptr)) {
		printf("HARNESS FAILED: obs_startup\n");
		return 1;
	}
	printf("libobs %s\n", obs_get_version_string());

	obs_audio_info audio_info = {};
	audio_info.samples_per_sec = 48000;
	audio_info.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&audio_info)) {
		printf("HARNESS FAILED: obs_reset_audio\n");
		return 1;
	}

	obs_video_info video_info = {};
	video_info.graphics_module = graphics;
	video_info.fps_num = 30;
	video_info.fps_den = 1;
	video_info.base_width = video_info.output_width = 1280;
	video_info.base_height = video_info.output_height = 720;
	video_info.output_format = VIDEO_FORMAT_NV12;
	video_info.gpu_conversion = true;
	video_info.colorspace = VIDEO_CS_709;
	video_info.range = VIDEO_RANGE_PARTIAL;
	video_info.scale_type = OBS_SCALE_BICUBIC;
	int video_result = obs_reset_video(&video_info);
	if (video_result != OBS_VIDEO_SUCCESS) {
		printf("HARNESS FAILED: obs_reset_video(%s) returned %d\n", graphics, video_result);
		return 1;
	}

	obs_module_t *module = nullptr;
	const std::string binary = plugin + "/Contents/MacOS/obs-rtmp-server";
	const std::string data = plugin + "/Contents/Resources";
	int open_result = obs_open_module(&module, binary.c_str(), data.c_str());
	if (open_result != MODULE_SUCCESS || !obs_init_module(module)) {
		printf("HARNESS FAILED: cannot load plugin %s (result %d)\n", binary.c_str(), open_result);
		return 1;
	}
	obs_post_load_modules();

	const char *id = "rtmp_server_source";
	uint32_t flags = obs_get_source_output_flags(id);
	printf("source '%s' registered: flags 0x%x (video %d, audio %d, async %d)\n", id, flags,
	       !!(flags & OBS_SOURCE_VIDEO), !!(flags & OBS_SOURCE_AUDIO), !!(flags & OBS_SOURCE_ASYNC));
	if (!flags) {
		printf("HARNESS FAILED: source type not registered\n");
		return 1;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "port", port);
	obs_data_set_string(settings, "stream_key", key.c_str());
	obs_source_t *source = obs_source_create(id, "harness", settings, nullptr);
	obs_data_release(settings);
	if (!source) {
		printf("HARNESS FAILED: obs_source_create\n");
		return 1;
	}

	AudioProbe audio;
	obs_source_add_audio_capture_callback(source, on_audio, &audio);

	// What the properties dialog would show.
	obs_properties_t *props = obs_source_properties(source);
	printf("properties: ");
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
		printf("%s ", obs_property_name(p));
	printf("\n");
	obs_properties_destroy(props);

	printf("waiting for a publisher on port %d, key '%s'\n", port, key.c_str());
	fflush(stdout);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
	std::string why = "no picture arrived";
	bool ok = false;
	uint32_t width = 0, height = 0;
	double top = 0, bottom = 0, audio_seconds = 0, rms = 0;
	int format = 0;

	// Per-pass bookkeeping: only pictures and audio that arrived since the
	// pass started count.
	int pass = 1;
	bool waiting_for_gap = false;
	uint64_t last_timestamp = 0;
	int frames_this_pass = 0;
	uint64_t audio_baseline_frames = 0;
	double audio_square_baseline = 0;
	auto last_new_frame = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		// The video thread moves async frames into place on its tick.
		if (obs_source_frame *frame = obs_source_get_frame(source)) {
			if (frame->timestamp != last_timestamp) {
				last_timestamp = frame->timestamp;
				last_new_frame = std::chrono::steady_clock::now();
				if (!waiting_for_gap)
					frames_this_pass++;
			}
			width = frame->width;
			height = frame->height;
			format = frame->format;
			if (frame->format == VIDEO_FORMAT_I420 || frame->format == VIDEO_FORMAT_NV12) {
				double t = 0, b = 0;
				for (uint32_t y = 0; y < height; y++) {
					const uint8_t *row = frame->data[0] + static_cast<size_t>(y) * frame->linesize[0];
					uint64_t sum = 0;
					for (uint32_t x = 0; x < width; x++)
						sum += row[x];
					(y < height / 2 ? t : b) += static_cast<double>(sum);
				}
				const double half = static_cast<double>(width) * (height / 2);
				top = t / half;
				bottom = b / half;
			}
			obs_source_release_frame(source, frame);
		}
		{
			std::lock_guard<std::mutex> lock(audio.mutex);
			const uint64_t frames = audio.frames - audio_baseline_frames;
			audio_seconds = frames / 48000.0;
			rms = frames ? std::sqrt((audio.square_sum - audio_square_baseline) / frames) : 0;
		}

		if (waiting_for_gap) {
			// Pass finished; wait for the old publisher to go quiet.
			auto quiet = std::chrono::steady_clock::now() - last_new_frame;
			if (quiet < std::chrono::milliseconds(1500))
				continue;
			waiting_for_gap = false;
			frames_this_pass = 0;
			width = height = 0;
			top = bottom = 0;
			{
				std::lock_guard<std::mutex> lock(audio.mutex);
				audio_baseline_frames = audio.frames;
				audio_square_baseline = audio.square_sum;
			}
			printf("pass %d: waiting for a new publisher\n", pass);
			fflush(stdout);
			continue;
		}

		char buf[200];
		if (!width || frames_this_pass < 3) {
			why = "no picture arrived";
		} else if (expect_w && (static_cast<int>(width) != expect_w || static_cast<int>(height) != expect_h)) {
			snprintf(buf, sizeof(buf), "picture is %ux%u, wanted %dx%d", width, height, expect_w, expect_h);
			why = buf;
		} else if (expect_top >= 0 && (std::fabs(top - expect_top) > 12 || std::fabs(bottom - expect_bottom) > 12)) {
			snprintf(buf, sizeof(buf), "luma top/bottom is %.1f/%.1f, wanted %.0f/%.0f", top, bottom, expect_top,
				 expect_bottom);
			why = buf;
		} else if (audio_seconds < min_audio_seconds) {
			snprintf(buf, sizeof(buf), "only %.2fs of audio reached OBS, wanted %.2fs", audio_seconds,
				 min_audio_seconds);
			why = buf;
		} else {
			printf("pass %d ok: %ux%u luma %.1f/%.1f, %.2fs of audio (RMS %.3f)\n", pass, width, height, top, bottom,
			       audio_seconds, rms);
			fflush(stdout);
			if (pass >= passes) {
				ok = true;
				break;
			}
			pass++;
			waiting_for_gap = true;
		}
	}

	printf("OBS sees: %ux%u (format %d), luma top/bottom %.1f/%.1f, %.2fs of audio (RMS %.3f), %d pass(es)\n", width,
	       height, format, top, bottom, audio_seconds, rms, ok ? pass : pass - 1);

	// Tear down the way OBS does: removing the source must not hang or crash.
	auto t0 = std::chrono::steady_clock::now();
	obs_source_remove(source);
	obs_source_release(source);
	auto destroy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	printf("source destroyed in %lld ms\n", static_cast<long long>(destroy_ms));
	obs_shutdown();

	if (ok)
		printf("HARNESS OK\n");
	else
		printf("HARNESS FAILED: %s\n", why.c_str());
	return ok ? 0 : 1;
}
