# 🔴 IMPORTANT: You're Running the OLD Version!

## Current Problem

Your `build_output.txt` shows you're running:
```
ffmpeg version git-2025-11-06-69b5f5cbf9
```

This is **version v10** (commit 69b5f5cbf9), which does NOT have the discontinuity fix!

The symptoms you're seeing are **expected for v10**:
- ❌ Seek executes but video doesn't restart
- ❌ Frame duplication increases (dup=658→1682)
- ❌ ALSA buffer xruns

## The Fix (v11) Hasn't Been Deployed Yet

Version **v8.0-zmq-11** (commit 5660e7395b) has the critical fix:
- Sends discontinuity signal to scheduler
- Flushes entire pipeline (decoder/filter/encoder)
- **Should** make reset work correctly

## Deploy v11 Now

### Option 1: Automated Script (Recommended)

I've created deployment scripts for you. Copy them to your Linux server:

1. **`deploy_v11.sh`** - Builds v11
2. **`test_v11.sh`** - Runs the test

On your Linux server:

```bash
cd /home/peter/ffmpeg

# Make scripts executable
chmod +x deploy_v11.sh test_v11.sh

# Deploy v11
./deploy_v11.sh

# Run test (in new terminal after MediaMTX is running)
./test_v11.sh
```

### Option 2: Manual Steps

```bash
cd /home/peter/ffmpeg

# Pull latest code
git pull origin expand_zmq_support

# Verify you have the discontinuity fix
git log --oneline -1
# Should show: 5660e7395b Fix seek: signal discontinuity to downstream pipeline

# Check the fix is present
grep -A5 "stream_index = -1" fftools/ffmpeg_demux.c
# Should show the discontinuity signal code

# Build
make clean
./configure --enable-libzmq
make -j$(nproc)

# Verify version
./ffmpeg -version | head -1
# Should show: ffmpeg version n8.0-zmq-11
```

## What to Look For After Deploying v11

Run the same test again. When you send `reset 2`, you should see:

**FFmpeg logs:**
```
Seeking input #2 to position 0
Seek successful for input #2
```

**Video behavior:**
- ✅ Video **immediately jumps to beginning** of risen_15k.mp4
- ✅ Frame dup counter stays low (< 50)
- ✅ **NO** "More than 1000 frames duplicated" warning
- ✅ **NO** ALSA buffer xruns

**Frame counter should reset:**
```
Before reset:  frame=1310 dup=658
After reset:   frame=30   dup=15   ← Low numbers!
```

## Key Difference: v10 vs v11

### v10 (What you're running now)
```c
// Only flushes demuxer buffers
avformat_flush(f->ctx);

// Decoder/filter/encoder still have old frames! ❌
```

### v11 (What you need to build)
```c
// Flushes demuxer buffers
avformat_flush(f->ctx);

// Signal discontinuity to flush ENTIRE pipeline ✅
dt.pkt_demux->stream_index = -1;
sch_demux_send(d->sch, f->index, dt.pkt_demux, 0);
```

## Deployment Checklist

- [ ] Pull latest code from `expand_zmq_support` branch
- [ ] Verify commit is `5660e7395b` or later
- [ ] Rebuild FFmpeg (`make clean && configure && make`)
- [ ] Verify version shows `8.0-zmq-11`
- [ ] Kill old FFmpeg processes (`pkill -9 ffmpeg`)
- [ ] Start MediaMTX
- [ ] Run test
- [ ] Send `reset 2` command
- [ ] **Verify video restarts from beginning!**

## If It Still Doesn't Work After v11

If you deploy v11 and it STILL doesn't work, then we'll need to investigate further. But first, **make sure you're running v11**!

Check the version in the FFmpeg output header:
```
ffmpeg version n8.0-zmq-11
```

Not:
```
ffmpeg version git-2025-11-06-69b5f5cbf9  ← This is v10!
```

---

**TL;DR:** Your server is running the old code. Deploy v11 first, then test again!
