#import <AudioToolbox/AudioToolbox.h>
#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "bridge_ipc.h"

enum { APP_AUDIO_BUFFERS = 12, APP_AUDIO_IN_FLIGHT = 2 };
static dispatch_queue_t audio_io;
static dispatch_source_t audio_source;
static int audio_fd = -1;
static AudioQueueRef output;
static AudioQueueBufferRef pending[APP_AUDIO_BUFFERS];
static unsigned head, pending_count, in_flight;
static db_audio_frame frame;
static size_t frame_bytes;
static uint32_t sample_rate, output_volume;
static uintptr_t generation;
static BOOL session_configured, session_active, session_interrupted, output_started;

static BOOL send_command(uint8_t command) {
    return send(audio_fd, &command, sizeof(command), 0) == sizeof(command);
}

// All mutable playback and socket state is owned by audio_io.
static void stop_output(void) {
    ++generation;
    if (output) {
        AudioQueueDispose(output, true);
        output = NULL;
    }
    if (session_active) {
        session_active = NO;
        AudioSessionSetActive(false);
    }
    if (audio_source) {
        dispatch_source_cancel(audio_source);
        dispatch_release(audio_source);
        audio_source = NULL;
    }
    audio_fd = -1;
    head = pending_count = in_flight = 0;
    frame_bytes = 0;
    output_started = NO;
}

static void session_interruption(void *context, UInt32 state) {
    (void)context;
    dispatch_async(audio_io, ^{
        session_interrupted = state == kAudioSessionBeginInterruption;
        if (session_interrupted) {
            if (audio_fd >= 0) send_command(DB_AUDIO_INTERRUPTED);
            session_active = NO;
            stop_output();
        }
    });
}

static BOOL request_buffers(void) {
    while (in_flight < APP_AUDIO_IN_FLIGHT && in_flight < pending_count) {
        if (!send_command(DB_AUDIO_PULL)) return NO;
        ++in_flight;
    }
    return YES;
}

static void refill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    uintptr_t callback_generation = (uintptr_t)context;
    dispatch_async(audio_io, ^{
        if (callback_generation != generation || queue != output) return;
        if (pending_count == APP_AUDIO_BUFFERS) {
            stop_output();
            return;
        }
        pending[(head + pending_count) % APP_AUDIO_BUFFERS] = buffer;
        ++pending_count;
        if (!request_buffers()) stop_output();
    });
}

static BOOL create_output(void) {
    if (!session_configured) {
        OSStatus status = AudioSessionInitialize(CFRunLoopGetMain(), kCFRunLoopDefaultMode,
                                                 session_interruption, NULL);
        if (status && status != kAudioSessionAlreadyInitialized) return NO;
        UInt32 category = kAudioSessionCategory_MediaPlayback;
        if (AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof(category), &category)) return NO;
        session_configured = YES;
    }
    if (AudioSessionSetActive(true)) return NO;
    session_active = YES;
    AudioStreamBasicDescription format = {
        .mSampleRate = sample_rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mFramesPerPacket = 1, .mBytesPerPacket = DB_AUDIO_CHANNELS * sizeof(int16_t),
        .mBytesPerFrame = DB_AUDIO_CHANNELS * sizeof(int16_t),
        .mChannelsPerFrame = DB_AUDIO_CHANNELS, .mBitsPerChannel = 16,
    };
    if (AudioQueueNewOutput(&format, refill, (void *)generation, NULL, NULL, 0, &output)) return NO;
    output_volume = UINT32_MAX;
    for (unsigned i = 0; i < APP_AUDIO_BUFFERS; ++i) {
        if (AudioQueueAllocateBuffer(output, sizeof(frame.samples), &pending[i])) return NO;
    }
    pending_count = APP_AUDIO_BUFFERS;
    return YES;
}

static BOOL play_frame(void) {
    if (!in_flight || frame.sample_rate != sample_rate || frame.volume > 127) return NO;
    if (!output && !create_output()) return NO;
    --in_flight;
    AudioQueueBufferRef buffer = pending[head];
    head = (head + 1) % APP_AUDIO_BUFFERS;
    --pending_count;
    memcpy(buffer->mAudioData, frame.samples, sizeof(frame.samples));
    buffer->mAudioDataByteSize = sizeof(frame.samples);
    if (output_volume != frame.volume) {
        output_volume = frame.volume;
        AudioQueueSetParameter(output, kAudioQueueParam_Volume, output_volume / 127.0f);
    }
    if (AudioQueueEnqueueBuffer(output, buffer, 0, NULL) || !request_buffers()) return NO;
    if (output_started || pending_count) return YES;
    if (AudioQueueStart(output, NULL)) return NO;
    output_started = YES;
    CFStringRef route = NULL;
    UInt32 route_size = sizeof(route);
    BOOL line_out = !AudioSessionGetProperty(kAudioSessionProperty_AudioRoute, &route_size, &route) &&
                    route && CFEqual(route, CFSTR("LineOut"));
    return send_command(line_out ? DB_AUDIO_STARTED_LINE_OUT : DB_AUDIO_STARTED_OTHER_ROUTE);
}

static void read_frame(void) {
    ssize_t count = recv(audio_fd, (uint8_t *)&frame + frame_bytes, sizeof(frame) - frame_bytes, 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (count <= 0) {
        stop_output();
        return;
    }
    frame_bytes += (size_t)count;
    if (frame_bytes == sizeof(frame)) {
        frame_bytes = 0;
        if (!play_frame()) stop_output();
    }
}

static void connect_output(uint32_t rate) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = DB_AUDIO_SOCKET_PATH };
    address.sun_len = SUN_LEN(&address);
    uid_t uid;
    gid_t gid;
    int ignore = 1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &ignore, sizeof(ignore)) ||
        connect(fd, (struct sockaddr *)&address, address.sun_len) || getpeereid(fd, &uid, &gid) || uid != 0) {
        close(fd);
        return;
    }
    audio_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, audio_io);
    if (!audio_source) {
        close(fd);
        return;
    }
    sample_rate = rate;
    audio_fd = fd;
    dispatch_source_set_event_handler(audio_source, ^{ if (audio_fd == fd) read_frame(); });
    dispatch_source_set_cancel_handler(audio_source, ^{ close(fd); });
    dispatch_resume(audio_source);
    if (send_command(DB_AUDIO_PULL)) in_flight = 1;
    else stop_output();
}

void dockbridge_app_audio_update(const db_reply *reply, BOOL available) {
    if (!available) return;
    uint32_t rate = reply->media.sample_rate;
    BOOL stream_open = reply->phase == DB_RECEIVING && reply->media.channels == DB_AUDIO_CHANNELS &&
                       (rate == 44100 || rate == 48000);
    BOOL playing = stream_open && reply->audio;
    BOOL foreground = [UIApplication sharedApplication].applicationState == UIApplicationStateActive;
    if (!audio_io) audio_io = dispatch_queue_create("local.dockbridge.audio", DISPATCH_QUEUE_SERIAL);
    dispatch_async(audio_io, ^{
        if (audio_fd >= 0 && (!stream_open || sample_rate != rate)) stop_output();
        if (playing && foreground && audio_fd < 0 && !session_interrupted) connect_output(rate);
    });
}

void dockbridge_app_audio_backgrounded(void) {
    if (!audio_io) return;
    dispatch_async(audio_io, ^{ if (audio_fd >= 0) send_command(DB_AUDIO_BACKGROUNDED); });
}
