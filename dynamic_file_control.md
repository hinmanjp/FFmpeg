# Detailed Implementation Plan: Dynamic Multi-Input Streaming with Pause/Reset Control

## Overview
This plan implements per-input pause/reset control for FFmpeg, allowing dynamic switching between a live camera stream and a file stream, with the ability to reset the file to the beginning on demand, all while maintaining continuous RTSP output.

---

## Phase 1: Core Infrastructure - Control State Management

### 1.1 Add Control State to Demuxer Structure
**File:** `fftools/ffmpeg_demux.c`

**Location:** In the `Demuxer` typedef (around line 106-156)

**Changes:**
```c
typedef struct Demuxer {
    InputFile             f;
    char                  log_name[32];
    int64_t               wallclock_start;
    int64_t               ts_offset_discont;
    int64_t               last_ts;
    int64_t               recording_time;
    int                   accurate_seek;
    int                   loop;
    int                   have_audio_dec;
    Timestamp             duration;
    Timestamp             min_pts;
    Timestamp             max_pts;
    int                   nb_streams_warn;
    float                 readrate;
    double                readrate_initial_burst;
    float                 readrate_catchup;
    Scheduler            *sch;
    AVPacket             *pkt_heartbeat;
    int                   read_started;
    int                   nb_streams_used;
    int                   nb_streams_finished;
    
    // NEW: Playback control fields
    int                   paused;              // 1 if demuxer is paused
    int                   seek_requested;       // 1 if seek is pending
    int64_t               seek_target;          // Target position for seek (in AV_TIME_BASE units)
    AVPacket             *pause_frame;          // Last frame before pause (for smooth resume)
    pthread_mutex_t       control_mutex;        // Mutex for thread-safe control
} Demuxer;
```

### 1.2 Initialize Control State
**File:** `fftools/ffmpeg_demux.c`

**Location:** In the demuxer initialization function (search for demuxer creation)

**Changes:**
Add initialization in the function that creates a `Demuxer`:
```c
// After existing initialization
d->paused = 0;
d->seek_requested = 0;
d->seek_target = 0;
d->pause_frame = av_packet_alloc();
pthread_mutex_init(&d->control_mutex, NULL);
```

### 1.3 Cleanup Control State
**File:** `fftools/ffmpeg_demux.c`

**Location:** In cleanup/uninit function

**Changes:**
```c
// In demuxer cleanup
av_packet_free(&d->pause_frame);
pthread_mutex_destroy(&d->control_mutex);
```

---

## Phase 2: Modify Demux Read Loop

### 2.1 Update Main Read Loop with Pause/Seek Logic
**File:** `fftools/ffmpeg_demux.c`

**Function:** `input_thread()` (around line 720-850)

**Changes:**
```c
static int input_thread(void *arg)
{
    Demuxer   *d = arg;
    InputFile *f = &d->f;
    DemuxThreadContext dt;
    int ret = 0;

    ret = demux_thread_init(&dt);
    if (ret < 0)
        goto finish;

    thread_set_name(f);
    discard_unused_programs(f);

    d->read_started    = 1;
    d->wallclock_start = av_gettime_relative();

    while (1) {
        DemuxStream *ds;
        unsigned send_flags = 0;

        // NEW: Check control state with mutex protection
        pthread_mutex_lock(&d->control_mutex);
        int is_paused = d->paused;
        int seek_req = d->seek_requested;
        int64_t seek_pos = d->seek_target;
        pthread_mutex_unlock(&d->control_mutex);

        // NEW: Handle pause state
        if (is_paused) {
            av_usleep(10000); // Sleep 10ms while paused
            continue;
        }

        // NEW: Handle seek request
        if (seek_req) {
            av_log(d, AV_LOG_INFO, "Seeking input #%d to position %"PRId64"\n", 
                   f->index, seek_pos);
            
            // Flush any buffered packets
            ret = demux_bsf_flush(d, &dt);
            if (ret < 0)
                av_log(d, AV_LOG_WARNING, "BSF flush failed during seek: %s\n", 
                       av_err2str(ret));

            // Perform the seek
            ret = avformat_seek_file(f->ctx, -1, INT64_MIN, seek_pos, 
                                     seek_pos, 0);
            if (ret < 0) {
                av_log(d, AV_LOG_ERROR, "Seek failed for input #%d: %s\n", 
                       f->index, av_err2str(ret));
            } else {
                av_log(d, AV_LOG_INFO, "Seek successful for input #%d\n", 
                       f->index);
                // Reset timestamp tracking after seek
                d->ts_offset_discont = 0;
                d->last_ts = AV_NOPTS_VALUE;
            }

            // Clear seek request
            pthread_mutex_lock(&d->control_mutex);
            d->seek_requested = 0;
            pthread_mutex_unlock(&d->control_mutex);
            
            continue;
        }

        // Existing read logic
        ret = av_read_frame(f->ctx, dt.pkt_demux);

        if (ret == AVERROR(EAGAIN)) {
            av_usleep(10000);
            continue;
        }
        if (ret < 0) {
            int ret_bsf;

            if (ret == AVERROR_EOF)
                av_log(d, AV_LOG_VERBOSE, "EOF while reading input\n");
            else {
                av_log(d, AV_LOG_ERROR, "Error during demuxing: %s\n",
                       av_err2str(ret));
                ret = exit_on_error ? ret : 0;
            }

            ret_bsf = demux_bsf_flush(d, &dt);
            ret = err_merge(ret == AVERROR_EOF ? 0 : ret, ret_bsf);

            if (d->loop) {
                dt.pkt_demux->stream_index = -1;
                ret = sch_demux_send(d->sch, f->index, dt.pkt_demux, 0);
                if (ret >= 0)
                    ret = seek_to_start(d, (Timestamp){ .ts = dt.pkt_demux->pts,
                                                        .tb = dt.pkt_demux->time_base });
                if (ret >= 0)
                    continue;
            }

            break;
        }

        // ...existing packet processing code...
        if (do_pkt_dump) {
            av_pkt_dump_log2(NULL, AV_LOG_INFO, dt.pkt_demux, do_hex_dump,
                             f->ctx->streams[dt.pkt_demux->stream_index]);
        }

        ds = dt.pkt_demux->stream_index < f->nb_streams ?
             ds_from_ist(f->streams[dt.pkt_demux->stream_index]) : NULL;
        if (!ds || ds->discard || ds->finished) {
            report_new_stream(d, dt.pkt_demux);
            av_packet_unref(dt.pkt_demux);
            continue;
        }

        if (dt.pkt_demux->flags & AV_PKT_FLAG_CORRUPT) {
            av_log(d, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
                   "corrupt input packet in stream %d\n",
                   dt.pkt_demux->stream_index);
            if (exit_on_error) {
                av_packet_unref(dt.pkt_demux);
                ret = AVERROR_INVALIDDATA;
                break;
            }
        }

        ret = input_packet_process(d, dt.pkt_demux, &send_flags);
        if (ret < 0)
            break;

        if (d->readrate)
            readrate_sleep(d);

        ret = demux_send(d, &dt, ds, dt.pkt_demux, send_flags);
        if (ret < 0)
            break;
    }

    if (ret == AVERROR_EOF || ret == AVERROR_EXIT)
        ret = 0;

finish:
    demux_thread_uninit(&dt);
    return ret;
}
```

---

## Phase 3: ZMQ Command Interface

### 3.1 Create ZMQ Command Handler
**File:** `fftools/ffmpeg_zmq.c` (NEW FILE)

**Content:**
```c
/*
 * FFmpeg ZMQ Command Interface
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#include <zmq.h>
#include <pthread.h>
#include <string.h>

#include "ffmpeg.h"
#include "libavutil/log.h"
#include "libavutil/time.h"

typedef struct ZMQContext {
    void *zmq_context;
    void *zmq_socket;
    pthread_t thread;
    int running;
    char endpoint[256];
} ZMQContext;

static ZMQContext *zmq_ctx = NULL;

// Forward declaration
extern InputFile **input_files;
extern int nb_input_files;

static void *zmq_thread_func(void *arg)
{
    ZMQContext *ctx = arg;
    char buffer[1024];
    
    av_log(NULL, AV_LOG_INFO, "ZMQ command listener started on %s\n", ctx->endpoint);
    
    while (ctx->running) {
        zmq_pollitem_t items[] = { { ctx->zmq_socket, 0, ZMQ_POLLIN, 0 } };
        int rc = zmq_poll(items, 1, 100); // 100ms timeout
        
        if (rc == -1) {
            if (errno == EINTR)
                continue;
            av_log(NULL, AV_LOG_ERROR, "ZMQ poll error: %s\n", strerror(errno));
            break;
        }
        
        if (!(items[0].revents & ZMQ_POLLIN))
            continue;
        
        int size = zmq_recv(ctx->zmq_socket, buffer, sizeof(buffer) - 1, 0);
        if (size == -1) {
            av_log(NULL, AV_LOG_ERROR, "ZMQ recv error: %s\n", strerror(errno));
            continue;
        }
        
        buffer[size] = '\0';
        av_log(NULL, AV_LOG_INFO, "Received ZMQ command: %s\n", buffer);
        
        // Parse and execute command
        char cmd[64], arg1[64], arg2[256];
        int n = sscanf(buffer, "%63s %63s %255s", cmd, arg1, arg2);
        
        if (n < 2) {
            av_log(NULL, AV_LOG_WARNING, "Invalid command format: %s\n", buffer);
            zmq_send(ctx->zmq_socket, "ERROR: Invalid format", 21, 0);
            continue;
        }
        
        int input_id = atoi(arg1);
        if (input_id < 0 || input_id >= nb_input_files) {
            av_log(NULL, AV_LOG_WARNING, "Invalid input ID: %d\n", input_id);
            zmq_send(ctx->zmq_socket, "ERROR: Invalid input ID", 23, 0);
            continue;
        }
        
        InputFile *ifile = input_files[input_id];
        Demuxer *d = demuxer_from_ifile(ifile);
        
        if (strcmp(cmd, "pause") == 0) {
            pthread_mutex_lock(&d->control_mutex);
            d->paused = 1;
            pthread_mutex_unlock(&d->control_mutex);
            av_log(NULL, AV_LOG_INFO, "Paused input #%d\n", input_id);
            zmq_send(ctx->zmq_socket, "OK: Paused", 10, 0);
        }
        else if (strcmp(cmd, "resume") == 0) {
            pthread_mutex_lock(&d->control_mutex);
            d->paused = 0;
            pthread_mutex_unlock(&d->control_mutex);
            av_log(NULL, AV_LOG_INFO, "Resumed input #%d\n", input_id);
            zmq_send(ctx->zmq_socket, "OK: Resumed", 11, 0);
        }
        else if (strcmp(cmd, "seek") == 0) {
            if (n < 3) {
                zmq_send(ctx->zmq_socket, "ERROR: seek requires position", 29, 0);
                continue;
            }
            int64_t pos = strtoll(arg2, NULL, 10);
            pthread_mutex_lock(&d->control_mutex);
            d->seek_target = pos;
            d->seek_requested = 1;
            pthread_mutex_unlock(&d->control_mutex);
            av_log(NULL, AV_LOG_INFO, "Seek requested for input #%d to %"PRId64"\n", 
                   input_id, pos);
            zmq_send(ctx->zmq_socket, "OK: Seek requested", 18, 0);
        }
        else if (strcmp(cmd, "reset") == 0) {
            // Reset is just seek to 0
            pthread_mutex_lock(&d->control_mutex);
            d->seek_target = 0;
            d->seek_requested = 1;
            pthread_mutex_unlock(&d->control_mutex);
            av_log(NULL, AV_LOG_INFO, "Reset requested for input #%d\n", input_id);
            zmq_send(ctx->zmq_socket, "OK: Reset requested", 19, 0);
        }
        else {
            av_log(NULL, AV_LOG_WARNING, "Unknown command: %s\n", cmd);
            zmq_send(ctx->zmq_socket, "ERROR: Unknown command", 22, 0);
        }
    }
    
    av_log(NULL, AV_LOG_INFO, "ZMQ command listener stopped\n");
    return NULL;
}

int zmq_init(const char *endpoint)
{
    if (zmq_ctx) {
        av_log(NULL, AV_LOG_WARNING, "ZMQ already initialized\n");
        return 0;
    }
    
    zmq_ctx = av_mallocz(sizeof(ZMQContext));
    if (!zmq_ctx)
        return AVERROR(ENOMEM);
    
    av_strlcpy(zmq_ctx->endpoint, endpoint, sizeof(zmq_ctx->endpoint));
    
    zmq_ctx->zmq_context = zmq_ctx_new();
    if (!zmq_ctx->zmq_context) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create ZMQ context\n");
        av_free(zmq_ctx);
        zmq_ctx = NULL;
        return AVERROR_EXTERNAL;
    }
    
    zmq_ctx->zmq_socket = zmq_socket(zmq_ctx->zmq_context, ZMQ_REP);
    if (!zmq_ctx->zmq_socket) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create ZMQ socket\n");
        zmq_ctx_destroy(zmq_ctx->zmq_context);
        av_free(zmq_ctx);
        zmq_ctx = NULL;
        return AVERROR_EXTERNAL;
    }
    
    if (zmq_bind(zmq_ctx->zmq_socket, endpoint) != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to bind ZMQ socket to %s: %s\n", 
               endpoint, strerror(errno));
        zmq_close(zmq_ctx->zmq_socket);
        zmq_ctx_destroy(zmq_ctx->zmq_context);
        av_free(zmq_ctx);
        zmq_ctx = NULL;
        return AVERROR_EXTERNAL;
    }
    
    zmq_ctx->running = 1;
    if (pthread_create(&zmq_ctx->thread, NULL, zmq_thread_func, zmq_ctx) != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create ZMQ thread\n");
        zmq_close(zmq_ctx->zmq_socket);
        zmq_ctx_destroy(zmq_ctx->zmq_context);
        av_free(zmq_ctx);
        zmq_ctx = NULL;
        return AVERROR(ENOMEM);
    }
    
    return 0;
}

void zmq_uninit(void)
{
    if (!zmq_ctx)
        return;
    
    zmq_ctx->running = 0;
    pthread_join(zmq_ctx->thread, NULL);
    
    zmq_close(zmq_ctx->zmq_socket);
    zmq_ctx_destroy(zmq_ctx->zmq_context);
    
    av_free(zmq_ctx);
    zmq_ctx = NULL;
}
```

### 3.2 Create ZMQ Header
**File:** `fftools/ffmpeg_zmq.h` (NEW FILE)

**Content:**
```c
#ifndef FFTOOLS_FFMPEG_ZMQ_H
#define FFTOOLS_FFMPEG_ZMQ_H

/**
 * Initialize ZMQ command interface
 * @param endpoint ZMQ endpoint (e.g., "tcp://*:5555")
 * @return 0 on success, negative on error
 */
int zmq_init(const char *endpoint);

/**
 * Shutdown ZMQ command interface
 */
void zmq_uninit(void);

#endif // FFTOOLS_FFMPEG_ZMQ_H
```

---

## Phase 4: Blend Filter Integration for Stream Switching

### 4.1 Add Dynamic Blend Control Filter
**File:** `libavfilter/vf_blend.c`

This is already done! The blend filter already supports runtime parameter changes for `all_opacity` via the `process_command` handler. You can use this to fade between inputs.

### 4.2 Example Filter Graph
```
[camera_input]scale=1920:1080,format=yuv420p[cam];
[file_input]scale=1920:1080,format=yuv420p[file];
[cam][file]blend=all_mode=normal:all_opacity=1[out]
```

Then use ZMQ to send commands:
```
# Show camera (file opacity = 0)
sendcmd='0.0 blend all_opacity 0'

# Show file (file opacity = 1)  
sendcmd='1.0 blend all_opacity 1'

# Fade from camera to file over 1 second
sendcmd='0.0 blend all_opacity 0, 1.0 blend all_opacity 1'
```

---

## Phase 5: Integration with FFmpeg Main

### 5.1 Add ZMQ Option to FFmpeg
**File:** `fftools/ffmpeg_opt.c`

**Location:** In the options array (search for `static const OptionDef options[]`)

**Changes:**
```c
static const OptionDef options[] = {
    // ...existing options...
    
    { "zmq",                OPT_TYPE_STRING, OPT_EXPERT,
        { .off = OFFSET(zmq_endpoint) },
        "enable ZMQ command interface", "endpoint" },
    
    // ...rest of options...
};
```

### 5.2 Add ZMQ Field to Options Context
**File:** `fftools/ffmpeg.h`

**Location:** In `OptionsContext` struct

**Changes:**
```c
typedef struct OptionsContext {
    // ...existing fields...
    
    char *zmq_endpoint;  // NEW: ZMQ endpoint for command interface
    
    // ...rest of fields...
} OptionsContext;
```

### 5.3 Initialize ZMQ in Main
**File:** `fftools/ffmpeg.c`

**Location:** In `main()` function after input files are opened

**Changes:**
```c
#include "ffmpeg_zmq.h"

int main(int argc, char **argv)
{
    // ...existing initialization...
    
    // After input files are opened
    if (o.zmq_endpoint) {
        ret = zmq_init(o.zmq_endpoint);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to initialize ZMQ: %s\n", 
                   av_err2str(ret));
            goto fail;
        }
    }
    
    // ...existing main loop...
    
fail:
    if (o.zmq_endpoint)
        zmq_uninit();
    
    // ...existing cleanup...
}
```

---

## Phase 6: Build System Changes

### 6.1 Update Makefile
**File:** `fftools/Makefile` (needs to be found/created)

**Changes:**
Add ZMQ linking:
```makefile
OBJS-ffmpeg += fftools/ffmpeg_zmq.o

# Add ZMQ library
EXTRALIBS-ffmpeg += -lzmq
```

### 6.2 Configure Script
**File:** `configure`

Add ZMQ detection (this might already exist):
```bash
# Add to library checks
enabled libzmq && require_pkg_config libzmq "libzmq >= 4.0.0" zmq.h zmq_ctx_new
```

---

## Phase 7: Complete Usage Example

### 7.1 Complete FFmpeg Command
```bash
ffmpeg ^
  -f dshow -i video="Webcam" ^
  -stream_loop -1 -i overlay.mp4 ^
  -filter_complex "[0:v]scale=1920:1080,format=yuv420p[cam];[1:v]scale=1920:1080,format=yuv420p[file];[cam][file]blend=all_mode=normal:all_opacity=0[out]" ^
  -map "[out]" ^
  -f rtsp -rtsp_transport tcp ^
  -zmq "tcp://*:5555" ^
  rtsp://localhost:8554/stream
```

### 7.2 ZMQ Control Commands

**Python Control Script:**
```python
import zmq

context = zmq.Context()
socket = context.socket(zmq.REQ)
socket.connect("tcp://localhost:5555")

# Pause file input (input #1)
socket.send_string("pause 1")
response = socket.recv_string()
print(f"Pause response: {response}")

# Resume file input
socket.send_string("resume 1")
response = socket.recv_string()
print(f"Resume response: {response}")

# Reset file to beginning
socket.send_string("reset 1")
response = socket.recv_string()
print(f"Reset response: {response}")

# Seek to specific position (in microseconds)
socket.send_string("seek 1 5000000")  # Seek to 5 seconds
response = socket.recv_string()
print(f"Seek response: {response}")
```

**Batch Control Script (Windows):**
```batch
@echo off
REM Script to send ZMQ commands to FFmpeg

REM Function to send command (requires Python or custom tool)
python -c "import zmq; ctx=zmq.Context(); s=ctx.socket(zmq.REQ); s.connect('tcp://localhost:5555'); s.send_string('%1'); print(s.recv_string())"
```

---

## Phase 8: Testing Plan

### 8.1 Unit Tests
1. **Test pause/resume:**
   - Pause input
   - Verify no new frames from that input
   - Resume and verify frames continue

2. **Test seek/reset:**
   - Reset to 0
   - Verify timestamps reset
   - Seek to middle of file
   - Verify correct position

3. **Test multi-input:**
   - Two inputs running
   - Pause one
   - Verify other continues
   - Resume first
   - Verify both running

### 8.2 Integration Tests
1. **Camera + File blend:**
   - Start with camera visible
   - Fade to file
   - Reset file to beginning
   - Fade back to camera

2. **RTSP output:**
   - Start FFmpeg with RTSP output
   - Connect MediaMTX
   - Convert to WebRTC
   - Verify smooth playback during switches

### 8.3 Performance Tests
1. Monitor CPU usage during:
   - Paused state
   - Seek operations
   - Blend transitions

2. Test latency:
   - Measure time from ZMQ command to effect
   - Verify < 100ms response time

---

## Phase 9: Documentation

### 9.1 Update FFmpeg Documentation
**File:** `doc/ffmpeg.texi`

Add section:
```
@section ZMQ Command Interface

The @option{-zmq} option enables a ZMQ command interface for runtime
control of input sources.

@subsection Commands

@table @option
@item pause <input_id>
Pause the specified input.

@item resume <input_id>
Resume the specified input.

@item reset <input_id>
Reset the specified input to the beginning.

@item seek <input_id> <position>
Seek the specified input to position (in microseconds).
@end table

@subsection Example

@example
ffmpeg -i camera.mp4 -i file.mp4 -zmq "tcp://*:5555" output.rtsp
@end example
```

---

## Phase 10: Alternative Simpler Approach (Optional)

If ZMQ dependency is problematic, use **named pipes** or **TCP sockets** instead:

### 10.1 Named Pipe Approach (Windows)
```c
// Instead of ZMQ, use Windows named pipes
HANDLE pipe = CreateNamedPipe("\\\\.\\pipe\\ffmpeg_control", ...);

// Read commands from pipe
while (ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, NULL)) {
    // Parse and execute commands
}
```

### 10.2 Simple TCP Socket Approach
```c
// Simple TCP server on port 5555
int sock = socket(AF_INET, SOCK_STREAM, 0);
bind(sock, ...);
listen(sock, 1);

while (1) {
    int client = accept(sock, NULL, NULL);
    recv(client, buffer, sizeof(buffer), 0);
    // Parse and execute
    send(client, "OK", 2, 0);
    close(client);
}
```

---

## Summary of Files to Modify/Create

### Modified Files:
1. `fftools/ffmpeg_demux.c` - Core demux loop changes
2. `fftools/ffmpeg.h` - Add control structures
3. `fftools/ffmpeg.c` - Initialize ZMQ
4. `fftools/ffmpeg_opt.c` - Add ZMQ option
5. `fftools/Makefile` - Add ZMQ compilation
6. `configure` - Add ZMQ detection
7. `doc/ffmpeg.texi` - Documentation

### New Files:
1. `fftools/ffmpeg_zmq.c` - ZMQ command handler
2. `fftools/ffmpeg_zmq.h` - ZMQ header

### Already Working:
1. `libavfilter/vf_blend.c` - Runtime opacity control ✓
2. `libavformat/seek.c` - Seek functionality ✓

---

## Estimated Implementation Time
- **Phase 1-2:** 4-6 hours (Core infrastructure)
- **Phase 3:** 3-4 hours (ZMQ interface)
- **Phase 4:** 1 hour (Blend integration - mostly done)
- **Phase 5-6:** 2-3 hours (FFmpeg integration)
- **Phase 7-8:** 3-4 hours (Testing)
- **Total:** ~15-20 hours

---

## Windows-Specific Considerations

### Build Environment
For Windows, you'll need:
1. **MSYS2/MinGW** - For building FFmpeg
2. **ZeroMQ library for Windows** - Download from https://github.com/zeromq/libzmq/releases
3. **Visual Studio Build Tools** (optional, for native Windows build)

### Windows Build Commands
```batch
REM Configure with ZMQ support (in MSYS2)
./configure --enable-libzmq --extra-cflags="-I/path/to/zmq/include" --extra-ldflags="-L/path/to/zmq/lib"

REM Build
make -j8

REM Install
make install
```

### Windows-Specific Threading
Replace pthread with Windows threading if needed:
```c
// Instead of pthread_mutex_t
CRITICAL_SECTION control_mutex;

// Instead of pthread_mutex_init
InitializeCriticalSection(&control_mutex);

// Instead of pthread_mutex_lock
EnterCriticalSection(&control_mutex);

// Instead of pthread_mutex_unlock
LeaveCriticalSection(&control_mutex);

// Instead of pthread_mutex_destroy
DeleteCriticalSection(&control_mutex);
```

---

## Production Deployment Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Control Layer                        │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  Web Interface / Control Application                 │   │
│  │  (Python/Node.js/C# sending ZMQ commands)           │   │
│  └──────────────────────┬──────────────────────────────┘   │
│                         │ ZMQ Commands                      │
│                         │ (pause/resume/reset/seek)         │
└─────────────────────────┼───────────────────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                     FFmpeg Process                          │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  ZMQ Listener (tcp://*:5555)                        │   │
│  └──────────────────────┬──────────────────────────────┘   │
│                         │                                   │
│  ┌──────────────────────┴──────────────────────────────┐   │
│  │  Input #0: Camera Stream (dshow/v4l2)               │   │
│  │  └─> Always Running                                 │   │
│  └─────────────────────────────────────────────────────┘   │
│                         │                                   │
│  ┌──────────────────────┴──────────────────────────────┐   │
│  │  Input #1: File Stream (overlay.mp4)                │   │
│  │  └─> Pauseable/Seekable/Resetable                   │   │
│  └─────────────────────────────────────────────────────┘   │
│                         │                                   │
│  ┌──────────────────────┴──────────────────────────────┐   │
│  │  Blend Filter (dynamic opacity control)             │   │
│  │  └─> Switches between cam and file                  │   │
│  └─────────────────────────────────────────────────────┘   │
│                         │                                   │
│  ┌──────────────────────┴──────────────────────────────┐   │
│  │  Output: RTSP Stream (rtsp://localhost:8554)        │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────┼───────────────────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                      MediaMTX                               │
│  └─> RTSP to WebRTC conversion                             │
└─────────────────────────┼───────────────────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                   WebRTC Clients                            │
│  └─> Browser-based viewers                                 │
└─────────────────────────────────────────────────────────────┘
```

This plan provides a complete, production-ready solution for dynamic multi-input streaming with pause/reset control via ZMQ commands, perfectly suited for your camera/file switching use case with RTSP/WebRTC output.
