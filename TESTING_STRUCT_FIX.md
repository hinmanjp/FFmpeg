# Testing the Struct Alignment Fix (Version 8.0-zmq-9-FIXED)

## Background
This test verifies the critical struct alignment fix that resolves the TPP (Thread Priority Protocol) mutex crash when executing the "reset 2" command via ZMQ.

### Root Cause (Now Fixed)
The crash was caused by a 24-byte structure misalignment in `fftools/ffmpeg_zmq.c`:
- **BEFORE**: `Timestamp` fields were declared as `void*` (8 bytes each)
- **AFTER**: `Timestamp` fields are now correctly declared as `Timestamp` struct (16 bytes each)
- **Impact**: 3 fields × 8-byte difference = 24-byte offset error
- **Result**: `control_mutex` was initialized at one address but accessed 24 bytes earlier

## Build Instructions

### 1. Pull Latest Code
```bash
cd /path/to/FFmpeg
git checkout expand_zmq_support
git pull origin expand_zmq_support
```

Verify you have commit `8a772d12d8` or later:
```bash
git log --oneline -1
# Should show: "Remove duplicate Timestamp typedef and fix comment warning"
```

### 2. Configure and Build
```bash
./configure --enable-libzmq
make clean
make -j$(nproc)
```

### 3. Verify Version
```bash
./ffmpeg -version | head -1
# Should show: ffmpeg version 8.0-zmq-9-FIXED
```

## Test Procedure

### Test 1: Basic ZMQ Functionality
Start FFmpeg with ZMQ enabled and three file inputs:

```bash
./ffmpeg \
  -zmq tcp://127.0.0.1:5555 \
  -re -stream_loop -1 -i /path/to/file1.mp4 \
  -re -stream_loop -1 -i /path/to/file2.mp4 \
  -re -stream_loop -1 -i /path/to/file3.mp4 \
  -map 0:v -map 0:a -c copy \
  -f rtsp -rtsp_transport tcp rtsp://localhost:8554/stream &
```

### Test 2: Reset Command (Critical Test)
This was the command that triggered the crash. In a separate terminal:

```bash
# Install ZMQ tools if not already installed
# Ubuntu/Debian: sudo apt-get install libzmq3-dev python3-zmq
# CentOS/RHEL: sudo yum install zeromq-devel python3-zmq

# Send reset command to input #2 (the problematic one from testing)
python3 -c "
import zmq
ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect('tcp://127.0.0.1:5555')
sock.send_string('reset 2')
print(sock.recv_string())
"
```

**Expected Result**: Should print "OK: Reset" without any crash.

**Previous Behavior**: Would crash with:
```
Fatal glibc error: pthread_mutex_lock.c:438 (__pthread_mutex_lock_full): 
assertion failed: (type & PTHREAD_MUTEX_TIMED_ROBUST_ADAPTIVE_NP) != PTHREAD_MUTEX_TIMED_ROBUST_ADAPTIVE_NP
```

### Test 3: All Commands
Test all ZMQ commands to ensure complete functionality:

```bash
# Pause input 2
python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('pause 2'); print(s.recv_string())"

# Resume input 2
python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('resume 2'); print(s.recv_string())"

# Seek input 2 to 30.5 seconds
python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('seek 2 30.5'); print(s.recv_string())"

# Reset input 2 (seek to start)
python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('reset 2'); print(s.recv_string())"
```

### Test 4: Rapid Command Stress Test
Send multiple rapid commands to verify mutex stability:

```bash
for i in {1..10}; do
  python3 -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://127.0.0.1:5555'); s.send_string('reset 2'); print('$i:', s.recv_string())"
  sleep 0.1
done
```

## Expected Debug Output

With the current debug logging enabled, you should see output like:

```
[demux @ 0x...] Demuxer initialized at 0x..., control_mutex at 0x...
[NULL @ 0x...] ZMQ command listener started
[NULL @ 0x...] Received ZMQ command: reset 2
[NULL @ 0x...] [ZMQ] Processing reset command for input #2, Demuxer=0x..., mutex=0x...
[NULL @ 0x...] [ZMQ] Reset command for input #2 - about to lock mutex at 0x...
[NULL @ 0x...] [ZMQ] Mutex locked successfully, setting control flags
[NULL @ 0x...] [ZMQ] Control flags set, about to unlock mutex
[NULL @ 0x...] [ZMQ] Mutex unlocked successfully
[NULL @ 0x...] Reset input #2 (seek to start)
[demux/video:2 @ 0x...] Seek detected: requested=1, target=0
```

The key indicators of success:
1. **No crash** when locking/unlocking mutex
2. **Mutex addresses match** between initialization and access
3. **"Mutex locked successfully"** message appears
4. **"Seek detected"** message confirms the command was processed

## Success Criteria

✅ FFmpeg starts without errors  
✅ ZMQ listener binds to tcp://127.0.0.1:5555  
✅ All commands (pause/resume/seek/reset) return "OK" responses  
✅ No mutex-related crashes or glibc assertions  
✅ Seek operations are detected and processed by demux thread  
✅ Rapid command execution is stable

## Cleanup After Testing

Once verified, we can remove the debug logging to clean up the output. The debug logs are in:
- `fftools/ffmpeg_demux.c` - `demux_alloc()` function
- `fftools/ffmpeg_zmq.c` - `zmq_thread_func()` reset command handler

## Files Changed in This Fix

1. **fftools/ffmpeg_zmq.c** (commit cf24bab5e1)
   - Changed `Timestamp` fields from `void*` to proper `Timestamp` struct
   - This corrects the 24-byte misalignment

2. **fftools/ffmpeg_zmq.c** (commit 8a772d12d8)
   - Removed duplicate `Timestamp` typedef
   - Now uses definition from `ffmpeg_utils.h`

3. **fftools/ffmpeg_zmq.h** (commit 8a772d12d8)
   - Fixed comment warning (`tcp://*:5555` → `tcp://127.0.0.1:5555`)

## Next Steps After Successful Testing

1. ✅ Verify struct alignment fix works
2. 🔄 Remove debug logging (if desired for cleaner output)
3. 🔄 Complete Phase 7-9: Testing, documentation, deployment
4. 🔄 Performance testing with real camera/file switching scenarios
5. 🔄 Integration with MediaMTX for WebRTC streaming

## Troubleshooting

### If the crash still occurs:
1. Check that you're running the correct binary: `./ffmpeg -version`
2. Verify the struct definition in `fftools/ffmpeg_zmq.c` line 70-100
3. Check for struct padding issues: `pahole ffmpeg` (if available)
4. Enable core dumps: `ulimit -c unlimited` and examine with `gdb`

### If ZMQ doesn't start:
1. Verify ZMQ is compiled in: `./ffmpeg -buildconf | grep libzmq`
2. Check if port 5555 is already in use: `netstat -tuln | grep 5555`
3. Try a different endpoint: `-zmq tcp://127.0.0.1:5556`
