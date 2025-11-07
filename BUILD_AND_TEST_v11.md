# Build and Test Instructions - Version 8.0-zmq-11

## What's New in v11

This version adds the **critical missing piece** to make seek/reset work correctly:
- **Signals discontinuity to downstream pipeline** after seeking
- This flushes decoder/filtergraph/encoder buffers in addition to demuxer buffers
- Should fix the "video continues from pause point instead of restarting" issue

## Build Instructions

### On Linux Server

```bash
cd /home/peter/ffmpeg
git pull origin expand_zmq_support

# Verify you have the latest commit
git log --oneline -1
# Should show: 5660e7395b Fix seek: signal discontinuity to downstream pipeline

# Clean build
make clean
./configure --enable-libzmq
make -j$(nproc)

# Verify version
./ffmpeg -version | head -1
# Should show: ffmpeg version n8.0-zmq-11
```

## Test Setup

### Prerequisites
1. MediaMTX server running on port 8554
2. Camera available at `/dev/video0`
3. Test video file at `~/rsc/test.mp4`
4. netcat (`nc`) for sending ZMQ commands

### Start FFmpeg with Dual Streams

```bash
./ffmpeg -f v4l2 -i /dev/video0 \
         -i ../../rsc/test.mp4 \
         -map 0:v -map 1:v -map 1:a \
         -c:v libx264 -preset ultrafast -tune zerolatency \
         -c:a aac -f rtsp -rtsp_transport tcp rtsp://localhost:8554/live \
         -zmq tcp://127.0.0.1:5555
```

## Test Scenarios

### Test 1: Basic Pause/Resume
```bash
# Pause file input (input #2)
echo "pause 2" | nc localhost 5555

# Wait 5 seconds

# Resume
echo "resume 2" | nc localhost 5555
```

**Expected Result:**
- Video pauses at current frame
- Resumes from where it paused
- No frame duplication warnings

### Test 2: Reset (Seek to Beginning)
```bash
# Pause file input
echo "pause 2" | nc localhost 5555

# Wait 10-15 seconds to make sure file has progressed significantly

# Reset - should seek to 0 and unpause
echo "reset 2" | nc localhost 5555
```

**Expected Result (v11 fix):**
- ✅ Video **immediately restarts from the beginning** of test.mp4
- ✅ No "Seeking input #2..." log appears
- ✅ "Seek successful for input #2" appears in logs
- ✅ **NEW**: "Sent discontinuity signal" appears (or similar)
- ✅ Frame counter in output resets (frame= restarts from low numbers)
- ✅ No excessive frame duplication (dup= should stay low)
- ✅ Audio stays in sync

**Previous Behavior (v10 and earlier):**
- ❌ Video continued from pause point
- ❌ Frame duplication counter skyrocketed (dup=350→583→1003...)
- ❌ ALSA buffer underruns

### Test 3: Multiple Resets
```bash
# Let video play for 30 seconds
sleep 30

# Pause and reset
echo "pause 2" | nc localhost 5555
sleep 5
echo "reset 2" | nc localhost 5555

# Wait 20 seconds
sleep 20

# Pause and reset again
echo "pause 2" | nc localhost 5555
sleep 5
echo "reset 2" | nc localhost 5555

# Repeat 2-3 more times
```

**Expected Result:**
- Each reset restarts video from beginning
- No crashes
- No memory leaks
- Frame duplication stays low throughout

### Test 4: Seek to Specific Position
```bash
# Seek to 10.5 seconds
echo "seek 2 10.5" | nc localhost 5555

# Wait, then seek to 5 seconds
sleep 10
echo "seek 2 5.0" | nc localhost 5555
```

**Expected Result:**
- Video jumps to specified position
- Playback continues from that position
- No frame duplication

## What to Look For in Logs

### Success Indicators (v11)
```
[demuxer] Seeking input #2 to position 0
[demuxer] Seek successful for input #2
[demuxer] Signaling discontinuity to downstream
```

No excessive duplication warnings:
```
frame= 1234 fps=30 q=28.0 size=   12345kB time=00:00:41.00 bitrate=dup=5 drop=0
```
(dup should stay low, < 50)

### Failure Indicators
```
frame= 1234 fps=30 q=28.0 size=   12345kB time=00:00:41.00 bitrate=dup=1643 drop=0
```
(dup increasing rapidly = old problem still present)

```
ALSA lib pcm.c:8545:(snd_pcm_recover) underrun occurred
```
(Audio buffer underruns = pipeline not flushed properly)

## Key Differences from v10

| Aspect | v10 | v11 |
|--------|-----|-----|
| Demuxer flush | ✅ Yes | ✅ Yes |
| Downstream flush | ❌ No | ✅ Yes (NEW) |
| Discontinuity signal | ❌ No | ✅ Yes (NEW) |
| Decoder flushed | ❌ No | ✅ Yes |
| Filtergraph flushed | ❌ No | ✅ Yes |
| Encoder flushed | ❌ No | ✅ Yes |
| Seek works correctly | ❌ No | ✅ Should work |

## Technical Details

The key change in v11 is in `fftools/ffmpeg_demux.c` after a successful seek:

```c
// NEW in v11: Signal discontinuity to downstream
av_packet_unref(dt.pkt_demux);
dt.pkt_demux->stream_index = -1;  // -1 = discontinuity signal
ret = sch_demux_send(d->sch, f->index, dt.pkt_demux, 0);
```

This triggers `demux_flush()` in the scheduler, which flushes:
1. All decoders connected to this demuxer
2. All filtergraphs in the pipeline
3. All encoders downstream
4. All buffered frames in the entire pipeline

## Troubleshooting

### If seek still doesn't work:
1. Check logs for "discontinuity signal" message
2. Look for any errors from `sch_demux_send()`
3. Check if decoders/filtergraphs are receiving flush signal
4. Verify no other code is preventing the pipeline flush

### If crashes occur:
1. Check for mutex deadlocks (unlikely with this change)
2. Verify packet is properly unreferenced before setting stream_index=-1
3. Check scheduler state

### If frame duplication persists:
1. The discontinuity signal may not be propagating correctly
2. Check if encoder is receiving the flush
3. May need additional investigation into encoder state

## Next Steps After Testing

If v11 works:
- Remove excessive debug logging
- Performance testing
- Integration testing with MediaMTX
- Documentation cleanup
- Consider it COMPLETE ✅

If v11 doesn't work:
- Need to investigate scheduler's `demux_flush()` implementation
- May need to add explicit encoder reset
- Check filtergraph buffering behavior
