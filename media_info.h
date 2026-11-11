#ifndef DOCKBRIDGE_MEDIA_INFO_H
#define DOCKBRIDGE_MEDIA_INFO_H

#include <stdint.h>

void dockbridge_media_format(uint32_t sample_rate, uint8_t channels);
void dockbridge_media_audio_connected(const uint8_t address[6]);
void dockbridge_media_audio_disconnected(void);
void dockbridge_media_avrcp_connected(uint16_t cid, const uint8_t address[6]);
void dockbridge_media_avrcp_disconnected(uint16_t cid);
void dockbridge_media_avrcp_event(uint8_t packet_type, const uint8_t *packet, uint16_t size);
void dockbridge_media_tick(uint32_t now);

#endif
