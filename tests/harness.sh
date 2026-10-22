#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 obs-rtmp-server contributors
# Loads the built plugin into the libobs of an installed OBS (headless, the OBS
# app itself is not touched), streams a test pattern at the "RTMP Server"
# source with a real RTMP client, and checks what OBS received. Then the
# publisher is killed mid-stream and a new one connects: the source must pick
# the stream up again.
#
#   harness.sh <obs-harness> <test-publisher> <path/to/obs-rtmp-server.plugin>

HARNESS="$1"
PUBLISHER="$2"
PLUGIN="$3"
PORT=${HARNESS_PORT:-19360}
URL="rtmp://127.0.0.1:$PORT/live/harness"

log=$(mktemp)
"$HARNESS" --plugin "$PLUGIN" --port "$PORT" --key harness --timeout 90 --passes 2 \
	--expect-size 640x360 --expect-luma 200,50 --min-audio-seconds 1.5 >"$log" 2>&1 &
harness_pid=$!

wait_for() { # <pattern> <seconds>
	for _ in $(seq 1 $(($2 * 5))); do
		grep -q "$1" "$log" && return 0
		kill -0 "$harness_pid" 2>/dev/null || return 1
		sleep 0.2
	done
	return 1
}

# Starting libobs (graphics included) takes a few seconds.
wait_for "waiting for a publisher" 30

"$PUBLISHER" "$URL" --seconds 60 --size 640x360 >"$log.pub1" 2>&1 &
pub1=$!
wait_for "pass 1 ok" 30
# Drop the first publisher without a goodbye.
kill -9 "$pub1" 2>/dev/null
wait "$pub1" 2>/dev/null

wait_for "pass 1: waiting for a new publisher" 20
"$PUBLISHER" "$URL" --seconds 60 --size 640x360 >"$log.pub2" 2>&1 &
pub2=$!
wait "$harness_pid"
status=$?
kill "$pub2" 2>/dev/null
wait "$pub2" 2>/dev/null

if [ "$status" -eq 0 ]; then
	echo "harness: ok ($(grep -E '^pass [0-9] ok' "$log" | tr '\n' ';'))"
else
	echo "harness: FAILED"
	cat "$log" "$log.pub1" "$log.pub2"
fi
rm -f "$log" "$log.pub1" "$log.pub2"
exit $status
