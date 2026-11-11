#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

#include "btstack_audio.h"
#include "bridge_ipc.h"

enum { FRAMES_PER_BUFFER = 512, BUFFER_COUNT = 3 };
static AudioQueueRef queue;
static AudioQueueBufferRef buffers[BUFFER_COUNT];
static void (*render_pcm)(int16_t *, uint16_t);
static UInt32 buffer_bytes;
static uint8_t volume = 127;
static int running;

static void fill_buffer(void *context, AudioQueueRef output, AudioQueueBufferRef buffer) {
    (void)context;
    if (!running || output != queue) return;
    memset(buffer->mAudioData, 0, buffer_bytes);
    render_pcm(buffer->mAudioData, FRAMES_PER_BUFFER);
    buffer->mAudioDataByteSize = buffer_bytes;
    OSStatus status = AudioQueueEnqueueBuffer(output, buffer, 0, NULL);
    if (status) fprintf(stderr, "AudioQueueEnqueueBuffer: %d\n", (int)status);
}

static void close_audio(void) {
    if (!queue) return;
    if (running) dockbridge_report(DB_EVENT_AUDIO_STOPPED);
    running = 0;
    AudioQueueDispose(queue, true);
    queue = NULL;
    AudioSessionSetActive(false);
}

static int init_audio(uint8_t channels, uint32_t rate, void (*playback)(int16_t *, uint16_t)) {
    close_audio();
    render_pcm = playback;
    buffer_bytes = FRAMES_PER_BUFFER * channels * sizeof(int16_t);
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = channels * sizeof(int16_t);
    format.mBytesPerPacket = format.mBytesPerFrame;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 16;
    OSStatus status = AudioQueueNewOutput(&format, fill_buffer, NULL,
                                         CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 0, &queue);
    for (unsigned i = 0; !status && i < BUFFER_COUNT; ++i) {
        status = AudioQueueAllocateBuffer(queue, buffer_bytes, &buffers[i]);
    }
    if (status) {
        fprintf(stderr, "Audio output initialization failed: %d\n", (int)status);
        close_audio();
        dockbridge_report(DB_EVENT_AUDIO_ERROR);
        return 1;
    }
    fprintf(stderr, "PCM output: %u Hz, %u channels\n", (unsigned)rate, channels);
    return 0;
}

static void set_volume(uint8_t level) {
    volume = level;
    if (queue) AudioQueueSetParameter(queue, kAudioQueueParam_Volume, volume / 127.0f);
}

static void stop_audio(void) {
    if (!queue || !running) return;
    running = 0;
    dockbridge_report(DB_EVENT_AUDIO_STOPPED);
    AudioQueueStop(queue, true);
    AudioSessionSetActive(false);
}

static void start_audio(void) {
    if (!queue || running) return;
    OSStatus status = AudioSessionSetActive(true);
    if (status) fprintf(stderr, "AudioSessionSetActive: %d\n", (int)status);
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
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) fill_buffer(NULL, queue, buffers[i]);
    status = AudioQueueStart(queue, NULL);
    if (status) {
        fprintf(stderr, "AudioQueueStart: %d\n", (int)status);
        stop_audio();
        dockbridge_report(DB_EVENT_AUDIO_ERROR);
    } else {
        dockbridge_report(DB_EVENT_AUDIO_STARTED);
    }
}

const btstack_audio_sink_t dockbridge_audio_sink = {
    .init = init_audio, .set_volume = set_volume, .start_stream = start_audio,
    .stop_stream = stop_audio, .close = close_audio
};
