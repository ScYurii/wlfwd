// SPDX-License-Identifier: GPL-2.0-only
/*
 * pdr-restart: ask the servreg notifier (QMI service 0x42) to restart a
 * protection domain, the same request icnss_trigger_recovery() sends.
 * Used to test that wlfwd survives a WLAN PD restart.
 *
 * Usage: pdr-restart <node> <port> <pd name, e.g. msm/modem/wlan_pd>
 */
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/qrtr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define SERVREG_RESTART_PD	0x0024

int main(int argc, char **argv)
{
	struct sockaddr_qrtr srv = { .sq_family = AF_QIPCRTR };
	struct timeval tv = { .tv_sec = 5 };
	uint8_t buf[128], resp[256];
	size_t len;
	ssize_t n;
	int sock;

	if (argc != 4 || strlen(argv[3]) > 64) {
		fprintf(stderr, "usage: %s <node> <port> <pd name>\n", argv[0]);
		return 1;
	}
	len = strlen(argv[3]);
	srv.sq_node = strtoul(argv[1], NULL, 0);
	srv.sq_port = strtoul(argv[2], NULL, 0);

	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* QMI header: request, txn 1, msg id, length; TLV 01 = name (no prefix) */
	buf[0] = 0;
	buf[1] = 1; buf[2] = 0;
	buf[3] = SERVREG_RESTART_PD & 0xff; buf[4] = SERVREG_RESTART_PD >> 8;
	buf[5] = (3 + len) & 0xff; buf[6] = (3 + len) >> 8;
	buf[7] = 0x01;
	buf[8] = len & 0xff; buf[9] = len >> 8;
	memcpy(buf + 10, argv[3], len);

	if (sendto(sock, buf, 10 + len, 0, (void *)&srv, sizeof(srv)) < 0) {
		perror("sendto");
		return 1;
	}
	n = recv(sock, resp, sizeof(resp), 0);
	if (n < 14) {
		perror("recv");
		return 1;
	}
	/* TLV 02 result: u16 result, u16 error */
	printf("restart %s: result %u error %u\n", argv[3],
	       resp[10] | resp[11] << 8, resp[12] | resp[13] << 8);
	return resp[10] | resp[11] << 8;
}
