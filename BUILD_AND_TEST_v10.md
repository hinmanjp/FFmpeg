# Build and Test Instructions - Version 8.0-zmq-10

## Current Issue Diagnosis

Your build output shows **commit 64d4dd6305** which is the struct alignment fix, but **BEFORE** the buffer flush fix (commit 69b5f5cbf9).

The Linux build server needs to pull the latest commits to get the `avformat_flush()` fix.

## Required Commits

You need **commit 69b5f5cbf9 or later** which includes:
1. ✅ Struct alignment fix (64d4dd6305) - **You have this**
2. ❌ Buffer flush fix (69b5f5cbf9) - **You need this**

## Step-by-Step Instructions

### 1. On Linux Build Server - Pull Latest Code

```bash
cd /path/to/FFmpeg
git checkout expand_zmq_support
git fetch origin
git pull origin expand_zmq_support

# Verify you have the latest commits
git log --oneline -5
```

**Expected output should show:**
```
69b5f5cbf9 (HEAD -> expand_zmq_support) Add documentation for seek buffer flush fix
64d4dd6305 Fix seek functionality: flush demuxer buffers after seek
1d0db7f8f2 Add comprehensive testing guide for struct alignment fix
8a772d12d8 Remove duplicate Timestamp typedef and fix comment warning
cf24bab5e1 CRITICAL FIX: Correct Demuxer struct alignment
```

### 2. Clean Build

```bash
make clean
make -j$(nproc)
```

### 3. Verify Version

```bash
./ffmpeg -version | head -1
```

**Expected:** `ffmpeg version git-2025-11-06-69b5f5cbf9` (or similar with 69b5f5cbf9 commit hash)

**NOT:** `ffmpeg version git-2025-11-06-64d4dd6305` (this is the old build)

### 4. Test Reset Command

```bash
# Start FFmpeg with ZMQ
./ffmpeg -zmq tcp://127.0.0.1:5555 \
  -re -stream_loop -1 -f lavfi -i testsrc=duration=60:size=1920x1080:rate=30 \
  -re -stream_loop -1 -f lavfi -i sine=frequency=1000:duration=60 \
  -re -stream_loop -1 -i risen_15k.mp4 \
  -map 0:v -map 1:a -c:v libx264 -preset veryfast -c:a aac \
  -f rtsp rtsp://localhost:8554/test &

FFMPEG_PID=$!
echo "FFmpeg PID: $FFMPEG_PID"
```

Wait 30 seconds, then send reset command:

```bash
sleep 30

python3 -c "
import zmq
ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect('tcp://127.0.0.1:5555')
sock.send_string('reset 2')
print('Response:', sock.recv_string())
"
```

### 5. Check Logs - What to Look For

**OLD BUILD (64d4dd6305) - WITHOUT FIX:**
```
[in#2/mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] Seeking input #2 to position 0
[in#2/mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] Seek successful for input #2
```
(No avformat_flush, playback continues from old position)

**NEW BUILD (69b5f5cbf9) - WITH FIX:**
```
[in#2/mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] Seeking input #2 to position 0
[in#2/mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] Seek successful for input #2
```
(avformat_flush called internally, playback restarts from beginning)

### 6. Visual Verification

Watch the RTSP stream at `rtsp://localhost:8554/test` using VLC or similar:

- **Before reset**: Stream shows content from ~30 seconds into the file
- **After reset**: Stream should jump back to the beginning of the file
- **Expected**: Video clearly restarts from the first frame

### 7. Multiple Reset Test

```bash
# Send 5 reset commands, 5 seconds apart
for i in {1..5}; do
  echo "Reset $i at $(date +%T)"
  python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('reset 2'); print(s.recv_string())"
  sleep 5
done
```

**Expected:** Each reset should restart the video, and FFmpeg should NOT crash/terminate.

## Known Issues to Investigate

### Issue 1: Process Terminates on 3rd Reset (From Your Log)

Your log shows:
```
Received ZMQ command: reset 2    (1st reset - works)
Received ZMQ command: reset 2    (2nd reset - works)
Received ZMQ command: reset 2    (3rd reset - works)
Killed
```

This needs investigation. Possible causes:
1. External process killed it (check `kill` signals)
2. Filter graph issue (the "More than 1000 frames duplicated" warning)
3. ALSA buffer issues (multiple "ALSA buffer xrun" warnings)
4. Seeking while encoder is busy

**To debug:**
```bash
# Run with full debug logging
./ffmpeg -loglevel debug -zmq tcp://127.0.0.1:5555 ... 2>&1 | tee ffmpeg_debug.log

# In another terminal, send resets and monitor
dmesg -w  # Check for kernel messages
```

### Issue 2: Frame Duplication Warning

```
[fc#0 @ 0x...] [vo0->#0:0 @ 0x...] More than 1000 frames duplicated
```

This indicates the encoder is duplicating input frames, possibly because:
- Input #2 (file) is paused/seeking while encoder needs frames
- Timing synchronization issue between inputs

**Potential fix:** May need to signal the scheduler/filter graph about the seek to reset timing.

## Success Criteria

After pulling latest code and rebuilding:

- ✅ `git log --oneline -1` shows commit 69b5f5cbf9 or later
- ✅ `./ffmpeg -version` shows the newer commit hash
- ✅ Reset command causes visible jump to beginning of video
- ✅ Multiple resets work without crashing
- ✅ No mutex errors or crashes

## If Still Not Working

If playback still doesn't restart after pulling latest code:

1. **Verify the code change is present:**
   ```bash
   grep -A 5 "Seek successful for input" fftools/ffmpeg_demux.c
   ```
   
   Should show:
   ```c
   av_log(d, AV_LOG_INFO, "Seek successful for input #%d\n", 
          f->index);
   
   // Flush demuxer's internal packet buffers to discard old frames
   avformat_flush(f->ctx);
   ```

2. **Check if different issue:** The problem might not be buffering but filter graph timing

3. **Provide new logs:** Send the output with the newer build so we can see what's different

## Quick Reference Commands

```bash
# Pull latest
git pull origin expand_zmq_support

# Check commit
git log --oneline -1

# Build
make clean && make -j$(nproc)

# Test
./ffmpeg -zmq tcp://127.0.0.1:5555 -re -stream_loop -1 -i file.mp4 -c copy -f rtsp rtsp://localhost:8554/test &

# Reset
python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('reset 2'); print(s.recv_string())"

# Kill
pkill -INT ffmpeg
```

Good luck with the rebuild! 🚀
