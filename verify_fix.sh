#!/bin/bash
# Verify v11 discontinuity fix is present in the source

echo "Checking if discontinuity fix is in fftools/ffmpeg_demux.c..."
echo ""

if grep -q "stream_index = -1" fftools/ffmpeg_demux.c; then
    echo "✅ Found: stream_index = -1"
else
    echo "❌ NOT FOUND: stream_index = -1"
    echo "The fix is missing!"
    exit 1
fi

if grep -q "sch_demux_send.*-1" fftools/ffmpeg_demux.c; then
    echo "✅ Found: sch_demux_send with discontinuity"
else
    echo "❌ NOT FOUND: sch_demux_send discontinuity call"
    exit 1
fi

echo ""
echo "Checking line numbers where the fix should be..."
grep -n "stream_index = -1" fftools/ffmpeg_demux.c | head -5

echo ""
echo "✅ Discontinuity fix appears to be present in source"
echo ""
echo "If FFmpeg was rebuilt after pulling this code, the fix should be active."
echo "Run 'git log --oneline -5' to see recent commits."
