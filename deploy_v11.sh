#!/bin/bash
# Deploy and test v11 on Linux server

set -e  # Exit on error

echo "======================================"
echo "Deploying FFmpeg v8.0-zmq-11"
echo "======================================"
echo ""

# Navigate to FFmpeg directory
cd /home/peter/ffmpeg

echo "1. Pulling latest code from expand_zmq_support branch..."
git fetch origin
git checkout expand_zmq_support
git pull origin expand_zmq_support

echo ""
echo "2. Verifying commit..."
git log --oneline -1
echo ""

EXPECTED_COMMIT="5660e7395b"
ACTUAL_COMMIT=$(git log --oneline -1 | awk '{print $1}')

if [[ ! "$ACTUAL_COMMIT" == "$EXPECTED_COMMIT"* ]]; then
    echo "WARNING: Expected commit starting with $EXPECTED_COMMIT but got $ACTUAL_COMMIT"
    echo "Continue anyway? (y/n)"
    read -r response
    if [[ ! "$response" =~ ^[Yy]$ ]]; then
        echo "Deployment aborted"
        exit 1
    fi
fi

echo "3. Cleaning previous build..."
make clean

echo ""
echo "4. Configuring build with ZMQ support..."
./configure --enable-libzmq

echo ""
echo "5. Building FFmpeg (this may take several minutes)..."
make -j$(nproc)

echo ""
echo "6. Verifying version..."
VERSION=$(./ffmpeg -version 2>&1 | head -1)
echo "$VERSION"

if [[ ! "$VERSION" == *"8.0-zmq-11"* ]]; then
    echo "ERROR: Version mismatch!"
    echo "Expected: 8.0-zmq-11"
    echo "Got: $VERSION"
    exit 1
fi

echo ""
echo "======================================"
echo "✅ Build Complete: v8.0-zmq-11"
echo "======================================"
echo ""
echo "Next steps:"
echo "1. Kill any running FFmpeg processes: pkill -9 ffmpeg"
echo "2. Start MediaMTX if not running"
echo "3. Run test: ./test_v11.sh"
echo ""
