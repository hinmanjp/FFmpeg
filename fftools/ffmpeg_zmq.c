/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * ZMQ Command Interface for Dynamic Input Control
 *
 * Provides a ZMQ REP socket listener that accepts runtime control commands
 * for individual input file demuxers. Supports pause, resume, seek, and reset.
 *
 * Command format: "<command> <input_id> [<optional_arg>]"
 * Examples:
 *   pause 0
 *   resume 0
 *   seek 0 30.5
 *   reset 0
 */

#include "config.h"

#if CONFIG_LIBZMQ
#include <zmq.h>
#endif

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "ffmpeg.h"
#include "ffmpeg_zmq.h"

#include "libavutil/avassert.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"

#if CONFIG_LIBZMQ

typedef struct ZMQContext {
    void            *zmq_context;
    void            *zmq_socket;
    pthread_t        zmq_thread;
    int              running;
    pthread_mutex_t  mutex;
} ZMQContext;

static ZMQContext *zmq_ctx = NULL;

// Demuxer is private to ffmpeg_demux.c, but InputFile is its first member
// so we can safely access the control fields via this forward declaration
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
    void                 *duration;
    void                 *min_pts;
    void                 *max_pts;
    int                   nb_streams_warn;
    float                 readrate;
    double                readrate_initial_burst;
    float                 readrate_catchup;
    void                 *sch;
    void                 *pkt_heartbeat;
    int                   read_started;
    int                   nb_streams_used;
    int                   nb_streams_finished;
    // Control fields we need
    int                   paused;
    int                   seek_requested;
    int64_t               seek_target;
    void                 *pause_frame;
    pthread_mutex_t       control_mutex;
} Demuxer;

static Demuxer *demuxer_from_ifile(InputFile *f)
{
    return (Demuxer*)f;
}

/**
 * ZMQ command listener thread function.
 * Runs in a loop receiving commands via ZMQ REP socket.
 */
static void *zmq_thread_func(void *arg)
{
    ZMQContext *ctx = (ZMQContext *)arg;
    char buffer[1024];
    int size;
    
    av_log(NULL, AV_LOG_INFO, "ZMQ command listener started\n");
    
    while (ctx->running) {
        // Receive command (blocking with timeout)
        size = zmq_recv(ctx->zmq_socket, buffer, sizeof(buffer) - 1, ZMQ_DONTWAIT);
        
        if (size == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                // No message available, sleep briefly
                av_usleep(10000); // 10ms
                continue;
            }
            av_log(NULL, AV_LOG_ERROR, "ZMQ recv error: %s\n", strerror(errno));
            continue;
        }
        
        buffer[size] = '\0';
        av_log(NULL, AV_LOG_INFO, "Received ZMQ command: %s\n", buffer);
        
        // Parse command: <cmd> <input_id> [<arg>]
        char cmd[64] = {0}, arg1[64] = {0}, arg2[256] = {0};
        int n = sscanf(buffer, "%63s %63s %255s", cmd, arg1, arg2);
        
        if (n < 2) {
            av_log(NULL, AV_LOG_WARNING, "Invalid command format: %s\n", buffer);
            zmq_send(ctx->zmq_socket, "ERROR: Invalid format. Use: <cmd> <input_id> [<arg>]", 52, 0);
            continue;
        }
        
        // Parse input_id
        char *endptr;
        long input_id_long = strtol(arg1, &endptr, 10);
        if (*endptr != '\0' || input_id_long < 0) {
            av_log(NULL, AV_LOG_WARNING, "Invalid input ID: %s\n", arg1);
            zmq_send(ctx->zmq_socket, "ERROR: Invalid input ID", 23, 0);
            continue;
        }
        int input_id = (int)input_id_long;
        
        // Validate input_id range
        if (input_id >= nb_input_files) {
            av_log(NULL, AV_LOG_WARNING, "Input ID %d out of range (nb_input_files=%d)\n", 
                   input_id, nb_input_files);
            zmq_send(ctx->zmq_socket, "ERROR: Input ID out of range", 28, 0);
            continue;
        }
        
        // Get demuxer for this input
        InputFile *ifile = input_files[input_id];
        Demuxer *d = demuxer_from_ifile(ifile);
        
        // Execute command
        if (strcmp(cmd, "pause") == 0) {
            pthread_mutex_lock(&d->control_mutex);
            d->paused = 1;
            pthread_mutex_unlock(&d->control_mutex);
            av_log(NULL, AV_LOG_INFO, "Paused input #%d\n", input_id);
            zmq_send(ctx->zmq_socket, "OK: Paused", 10, 0);
            
        } else if (strcmp(cmd, "resume") == 0) {
            pthread_mutex_lock(&d->control_mutex);
            d->paused = 0;
            pthread_mutex_unlock(&d->control_mutex);
            av_log(NULL, AV_LOG_INFO, "Resumed input #%d\n", input_id);
            zmq_send(ctx->zmq_socket, "OK: Resumed", 11, 0);
            
        } else if (strcmp(cmd, "seek") == 0) {
            if (n < 3) {
                av_log(NULL, AV_LOG_WARNING, "Seek command requires time argument\n");
                zmq_send(ctx->zmq_socket, "ERROR: Seek requires time argument", 34, 0);
                continue;
            }
            
            // Parse seek time (in seconds)
            double seek_time = atof(arg2);
            if (seek_time < 0) {
                av_log(NULL, AV_LOG_WARNING, "Invalid seek time: %f\n", seek_time);
                zmq_send(ctx->zmq_socket, "ERROR: Invalid seek time", 24, 0);
                continue;
            }
            
            // Convert to AV_TIME_BASE units
            int64_t seek_pos = (int64_t)(seek_time * AV_TIME_BASE);
            
            pthread_mutex_lock(&d->control_mutex);
            d->seek_requested = 1;
            d->seek_target = seek_pos;
            pthread_mutex_unlock(&d->control_mutex);
            
            av_log(NULL, AV_LOG_INFO, "Seek requested for input #%d to %.2f seconds\n", 
                   input_id, seek_time);
            zmq_send(ctx->zmq_socket, "OK: Seek requested", 18, 0);
            
        } else if (strcmp(cmd, "reset") == 0) {
            // Reset = seek to beginning and unpause
            pthread_mutex_lock(&d->control_mutex);
            d->seek_requested = 1;
            d->seek_target = 0;
            d->paused = 0;
            pthread_mutex_unlock(&d->control_mutex);
            
            av_log(NULL, AV_LOG_INFO, "Reset input #%d (seek to start)\n", input_id);
            zmq_send(ctx->zmq_socket, "OK: Reset", 9, 0);
            
        } else {
            av_log(NULL, AV_LOG_WARNING, "Unknown command: %s\n", cmd);
            zmq_send(ctx->zmq_socket, "ERROR: Unknown command. Use: pause, resume, seek, reset", 55, 0);
        }
    }
    
    av_log(NULL, AV_LOG_INFO, "ZMQ command listener stopped\n");
    return NULL;
}

int ffmpeg_zmq_init(const char *zmq_endpoint)
{
    int ret;
    
    av_log(NULL, AV_LOG_INFO, "Initializing ZMQ command interface on %s\n", zmq_endpoint);
    
    // Allocate context
    zmq_ctx = av_mallocz(sizeof(*zmq_ctx));
    if (!zmq_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Failed to allocate ZMQ context\n");
        return AVERROR(ENOMEM);
    }
    
    // Initialize ZMQ context
    zmq_ctx->zmq_context = zmq_ctx_new();
    if (!zmq_ctx->zmq_context) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create ZMQ context\n");
        av_freep(&zmq_ctx);
        return AVERROR_EXTERNAL;
    }
    
    // Create REP socket
    zmq_ctx->zmq_socket = zmq_socket(zmq_ctx->zmq_context, ZMQ_REP);
    if (!zmq_ctx->zmq_socket) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create ZMQ socket\n");
        zmq_ctx_destroy(zmq_ctx->zmq_context);
        av_freep(&zmq_ctx);
        return AVERROR_EXTERNAL;
    }
    
    // Set socket timeout
    int timeout = 100; // 100ms
    zmq_setsockopt(zmq_ctx->zmq_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Bind socket
    ret = zmq_bind(zmq_ctx->zmq_socket, zmq_endpoint);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to bind ZMQ socket to %s: %s\n", 
               zmq_endpoint, strerror(errno));
        zmq_close(zmq_ctx->zmq_socket);
        zmq_ctx_destroy(zmq_ctx->zmq_context);
        av_freep(&zmq_ctx);
        return AVERROR_EXTERNAL;    }
    
    // Initialize mutex without priority protocol to avoid TPP errors across threads
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE);
    pthread_mutex_init(&zmq_ctx->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Start listener thread
    zmq_ctx->running = 1;
    ret = pthread_create(&zmq_ctx->zmq_thread, NULL, zmq_thread_func, zmq_ctx);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create ZMQ thread: %s\n", strerror(ret));
        zmq_close(zmq_ctx->zmq_socket);
        zmq_ctx_destroy(zmq_ctx->zmq_context);
        pthread_mutex_destroy(&zmq_ctx->mutex);
        av_freep(&zmq_ctx);
        return AVERROR(ret);
    }
    
    av_log(NULL, AV_LOG_INFO, "ZMQ command interface initialized successfully\n");
    return 0;
}

void ffmpeg_zmq_cleanup(void)
{
    if (!zmq_ctx)
        return;
    
    av_log(NULL, AV_LOG_INFO, "Cleaning up ZMQ command interface\n");
    
    // Signal thread to stop
    pthread_mutex_lock(&zmq_ctx->mutex);
    zmq_ctx->running = 0;
    pthread_mutex_unlock(&zmq_ctx->mutex);
    
    // Wait for thread to finish
    pthread_join(zmq_ctx->zmq_thread, NULL);
    
    // Close socket and context
    if (zmq_ctx->zmq_socket)
        zmq_close(zmq_ctx->zmq_socket);
    if (zmq_ctx->zmq_context)
        zmq_ctx_destroy(zmq_ctx->zmq_context);
    
    // Destroy mutex
    pthread_mutex_destroy(&zmq_ctx->mutex);
    
    // Free context
    av_freep(&zmq_ctx);
    
    av_log(NULL, AV_LOG_INFO, "ZMQ command interface cleaned up\n");
}

#else // !CONFIG_LIBZMQ

int ffmpeg_zmq_init(const char *zmq_endpoint)
{
    av_log(NULL, AV_LOG_ERROR, "FFmpeg was not compiled with ZMQ support\n");
    return AVERROR(ENOSYS);
}

void ffmpeg_zmq_cleanup(void)
{
    // No-op when ZMQ is not compiled
}

#endif // CONFIG_LIBZMQ
