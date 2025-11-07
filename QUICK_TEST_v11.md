# Quick Deploy & Test - Version 8.0-zmq-11

## 🚀 Deploy on Linux Server

```bash
# Pull latest code
cd /home/peter/ffmpeg
git pull origin expand_zmq_support

# Verify commit
git log --oneline -1
# Should show: 4b1c79c8e1 Add session summary for v11 discontinuity fix

# Build
make clean
./configure --enable-libzmq
make -j$(nproc)

# Verify version
./ffmpeg -version | head -1
# Expected: ffmpeg version n8.0-zmq-11
```

## ▶️ Start Dual-Stream Test

```bash
./ffmpeg -f v4l2 -i /dev/video0 \
         -i ../../rsc/test.mp4 \
         -map 0:v -map 1:v -map 1:a \
         -c:v libx264 -preset ultrafast -tune zerolatency \
         -c:a aac -f rtsp -rtsp_transport tcp rtsp://localhost:8554/live \
         -zmq tcp://127.0.0.1:5555
```

## 🧪 Test Reset (The Critical Test)

```bash
# Let video play 20-30 seconds so you can see it's NOT at the beginning
sleep 30

# Pause the file input
echo "pause 2" | nc localhost 5555

# Wait a bit
sleep 10

# Reset - THIS SHOULD RESTART FROM BEGINNING NOW
echo "reset 2" | nc localhost 5555
```

## ✅ Success Indicators

Watch FFmpeg output for:
- `[demuxer] Seeking input #2 to position 0`
- `[demuxer] Seek successful for input #2`
- Frame counter resets to low numbers
- **No excessive frame duplication** (dup should stay < 50)
- **No ALSA underrun errors**

Watch video stream (WebRTC viewer):
- **Video immediately jumps to beginning of test.mp4**
- Smooth playback resumes from start
- Audio stays in sync

## ❌ Failure Indicators

FFmpeg output:
- `dup=350` or higher and increasing rapidly
- `ALSA lib pcm.c:8545:(snd_pcm_recover) underrun occurred`
- Video continues from pause point instead of restarting

## 🔍 Quick Checks

```bash
# Check if process is running
ps aux | grep ffmpeg

# Send status (if you add a status command)
echo "status" | nc localhost 5555

# Kill if needed
pkill -9 ffmpeg
```

## 📝 Test Matrix

| Command | Expected Result |
|---------|----------------|
| `pause 2` | File input pauses at current frame |
| `resume 2` | File input continues from where it paused |
| `reset 2` | File input **restarts from beginning** ⭐ |
| `seek 2 5.0` | File input jumps to 5 seconds |

## 🎯 What Changed in v11

**One critical addition:**
```c
// After successful seek, signal discontinuity
dt.pkt_demux->stream_index = -1;
sch_demux_send(d->sch, f->index, dt.pkt_demux, 0);
```

This flushes the **entire downstream pipeline** (decoder → filtergraph → encoder), not just the demuxer.

## 📊 Expected vs Previous

| Metric | v10 | v11 Expected |
|--------|-----|--------------|
| Seek executes | ✅ | ✅ |
| Demuxer flushed | ✅ | ✅ |
| Pipeline flushed | ❌ | ✅ |
| Video restarts | ❌ | ✅ |
| Frame dup count | 1000+ | < 50 |

## 🐛 If It Doesn't Work

1. Check logs for discontinuity signal
2. Verify `sch_demux_send()` return value
3. Check if decoders are receiving flush
4. Report findings for further investigation

## 📁 Key Files

- `DISCONTINUITY_FIX.md` - Detailed technical explanation
- `BUILD_AND_TEST_v11.md` - Comprehensive testing guide
- `SESSION_SUMMARY_v11.md` - What was done this session
- `fftools/ffmpeg_demux.c` - Implementation (lines 773-810)

## 🎉 If Successful

This completes Phase 6 of the implementation! Next steps:
- Remove debug logging
- Performance testing
- Integration with MediaMTX/WebRTC
- Final documentation

---

**Version:** 8.0-zmq-11  
**Branch:** expand_zmq_support  
**Key Commit:** 5660e7395b Fix seek: signal discontinuity to downstream pipeline  
**Date:** November 6, 2025
