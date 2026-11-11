#include "media_info.h"
#include "bridge_ipc.h"
#include "btstack.h"
#include <string.h>

enum { METADATA_INTERVAL_MS = 10000, POSITION_INTERVAL_MS = 2000, QUERY_TIMEOUT_MS = 4000 };

typedef struct {
    char title[DB_MEDIA_TEXT_SIZE];
    char artist[DB_MEDIA_TEXT_SIZE];
    char album[DB_MEDIA_TEXT_SIZE];
    uint32_t duration_ms;
} pending_track;

static db_media_snapshot media, last_report;
static pending_track pending;
static uint8_t audio_address[6], control_address[6];
static uint16_t control_cid;
static int audio_connected, waiting_metadata, discard_metadata;
static uint32_t next_metadata, next_position, metadata_deadline;

static int has_track(void) {
    return media.title[0] || media.artist[0] || media.album[0];
}

static int active_control(void) {
    return audio_connected && control_cid && !memcmp(audio_address, control_address, sizeof(audio_address));
}

static void report(void) {
    if (!memcmp(&media, &last_report, sizeof(media))) return;
    last_report = media;
    dockbridge_report_media(&media);
}

static void clear_track(void) {
    memset(media.title, 0, sizeof(media.title));
    memset(media.artist, 0, sizeof(media.artist));
    memset(media.album, 0, sizeof(media.album));
    media.timing = 0;
    media.duration_ms = media.position_ms = 0;
}

static void restart_queries(void) {
    waiting_metadata = discard_metadata = 0;
    next_metadata = next_position = 0;
}

void dockbridge_media_format(uint32_t sample_rate, uint8_t channels) {
    media.sample_rate = sample_rate;
    media.channels = channels;
    media.codec = DB_CODEC_SBC;
    report();
}

void dockbridge_media_audio_connected(const uint8_t address[6]) {
    if (audio_connected && !memcmp(audio_address, address, sizeof(audio_address))) return;
    memcpy(audio_address, address, sizeof(audio_address));
    audio_connected = 1;
    clear_track();
    restart_queries();
    report();
}

void dockbridge_media_audio_disconnected(void) {
    audio_connected = 0;
    clear_track();
    media.sample_rate = media.channels = media.codec = 0;
    restart_queries();
    report();
}

void dockbridge_media_avrcp_connected(uint16_t cid, const uint8_t address[6]) {
    if (audio_connected && memcmp(audio_address, address, sizeof(audio_address))) return;
    if (control_cid == cid && !memcmp(control_address, address, sizeof(control_address))) return;
    control_cid = cid;
    memcpy(control_address, address, sizeof(control_address));
    clear_track();
    restart_queries();
    report();
}

void dockbridge_media_avrcp_disconnected(uint16_t cid) {
    if (cid != control_cid) return;
    control_cid = 0;
    clear_track();
    restart_queries();
    report();
}

static void copy_text(char field[DB_MEDIA_TEXT_SIZE], const uint8_t *text, size_t count) {
    if (count >= DB_MEDIA_TEXT_SIZE) count = DB_MEDIA_TEXT_SIZE - 1;
    const uint8_t *nul = memchr(text, 0, count);
    if (nul) count = (size_t)(nul - text);
    if (count) {
        size_t lead = count - 1;
        while (lead && (text[lead] & 0xc0) == 0x80) --lead;
        uint8_t first = text[lead];
        unsigned bytes = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 :
                         (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
        if (!bytes || count - lead < bytes) count = lead;
    }
    memset(field, 0, DB_MEDIA_TEXT_SIZE);
    memcpy(field, text, count);
}

static int valid_duration(uint32_t duration) {
    return duration && duration != UINT32_MAX;
}

static void apply_metadata(void) {
    int changed = strcmp(media.title, pending.title) || strcmp(media.artist, pending.artist) ||
                  strcmp(media.album, pending.album);
    if (changed) clear_track();
    memcpy(media.title, pending.title, sizeof(media.title));
    memcpy(media.artist, pending.artist, sizeof(media.artist));
    memcpy(media.album, pending.album, sizeof(media.album));
    if (has_track() && valid_duration(pending.duration_ms)) {
        media.duration_ms = pending.duration_ms;
        media.timing |= DB_HAS_DURATION;
    }
    if (has_track()) next_position = 0;
    report();
}

void dockbridge_media_avrcp_event(uint8_t packet_type, const uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET || size < 6 || packet[0] != HCI_EVENT_AVRCP_META ||
        !active_control() || little_endian_read_16(packet, 3) != control_cid) return;
    uint8_t subevent = packet[2];
    uint8_t response = packet[5];
    switch (subevent) {
        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            if (response != AVRCP_CTYPE_RESPONSE_CHANGED_STABLE) return;
            clear_track();
            if (waiting_metadata) discard_metadata = 1;
            next_metadata = 0;
            report();
            return;
        case AVRCP_SUBEVENT_NOTIFICATION_NOW_PLAYING_CONTENT_CHANGED:
            if (response == AVRCP_CTYPE_RESPONSE_CHANGED_STABLE) next_metadata = 0;
            return;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            if (response == AVRCP_CTYPE_RESPONSE_CHANGED_STABLE) {
                next_position = 0;
                if (!has_track()) next_metadata = 0;
            }
            return;
        case AVRCP_SUBEVENT_PLAY_STATUS:
            if (size < 15 || response != AVRCP_CTYPE_RESPONSE_IMPLEMENTED_STABLE || !has_track()) return;
            media.position_ms = avrcp_subevent_play_status_get_song_position(packet);
            if (media.position_ms != UINT32_MAX) media.timing |= DB_HAS_POSITION;
            else media.timing &= ~DB_HAS_POSITION;
            uint32_t duration = avrcp_subevent_play_status_get_song_length(packet);
            if (valid_duration(duration)) {
                media.duration_ms = duration;
                media.timing |= DB_HAS_DURATION;
            }
            report();
            return;
        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            if (!waiting_metadata || size < 7) return;
            waiting_metadata = 0;
            if (!discard_metadata && response == AVRCP_CTYPE_RESPONSE_IMPLEMENTED_STABLE &&
                !avrcp_subevent_now_playing_info_done_get_status(packet)) apply_metadata();
            return;
        default:
            break;
    }
    if (!waiting_metadata || discard_metadata || response != AVRCP_CTYPE_RESPONSE_IMPLEMENTED_STABLE) return;
    if (subevent == AVRCP_SUBEVENT_NOW_PLAYING_SONG_LENGTH_MS_INFO) {
        if (size >= 10) pending.duration_ms = avrcp_subevent_now_playing_song_length_ms_info_get_song_length(packet);
        return;
    }
    if (size < 7 || packet[6] > size - 7) return;
    switch (subevent) {
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO: copy_text(pending.title, packet + 7, packet[6]); break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO: copy_text(pending.artist, packet + 7, packet[6]); break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO: copy_text(pending.album, packet + 7, packet[6]); break;
    }
}

static int due(uint32_t now, uint32_t deadline) {
    return !deadline || (int32_t)(now - deadline) >= 0;
}

void dockbridge_media_tick(uint32_t now) {
    if (!active_control()) return;
    if (waiting_metadata) {
        if (!due(now, metadata_deadline)) return;
        waiting_metadata = 0;
    }
    if (due(now, next_metadata)) {
        static avrcp_media_attribute_id_t attributes[] = {
            AVRCP_MEDIA_ATTR_TITLE, AVRCP_MEDIA_ATTR_ARTIST, AVRCP_MEDIA_ATTR_ALBUM, AVRCP_MEDIA_ATTR_SONG_LENGTH_MS
        };
        memset(&pending, 0, sizeof(pending));
        discard_metadata = 0;
        waiting_metadata = 1;
        metadata_deadline = now + QUERY_TIMEOUT_MS;
        if (avrcp_controller_get_element_attributes(control_cid, sizeof(attributes) / sizeof(attributes[0]),
                                                    attributes) == ERROR_CODE_SUCCESS) {
            next_metadata = now + METADATA_INTERVAL_MS;
            return;
        }
        waiting_metadata = 0;
    }
    if (has_track() && due(now, next_position) &&
        avrcp_controller_get_play_status(control_cid) == ERROR_CODE_SUCCESS) {
        next_position = now + POSITION_INTERVAL_MS;
    }
}
