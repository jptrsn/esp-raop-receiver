#ifndef RAOP_H
#define RAOP_H

#include <stdbool.h>
#include <stdint.h>

struct raop_ctx_s;

typedef enum {
    RAOP_STREAM,
    RAOP_STOP,
    RAOP_FLUSH,
    RAOP_SETUP,
    RAOP_VOLUME,
    RAOP_PROGRESS,
    RAOP_METADATA,
    RAOP_ARTWORK,
    RAOP_PAUSE,
    RAOP_PLAY,
    RAOP_RESUME,
    RAOP_TOGGLE,
    RAOP_PREV,
    RAOP_NEXT,
    RAOP_FWD,
    RAOP_REW,
    RAOP_VOLUME_UP,
    RAOP_VOLUME_DOWN
} raop_event_t;

typedef bool (*raop_cmd_cb_t)(raop_event_t event, ...);
typedef void (*raop_data_cb_t)(uint8_t *data, size_t len);

struct raop_ctx_s *raop_create(uint32_t host, char *name,
                               unsigned char mac[6], int latency,
                               raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb);

void raop_delete(struct raop_ctx_s *ctx);
void raop_abort(struct raop_ctx_s *ctx);
bool raop_cmd(struct raop_ctx_s *ctx, raop_event_t event, void *param);

#endif // RAOP_H