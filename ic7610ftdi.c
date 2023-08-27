/*
 * Receive I/Q data from Icom IC-7610's USB 3 port.
 *
 * Link against libftd3xx.
 *
 * Copyright (C) 2023 Christoph Berg DF7CB <cb@df7cb.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <fcntl.h>
#include "ftd3xx.h"
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CMD_OUT 0x02
#define CMD_IN  0x82
#define IQ_IN   0x84
#define TIMEOUT 100 // ms
#define READ_SIZE 256 * 1024

#define CMD_INDEX 4
#define SUBCMD_INDEX 5
#define DATA_INDEX 6

static int keep_going = true;

static int
get_device_list()
{
	FT_DEVICE_LIST_INFO_NODE nodes[16];
	DWORD count;
	int res;

	if ((res = FT_CreateDeviceInfoList(&count)) != FT_OK) {
		printf("FT_CreateDeviceInfoList: %d\n", res);
		exit(1);
	}

	if ((res = FT_GetDeviceInfoList(nodes, &count)) != FT_OK) {
		printf("FT_CreateDeviceInfoList: %d\n", res);
		exit(1);
	}

	for (DWORD i = 0; i < count; i++)
	{
		printf("Device[%d]\n", i);
		printf("\tFlags: 0x%x %s | Type: %d | ID: 0x%08X\n",
				nodes[i].Flags,
				nodes[i].Flags & FT_FLAGS_SUPERSPEED ? "[USB 3]" :
				nodes[i].Flags & FT_FLAGS_HISPEED ? "[USB 2]" :
				nodes[i].Flags & FT_FLAGS_OPENED ? "[OPENED]" : "",
				nodes[i].Type,
				nodes[i].ID);
		printf("\tSerialNumber=%s\n", nodes[i].SerialNumber);
		printf("\tDescription=%s\n", nodes[i].Description);
	}

	return 0;
}


static int
send_cmd(FT_HANDLE handle, uint8_t *cmd, int cmd_size)
{
	uint8_t buf[32] = {0xfe, 0xfe, 0x98, 0xe0};
	int buf_size = 4;
	int i, res;
	DWORD count;

	for (i = 0; i < cmd_size; i++) {
		buf[buf_size++] = cmd[i];
	}
	buf[buf_size++] = 0xfd;
	for (i = buf_size; i % 4 > 0; i++) {
		buf[buf_size++] = 0xff;
	}

	for (i = 0; i < buf_size; i++) {
		printf("%02x ", buf[i]);
	}
	printf("\n");

	if ((res = FT_WritePipe(handle, CMD_OUT, buf, buf_size, &count, 0)) != FT_OK) {
		printf("FT_WritePipe: %d\n", res);
		return false;
	}
	if (count != buf_size) {
		printf("FT_WritePipe wrote %d bytes, but we wanted %d\n", count, buf_size);
		return false;
	}
	return true;
}

static int
read_reply(FT_HANDLE handle)
{
	uint8_t buf[READ_SIZE];
	int i, res;
	DWORD count;

	if ((res = FT_ReadPipe(handle, CMD_IN, buf, sizeof(buf), &count, 0)) != FT_OK) {
		printf("FT_ReadPipe: %d\n", res);
		res = FT_AbortPipe(handle, CMD_IN);
		printf("FT_AbortPipe: %d\n", res);
		return false;
	}

	for (i = 0; i < count; i++) {
		printf("%02x ", buf[i]);
	}

	switch (buf[CMD_INDEX]) {
		case 0x1a: switch (buf[SUBCMD_INDEX]) {
				   case 0x0a: printf("OVF: %d", buf[DATA_INDEX]); break;
				   case 0x0b: printf("IQ data output: %d", buf[DATA_INDEX]); break;
			   }
			   break;
		case 0x1c: switch (buf[SUBCMD_INDEX]) {
				   case 0x00: printf("TX: %d", buf[DATA_INDEX]); break;
				   case 0x02: printf("XFC: %d", buf[DATA_INDEX]); break;
			   }
			   break;
		case 0xfa: printf("NG"); break;
		case 0xfb: printf("OK"); break;
	}
	printf("\n");

	return true;
}

int
open_file(char *filename)
{
	int fd;
	if ((fd = creat(filename, 0666)) < 0) {
		perror("open");
		return false;
	}
	printf("Writing to %s\n", filename);
	return fd;
}

int
tcp_connect(char *host, char *port)
{
	int s, sfd;
	struct addrinfo  hints;
	struct addrinfo  *result, *rp;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;          /* Any protocol */

	s = getaddrinfo(host, port, &hints, &result);
	if (s != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		exit(EXIT_FAILURE);
	}

	printf("Connecting to %s:%s\n", host, port);
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sfd == -1)
			continue;

		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;                  /* Success */

		close(sfd);
	}

	freeaddrinfo(result);           /* No longer needed */

	if (rp == NULL) {               /* No address succeeded */
		fprintf(stderr, "Could not connect\n");
		exit(EXIT_FAILURE);
	}

	return sfd;
}

void
signal_handler()
{
	keep_going = false;
}

int
recv_iq(FT_HANDLE handle, int fd)
{
	uint8_t buf[READ_SIZE];
	int res;
	DWORD count;
	long long int bytes = 0;

	signal(SIGINT, signal_handler);

	while (keep_going) {
		if ((res = FT_ReadPipe(handle, IQ_IN, buf, sizeof(buf), &count, 0)) != FT_OK) {
			printf("FT_ReadPipe: %d\n", res);
			return false;
		}

		if ((res = write(fd, buf, count)) < 0) {
			perror("write");
			return false;
		}

		bytes += count;
		printf("\rRX %lld MiB ", bytes / (1024 * 1024));
		fflush(stdout);
	}
	printf("\n");
	return true;
}

int main(int argc, char *argv[])
{
	long devnum = get_device_list();
	FT_HANDLE handle;
	int fd = 0;

	FT_Create((PVOID) devnum, FT_OPEN_BY_INDEX, &handle);
	if (!handle) {
		printf("Failed to open FTDI device\n");
		return 1;
	}

	FT_SetPipeTimeout(handle, CMD_IN, TIMEOUT);

	uint8_t cmd[] = {0x1a, 0x0b}; // is IQ enabled?
	send_cmd(handle, cmd, sizeof(cmd));
	read_reply(handle);

	if (argc == 2) {
		fd = open_file(argv[1]);
	} else if (argc == 3) {
		fd = tcp_connect(argv[1], argv[2]);
	}

	if (fd > 0) {
		uint8_t cmd2[] = {0x1a, 0x0b, 0x01}; // enable IQ from Main VFO
		send_cmd(handle, cmd2, sizeof(cmd2));
		read_reply(handle);

		recv_iq(handle, fd);
	}

	uint8_t cmd3[] = {0x1a, 0x0b, 0x00}; // disable IQ
	send_cmd(handle, cmd3, sizeof(cmd3));
	read_reply(handle);

	close(fd);
	FT_AbortPipe(handle, CMD_OUT);
	FT_AbortPipe(handle, CMD_IN);
	FT_AbortPipe(handle, IQ_IN);
	FT_Close(handle);

	return 0;
}
