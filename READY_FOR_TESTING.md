# Struct Alignment Fix - Ready for Linux Build Testing

## Summary
All code fixes have been committed and pushed to the `expand_zmq_support` branch. The critical struct alignment issue has been resolved.

## What Was Fixed

### The Problem
- **Location**: `fftools/ffmpeg_zmq.c`, `Demuxer` struct definition
- **Bug**: `Timestamp` fields declared as `void*` (8 bytes) instead of `Timestamp` struct (16 bytes)
- **Impact**: 24-byte misalignment causing `control_mutex` to be accessed at wrong memory offset
- **Symptom**: Fatal glibc TPP crash when executing "reset 2" command

### The Solution
- **Commit cf24bab5e1**: Changed `duration`, `min_pts`, `max_pts` from `void*` to `Timestamp`
- **Commit 8a772d12d8**: Removed duplicate typedef, now uses `Timestamp` from `ffmpeg_utils.h`
- **Result**: Correct 16-byte alignment for each `Timestamp` field

## Current State

### Version
```
8.0-zmq-9-FIXED
```

### Recent Commits (in order)
1. `e5941bc196` - Fix TPP crash: disable priority protocol on control mutexes
2. `8a3e6665fc` - Add detailed mutex debugging logs
3. `cf24bab5e1` - **CRITICAL FIX**: Correct Demuxer struct alignment
4. `8a772d12d8` - Remove duplicate Timestamp typedef and fix comment warning
5. `1d0db7f8f2` - Add comprehensive testing guide for struct alignment fix

### Files Modified
- ✅ `fftools/ffmpeg_zmq.c` - Struct alignment corrected
- ✅ `fftools/ffmpeg_zmq.h` - Comment warning fixed
- ✅ `RELEASE` - Version bumped to 8.0-zmq-9-FIXED
- ✅ `TESTING_STRUCT_FIX.md` - Comprehensive testing guide added

## Next Steps on Linux Build Server

### 1. Pull and Build
```bash
cd /path/to/FFmpeg
git checkout expand_zmq_support
git pull origin expand_zmq_support
./configure --enable-libzmq
make clean && make -j$(nproc)
```

### 2. Run Critical Test
```bash
# Start FFmpeg with ZMQ (in background)
./ffmpeg -zmq tcp://127.0.0.1:5555 \
  -re -stream_loop -1 -i file1.mp4 \
  -re -stream_loop -1 -i file2.mp4 \
  -re -stream_loop -1 -i file3.mp4 \
  -map 0:v -map 0:a -c copy \
  -f rtsp rtsp://localhost:8554/stream &

# Send reset command (this was crashing before)
python3 -c "
import zmq
ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect('tcp://127.0.0.1:5555')
sock.send_string('reset 2')
print(sock.recv_string())  # Should print: OK: Reset
"
```

### 3. Expected Outcome
- ✅ No crash
- ✅ Response: "OK: Reset"
- ✅ Debug log shows successful mutex lock/unlock
- ✅ Demux thread detects seek request

## Testing Documentation
See `TESTING_STRUCT_FIX.md` for complete testing procedures including:
- All ZMQ commands (pause/resume/seek/reset)
- Stress testing with rapid commands
- Expected debug output
- Troubleshooting guide

## After Successful Testing

### Optional: Remove Debug Logging
If the fix is verified, we can clean up verbose debug logs:
- `fftools/ffmpeg_demux.c` - demux_alloc() function
- `fftools/ffmpeg_zmq.c` - zmq_thread_func() reset handler

### Proceed to Phase 7-9
- Complete integration testing
- Update documentation
- Deploy to production

## Key Technical Details

### Timestamp Struct Definition (from ffmpeg_utils.h)
```c
typedef struct Timestamp {
    int64_t    ts;        // 8 bytes
    AVRational tb;        // 8 bytes (2 × int32_t)
} Timestamp;              // Total: 16 bytes
```

### Demuxer Struct Layout (Corrected)
```c
typedef struct Demuxer {
    InputFile             f;                    // Offset 0
    char                  log_name[32];         // +variable
    int64_t               wallclock_start;      // +32
    // ... more fields ...
    Timestamp             duration;             // +X (16 bytes)
    Timestamp             min_pts;              // +X+16 (16 bytes)
    Timestamp             max_pts;              // +X+32 (16 bytes)
    // ... more fields ...
    pthread_mutex_t       control_mutex;        // Now at CORRECT offset!
} Demuxer;
```

### Why This Matters
- **Before**: Each `Timestamp` was 8 bytes → mutex offset was 24 bytes too early
- **After**: Each `Timestamp` is 16 bytes → mutex offset is correct
- **Result**: Mutex initialization and access now use the same memory location

## Contact
If the test fails or there are any issues, please provide:
1. Full FFmpeg log output (with debug messages)
2. Output of `git log --oneline -5`
3. Output of `./ffmpeg -version`
4. Core dump (if crash occurs): `gdb ./ffmpeg core`
