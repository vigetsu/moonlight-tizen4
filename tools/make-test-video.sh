#!/bin/sh
# Generates an H.264 test clip for the bridge's --test mode (needs ffmpeg).
# Usage: tools/make-test-video.sh [WIDTHxHEIGHT] [FPS]
set -e
SIZE=${1:-1920x1080}
FPS=${2:-60}
OUT_DIR=$(dirname "$0")/../testdata
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/test-${SIZE}-${FPS}.h264"
# No B-frames and access unit delimiters on, like a real Sunshine stream.
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=${SIZE}:rate=${FPS}" -t 10 \
  -c:v libx264 -profile:v high -preset veryfast -tune zerolatency -bf 0 -g $((FPS * 2)) \
  -x264-params aud=1 -bsf:v h264_mp4toannexb -f h264 "$OUT"
echo "$OUT"
