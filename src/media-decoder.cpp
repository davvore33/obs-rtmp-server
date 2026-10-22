// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "media-decoder.h"

#include <plugin-support.h>
#include <util/base.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace rtmpsrc {

namespace {

std::string av_error_string(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(code, buf, sizeof(buf));
	return buf;
}

// Decoders give up after this many bad packets in a row; the caller reopens at
// the next keyframe.
constexpr int kMaxConsecutiveErrors = 30;

bool same_config(const std::vector<uint8_t> &have, const uint8_t *config, size_t size)
{
	return have.size() == size && (size == 0 || std::memcmp(have.data(), config, size) == 0);
}

bool set_extradata(AVCodecContext *ctx, const uint8_t *config, size_t size)
{
	if (size == 0)
		return true;
	ctx->extradata = static_cast<uint8_t *>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
	if (!ctx->extradata)
		return false;
	std::memcpy(ctx->extradata, config, size);
	ctx->extradata_size = static_cast<int>(size);
	return true;
}

const AVCodec *find_video_decoder(rtmp::VideoCodec codec)
{
	switch (codec) {
	case rtmp::VideoCodec::H264:
		return avcodec_find_decoder(AV_CODEC_ID_H264);
	case rtmp::VideoCodec::HEVC:
		return avcodec_find_decoder(AV_CODEC_ID_HEVC);
	case rtmp::VideoCodec::AV1:
		// The native AV1 decoder only does hardware decoding: prefer a software
		// library (OBS's FFmpeg ships libaom; other builds may have dav1d).
		for (const char *name : {"libdav1d", "libaom-av1"}) {
			if (const AVCodec *software = avcodec_find_decoder_by_name(name))
				return software;
		}
		return avcodec_find_decoder(AV_CODEC_ID_AV1);
	case rtmp::VideoCodec::VP9:
		return avcodec_find_decoder(AV_CODEC_ID_VP9);
	default:
		return nullptr;
	}
}

// OBS pixel format for what the decoder produced, or NONE if it needs converting.
// `full_range` is set for the deprecated "J" formats that imply full range.
video_format obs_format_for(int pix_fmt, bool &full_range)
{
	full_range = false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	switch (pix_fmt) {
	case AV_PIX_FMT_YUVJ420P:
		full_range = true;
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUVJ422P:
		full_range = true;
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUVJ444P:
		full_range = true;
		return VIDEO_FORMAT_I444;
	default:
		break;
	}
#pragma clang diagnostic pop

	switch (pix_fmt) {
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUV444P12LE:
		return VIDEO_FORMAT_I412;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_BGR0:
		return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

// What to convert an unsupported format into.
AVPixelFormat conversion_target(int pix_fmt)
{
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
	if (!desc)
		return AV_PIX_FMT_YUV420P;
	if ((desc->flags & AV_PIX_FMT_FLAG_RGB) || (desc->flags & AV_PIX_FMT_FLAG_ALPHA))
		return AV_PIX_FMT_BGRA;
	if (desc->comp[0].depth > 8)
		return AV_PIX_FMT_YUV420P10LE;
	return AV_PIX_FMT_YUV420P;
}

bool is_yuv(video_format format)
{
	return format != VIDEO_FORMAT_BGRA && format != VIDEO_FORMAT_BGRX && format != VIDEO_FORMAT_RGBA;
}

video_trc trc_for(int av_trc)
{
	switch (av_trc) {
	case AVCOL_TRC_SMPTE2084:
		return VIDEO_TRC_PQ;
	case AVCOL_TRC_ARIB_STD_B67:
		return VIDEO_TRC_HLG;
	case AVCOL_TRC_IEC61966_2_1:
		return VIDEO_TRC_SRGB;
	default:
		return VIDEO_TRC_DEFAULT;
	}
}

video_colorspace colorspace_for(const AVFrame &frame, video_trc trc)
{
	switch (frame.colorspace) {
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		if (trc == VIDEO_TRC_PQ)
			return VIDEO_CS_2100_PQ;
		if (trc == VIDEO_TRC_HLG)
			return VIDEO_CS_2100_HLG;
		return VIDEO_CS_709;
	default:
		// Unspecified: follow the usual convention of HD = 709, SD = 601.
		return frame.height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
	}
}

// OBS speaker layouts whose channel order matches FFmpeg's default layout.
speaker_layout speakers_for(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 4:
		return SPEAKERS_4POINT0;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

} // namespace

// ---------------------------------------------------------------- video

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder()
{
	close();
}

void VideoDecoder::close()
{
	if (sws_) {
		sws_freeContext(sws_);
		sws_ = nullptr;
	}
	av_frame_free(&converted_);
	av_frame_free(&frame_);
	av_packet_free(&packet_);
	avcodec_free_context(&ctx_);
	codec_ = rtmp::VideoCodec::Unknown;
	config_.clear();
	consecutive_errors_ = 0;
}

bool VideoDecoder::matches(rtmp::VideoCodec codec, const uint8_t *config, size_t config_size) const
{
	return ctx_ && codec == codec_ && same_config(config_, config, config_size);
}

bool VideoDecoder::open(rtmp::VideoCodec codec, const uint8_t *config, size_t config_size, std::string &error)
{
	close();

	const AVCodec *decoder = find_video_decoder(codec);
	if (!decoder) {
		error = std::string("this build of FFmpeg has no ") + rtmp::codec_name(codec) + " decoder";
		return false;
	}

	ctx_ = avcodec_alloc_context3(decoder);
	packet_ = av_packet_alloc();
	frame_ = av_frame_alloc();
	converted_ = av_frame_alloc();
	if (!ctx_ || !packet_ || !frame_ || !converted_ || !set_extradata(ctx_, config, config_size)) {
		error = "out of memory";
		close();
		return false;
	}

	ctx_->pkt_timebase = {1, 1000};
	// Slice threading only: frame threading would add a frame of delay per thread.
	ctx_->thread_count = 0;
	ctx_->thread_type = FF_THREAD_SLICE;

	int ret = avcodec_open2(ctx_, decoder, nullptr);
	if (ret < 0) {
		error = std::string("cannot open ") + decoder->name + ": " + av_error_string(ret);
		close();
		return false;
	}

	codec_ = codec;
	config_.assign(config, config + config_size);
	obs_log(LOG_INFO, "video decoder: %s (%s)", rtmp::codec_name(codec), decoder->name);
	return true;
}

bool VideoDecoder::decode(const rtmp::VideoPacket &p, const FrameCallback &on_frame)
{
	if (!ctx_)
		return false;

	packet_->data = const_cast<uint8_t *>(p.data);
	packet_->size = static_cast<int>(p.size);
	packet_->dts = p.dts_ms;
	packet_->pts = p.dts_ms + p.cts_ms;
	packet_->flags = p.keyframe ? AV_PKT_FLAG_KEY : 0;

	int ret = avcodec_send_packet(ctx_, packet_);
	av_packet_unref(packet_);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		if (++consecutive_errors_ >= kMaxConsecutiveErrors)
			return false;
		obs_log(LOG_DEBUG, "video decode error: %s", av_error_string(ret).c_str());
		return true;
	}
	consecutive_errors_ = 0;

	for (;;) {
		ret = avcodec_receive_frame(ctx_, frame_);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			obs_log(LOG_DEBUG, "video decode error: %s", av_error_string(ret).c_str());
			break;
		}
		emit(*frame_, on_frame);
		av_frame_unref(frame_);
	}
	return true;
}

void VideoDecoder::emit(const AVFrame &frame, const FrameCallback &on_frame)
{
	const AVFrame *source = &frame;
	bool implied_full_range = false;
	video_format format = obs_format_for(frame.format, implied_full_range);

	if (format == VIDEO_FORMAT_NONE) {
		const AVPixelFormat target = conversion_target(frame.format);
		if (converted_->width != frame.width || converted_->height != frame.height ||
		    converted_->format != target) {
			av_frame_unref(converted_);
			converted_->width = frame.width;
			converted_->height = frame.height;
			converted_->format = target;
			if (av_frame_get_buffer(converted_, 32) < 0)
				return;
		}
		sws_ = sws_getCachedContext(sws_, frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
					    frame.width, frame.height, target, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws_)
			return;
		if (av_frame_make_writable(converted_) < 0)
			return;
		sws_scale(sws_, frame.data, frame.linesize, 0, frame.height, converted_->data, converted_->linesize);
		converted_->colorspace = frame.colorspace;
		converted_->color_range = frame.color_range;
		converted_->color_trc = frame.color_trc;
		source = converted_;
		format = obs_format_for(target, implied_full_range);
		if (format == VIDEO_FORMAT_NONE)
			return;
	}

	DecodedVideo out;
	for (int i = 0; i < AV_NUM_DATA_POINTERS && i < MAX_AV_PLANES; i++) {
		out.data[i] = source->data[i];
		out.linesize[i] = static_cast<uint32_t>(source->linesize[i]);
	}
	out.width = static_cast<uint32_t>(source->width);
	out.height = static_cast<uint32_t>(source->height);
	out.format = format;
	out.trc = trc_for(source->color_trc);
	out.colorspace = colorspace_for(*source, out.trc);
	out.range = (implied_full_range || source->color_range == AVCOL_RANGE_JPEG || !is_yuv(format))
			    ? VIDEO_RANGE_FULL
			    : VIDEO_RANGE_PARTIAL;
	out.pts_ms = frame.pts != AV_NOPTS_VALUE ? frame.pts : frame.best_effort_timestamp;

	on_frame(out);
}

// ---------------------------------------------------------------- audio

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder()
{
	close();
}

void AudioDecoder::close()
{
	swr_free(&swr_);
	swr_in_rate_ = 0;
	swr_in_format_ = -1;
	swr_in_channels_ = 0;
	av_frame_free(&frame_);
	av_packet_free(&packet_);
	avcodec_free_context(&ctx_);
	codec_ = rtmp::AudioCodec::Unknown;
	config_.clear();
	consecutive_errors_ = 0;
}

bool AudioDecoder::matches(rtmp::AudioCodec codec, const uint8_t *config, size_t config_size) const
{
	return ctx_ && codec == codec_ && same_config(config_, config, config_size);
}

bool AudioDecoder::open(rtmp::AudioCodec codec, const uint8_t *config, size_t config_size,
			uint32_t sample_rate_hint, int channels_hint, std::string &error)
{
	close();

	AVCodecID id = AV_CODEC_ID_NONE;
	switch (codec) {
	case rtmp::AudioCodec::AAC:
		id = AV_CODEC_ID_AAC;
		break;
	case rtmp::AudioCodec::MP3:
		id = AV_CODEC_ID_MP3;
		break;
	case rtmp::AudioCodec::Opus:
		id = AV_CODEC_ID_OPUS;
		break;
	default:
		break;
	}

	const AVCodec *decoder = id == AV_CODEC_ID_NONE ? nullptr : avcodec_find_decoder(id);
	if (!decoder) {
		error = std::string("this build of FFmpeg has no ") + rtmp::codec_name(codec) + " decoder";
		return false;
	}

	ctx_ = avcodec_alloc_context3(decoder);
	packet_ = av_packet_alloc();
	frame_ = av_frame_alloc();
	if (!ctx_ || !packet_ || !frame_ || !set_extradata(ctx_, config, config_size)) {
		error = "out of memory";
		close();
		return false;
	}

	ctx_->pkt_timebase = {1, 1000};
	if (codec == rtmp::AudioCodec::MP3) {
		// MP3 has no out-of-band config; the tag header is the only hint.
		if (sample_rate_hint)
			ctx_->sample_rate = static_cast<int>(sample_rate_hint);
		if (channels_hint > 0)
			av_channel_layout_default(&ctx_->ch_layout, channels_hint);
	}

	int ret = avcodec_open2(ctx_, decoder, nullptr);
	if (ret < 0) {
		error = std::string("cannot open ") + decoder->name + ": " + av_error_string(ret);
		close();
		return false;
	}

	codec_ = codec;
	config_.assign(config, config + config_size);
	obs_log(LOG_INFO, "audio decoder: %s (%s)", rtmp::codec_name(codec), decoder->name);
	return true;
}

bool AudioDecoder::decode(const rtmp::AudioPacket &p, const FrameCallback &on_frame)
{
	if (!ctx_)
		return false;

	packet_->data = const_cast<uint8_t *>(p.data);
	packet_->size = static_cast<int>(p.size);
	packet_->dts = p.dts_ms;
	packet_->pts = p.dts_ms;

	int ret = avcodec_send_packet(ctx_, packet_);
	av_packet_unref(packet_);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		if (++consecutive_errors_ >= kMaxConsecutiveErrors)
			return false;
		obs_log(LOG_DEBUG, "audio decode error: %s", av_error_string(ret).c_str());
		return true;
	}
	consecutive_errors_ = 0;

	// A packet can decode to more than one frame (MP3); space them out in time.
	double offset_ms = 0;
	for (;;) {
		ret = avcodec_receive_frame(ctx_, frame_);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			obs_log(LOG_DEBUG, "audio decode error: %s", av_error_string(ret).c_str());
			break;
		}
		const int rate = frame_->sample_rate;
		const int samples = frame_->nb_samples;
		emit(*frame_, p.dts_ms + static_cast<int64_t>(offset_ms), on_frame);
		if (rate > 0)
			offset_ms += samples * 1000.0 / rate;
		av_frame_unref(frame_);
	}
	return true;
}

bool AudioDecoder::emit(const AVFrame &frame, int64_t pts_ms, const FrameCallback &on_frame)
{
	const int channels = frame.ch_layout.nb_channels;
	const int rate = frame.sample_rate;
	if (channels <= 0 || rate <= 0 || frame.nb_samples <= 0)
		return false;

	DecodedAudio out;
	out.frames = static_cast<uint32_t>(frame.nb_samples);
	out.sample_rate = static_cast<uint32_t>(rate);
	out.pts_ms = pts_ms;
	out.layout = speakers_for(channels);

	if (frame.format == AV_SAMPLE_FMT_FLTP && out.layout != SPEAKERS_UNKNOWN) {
		for (int ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++)
			out.planes[ch] = reinterpret_cast<const float *>(frame.extended_data[ch]);
		on_frame(out);
		return true;
	}

	// Convert to planar float, downmixing layouts OBS has no matching speaker set for.
	const int out_channels = out.layout == SPEAKERS_UNKNOWN ? 2 : channels;
	if (!swr_ || swr_in_rate_ != rate || swr_in_format_ != frame.format || swr_in_channels_ != channels) {
		swr_free(&swr_);
		AVChannelLayout out_layout;
		av_channel_layout_default(&out_layout, out_channels);
		int ret = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLTP, rate, &frame.ch_layout,
					      static_cast<AVSampleFormat>(frame.format), rate, 0, nullptr);
		if (ret < 0 || swr_init(swr_) < 0) {
			swr_free(&swr_);
			obs_log(LOG_WARNING, "cannot convert %d-channel audio", channels);
			return false;
		}
		swr_in_rate_ = rate;
		swr_in_format_ = frame.format;
		swr_in_channels_ = channels;
	}

	converted_.resize(static_cast<size_t>(out_channels));
	uint8_t *planes[MAX_AV_PLANES] = {};
	for (int ch = 0; ch < out_channels; ch++) {
		converted_[static_cast<size_t>(ch)].resize(static_cast<size_t>(frame.nb_samples) + 256);
		planes[ch] = reinterpret_cast<uint8_t *>(converted_[static_cast<size_t>(ch)].data());
	}
	int produced = swr_convert(swr_, planes, frame.nb_samples + 256, const_cast<const uint8_t **>(frame.extended_data),
				   frame.nb_samples);
	if (produced <= 0)
		return false;

	for (int ch = 0; ch < out_channels; ch++)
		out.planes[ch] = converted_[static_cast<size_t>(ch)].data();
	out.frames = static_cast<uint32_t>(produced);
	out.layout = speakers_for(out_channels);
	on_frame(out);
	return true;
}

} // namespace rtmpsrc
