#ifndef DOCKBRIDGE_IPC_H
#define DOCKBRIDGE_IPC_H

#include <stdint.h>

#define DB_STATE_DIR "/var/lib/dockbridge"
#define DB_SOCKET_DIR "/var/lib/dockbridge-ipc"
#define DB_SOCKET_PATH DB_SOCKET_DIR "/control.sock"
#define DB_AUDIO_SOCKET_PATH DB_SOCKET_DIR "/audio.sock"
#define DB_EXECUTABLE "/usr/bin/dockbridge"
#define DB_WORKER_FD 3

enum { DB_STATUS = 1, DB_START, DB_STOP, DB_PAIR, DB_AUTO_ENABLE, DB_AUTO_DISABLE };
enum { DB_AUTO_OFF, DB_AUTO_ARMED, DB_AUTO_PAUSED };
enum { DB_SYSTEM, DB_STARTING, DB_RECEIVING, DB_STOPPING, DB_FAILED };
enum { DB_OK, DB_BUSY, DB_ERROR };
enum { DB_DIAGNOSTIC_NONE, DB_DIAGNOSTIC_TIMEOUT, DB_DIAGNOSTIC_TRANSPORT };
enum {
    DB_EVENT_READY = 1, DB_EVENT_PAIR_OPEN, DB_EVENT_PAIR_CLOSED,
    DB_EVENT_AUDIO_STARTED, DB_EVENT_AUDIO_STOPPED, DB_EVENT_AUDIO_ERROR,
    DB_EVENT_RADIO_ACQUIRE, DB_EVENT_RADIO_RELEASE, DB_EVENT_INIT_TIMEOUT, DB_EVENT_TRANSPORT_ERROR,
    DB_EVENT_MEDIA
};

enum { DB_CODEC_SBC = 1 };
enum {
    DB_AUDIO_PULL = 1, DB_AUDIO_STARTED_LINE_OUT, DB_AUDIO_STARTED_OTHER_ROUTE,
    DB_AUDIO_BACKGROUNDED, DB_AUDIO_INTERRUPTED
};
enum { DB_AUDIO_FRAMES = 512, DB_AUDIO_CHANNELS = 2 };
typedef struct {
    uint32_t sample_rate;
    uint32_t volume;
    int16_t samples[DB_AUDIO_FRAMES * DB_AUDIO_CHANNELS];
} db_audio_frame;
enum { DB_HAS_POSITION = 1, DB_HAS_DURATION = 2 };
enum { DB_MEDIA_TEXT_SIZE = 101, DB_DEVICE_NAME_SIZE = 160 };

typedef struct {
    uint32_t sample_rate;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint8_t codec;
    uint8_t channels;
    uint8_t timing;
    char title[DB_MEDIA_TEXT_SIZE];
    char artist[DB_MEDIA_TEXT_SIZE];
    char album[DB_MEDIA_TEXT_SIZE];
    uint8_t device_connected;
    char device_name[DB_DEVICE_NAME_SIZE];
} db_media_snapshot;

typedef struct {
    uint8_t kind;
    db_media_snapshot media;
} db_worker_event;

typedef struct {
    uint8_t phase;
    uint8_t pairing;
    uint8_t audio;
    uint8_t result;
    uint8_t diagnostic;
    uint8_t auto_mode;
    db_media_snapshot media;
} db_reply;

void dockbridge_report(uint8_t event);
void dockbridge_report_media(const db_media_snapshot *media);
int dockbridge_trace_hci_init(void);
int dockbridge_service(void);

#endif
