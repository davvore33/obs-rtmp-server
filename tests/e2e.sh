#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 obs-rtmp-server contributors
# End to end: a real RTMP client (FFmpeg, via test-publisher) streams a known
# test pattern into rtmp-probe, which decodes it with the plugin's decoders and
# checks size, picture content and audio level.
#
#   e2e.sh <rtmp-probe> <test-publisher>

PROBE="$1"
PUBLISHER="$2"
PORT=${E2E_PORT:-19350}
FAILED=0

run_case() {
	name="$1"; shift
	publisher_args="$1"; shift
	log=$(mktemp)
	"$PROBE" --decode --selftest --port "$PORT" --key e2e --timeout 25 "$@" >"$log" 2>&1 &
	probe_pid=$!
	sleep 1
	"$PUBLISHER" "rtmp://127.0.0.1:$PORT/live/e2e" $publisher_args >"$log.pub" 2>&1
	wait "$probe_pid"
	status=$?
	if [ "$status" -eq 0 ]; then
		echo "e2e $name: ok ($(grep 'SELFTEST OK' "$log" | cut -d: -f2-))"
	else
		echo "e2e $name: FAILED"
		cat "$log" "$log.pub"
		FAILED=1
	fi
	rm -f "$log" "$log.pub"
}

# H.264 + AAC, no B-frames
run_case "h264-aac" "--seconds 4 --size 640x360 --fps 30" \
	--min-video-frames 60 --min-audio-seconds 2 --expect-size 640x360 \
	--expect-luma 200,50 --expect-audio-rms 0.25,0.45

# H.264 with B-frames (composition time offsets, decoder reordering)
run_case "h264-bframes" "--seconds 4 --size 854x480 --fps 30 --bframes 2" \
	--min-video-frames 60 --min-audio-seconds 2 --expect-size 854x480 \
	--expect-luma 200,50 --expect-audio-rms 0.25,0.45

# Video only
run_case "video-only" "--seconds 3 --size 320x240 --fps 25 --no-audio" \
	--min-video-frames 40 --expect-size 320x240 --expect-luma 200,50

# Audio only
run_case "audio-only" "--seconds 3 --no-video" \
	--min-audio-seconds 1.5 --expect-audio-rms 0.25,0.45

exit $FAILED
