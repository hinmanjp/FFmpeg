# Session Summary: Seek Discontinuity Fix (Version 8.0-zmq-11)

**Date:** November 6, 2025  
**Session Goal:** Fix the seek/reset functionality to properly restart video playback from the beginning

## Problem Identified

From the previous session (v10), the seek functionality was partially working:
- ✅ `avformat_seek_file()` executed successfully
- ✅ `avformat_flush()` was called to flush demuxer buffers
- ❌ **Video did NOT restart from beginning** - continued from pause point
- ❌ Frame duplication counter increased rapidly (dup=350→583→1003→1643→2013)
- ❌ ALSA buffer underruns (audio sync issues)

## Root Cause Analysis

The problem was that **only the demuxer was being flushed**. The downstream components in the FFmpeg pipeline were not aware of the discontinuity:

```
Demuxer → Decoder → Filtergraph → Encoder → Muxer → RTSP Output
  ✅         ❌         ❌           ❌        
flushed   not       not         not
         flushed   flushed     flushed
```

After seeking:
1. Demuxer seeks to position 0 and flushes its buffers ✅
2. Demuxer starts reading new frames from position 0 ✅
3. BUT decoder still has buffered frames from before seek ❌
4. BUT filtergraph still has buffered frames ❌
5. BUT encoder is waiting for input and duplicates frames ❌

Result: Old buffered frames continue to be processed while new frames are stuck in queues.

## The Fix: Discontinuity Signal

### Discovery

Found in FFmpeg scheduler API documentation (`fftools/ffmpeg_sched.h`):

```c
/**
 * Called by demuxer tasks to communicate with their downstreams. The following
 * may be sent:
 * - a demuxed packet for the stream identified by pkt->stream_index;
 * - demuxer discontinuity/reset (e.g. after a seek) - this is signalled by an
 *   empty packet with stream_index=-1.
 */
int sch_demux_send(Scheduler *sch, unsigned demux_idx, struct AVPacket *pkt,
                   unsigned flags);
```

**Key insight:** FFmpeg's scheduler expects demuxers to signal discontinuities (like seeks) by sending an empty packet with `stream_index=-1`.

### Implementation

Modified `fftools/ffmpeg_demux.c` in the seek handling code:

```c
// Perform the seek
ret = avformat_seek_file(f->ctx, -1, INT64_MIN, seek_pos, seek_pos, 0);
if (ret < 0) {
    av_log(d, AV_LOG_ERROR, "Seek failed for input #%d: %s\n", 
           f->index, av_err2str(ret));
} else {
    av_log(d, AV_LOG_INFO, "Seek successful for input #%d\n", f->index);
    
    // Flush demuxer's internal packet buffers
    avformat_flush(f->ctx);
    
    // Signal discontinuity to downstream (decoder/filtergraph/encoder)
    // This flushes the entire pipeline
    av_packet_unref(dt.pkt_demux);
    dt.pkt_demux->stream_index = -1;
    ret = sch_demux_send(d->sch, f->index, dt.pkt_demux, 0);
    if (ret < 0 && ret != AVERROR_EOF) {
        av_log(d, AV_LOG_ERROR, "Failed to send discontinuity signal: %s\n",
               av_err2str(ret));
    }
    
    // Reset timestamp tracking
    d->ts_offset_discont = 0;
    d->last_ts = AV_NOPTS_VALUE;
}
```

### What This Does

When the scheduler receives `stream_index=-1`, it calls `demux_flush()` which:

1. **Flushes all decoders** connected to this demuxer
2. **Flushes all filtergraphs** in the pipeline
3. **Flushes all encoders** downstream
4. **Clears all buffered packets/frames** throughout the entire pipeline

Now the pipeline looks like:

```
Demuxer → Decoder → Filtergraph → Encoder → Muxer → RTSP Output
  ✅         ✅         ✅           ✅        
flushed   flushed   flushed     flushed
```

## Changes Made

### Modified Files

1. **`fftools/ffmpeg_demux.c`**
   - Added discontinuity signal after successful seek
   - Calls `sch_demux_send()` with `stream_index=-1`

2. **`RELEASE`**
   - Updated to version `8.0-zmq-11`

### New Documentation Files

1. **`DISCONTINUITY_FIX.md`**
   - Detailed explanation of the problem and solution
   - Code examples
   - Technical details

2. **`BUILD_AND_TEST_v11.md`**
   - Build instructions for v11
   - Comprehensive test scenarios
   - Expected results vs. previous behavior
   - Troubleshooting guide

## Git Commits

```
commit c98726f151
Add documentation for v11 discontinuity fix

commit 5660e7395b
Fix seek: signal discontinuity to downstream pipeline
```

## Expected Results

After building and deploying v11, the reset command should:

✅ **Immediately restart video from beginning**  
✅ **Frame duplication stays low** (< 50)  
✅ **No ALSA buffer underruns**  
✅ **Audio/video stay in sync**  
✅ **Multiple resets work correctly**  

## Testing Instructions

Build and deploy to Linux server:

```bash
cd /home/peter/ffmpeg
git pull origin expand_zmq_support
make clean
./configure --enable-libzmq
make -j$(nproc)
./ffmpeg -version | head -1  # Should show: ffmpeg version n8.0-zmq-11
```

Run dual-stream test:

```bash
./ffmpeg -f v4l2 -i /dev/video0 \
         -i ../../rsc/test.mp4 \
         -map 0:v -map 1:v -map 1:a \
         -c:v libx264 -preset ultrafast -tune zerolatency \
         -c:a aac -f rtsp -rtsp_transport tcp rtsp://localhost:8554/live \
         -zmq tcp://127.0.0.1:5555
```

Test reset:

```bash
# Let video play for 20-30 seconds
echo "pause 2" | nc localhost 5555
sleep 10
echo "reset 2" | nc localhost 5555
# Video should immediately restart from beginning
```

## Next Steps

1. **Build and test v11 on Linux server**
2. **Verify reset command works correctly**
3. **If successful:**
   - Remove excessive debug logging
   - Performance testing
   - Integration testing with MediaMTX/WebRTC
   - Mark Phase 6 as COMPLETE ✅

4. **If not successful:**
   - Investigate scheduler's `demux_flush()` implementation
   - Check if discontinuity signal is propagating correctly
   - May need to add explicit filtergraph/encoder reset

## Technical Notes

### Why Previous Attempts Failed

- **v8-v9**: No buffer flushing at all
- **v10**: Only flushed demuxer buffers with `avformat_flush()`
- **v11**: Flushes entire pipeline using scheduler's discontinuity mechanism ✅

### Scheduler Flow

```
demux_send() with stream_index=-1
    ↓
demux_flush()
    ↓
Sends flush packets to all decoders
    ↓
Decoders flush and forward to filtergraphs
    ↓
Filtergraphs flush and forward to encoders
    ↓
Encoders flush and reset state
```

### Key API Usage

- `avformat_seek_file()` - Seek in the file
- `avformat_flush()` - Flush demuxer's internal buffers
- `sch_demux_send(d->sch, f->index, pkt, 0)` with `pkt->stream_index=-1` - Signal discontinuity to scheduler
- Scheduler's `demux_flush()` - Propagates flush through pipeline

## Confidence Level

**High confidence** this will fix the issue because:

1. ✅ Using official FFmpeg API as documented
2. ✅ Following the same pattern used for loop functionality
3. ✅ Addresses root cause (downstream buffers not flushed)
4. ✅ Based on reading scheduler source code implementation

The discontinuity signal is the **standard FFmpeg mechanism** for handling seeks and other timeline discontinuities.

## Files to Review

- `DISCONTINUITY_FIX.md` - Problem analysis and solution details
- `BUILD_AND_TEST_v11.md` - Complete testing guide
- `fftools/ffmpeg_demux.c` (lines 773-810) - Implementation
- `fftools/ffmpeg_sched.h` (lines 334-365) - API documentation
- `fftools/ffmpeg_sched.c` (lines 2020-2108) - Scheduler implementation

## Version History

- v1-v7: Initial ZMQ and pause/resume implementation
- v8-v9: Initial seek implementation (didn't work)
- v10: Added `avformat_flush()` (still didn't work)
- **v11: Added discontinuity signal (should work!)** ← Current version
