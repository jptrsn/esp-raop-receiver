/*
 *
 * (c) Philippe 2016-2017, philippe_44@outlook.com
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 */

#include "platform.h"
#include <mbedtls/aes.h>
#include <mbedtls/version.h>
#include "esp_heap_caps.h"

#include "rtp.h"
#include "log_util.h"
#include "util.h"

// Network byte order conversions for 64-bit
#ifndef htonll
#define htonll(x) ((uint64_t)(htonl((uint32_t)((x) >> 32))) | ((uint64_t)(htonl((uint32_t)(x))) << 32))
#endif

#define NTP_EPOCH_OFFSET	2208988800ULL
#define FRAMES_PER_PACKET 	352
#define RTP_STACK_SIZE		(4*1024)
#define MAX_PACKET			2048
#define RTP_SYNC			(2*1024*1024)

typedef u16_t seq_t;
typedef struct __attribute__((__packed__)) audio_packet_s {
	u8_t version_pkt;
	u8_t marker_paytype;
	u16_t seq_number;
	u32_t timestamp;
	u32_t ssrc;
	u8_t payload[];
} audio_packet_t;

struct rtp_s {
	bool running;
	unsigned buffer_frames;
	TaskHandle_t thread;
	StaticTask_t *xTaskBuffer;
	StackType_t xStack[RTP_STACK_SIZE] __attribute__ ((aligned (4)));
	raop_cmd_cb_t cmd_cb;
	raop_data_cb_t data_cb;
	struct {
		unsigned short rport, lport;
		int sock;
	} ctrl, time, data;
	struct in_addr peer;
	int latency;
	struct {
		u32_t 	rtp, time;
		u64_t	local;
	} sync;
	int frame_size;
	struct {
		u16_t seqno;
		u32_t rtptime, last_rtptime;
		u32_t missing;
		s16_t* buf;
	} record;
	struct {
		char *aeskey, *aesiv;
		char *fmtp;
		mbedtls_aes_context ctx;
	} decrypt;
	struct timing_s {
		u64_t local, remote;
	} timing;
	bool first_packet;
	struct {
		u8_t *buffer;
		size_t	size, len;
	} backlog;
	bool	ab_sync;
};

static void*	rtp_thread(void *arg);
static bool 	handle_rtp_packet(rtp_t *ctx, audio_packet_t *packet, ssize_t len);
static bool 	handle_control_packet(rtp_t *ctx);
static bool 	handle_time_packet(rtp_t *ctx);
static bool		rtp_request_resend(rtp_t *ctx, seq_t first, seq_t last);
static bool 	rtp_request_timing(rtp_t *ctx);

extern log_level	raop_loglevel;
static log_level 	*loglevel = &raop_loglevel;

#define VALGRIND

/*---------------------------------------------------------------------------*/
static inline u64_t get_ntp_time(void) {
	return (u64_t) (NTP_EPOCH_OFFSET + gettime_ms() / 1000) << 32;
}

/*---------------------------------------------------------------------------*/
// seq are 16 bits, they wrap, so the trick is to offset them to 0 and then use modulo
static inline seq_t seq_order(seq_t a, seq_t b) {
	s16_t seq_a = a;
	s16_t seq_b = b;
	return seq_a - seq_b;
}

/*---------------------------------------------------------------------------*/
rtp_resp_t rtp_init(struct in_addr peer, int latency,
					char *aeskey, char *aesiv, char *fmtp,
					short unsigned pCtrlPort, short unsigned pTimePort,
					uint8_t *buffer, size_t size,
					raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb)
{
	int i = 0;
	rtp_resp_t resp = { 0, 0, 0, NULL };
	rtp_t *ctx = calloc(1, sizeof(rtp_t));

	if (!ctx) return resp;

	ctx->peer = peer;
	ctx->decrypt.aeskey = aeskey;
	ctx->decrypt.aesiv = aesiv;
	ctx->decrypt.fmtp = fmtp;
	ctx->cmd_cb = cmd_cb;
	ctx->data_cb = data_cb;
	ctx->latency = latency;
	ctx->first_packet = true;
	ctx->backlog.buffer = buffer;
	ctx->backlog.size = size;
	ctx->ab_sync = true;

	if (sscanf(fmtp, "%*d %d %*d", &ctx->frame_size) < 1) {
		LOG_ERROR("invalid fmtp %s", fmtp);
		goto error;
	}

	ctx->frame_size *= 4;
	ctx->record.buf = heap_caps_malloc(ctx->frame_size * FRAMES_PER_PACKET, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	memset(ctx->record.buf, 0, ctx->frame_size * FRAMES_PER_PACKET);

	if (aeskey) {
		mbedtls_aes_init(&ctx->decrypt.ctx);
		mbedtls_aes_setkey_dec(&ctx->decrypt.ctx, (const unsigned char*) aeskey, 128);
	}

	ctx->data.rport = pCtrlPort;
	ctx->ctrl.rport = pCtrlPort;
	ctx->time.rport = pTimePort;

	ctx->data.sock = bind_socket(&ctx->data.lport, SOCK_DGRAM);
	ctx->ctrl.sock = bind_socket(&ctx->ctrl.lport, SOCK_DGRAM);
	ctx->time.sock = bind_socket(&ctx->time.lport, SOCK_DGRAM);

	if (ctx->data.sock < 0 || ctx->ctrl.sock < 0 || ctx->time.sock < 0) {
		LOG_ERROR("Cannot bind data or control sockets", NULL);
		goto error;
	}

	resp.cport = ctx->ctrl.lport;
	resp.tport = ctx->time.lport;
	resp.aport = ctx->data.lport;
	resp.ctx = ctx;

	ctx->running = true;
    ctx->xTaskBuffer = (StaticTask_t*) heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	ctx->thread = xTaskCreateStaticPinnedToCore( (TaskFunction_t) rtp_thread, "rtp_thread", RTP_STACK_SIZE, ctx,
											  ESP_TASK_PRIO_MIN + 2 + 1, ctx->xStack, ctx->xTaskBuffer,
											  tskNO_AFFINITY);

	return resp;

error:
	if (ctx->data.sock > 0) closesocket(ctx->data.sock);
	if (ctx->ctrl.sock > 0) closesocket(ctx->ctrl.sock);
	if (ctx->time.sock > 0) closesocket(ctx->time.sock);
	if (aeskey) mbedtls_aes_free(&ctx->decrypt.ctx);
	free(ctx);

	return resp;
}

/*---------------------------------------------------------------------------*/
void rtp_end(rtp_t *ctx)
{
	if (!ctx) return;

	if (ctx->running) {
		ctx->running = false;
		shutdown(ctx->data.sock, SHUT_RDWR);
		shutdown(ctx->ctrl.sock, SHUT_RDWR);
		shutdown(ctx->time.sock, SHUT_RDWR);
		vTaskDelay(100 / portTICK_PERIOD_MS);
		vTaskDelete(ctx->thread);
		if (ctx->xTaskBuffer) free(ctx->xTaskBuffer);
	}

	if (ctx->data.sock > 0) closesocket(ctx->data.sock);
	if (ctx->ctrl.sock > 0) closesocket(ctx->ctrl.sock);
	if (ctx->time.sock > 0) closesocket(ctx->time.sock);

	if (ctx->decrypt.aeskey) {
		mbedtls_aes_free(&ctx->decrypt.ctx);
		free(ctx->decrypt.aeskey);
	}

	if (ctx->decrypt.aesiv) free(ctx->decrypt.aesiv);
	if (ctx->decrypt.fmtp) free(ctx->decrypt.fmtp);
	if (ctx->record.buf) free(ctx->record.buf);

	free(ctx);
}

/*---------------------------------------------------------------------------*/
bool rtp_flush(rtp_t *ctx, unsigned short seqno, unsigned int rtptime, bool exit_locked)
{
	bool flushed = false;

	if (seq_order(ctx->record.seqno, seqno) < 0) {
		LOG_INFO("flush not %hu - seqno: %hu, rtptime: %u (W:%d)", seqno, ctx->record.seqno, rtptime, exit_locked);
		return false;
	}

	LOG_INFO("flush %hu - seqno: %hu, rtptime: %u (W:%d)", seqno, ctx->record.seqno, rtptime, exit_locked);

	memset(ctx->record.buf, 0, ctx->frame_size * FRAMES_PER_PACKET);
	ctx->record.seqno = seqno;
	ctx->record.rtptime = rtptime;
	ctx->first_packet = true;

	return flushed;
}

/*---------------------------------------------------------------------------*/
void rtp_flush_release(rtp_t *ctx) { }

/*---------------------------------------------------------------------------*/
void rtp_record(rtp_t *ctx, unsigned short seqno, unsigned rtptime) {
	ctx->record.seqno = seqno;
	ctx->record.rtptime = rtptime;
}

/*---------------------------------------------------------------------------*/
static void* rtp_thread(void *arg) {
	rtp_t *ctx = (rtp_t*) arg;
	audio_packet_t *packet = (audio_packet_t*) malloc(MAX_PACKET);

	while (ctx->running) {
		ssize_t plen;
		fd_set fds;
		int max_fd = max(ctx->data.sock, max(ctx->ctrl.sock, ctx->time.sock));
		struct timeval timeout = {0, 100*1000};

		FD_ZERO(&fds);
		FD_SET(ctx->data.sock, &fds);
		FD_SET(ctx->ctrl.sock, &fds);
		FD_SET(ctx->time.sock, &fds);

		if (select(max_fd + 1, &fds, NULL, NULL, &timeout) <= 0) continue;

		if (FD_ISSET(ctx->data.sock, &fds)) {
			plen = recv(ctx->data.sock, (void*) packet, MAX_PACKET, 0);

			if (plen > 0) {
				handle_rtp_packet(ctx, packet, plen);
			}
		}

		if (FD_ISSET(ctx->ctrl.sock, &fds)) {
			handle_control_packet(ctx);
		}

		if (FD_ISSET(ctx->time.sock, &fds)) {
			handle_time_packet(ctx);
		}
	}

	free(packet);
	vTaskSuspend(NULL);

	return NULL;
}

/*---------------------------------------------------------------------------*/
static bool handle_rtp_packet(rtp_t *ctx, audio_packet_t *packet, ssize_t plen) {
	u16_t seqno = ntohs(packet->seq_number);
	u32_t timestamp = ntohl(packet->timestamp);
	s16_t *pcm = NULL;
	int i, len = 0;

	if (ctx->first_packet) {
		ctx->record.seqno = seqno;
		ctx->first_packet = false;
		LOG_INFO("first packet seqno:%hu", seqno);
	}

	// a missing packet or out of order
	if (seq_order(ctx->record.seqno, seqno)) {
		seq_t gap = seq_order(seqno, ctx->record.seqno);

		LOG_WARN("missing packet %hu %hu (gap:%hd)", ctx->record.seqno, seqno, gap);

		// don't request silly gap
		if (gap > 100) {
			LOG_ERROR("gap of %hu, something really wrong", gap);
			ctx->record.seqno = seqno;
		} else {
			ctx->record.missing += gap;
			rtp_request_resend(ctx, ctx->record.seqno, seqno - 1);
		}
	}

	// data packet
	u8_t type = packet->marker_paytype & 0x7f;

	if (type == 0x60 || type == 0x56) {
		int plen_16 = (plen - sizeof(audio_packet_t)) / 2;
		u8_t *payload = packet->payload;
		int alac_len = plen - sizeof(audio_packet_t) - 4;

		if (ctx->decrypt.aeskey) {
			u8_t iv[16];
			u8_t *decrypted = alloca(((plen_16 + 15) / 16) * 16);
			memcpy(iv, ctx->decrypt.aesiv, 16);
			mbedtls_aes_crypt_cbc(&ctx->decrypt.ctx, MBEDTLS_AES_DECRYPT, plen_16 & ~0xf, iv, payload, decrypted);

			if (plen_16 & 0xf) {
				memcpy(decrypted + (plen_16 & ~0xf), payload + (plen_16 & ~0xf), plen_16 & 0xf);
			}

			payload = decrypted;
		}

		pcm = ctx->record.buf + (seqno - ctx->record.seqno) * FRAMES_PER_PACKET * 2;

		// decode the ALAC frames into buffer, starting at the right offset
		for (i = 0; i < FRAMES_PER_PACKET && alac_len > 0; i++) {
			int audio_len = (payload[0] << 16) | (payload[1] << 8) | payload[2];
			payload += 3;
			alac_len -= 3;

			// normal ALAC coded frames (but seems that first is not)
			if (i > 0) {
				memcpy(pcm, payload, audio_len * 4);
				pcm += audio_len * 2;
				len += audio_len;
			}

			payload += audio_len * 4;
			alac_len -= audio_len * 4;
		}

		// set record position to expected next packet
		if (seq_order(seqno, ctx->record.seqno) >= 0) {
			ctx->record.seqno = seqno + 1;
		}

		// send audio frames if we have received expected packet and backlog allows
		if (seqno == ctx->record.seqno - 1) {
			len = ctx->frame_size * FRAMES_PER_PACKET / 4;

			if (ctx->backlog.len) {
				ctx->backlog.len -= min(ctx->backlog.len, len * 4);
			}

			ctx->data_cb((u8_t*) ctx->record.buf, len * 4);

			// and we can also restart the next buffer
			memset(ctx->record.buf, 0, ctx->frame_size * FRAMES_PER_PACKET);
		}
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static bool handle_time_packet(rtp_t *ctx) {
	u8_t req[32];
	ssize_t plen;
	struct sockaddr_in peer;
	socklen_t addrlen = sizeof(peer);
	u64_t now = get_ntp_time();

	plen = recvfrom(ctx->time.sock, req, sizeof(req), 0, (struct sockaddr*) &peer, &addrlen);

	if (plen > 0) {
		u8_t resp[32];

		// make socket destination-specific
		peer.sin_port = htons(ctx->time.rport);
		peer.sin_addr.s_addr = S_ADDR(ctx->peer);

		resp[0] = 0x80;
		resp[1] = 0xd3;

		// copy reference timestamp
		memcpy(resp + 8, req + 24, 8);

		// transmit timestamp (when it was received)
		*(u64_t*)(resp + 16) = htonll(now);

		// response timestamp (our clock)
		*(u64_t*)(resp + 24) = htonll(now);

		sendto(ctx->time.sock, resp, sizeof(resp), 0, (struct sockaddr*) &peer, sizeof(peer));

		LOG_SDEBUG("time packet", NULL);
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static bool rtp_request_timing(rtp_t *ctx) {
	u8_t req[32];
	struct sockaddr_in peer;
	u64_t now = get_ntp_time();

	req[0] = 0x80;
	req[1] = 0xd2;
	*(u16_t*)(req + 2) = htons(0x0007);

	memset(req + 8, 0, 16);

	// send timestamp
	*(u64_t*)(req + 24) = htonll(now);

	peer.sin_port = htons(ctx->time.rport);
	peer.sin_addr.s_addr = S_ADDR(ctx->peer);
	peer.sin_family = AF_INET;

	sendto(ctx->time.sock, req, sizeof(req), 0, (struct sockaddr*) &peer, sizeof(peer));

	LOG_SDEBUG("request timing", NULL);

	return true;
}

/*---------------------------------------------------------------------------*/
static bool handle_control_packet(rtp_t *ctx) {
	u8_t req[8];
	ssize_t plen;

	plen = recv(ctx->ctrl.sock, req, sizeof(req), 0);

	if (plen > 0) {
		u8_t type = req[1] & ~0x80;

		if (type == 0x55) {
			u32_t a = ntohl(*(u32_t*)(req + 4));
			LOG_WARN("retransmit request ack %u", a);
		} else if (type == 0x56) {
			seq_t seqno = ntohs(*(u16_t*)(req + 4));
			seq_t count = ntohs(*(u16_t*)(req + 6));
			LOG_WARN("retransmit request %hu %hu", seqno, count);
		}
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static bool rtp_request_resend(rtp_t *ctx, seq_t first, seq_t last) {
	struct sockaddr_in peer;
	u8_t req[8];

	if (!ctx || !ctx->ctrl.rport) return false;

	LOG_INFO("resend request %hu %hu", first, last);

	req[0] = 0x80;
	req[1] = 0x55 | 0x80;
	*(u16_t*)(req + 2) = htons(0x0001);
	*(u16_t*)(req + 4) = htons(first);
	*(u16_t*)(req + 6) = htons(last - first + 1);

	peer.sin_port = htons(ctx->ctrl.rport);
	peer.sin_addr.s_addr = S_ADDR(ctx->peer);
	peer.sin_family = AF_INET;

	sendto(ctx->ctrl.sock, req, sizeof(req), 0, (struct sockaddr*) &peer, sizeof(peer));

	return true;
}