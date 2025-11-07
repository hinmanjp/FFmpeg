# URGENT: Deploy v12-debug to See What's Happening

## Problem
v11 is deployed but the discontinuity signal isn't being sent (no logs visible).

## Solution: Deploy v12-debug

This version has **extensive logging** around the discontinuity signal to see exactly what's happening.

### Deploy v12-debug

```bash
cd /home/peter/ffmpeg
git pull origin expand_zmq_support
make clean
./configure --enable-libzmq
make -j$(nproc)

# Verify version
./ffmpeg -version | head -1
# Should show: v8.0-zmq-12-debug
```

### Run Test

```bash
./ffmpeg -f v4l2 -i /dev/video2 \
         -f alsa -i hw:1 \
         -i risen_15k.mp4 \
         -filter_complex "[0:v]scale=1920:1080[v0];[2:v]scale=1920:1080[v2];[v0][v2]zmq=bind_address=tcp\\://127.0.0.1\\:5556[vout];[1:a][2:a]amerge=inputs=2,pan=stereo|c0=c0|c1=c1,azmq=bind_address=tcp\\://127.0.0.1\\:5557[aout]" \
         -map "[vout]" -map "[aout]" \
         -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -sc_threshold 0 \
         -b:v 2500k -maxrate 2500k -bufsize 1250k -pix_fmt yuv420p \
         -c:a libopus -b:a 96k \
         -f rtsp -rtsp_transport tcp rtsp://localhost:8554/test \
         -zmq tcp://127.0.0.1:5555 2>&1 | tee ffmpeg_debug.log
```

### Send Reset

```bash
echo "reset 2" | nc localhost 5555
```

### Expected New Logs

After "Seek successful", you should now see:
```
[in#2] Demuxer buffers flushed for input #2
[in#2] Sending discontinuity signal for input #2 (stream_index=-1)
[in#2] Discontinuity signal sent, result: SUCCESS
[in#2] Timestamp tracking reset for input #2
```

### If You DON'T See These Logs

Then the code isn't being executed, which means:
1. The build pulled old source (check with `git log --oneline -1`)
2. Or there's a compiler issue
3. Or the code path isn't reached

### Verify Source

```bash
# Check the fix is present
grep -A2 "stream_index = -1" fftools/ffmpeg_demux.c
grep -A2 "Sending discontinuity signal" fftools/ffmpeg_demux.c
```

You should see the new logging code.
