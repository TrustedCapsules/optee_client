/*
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <optee_msg_supplicant.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <tee_client_api.h>
#include <teec_trace.h>
#include <tee_supp_network.h>
#include <tee_supplicant.h>
#include <unistd.h>
#include <__tee_ipsocket.h>
#include <__tee_isocket_defines.h>
#include <__tee_tcpsocket_defines.h>
#include <__tee_udpsocket_defines.h>

#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#include <linux/tee.h>

#define TS_NSEC_PER_SEC	1000000000

static void ts_add(const struct timespec *a, const struct timespec *b,
		   struct timespec *res)
{
	res->tv_sec = a->tv_sec + b->tv_sec;
	res->tv_nsec = a->tv_nsec + b->tv_nsec;
	if (res->tv_nsec >= TS_NSEC_PER_SEC) {
		res->tv_sec++;
		res->tv_nsec -= TS_NSEC_PER_SEC;
	}
}

static int ts_diff_to_polltimeout(const struct timespec *a,
				  const struct timespec *b)
{
	struct timespec diff;

	diff.tv_sec = a->tv_sec - b->tv_sec;
	diff.tv_nsec = a->tv_nsec - b->tv_nsec;
	if (a->tv_nsec < b->tv_nsec) {
		diff.tv_nsec += TS_NSEC_PER_SEC;
		diff.tv_sec--;
	}

	if ((diff.tv_sec - 1) > (INT_MAX / 1000))
		return INT_MAX;
	return diff.tv_sec * 1000 + diff.tv_nsec / (TS_NSEC_PER_SEC / 1000);
}

static void ts_delay_from_millis(uint32_t millis, struct timespec *res)
{
	res->tv_sec = millis / 1000;
	res->tv_nsec = (millis % 1000) * (TS_NSEC_PER_SEC / 1000);
}

static TEEC_Result poll_with_timeout(struct pollfd *pfd, nfds_t nfds,
				     uint32_t timeout)
{
	struct timespec now;
	struct timespec until;
	int to = 0;
	int r;

	if (timeout == OPTEE_MRC_SOCKET_TIMEOUT_BLOCKING) {
		to = -1;
	} else {
		struct timespec delay;

		ts_delay_from_millis(timeout, &delay);

		if (clock_gettime(CLOCK_REALTIME, &now))
			return TEEC_ERROR_GENERIC;

		ts_add(&now, &delay, &until);
	}

	while (true) {
		if (to != -1)
			to = ts_diff_to_polltimeout(&until, &now);

		r = poll(pfd, nfds, to);
		if (!r)
			return TEE_ISOCKET_ERROR_TIMEOUT;
		if (r == -1) {
			/*
			 * If we're interrupted by a signal treat
			 * recalculate the timeout (if needed) and wait
			 * again.
			 */
			if (errno == EINTR) {
				if (to != -1 &&
				    clock_gettime(CLOCK_REALTIME, &now))
					return TEEC_ERROR_GENERIC;
				continue;
			}
			return TEEC_ERROR_BAD_PARAMETERS;
		}
		return TEEC_SUCCESS;
	}
}

static TEEC_Result write_with_timeout(int fd, const void *buf, size_t *blen,
				      uint32_t timeout)
{
	TEEC_Result res;
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	ssize_t r;

	res = poll_with_timeout(&pfd, 1, timeout);
	if (res != TEEC_SUCCESS)
		return res;

	r = write(fd, buf, *blen);
	if (r == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return TEE_ISOCKET_ERROR_TIMEOUT;
		return TEEC_ERROR_BAD_PARAMETERS;
	}
	*blen = r;
	return TEEC_SUCCESS;
}

static TEEC_Result read_with_timeout(int fd, void *buf, size_t *blen,
				     uint32_t timeout)
{
	TEEC_Result res;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	ssize_t r;

	res = poll_with_timeout(&pfd, 1, timeout);
	if (res != TEEC_SUCCESS)
		return res;

	r = read(fd, buf, *blen);
	if (r == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return TEE_ISOCKET_ERROR_TIMEOUT;
		return TEEC_ERROR_BAD_PARAMETERS;
	}
	*blen = r;
	return TEEC_SUCCESS;
}

static bool chk_pt(struct tee_ioctl_param *param, uint32_t type)
{
	return (param->attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) == type;
}

static int fd_flags_add(int fd, int flags)
{
	int val;

	val = fcntl(fd, F_GETFD, 0);
	if (val == -1)
		return -1;

	val |= flags;

	return fcntl(fd, F_SETFL, val);
}

static TEEC_Result sock_connect(uint32_t ip_vers, unsigned int protocol,
				const char *server, uint16_t port, int *ret_fd)
{
	TEEC_Result r = TEEC_ERROR_GENERIC;
	struct addrinfo hints;
	struct addrinfo *res0;
	struct addrinfo *res;
	int fd = -1;
	char port_name[10];

	snprintf(port_name, sizeof(port_name), "%" PRIu16, port);

	memset(&hints, 0, sizeof(hints));

	switch (ip_vers) {
	case TEE_IP_VERSION_DC:
		hints.ai_family = AF_UNSPEC;
		break;
	case TEE_IP_VERSION_4:
		hints.ai_family = AF_INET;
		break;
	case TEE_IP_VERSION_6:
		hints.ai_family = AF_INET6;
		break;
	default:
		return TEEC_ERROR_BAD_PARAMETERS;
	}

	if (protocol == TEE_ISOCKET_PROTOCOLID_TCP)
		hints.ai_socktype = SOCK_STREAM;
	else if (protocol == TEE_ISOCKET_PROTOCOLID_UDP)
		hints.ai_socktype = SOCK_DGRAM;
	else
		return TEEC_ERROR_BAD_PARAMETERS;

	if (getaddrinfo(server, port_name, &hints, &res0))
		return TEE_ISOCKET_ERROR_HOSTNAME;

	for (res = res0; res; res = res->ai_next) {
		fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd == -1) {
			if (errno == ENOMEM || errno == ENOBUFS)
				r = TEE_ISOCKET_ERROR_OUT_OF_RESOURCES;
			else
				r = TEEC_ERROR_GENERIC;
			continue;
		}

		if (connect(fd, res->ai_addr, res->ai_addrlen)) {
			if (errno == ETIMEDOUT)
				r = TEE_ISOCKET_ERROR_TIMEOUT;
			else
				r = TEEC_ERROR_COMMUNICATION;

			close(fd);
			fd = -1;
			continue;
		}

		if (fd_flags_add(fd, O_NONBLOCK)) {
			close(fd);
			fd = -1;
			r = TEEC_ERROR_GENERIC;
			break;
		}

		r = TEEC_SUCCESS;
		break;
	}

	freeaddrinfo(res0);
	*ret_fd = fd;
	return r;
}

static TEEC_Result tee_network_open(size_t num_params,
				   struct tee_ioctl_param *params)
{
	TEEC_Result res;
	int fd;
	char *server;
	uint32_t ip_vers;
	uint16_t port;
	uint32_t protocol;

	if (num_params != 4 ||
	    !chk_pt(params + 0, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT) ||
	    !chk_pt(params + 1, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT) ||
	    !chk_pt(params + 2, TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT) ||
	    !chk_pt(params + 3, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT))
		return TEEC_ERROR_BAD_PARAMETERS;

	port = params[1].u.value.a;
	protocol = params[1].u.value.b;
	ip_vers = params[1].u.value.c;

	server = tee_supp_param_to_va(params + 2);
	if (!server || server[params[2].u.memref.size - 1] != '\0')
		return TEE_ISOCKET_ERROR_HOSTNAME;

	res = sock_connect(ip_vers, protocol, server, port, &fd);
	if (res != TEEC_SUCCESS)
		return res;

	params[3].u.value.a = fd;
	return TEEC_SUCCESS;
}

static TEEC_Result tee_network_close(size_t num_params,
				    struct tee_ioctl_param *params)
{
	int fd;

	if (num_params != 1 ||
	    !chk_pt(params + 0, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT))
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].u.value.b;
	if (fd < 0)
		return TEEC_ERROR_BAD_PARAMETERS;
	if (close(fd)) {
		EMSG("tee_socket_close: close(%d): %s", fd, strerror(errno));
		return TEEC_ERROR_GENERIC;
	}
	return TEEC_SUCCESS;
}

static TEEC_Result tee_network_send(size_t num_params,
					struct tee_ioctl_param *params)
{
	TEEC_Result res;
	int fd;
	void *buf;
	size_t bytes;

	if (num_params != 3 ||
	    !chk_pt(params + 0, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT) ||
	    !chk_pt(params + 1, TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT) ||
	    !chk_pt(params + 2, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT))
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].u.value.b;
	if (fd < 0)
		return TEEC_ERROR_BAD_PARAMETERS;

	buf = tee_supp_param_to_va(params + 1);
	bytes = params[1].u.memref.size;
	res = write_with_timeout(fd, buf, &bytes, params[2].u.value.a);
	if (res == TEEC_SUCCESS)
		params[2].u.value.b = bytes;
	return res;
}

static TEEC_Result tee_network_recv(size_t num_params,
				   struct tee_ioctl_param *params)
{
	TEEC_Result res;
	int fd;
	void *buf;
	size_t bytes;

	if (num_params != 3 ||
	    !chk_pt(params + 0, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT) ||
	    !chk_pt(params + 1, TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT) ||
	    !chk_pt(params + 2, TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT))
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].u.value.b;
	if (fd < 0)
		return TEEC_ERROR_BAD_PARAMETERS;

	buf = tee_supp_param_to_va(params + 1);

	bytes = params[1].u.memref.size;
	res = read_with_timeout(fd, buf, &bytes, params[0].u.value.c);
	if (res == TEEC_SUCCESS)
		params[2].u.value.a = bytes;

	return res;
}

TEEC_Result tee_supp_network_process(size_t num_params,
				struct tee_ioctl_param *params)
{
	if (!num_params || !tee_supp_param_is_value(params))
		return TEEC_ERROR_BAD_PARAMETERS;

	switch (params->u.value.a) {
	case OPTEE_MRC_NETWORK_OPEN:
		return tee_network_open(num_params, params);
	case OPTEE_MRC_NETWORK_CLOSE:
		return tee_network_close(num_params, params);
	case OPTEE_MRC_NETWORK_SEND:
		return tee_network_send(num_params, params);
	case OPTEE_MRC_NETWORK_RECV:
		return tee_network_recv(num_params, params);
    default:
		return TEEC_ERROR_BAD_PARAMETERS;
	}
}
