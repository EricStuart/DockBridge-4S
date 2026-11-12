#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "btstack_audio.h"
#include "btstack_run_loop.h"
#include "bridge_ipc.h"

// The iOS 6 SDK exports these IOKit functions without their declarations.
extern int IOPMAssertionCreateWithName(CFStringRef type, uint32_t level, CFStringRef name, uint32_t *id);
extern int IOPMAssertionRelease(uint32_t id);

enum { BUFFER_COUNT = 3, STALL_MS = 1000, RETRY_MS = 2000 };
static AudioQueueRef queue;
static AudioQueueBufferRef buffers[BUFFER_COUNT];
static AudioStreamBasicDescription format;
static void (*render_pcm)(int16_t *, uint16_t);
static UInt32 buffer_bytes;
static uint32_t sleep_assertion, last_render, retry_at;
static uint8_t volume = 127;
static int running, requested, paused, interrupted, reset_output, session_active, retry_pending;
static CFSocketRef relay_listener, relay_client;
static uint32_t relay_activity, relay_frames;
static int relay_playing;

static void keep_system_awake(void) {
    if (sleep_assertion) return;
    uint32_t id = 0;
    int result = IOPMAssertionCreateWithName(CFSTR("PreventUserIdleSystemSleep"), 255,
                                              CFSTR("DockBridge Bluetooth audio"), &id);
    if (result) fprintf(stderr, "Audio sleep assertion failed: 0x%08x\n", (unsigned)result);
    else {
        sleep_assertion = id;
        fprintf(stderr, "Audio sleep assertion acquired.\n");
    }
}

static void allow_system_sleep(void) {
    if (!sleep_assertion) return;
    int result = IOPMAssertionRelease(sleep_assertion);
    if (result) fprintf(stderr, "Audio sleep assertion release failed: 0x%08x\n", (unsigned)result);
    else sleep_assertion = 0;
}

static void fill_buffer(void *context, AudioQueueRef output, AudioQueueBufferRef buffer) {
    (void)context;
    if (!running || output != queue) return;
    memset(buffer->mAudioData, 0, buffer_bytes);
    render_pcm(buffer->mAudioData, DB_AUDIO_FRAMES);
    buffer->mAudioDataByteSize = buffer_bytes;
    OSStatus status = AudioQueueEnqueueBuffer(output, buffer, 0, NULL);
    if (status) fprintf(stderr, "AudioQueueEnqueueBuffer: %d\n", (int)status);
    else last_render = btstack_run_loop_get_time_ms();
}

static void dispose_output(void) {
    running = 0;
    if (queue) {
        AudioQueueDispose(queue, true);
        queue = NULL;
    }
    if (session_active) {
        AudioSessionSetActive(false);
        session_active = 0;
    }
}

static void close_relay_client(void) {
    if (!relay_client) return;
    CFSocketInvalidate(relay_client);
    CFRelease(relay_client);
    relay_client = NULL;
    if (relay_playing) fprintf(stderr, "App audio relay closed after %u buffers.\n", (unsigned)relay_frames);
    relay_playing = 0;
    relay_frames = 0;
}

static void relay_pull(CFSocketRef socket, CFSocketCallBackType type, CFDataRef address,
                       const void *data, void *context) {
    (void)type; (void)address; (void)data; (void)context;
    int fd = CFSocketGetNative(socket);
    uint8_t request;
    ssize_t count = recv(fd, &request, sizeof(request), 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (!requested || count != sizeof(request)) {
        close_relay_client();
        return;
    }
    switch (request) {
        case DB_AUDIO_STARTED_LINE_OUT:
        case DB_AUDIO_STARTED_OTHER_ROUTE:
            fprintf(stderr, "App audio playback started, route: %s.\n",
                    request == DB_AUDIO_STARTED_LINE_OUT ? "LineOut" : "other");
            if (!paused) dockbridge_report(DB_EVENT_AUDIO_STARTED);
            return;
        case DB_AUDIO_BACKGROUNDED:
            fprintf(stderr, "App entered background after %u audio buffers.\n", (unsigned)relay_frames);
            return;
        case DB_AUDIO_INTERRUPTED:
            fprintf(stderr, "App audio session interrupted after %u buffers.\n", (unsigned)relay_frames);
            return;
        case DB_AUDIO_PULL:
            break;
        default:
            close_relay_client();
            return;
    }
    if (!relay_playing) {
        dispose_output();
        relay_playing = 1;
        fprintf(stderr, "App audio relay connected.\n");
    }
    db_audio_frame frame = { .sample_rate = (uint32_t)format.mSampleRate, .volume = volume };
    if (!paused) render_pcm(frame.samples, DB_AUDIO_FRAMES);
    if (send(fd, &frame, sizeof(frame), 0) != sizeof(frame)) {
        close_relay_client();
        return;
    }
    ++relay_frames;
    relay_activity = btstack_run_loop_get_time_ms();
}

// CFSocket callbacks run on the receiver loop, which also owns the SBC buffers.
static CFSocketRef relay_read_source(int fd, CFSocketCallBack callback) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) {
        close(fd);
        return NULL;
    }
    CFSocketRef socket = CFSocketCreateWithNative(NULL, fd, kCFSocketReadCallBack, callback, NULL);
    if (!socket) {
        close(fd);
        return NULL;
    }
    CFRunLoopSourceRef source = CFSocketCreateRunLoopSource(NULL, socket, 0);
    if (!source) {
        CFSocketInvalidate(socket);
        CFRelease(socket);
        return NULL;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRelease(source);
    return socket;
}

static void relay_accept(CFSocketRef socket, CFSocketCallBackType type, CFDataRef address,
                         const void *data, void *context) {
    (void)type; (void)address; (void)data; (void)context;
    int fd = accept(CFSocketGetNative(socket), NULL, NULL);
    if (fd < 0) return;
    uid_t uid;
    gid_t gid;
    int ignore = 1;
    if (relay_client || !requested || format.mChannelsPerFrame != DB_AUDIO_CHANNELS ||
        getpeereid(fd, &uid, &gid) || (uid != 501 && uid != 0) ||
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &ignore, sizeof(ignore))) {
        close(fd);
        return;
    }
    relay_client = relay_read_source(fd, relay_pull);
    relay_activity = btstack_run_loop_get_time_ms();
}

void dockbridge_audio_open_relay(void) {
    struct stat info;
    if (lstat(DB_SOCKET_DIR, &info) || !S_ISDIR(info.st_mode) || info.st_uid != 0 || (info.st_mode & 022)) return;
    if (!lstat(DB_AUDIO_SOCKET_PATH, &info) &&
        (!S_ISSOCK(info.st_mode) || info.st_uid != 0 || unlink(DB_AUDIO_SOCKET_PATH))) return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = DB_AUDIO_SOCKET_PATH };
    address.sun_len = SUN_LEN(&address);
    if (bind(fd, (struct sockaddr *)&address, address.sun_len)) {
        close(fd);
        return;
    }
    if (chmod(DB_AUDIO_SOCKET_PATH, 0666) || listen(fd, 1)) close(fd);
    else relay_listener = relay_read_source(fd, relay_accept);
    if (!relay_listener) unlink(DB_AUDIO_SOCKET_PATH);
}

void dockbridge_audio_close_relay(void) {
    close_relay_client();
    if (!relay_listener) return;
    CFSocketInvalidate(relay_listener);
    CFRelease(relay_listener);
    relay_listener = NULL;
    unlink(DB_AUDIO_SOCKET_PATH);
}

static OSStatus create_output(void) {
    OSStatus status = AudioQueueNewOutput(&format, fill_buffer, NULL,
                                         CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 0, &queue);
    for (unsigned i = 0; !status && i < BUFFER_COUNT; ++i) {
        status = AudioQueueAllocateBuffer(queue, buffer_bytes, &buffers[i]);
    }
    return status;
}

static void set_volume(uint8_t level) {
    volume = level;
    if (queue) AudioQueueSetParameter(queue, kAudioQueueParam_Volume, volume / 127.0f);
}

static void stop_audio(void) {
    if (requested && !paused) dockbridge_report(DB_EVENT_AUDIO_STOPPED);
    requested = paused = reset_output = retry_pending = 0;
    close_relay_client();
    dispose_output();
    allow_system_sleep();
}

static void pause_audio(void) {
    if (paused) return;
    if (!relay_playing) {
        stop_audio();
        return;
    }
    // Keep the App's output available for a Bluetooth Play command while locked.
    paused = 1;
    dockbridge_report(DB_EVENT_AUDIO_STOPPED);
    fprintf(stderr, "App audio relay retained during pause.\n");
}

static void close_audio(void) {
    stop_audio();
    render_pcm = NULL;
}

static int init_audio(uint8_t channels, uint32_t rate, void (*playback)(int16_t *, uint16_t)) {
    close_audio();
    render_pcm = playback;
    buffer_bytes = DB_AUDIO_FRAMES * channels * sizeof(int16_t);
    format = (AudioStreamBasicDescription){
        .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mFramesPerPacket = 1, .mBytesPerFrame = channels * sizeof(int16_t),
        .mBytesPerPacket = channels * sizeof(int16_t), .mChannelsPerFrame = channels,
        .mBitsPerChannel = 16,
    };
    fprintf(stderr, "PCM output: %u Hz, %u channels\n", (unsigned)rate, channels);
    return 0;
}

static OSStatus start_output(void) {
    OSStatus status = AudioSessionSetActive(true);
    if (status) {
        fprintf(stderr, "AudioSessionSetActive: %d\n", (int)status);
        return status;
    }
    session_active = 1;
    status = create_output();
    if (status) {
        fprintf(stderr, "Audio output initialization failed: %d\n", (int)status);
        return status;
    }
    CFStringRef route = NULL;
    UInt32 route_size = sizeof(route);
    if (AudioSessionGetProperty(kAudioSessionProperty_AudioRoute, &route_size, &route) == noErr && route) {
        char name[160];
        if (CFStringGetCString(route, name, sizeof(name), kCFStringEncodingUTF8)) {
            fprintf(stderr, "Audio route: %s\n", name);
        }
    }
    set_volume(volume);
    running = 1;
    last_render = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) fill_buffer(NULL, queue, buffers[i]);
    status = AudioQueueStart(queue, NULL);
    if (status) fprintf(stderr, "AudioQueueStart: %d\n", (int)status);
    return status;
}

static void try_output(uint32_t now) {
    keep_system_awake();
    if (start_output()) {
        dispose_output();
        allow_system_sleep();
        dockbridge_report(DB_EVENT_AUDIO_ERROR);
        retry_at = now + RETRY_MS;
        retry_pending = 1;
    } else {
        retry_pending = 0;
        dockbridge_report(DB_EVENT_AUDIO_STARTED);
    }
}

static void start_audio(void) {
    if (!render_pcm || (requested && !paused)) return;
    paused = 0;
    if (relay_playing) {
        dockbridge_report(DB_EVENT_AUDIO_STARTED);
        fprintf(stderr, "App audio resumed on the existing relay.\n");
        return;
    }
    requested = 1;
    if (!interrupted && !reset_output) try_output(btstack_run_loop_get_time_ms());
}

void dockbridge_audio_interruption(void *context, UInt32 state) {
    (void)context;
    interrupted = state == kAudioSessionBeginInterruption;
    if (!requested) return;
    fprintf(stderr, "Audio session interruption %s.\n", interrupted ? "began" : "ended");
    if (interrupted) {
        session_active = 0;
        reset_output = 1;
    }
    retry_pending = 0;
}

void dockbridge_audio_tick(uint32_t now) {
    if (!requested) return;
    if (relay_client && (int32_t)(now - relay_activity) >= STALL_MS) {
        fprintf(stderr, "App audio relay timed out.\n");
        close_relay_client();
    }
    if (relay_playing) return;
    if (paused) {
        stop_audio();
        return;
    }
    // Tear down and restart on the worker tick, outside the AudioSession callback.
    if (reset_output) {
        reset_output = 0;
        dispose_output();
        allow_system_sleep();
        dockbridge_report(DB_EVENT_AUDIO_STOPPED);
    }
    if (interrupted) return;
    if (running) {
        if ((int32_t)(now - last_render) < STALL_MS) return;
        fprintf(stderr, "Audio output stalled; reopening AudioQueue.\n");
        dispose_output();
    }
    if (!retry_pending || (int32_t)(now - retry_at) >= 0) try_output(now);
}

const btstack_audio_sink_t dockbridge_audio_sink = {
    .init = init_audio, .set_volume = set_volume, .start_stream = start_audio,
    .stop_stream = pause_audio, .close = close_audio
};
