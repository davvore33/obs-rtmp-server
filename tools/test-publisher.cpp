// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
// A tiny RTMP publisher for testing, built on the FFmpeg libraries OBS ships,
// so no ffmpeg binary needs to be installed. It sends a synthetic picture
// with a known layout (bright top half, dark bottom half, a moving bar) and a
// 440 Hz tone, paced in real time.
//
//   test-publisher rtmp://127.0.0.1:1935/live/stream [--seconds 5] [--size 640x360]
//                  [--fps 30] [--codec h264|hevc] [--bframes 2] [--no-audio] [--no-video]

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr int kTopLuma = 200;
constexpr int kBottomLuma = 50;
constexpr int kSampleRate = 48000;

struct Options {
	std::string url;
	double seconds = 5;
	int width = 640;
	int height = 360;
	int fps = 30;
	std::string codec = "h264";
	int bframes = 0;
	bool audio = true;
	bool video = true;
};

[[noreturn]] void die(const char *what, int err = 0)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = "";
	if (err)
		av_strerror(err, buf, sizeof(buf));
	fprintf(stderr, "test-publisher: %s%s%s\n", what, err ? ": " : "", buf);
	exit(1);
}

void fill_picture(AVFrame *f, int index)
{
	const int bar_x = (index * 8) % (f->width - 16);
	for (int y = 0; y < f->height; y++) {
		uint8_t *row = f->data[0] + y * f->linesize[0];
		memset(row, y < f->height / 2 ? kTopLuma : kBottomLuma, static_cast<size_t>(f->width));
		memset(row + bar_x, 235, 16);
	}
	for (int y = 0; y < f->height / 2; y++) {
		memset(f->data[1] + y * f->linesize[1], 128, static_cast<size_t>(f->width / 2));
		memset(f->data[2] + y * f->linesize[2], 128, static_cast<size_t>(f->width / 2));
	}
}

} // namespace

int main(int argc, char **argv)
{
	Options opt;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char * {
			if (i + 1 >= argc)
				die("missing value for option");
			return argv[++i];
		};
		if (a == "--seconds")
			opt.seconds = atof(next());
		else if (a == "--size") {
			if (sscanf(next(), "%dx%d", &opt.width, &opt.height) != 2)
				die("--size must look like 640x360");
		} else if (a == "--fps")
			opt.fps = atoi(next());
		else if (a == "--codec")
			opt.codec = next();
		else if (a == "--bframes")
			opt.bframes = atoi(next());
		else if (a == "--no-audio")
			opt.audio = false;
		else if (a == "--no-video")
			opt.video = false;
		else if (a[0] != '-')
			opt.url = a;
		else
			die("unknown option");
	}
	if (opt.url.empty()) {
		fprintf(stderr, "usage: %s <rtmp-url> [--seconds N] [--size WxH] [--fps N] [--codec h264|hevc] "
				"[--bframes N] [--no-audio] [--no-video]\n",
			argv[0]);
		return 2;
	}
	opt.width &= ~1;
	opt.height &= ~1;

	AVFormatContext *oc = nullptr;
	int ret = avformat_alloc_output_context2(&oc, nullptr, "flv", opt.url.c_str());
	if (ret < 0)
		die("cannot create FLV muxer", ret);

	AVCodecContext *venc = nullptr;
	AVStream *vstream = nullptr;
	AVFrame *vframe = nullptr;
	if (opt.video) {
		const char *name = opt.codec == "hevc" ? "hevc_videotoolbox" : "libx264";
		const AVCodec *codec = avcodec_find_encoder_by_name(name);
		if (!codec)
			die("video encoder not available in this FFmpeg build");
		venc = avcodec_alloc_context3(codec);
		venc->width = opt.width;
		venc->height = opt.height;
		venc->time_base = {1, opt.fps};
		venc->framerate = {opt.fps, 1};
		venc->pix_fmt = AV_PIX_FMT_YUV420P;
		venc->gop_size = opt.fps; // a keyframe every second
		venc->max_b_frames = opt.bframes;
		venc->bit_rate = 1500000;
		if (opt.codec != "hevc") {
			av_opt_set(venc->priv_data, "preset", "ultrafast", 0);
			if (opt.bframes == 0)
				av_opt_set(venc->priv_data, "tune", "zerolatency", 0);
		}
		if (oc->oformat->flags & AVFMT_GLOBALHEADER)
			venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		ret = avcodec_open2(venc, codec, nullptr);
		if (ret < 0)
			die("cannot open video encoder", ret);
		vstream = avformat_new_stream(oc, nullptr);
		avcodec_parameters_from_context(vstream->codecpar, venc);
		vstream->time_base = venc->time_base;

		vframe = av_frame_alloc();
		vframe->format = venc->pix_fmt;
		vframe->width = venc->width;
		vframe->height = venc->height;
		av_frame_get_buffer(vframe, 0);
	}

	AVCodecContext *aenc = nullptr;
	AVStream *astream = nullptr;
	AVFrame *aframe = nullptr;
	if (opt.audio) {
		const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
		if (!codec)
			die("AAC encoder not available in this FFmpeg build");
		aenc = avcodec_alloc_context3(codec);
		aenc->sample_rate = kSampleRate;
		aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
		av_channel_layout_default(&aenc->ch_layout, 2);
		aenc->bit_rate = 128000;
		aenc->time_base = {1, kSampleRate};
		if (oc->oformat->flags & AVFMT_GLOBALHEADER)
			aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		ret = avcodec_open2(aenc, codec, nullptr);
		if (ret < 0)
			die("cannot open AAC encoder", ret);
		astream = avformat_new_stream(oc, nullptr);
		avcodec_parameters_from_context(astream->codecpar, aenc);
		astream->time_base = {1, kSampleRate};

		aframe = av_frame_alloc();
		aframe->format = aenc->sample_fmt;
		aframe->sample_rate = aenc->sample_rate;
		aframe->nb_samples = aenc->frame_size;
		av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout);
		av_frame_get_buffer(aframe, 0);
	}

	ret = avio_open(&oc->pb, opt.url.c_str(), AVIO_FLAG_WRITE);
	if (ret < 0)
		die("cannot connect (is the server running?)", ret);
	ret = avformat_write_header(oc, nullptr);
	if (ret < 0)
		die("cannot write stream header", ret);

	AVPacket *pkt = av_packet_alloc();
	auto write_packets = [&](AVCodecContext *enc, AVStream *st) {
		for (;;) {
			int r = avcodec_receive_packet(enc, pkt);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				return;
			if (r < 0)
				die("encoder failed", r);
			av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
			pkt->stream_index = st->index;
			r = av_interleaved_write_frame(oc, pkt);
			if (r < 0)
				die("lost connection to the server", r);
		}
	};

	const auto start = std::chrono::steady_clock::now();
	auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };

	int64_t video_index = 0;
	int64_t audio_samples = 0;
	double phase = 0;
	printf("publishing to %s for %.1fs (%dx%d %s%s)\n", opt.url.c_str(), opt.seconds, opt.width, opt.height,
	       opt.video ? opt.codec.c_str() : "no video", opt.audio ? " + AAC" : "");
	fflush(stdout);

	// Produce whichever stream is behind, paced to wall-clock time.
	while (elapsed() < opt.seconds) {
		const double next_video = opt.video ? static_cast<double>(video_index) / opt.fps : 1e9;
		const double next_audio = opt.audio ? static_cast<double>(audio_samples) / kSampleRate : 1e9;
		const double due = std::min(next_video, next_audio);
		const double wait = due - elapsed();
		if (wait > 0)
			std::this_thread::sleep_for(std::chrono::duration<double>(wait));

		if (next_video <= next_audio) {
			av_frame_make_writable(vframe);
			fill_picture(vframe, static_cast<int>(video_index));
			vframe->pts = video_index++;
			avcodec_send_frame(venc, vframe);
			write_packets(venc, vstream);
		} else {
			av_frame_make_writable(aframe);
			for (int n = 0; n < aframe->nb_samples; n++) {
				float v = static_cast<float>(0.5 * std::sin(phase));
				phase += 2 * M_PI * 440.0 / kSampleRate;
				reinterpret_cast<float *>(aframe->data[0])[n] = v;
				reinterpret_cast<float *>(aframe->data[1])[n] = v;
			}
			aframe->pts = audio_samples;
			audio_samples += aframe->nb_samples;
			avcodec_send_frame(aenc, aframe);
			write_packets(aenc, astream);
		}
	}

	if (venc) {
		avcodec_send_frame(venc, nullptr);
		write_packets(venc, vstream);
	}
	if (aenc) {
		avcodec_send_frame(aenc, nullptr);
		write_packets(aenc, astream);
	}
	av_write_trailer(oc);
	avio_closep(&oc->pb);
	printf("done: %lld video frames, %lld audio samples\n", static_cast<long long>(video_index),
	       static_cast<long long>(audio_samples));
	return 0;
}
