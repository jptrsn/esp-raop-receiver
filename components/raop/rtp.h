#ifndef RTP_H
#define RTP_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include "raop.h"

typedef struct rtp_s rtp_t;

typedef struct {
    unsigned short cport, tport, aport;
    rtp_t *ctx;
} rtp_resp_t;

rtp_resp_t rtp_init(struct in_addr peer, int latency,
                    char *aeskey, char *aesiv, char *fmtp,
                    short unsigned pCtrlPort, short unsigned pTimePort,
                    uint8_t *buffer, size_t size,
                    raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb);

void rtp_end(rtp_t *ctx);
bool rtp_flush(rtp_t *ctx, unsigned short seqno, unsigned int rtptime, bool exit_locked);
void rtp_flush_release(rtp_t *ctx);
void rtp_record(rtp_t *ctx, unsigned short seqno, unsigned rtptime);

#endif // RTP_H