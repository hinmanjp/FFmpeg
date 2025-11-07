#!/bin/bash
# Test script for FFmpeg v8.0-zmq-11

set -e

echo "======================================"
echo "Testing FFmpeg v8.0-zmq-11"
echo "======================================"
echo ""

# Check version
VERSION=$(./ffmpeg -version 2>&1 | head -1)
if [[ ! "$VERSION" == *"8.0-zmq-11"* ]]; then
    echo "ERROR: Wrong version!"
    echo "Expected: 8.0-zmq-11"
    echo "Got: $VERSION"
    exit 1
fi

echo "✅ Running correct version: v8.0-zmq-11"
echo ""

# Kill any existing FFmpeg processes
echo "Killing any existing FFmpeg processes..."
pkill -9 ffmpeg || true
sleep 2

# Check if MediaMTX is running
if ! pgrep -x "mediamtx" > /dev/null; then
    echo "WARNING: MediaMTX is not running!"
    echo "Start MediaMTX in another terminal, then press Enter to continue..."
    read
fi

echo ""
echo "Starting FFmpeg with dual-stream setup..."
echo "- Input 0: Camera (/dev/video2)"
echo "- Input 1: Audio (hw:1)"
echo "- Input 2: Video file (risen_15k.mp4)"
echo "- ZMQ control: tcp://127.0.0.1:5555"
echo ""
echo "Commands to test in another terminal:"
echo "  echo 'pause 2' | nc localhost 5555    # Pause file input"
echo "  echo 'reset 2' | nc localhost 5555    # Reset to beginning"
echo "  echo 'resume 2' | nc localhost 5555   # Resume playback"
echo "  echo 'seek 2 10.0' | nc localhost 5555 # Seek to 10 seconds"
echo ""
echo "Press Ctrl+C to stop"
echo ""

./ffmpeg -f v4l2 -i /dev/video2 \
         -f alsa -i hw:1 \
         -i risen_15k.mp4 \
         -filter_complex "[0:v]scale=1920:1080[v0];[2:v]scale=1920:1080[v2];[v0][v2]zmq=bind_address=tcp\\://127.0.0.1\\:5556[vout];[1:a][2:a]amerge=inputs=2,pan=stereo|c0=c0|c1=c1,azmq=bind_address=tcp\\://127.0.0.1\\:5557[aout]" \
         -map "[vout]" -map "[aout]" \
         -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -sc_threshold 0 \
         -b:v 2500k -maxrate 2500k -bufsize 1250k -pix_fmt yuv420p \
         -c:a libopus -b:a 96k \
         -f rtsp -rtsp_transport tcp rtsp://localhost:8554/test \
         -zmq tcp://127.0.0.1:5555
