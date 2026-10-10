// SPDX-License-Identifier: GPL-2.0-only
/*
 * wlfwd: the host side of WCN3990 firmware bring-up that vendor cnss-daemon
 * does on Android and downstream icnss (v1) leaves to userspace:
 *   ind_register -> FW_MEM_READY -> REGDB -> BDF -> CAL_REPORT -> MAC,
 * then serve calibration update/download indications.
 *
 * The WLFW service (0x45) is tracked through the QRTR name service
 * (NEW_LOOKUP): after a WLAN PD restart it comes back on a new port and
 * the whole sequence is replayed.
 *
 * Usage: wlfwd <firmware dir> <MAC> [state dir]
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/qrtr.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QMI_REQ			0
#define QMI_RESP		2
#define QMI_IND			4

#define WLFW_IND_REGISTER	0x0020
#define WLFW_FW_READY_IND	0x0021
#define WLFW_CAP		0x0024
#define WLFW_BDF_DOWNLOAD	0x0025
#define WLFW_CAL_REPORT		0x0026
#define WLFW_CAL_DOWNLOAD	0x0027
#define WLFW_INIT_CAL_DL_IND	0x0028
#define WLFW_CAL_UPDATE		0x0029
#define WLFW_INIT_CAL_UPD_IND	0x002A
#define WLFW_MAC_ADDR		0x0033
#define WLFW_FW_MEM_READY_IND	0x0037
#define WLFW_FW_INIT_DONE_IND	0x0038
#define WLFW_XO_CAL_IND		0x003D
#define WLFW_CAL_DONE_IND	0x003E

#define BDF_TYPE_BIN		0
#define BDF_TYPE_REGDB		4
#define SEG_MAX			6144
#define MAX_NUM_CAL		5
#define CLIENT_ID		0x43444d4e	/* arbitrary, != icnss "KNEL" */
#define WLFW_SERVICE		0x45

static int sock;
static struct sockaddr_qrtr srv;
static uint16_t txn = 1;
static const char *state_dir = "/var/lib/wlfwd";
static int fw_mem_ready;
static int srv_up;		/* WLFW server currently registered */
static int need_init;		/* new server seen, run the sequence */
static uint32_t local_node;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("[%5ld.%03ld] ", ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}
#define log logf_

static size_t put_tlv(uint8_t *p, uint8_t type, const void *val, uint16_t len)
{
	p[0] = type;
	p[1] = len & 0xff;
	p[2] = len >> 8;
	memcpy(p + 3, val, len);
	return 3 + len;
}

static const uint8_t *get_tlv(const uint8_t *msg, ssize_t n, uint8_t type,
			      uint16_t *len)
{
	ssize_t off = 7;

	while (off + 3 <= n) {
		uint16_t l = msg[off + 1] | msg[off + 2] << 8;

		if (off + 3 + l > n)
			break;
		if (msg[off] == type) {
			*len = l;
			return msg + off + 3;
		}
		off += 3 + l;
	}
	return NULL;
}

static uint32_t get_u32(const uint8_t *msg, ssize_t n, uint8_t type, uint32_t def)
{
	const uint8_t *v;
	uint16_t len;

	v = get_tlv(msg, n, type, &len);
	if (!v || len < 4)
		return def;
	return v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
}

static int check_result(const uint8_t *msg, ssize_t n, const char *what)
{
	const uint8_t *r;
	uint16_t len;

	r = get_tlv(msg, n, 0x02, &len);
	if (!r || len < 4) {
		log("%s: no result TLV\n", what);
		return -1;
	}
	if (r[0] | r[1] << 8) {
		log("%s: QMI error %u\n", what, r[2] | r[3] << 8);
		return -1;
	}
	return 0;
}

static int send_msg(uint8_t type, uint16_t id, uint16_t msg_id,
		    const uint8_t *tlvs, size_t tlv_len)
{
	uint8_t buf[SEG_MAX + 128];

	buf[0] = type;
	buf[1] = id & 0xff; buf[2] = id >> 8;
	buf[3] = msg_id & 0xff; buf[4] = msg_id >> 8;
	buf[5] = tlv_len & 0xff; buf[6] = tlv_len >> 8;
	memcpy(buf + 7, tlvs, tlv_len);
	if (sendto(sock, buf, 7 + tlv_len, 0, (void *)&srv, sizeof(srv)) < 0) {
		log("sendto: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void handle_ind(const uint8_t *msg, ssize_t n);

/* Name service notifications about WLFW appearing/disappearing */
static void handle_ctrl(const uint8_t *buf, ssize_t n)
{
	struct qrtr_ctrl_pkt pkt;

	if (n < (ssize_t)sizeof(pkt))
		return;
	memcpy(&pkt, buf, sizeof(pkt));
	if (pkt.server.service != WLFW_SERVICE)
		return;

	switch (pkt.cmd) {
	case QRTR_TYPE_NEW_SERVER:
		srv.sq_node = pkt.server.node;
		srv.sq_port = pkt.server.port;
		srv_up = 1;
		need_init = 1;
		log("WLFW server up: node %u port %u\n", srv.sq_node, srv.sq_port);
		break;
	case QRTR_TYPE_DEL_SERVER:
		if (pkt.server.node == srv.sq_node && pkt.server.port == srv.sq_port) {
			srv_up = 0;
			fw_mem_ready = 0;
			log("WLFW server gone (PD restart?)\n");
		}
		break;
	}
}

/*
 * Receive one packet. Control packets are consumed here and reported as 0;
 * QMI packets from the current WLFW server return their length.
 */
static ssize_t rx(uint8_t *buf, size_t size)
{
	struct sockaddr_qrtr from;
	socklen_t fl = sizeof(from);
	ssize_t n;

	n = recvfrom(sock, buf, size, 0, (void *)&from, &fl);
	if (n < 0)
		return n;
	if (from.sq_port == QRTR_PORT_CTRL) {
		handle_ctrl(buf, n);
		return 0;
	}
	if (!srv_up || from.sq_node != srv.sq_node || from.sq_port != srv.sq_port)
		return 0;	/* stale packet from a previous server */
	return n;
}

/* Request/response; indications arriving meanwhile are queued for later */
static uint8_t pending[8][SEG_MAX + 128];
static ssize_t pending_len[8];
static int npending;

static ssize_t qmi_call(uint16_t msg_id, const uint8_t *tlvs, size_t tlv_len,
			uint8_t *resp, size_t resp_size)
{
	uint16_t id = txn++;
	ssize_t n;

	if (send_msg(QMI_REQ, id, msg_id, tlvs, tlv_len))
		return -1;

	for (;;) {
		n = rx(resp, resp_size);
		if (n < 0) {
			log("recv (msg 0x%x): %s\n", msg_id, strerror(errno));
			return -1;
		}
		if (!srv_up) {
			log("server gone during msg 0x%x\n", msg_id);
			return -1;
		}
		if (n < 7)
			continue;
		if (resp[0] == QMI_IND) {
			if (npending < 8) {
				memcpy(pending[npending], resp, n);
				pending_len[npending++] = n;
			}
			continue;
		}
		if (resp[0] == QMI_RESP && (resp[1] | resp[2] << 8) == id &&
		    (resp[3] | resp[4] << 8) == msg_id)
			return n;
	}
}

static void flush_pending(void)
{
	int i;

	for (i = 0; i < npending; i++)
		handle_ind(pending[i], pending_len[i]);
	npending = 0;
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
	uint8_t *data;
	FILE *f;
	long sz;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	rewind(f);
	data = malloc(sz ? sz : 1);
	if (!data || fread(data, 1, sz, f) != (size_t)sz) {
		fclose(f);
		free(data);
		return NULL;
	}
	fclose(f);
	*size = sz;
	return data;
}

/*
 * BDF_DOWNLOAD and CAL_DOWNLOAD share the layout:
 * 01 valid, 10 file_id, 11 total_size, 12 seg_id, 13 data, 14 end, 15 type
 */
static int upload(uint16_t msg_id, const char *path, uint32_t file_id,
		  int bdf_type)
{
	uint8_t resp[256], tlvs[SEG_MAX + 64], dl[2 + SEG_MAX];
	uint32_t total, seg = 0, sent = 0;
	uint8_t *data;
	size_t off;
	ssize_t n;

	data = read_file(path, &total);
	if (!data) {
		log("cannot read %s\n", path);
		return -1;
	}
	log("upload %s: %u bytes, file_id 0x%x, type %d\n", path, total,
	    file_id, bdf_type);

	while (sent < total) {
		uint32_t chunk = total - sent > SEG_MAX ? SEG_MAX : total - sent;
		uint8_t one = 1, end = sent + chunk == total, t = bdf_type;

		off = 0;
		off += put_tlv(tlvs + off, 0x01, &one, 1);
		off += put_tlv(tlvs + off, 0x10, &file_id, 4);
		off += put_tlv(tlvs + off, 0x11, &total, 4);
		off += put_tlv(tlvs + off, 0x12, &seg, 4);
		dl[0] = chunk & 0xff;
		dl[1] = chunk >> 8;
		memcpy(dl + 2, data + sent, chunk);
		off += put_tlv(tlvs + off, 0x13, dl, 2 + chunk);
		off += put_tlv(tlvs + off, 0x14, &end, 1);
		if (bdf_type >= 0)
			off += put_tlv(tlvs + off, 0x15, &t, 1);

		n = qmi_call(msg_id, tlvs, off, resp, sizeof(resp));
		if (n < 0 || check_result(resp, n, "download")) {
			free(data);
			return -1;
		}
		sent += chunk;
		seg++;
	}
	log("  %u segment(s) OK\n", seg);
	free(data);
	return 0;
}

static void cal_path(char *buf, size_t sz, uint32_t cal_id)
{
	snprintf(buf, sz, "%s/wlfw_cal_%02u.bin", state_dir, cal_id);
}

/* Firmware has calibration data for us: pull it with CAL_UPDATE_REQ */
static void cal_update(uint32_t cal_id, uint32_t total)
{
	uint8_t resp[SEG_MAX + 128], tlvs[32];
	uint32_t seg = 0, got = 0;
	char path[256], tmp[264];
	const uint8_t *d;
	uint16_t len;
	FILE *f;
	ssize_t n;
	size_t off;

	cal_path(path, sizeof(path), cal_id);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "wb");
	if (!f) {
		log("cal_update: cannot write %s\n", tmp);
		return;
	}

	for (;;) {
		int32_t id = cal_id;

		off = 0;
		off += put_tlv(tlvs + off, 0x01, &id, 4);
		off += put_tlv(tlvs + off, 0x02, &seg, 4);
		n = qmi_call(WLFW_CAL_UPDATE, tlvs, off, resp, sizeof(resp));
		if (n < 0 || check_result(resp, n, "CAL_UPDATE"))
			break;
		d = get_tlv(resp, n, 0x13, &len);
		if (d && len >= 2) {
			uint16_t dl = d[0] | d[1] << 8;

			fwrite(d + 2, 1, dl, f);
			got += dl;
		}
		d = get_tlv(resp, n, 0x14, &len);
		if (!d || d[0])
			break;
		seg++;
	}
	fclose(f);

	if (got && got == total) {
		rename(tmp, path);
		log("cal %u saved: %s (%u bytes)\n", cal_id, path, got);
	} else {
		log("cal %u: got %u of %u bytes, not saved\n", cal_id, got, total);
		unlink(tmp);
	}
}

static const char *ind_name(uint16_t id)
{
	switch (id) {
	case WLFW_FW_READY_IND:		return "FW_READY";
	case WLFW_INIT_CAL_DL_IND:	return "INITIATE_CAL_DOWNLOAD";
	case WLFW_INIT_CAL_UPD_IND:	return "INITIATE_CAL_UPDATE";
	case 0x002B:			return "MSA_READY";
	case 0x002C:			return "PIN_CONNECT_RESULT";
	case 0x0035:			return "REQUEST_MEM";
	case WLFW_FW_MEM_READY_IND:	return "FW_MEM_READY";
	case WLFW_FW_INIT_DONE_IND:	return "FW_INIT_DONE";
	case 0x0039:			return "REJUVENATE";
	case WLFW_XO_CAL_IND:		return "XO_CAL";
	case WLFW_CAL_DONE_IND:		return "CAL_DONE";
	default:			return "?";
	}
}

static void handle_ind(const uint8_t *msg, ssize_t n)
{
	uint16_t id = msg[3] | msg[4] << 8;
	char path[256];
	uint32_t cal_id;

	log("IND 0x%04x %s (%zd bytes)\n", id, ind_name(id), n);

	switch (id) {
	case WLFW_FW_MEM_READY_IND:
		fw_mem_ready = 1;
		break;
	case WLFW_INIT_CAL_UPD_IND:
		cal_update(get_u32(msg, n, 0x01, 0), get_u32(msg, n, 0x02, 0));
		break;
	case WLFW_INIT_CAL_DL_IND:
		cal_id = get_u32(msg, n, 0x01, 0);
		cal_path(path, sizeof(path), cal_id);
		if (access(path, R_OK))
			log("no saved cal %u, nothing to download\n", cal_id);
		else
			upload(WLFW_CAL_DOWNLOAD, path, cal_id, -1);
		break;
	}
}

/* Wait for indications for up to `ms` (0 = forever) */
static void serve(int ms, int until_mem_ready)
{
	uint8_t buf[SEG_MAX + 128];
	struct timeval tv;
	ssize_t n;
	struct timespec start, now;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (until_mem_ready && fw_mem_ready)
			return;
		if (!until_mem_ready && need_init)
			return;
		if (until_mem_ready && !srv_up)
			return;
		if (ms) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			long left = ms - ((now.tv_sec - start.tv_sec) * 1000 +
					  (now.tv_nsec - start.tv_nsec) / 1000000);
			if (left <= 0)
				return;
			tv.tv_sec = left / 1000;
			tv.tv_usec = (left % 1000) * 1000;
		} else {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
		}
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		n = rx(buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			log("recv: %s\n", strerror(errno));
			return;
		}
		if (n >= 7 && buf[0] == QMI_IND)
			handle_ind(buf, n);
	}
}

static void set_timeout(int sec)
{
	struct timeval tv = { .tv_sec = sec };

	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int ind_register(void)
{
	uint8_t resp[256], tlvs[64], one = 1;
	uint32_t client = CLIENT_ID;
	size_t off = 0;
	ssize_t n;

	off += put_tlv(tlvs + off, 0x11, &one, 1);	/* initiate_cal_download */
	off += put_tlv(tlvs + off, 0x12, &one, 1);	/* initiate_cal_update */
	off += put_tlv(tlvs + off, 0x15, &client, 4);	/* client_id */
	off += put_tlv(tlvs + off, 0x17, &one, 1);	/* fw_mem_ready */
	off += put_tlv(tlvs + off, 0x18, &one, 1);	/* fw_init_done */
	off += put_tlv(tlvs + off, 0x1A, &one, 1);	/* xo_cal */
	off += put_tlv(tlvs + off, 0x1B, &one, 1);	/* cal_done */

	n = qmi_call(WLFW_IND_REGISTER, tlvs, off, resp, sizeof(resp));
	if (n < 0 || check_result(resp, n, "IND_REGISTER"))
		return -1;
	log("IND_REGISTER OK, fw_status 0x%x\n", get_u32(resp, n, 0x10, 0));
	return 0;
}

/* CAL_REPORT lists the cal ids we have saved (TLV 01: u8 count, s32[]) */
static int cal_report(void)
{
	uint8_t resp[256], tlvs[64], meta[1 + 4 * MAX_NUM_CAL];
	char path[256];
	uint8_t cnt = 0;
	int32_t id;
	ssize_t n;

	for (id = 0; id < MAX_NUM_CAL; id++) {
		cal_path(path, sizeof(path), id);
		if (!access(path, R_OK)) {
			memcpy(meta + 1 + 4 * cnt, &id, 4);
			cnt++;
		}
	}
	meta[0] = cnt;
	n = qmi_call(WLFW_CAL_REPORT, tlvs,
		     put_tlv(tlvs, 0x01, meta, 1 + 4 * cnt), resp, sizeof(resp));
	if (n < 0 || check_result(resp, n, "CAL_REPORT"))
		return -1;
	log("CAL_REPORT OK (%u saved cal file(s))\n", cnt);
	return 0;
}

static int send_mac(const char *str)
{
	uint8_t resp[256], tlvs[16], mac[6];
	ssize_t n;

	if (sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
		log("bad MAC %s\n", str);
		return -1;
	}
	n = qmi_call(WLFW_MAC_ADDR, tlvs, put_tlv(tlvs, 0x10, mac, 6),
		     resp, sizeof(resp));
	if (n < 0 || check_result(resp, n, "MAC_ADDR"))
		return -1;
	log("MAC %s sent\n", str);
	return 0;
}

static char *pick_bdf(const char *dir, uint32_t board_id)
{
	static char path[512];

	if (board_id != 0xff) {
		if (board_id < 0x100)
			snprintf(path, sizeof(path), "%s/bdwlan.b%02x", dir, board_id);
		else
			snprintf(path, sizeof(path), "%s/bdwlan.%03x", dir, board_id);
		if (!access(path, R_OK))
			return path;
	}
	snprintf(path, sizeof(path), "%s/bdwlan.bin", dir);
	return path;
}

/* Full bring-up sequence for the current server; -1 if it failed */
static int bring_up(const char *fw_dir, const char *mac)
{
	uint32_t board_id;
	uint8_t resp[1024];
	char regdb[512];
	ssize_t n;

	npending = 0;
	fw_mem_ready = 0;
	set_timeout(5);

	if (ind_register())
		return -1;

	n = qmi_call(WLFW_CAP, NULL, 0, resp, sizeof(resp));
	if (n < 0 || check_result(resp, n, "CAP"))
		return -1;
	board_id = get_u32(resp, n, 0x11, 0xff);
	log("board_id 0x%x\n", board_id);
	flush_pending();

	if (!fw_mem_ready) {
		log("waiting up to 10 s for FW_MEM_READY\n");
		serve(10000, 1);
		if (!srv_up)
			return -1;
		log(fw_mem_ready ? "FW_MEM_READY received\n" :
			"no FW_MEM_READY, continuing\n");
	}

	set_timeout(5);
	snprintf(regdb, sizeof(regdb), "%s/regdb.bin", fw_dir);
	if (upload(WLFW_BDF_DOWNLOAD, regdb, board_id, BDF_TYPE_REGDB))
		return -1;
	if (upload(WLFW_BDF_DOWNLOAD, pick_bdf(fw_dir, board_id), board_id,
		   BDF_TYPE_BIN))
		return -1;
	flush_pending();
	if (cal_report())
		return -1;
	flush_pending();
	if (send_mac(mac))
		return -1;
	flush_pending();
	return 0;
}

static int send_lookup(void)
{
	struct sockaddr_qrtr ns = {
		.sq_family = AF_QIPCRTR,
		.sq_node = local_node,
		.sq_port = QRTR_PORT_CTRL,
	};
	struct qrtr_ctrl_pkt pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
	pkt.server.service = WLFW_SERVICE;
	pkt.server.instance = 0;	/* any */
	if (sendto(sock, &pkt, sizeof(pkt), 0, (void *)&ns, sizeof(ns)) < 0) {
		log("NEW_LOOKUP: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_qrtr sq;
	socklen_t sl = sizeof(sq);

	if (argc < 3 || argc > 4) {
		fprintf(stderr, "usage: %s <fw dir> <MAC> [state dir]\n", argv[0]);
		return 1;
	}
	if (argc == 4)
		state_dir = argv[3];
	mkdir(state_dir, 0700);

	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (sock < 0 || getsockname(sock, (void *)&sq, &sl) < 0) {
		perror("socket");
		return 1;
	}
	local_node = sq.sq_node;
	srv.sq_family = AF_QIPCRTR;

	if (send_lookup())
		return 1;
	log("waiting for WLFW service 0x%x\n", WLFW_SERVICE);

	for (;;) {
		int tries;

		serve(0, 0);		/* returns when a new server needs init */
		if (!need_init)
			continue;
		need_init = 0;

		/*
		 * The service shows up before icnss has set up MSA, and CAP then
		 * fails (QMI error 90): retry while the same server is alive.
		 */
		for (tries = 1; tries <= 15; tries++) {
			if (!bring_up(argv[1], argv[2])) {
				log("init sequence done, serving indications\n");
				break;
			}
			if (!srv_up || need_init)
				break;
			log("bring-up failed (try %d), retrying in 2 s\n", tries);
			serve(2000, 0);
			if (!srv_up || need_init)
				break;
		}
	}
	return 0;
}
