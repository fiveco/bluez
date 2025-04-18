// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation.
 *
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>

#include <glib.h>

#define SEC_NSEC(_t)  ((_t) * 1000000000LL)
#define TS_NSEC(_ts)  (SEC_NSEC((_ts)->tv_sec) + (_ts)->tv_nsec)

#if !HAVE_DECL_SOF_TIMESTAMPING_TX_COMPLETION
#define SOF_TIMESTAMPING_TX_COMPLETION	(1 << 18)
#endif
#if !HAVE_DECL_SCM_TSTAMP_COMPLETION
#define SCM_TSTAMP_COMPLETION		(SCM_TSTAMP_ACK + 1)
#endif
#define TS_TX_RECORD_MASK		(SOF_TIMESTAMPING_TX_RECORD_MASK | \
						SOF_TIMESTAMPING_TX_COMPLETION)

struct tx_tstamp_data {
	struct {
		uint32_t id;
		uint32_t type;
	} expect[16];
	unsigned int pos;
	unsigned int count;
	unsigned int sent;
	uint32_t so_timestamping;
	bool stream;
};

static inline void tx_tstamp_init(struct tx_tstamp_data *data,
				uint32_t so_timestamping, bool stream)
{
	memset(data, 0, sizeof(*data));
	memset(data->expect, 0xff, sizeof(data->expect));

	data->so_timestamping = so_timestamping;
	data->stream = stream;
}

static inline int tx_tstamp_expect(struct tx_tstamp_data *data, size_t len)
{
	unsigned int pos = data->count;
	int steps;

	if (data->stream && len)
		data->sent += len - 1;

	if (data->so_timestamping & SOF_TIMESTAMPING_TX_SCHED) {
		g_assert(pos < ARRAY_SIZE(data->expect));
		data->expect[pos].type = SCM_TSTAMP_SCHED;
		data->expect[pos].id = data->sent;
		pos++;
	}

	if (data->so_timestamping & SOF_TIMESTAMPING_TX_SOFTWARE) {
		g_assert(pos < ARRAY_SIZE(data->expect));
		data->expect[pos].type = SCM_TSTAMP_SND;
		data->expect[pos].id = data->sent;
		pos++;
	}

	if (data->so_timestamping & SOF_TIMESTAMPING_TX_COMPLETION) {
		g_assert(pos < ARRAY_SIZE(data->expect));
		data->expect[pos].type = SCM_TSTAMP_COMPLETION;
		data->expect[pos].id = data->sent;
		pos++;
	}

	if (!data->stream || len)
		data->sent++;

	steps = pos - data->count;
	data->count = pos;
	return steps;
}

static inline int tx_tstamp_recv(struct tx_tstamp_data *data, int sk, int len)
{
	unsigned char control[512];
	ssize_t ret;
	char buf[1024];
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct scm_timestamping *tss = NULL;
	struct sock_extended_err *serr = NULL;
	struct timespec now;
	unsigned int i;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	ret = recvmsg(sk, &msg, MSG_ERRQUEUE);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return data->count - data->pos;

		tester_warn("Failed to read from errqueue: %s (%d)",
							strerror(errno), errno);
		return -EINVAL;
	}

	if (data->so_timestamping & SOF_TIMESTAMPING_OPT_TSONLY) {
		if (ret != 0) {
			tester_warn("Packet copied back to errqueue");
			return -EINVAL;
		}
	} else if (len > ret) {
		tester_warn("Packet not copied back to errqueue: %zd", ret);
		return -EINVAL;
	}

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
					cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET &&
					cmsg->cmsg_type == SCM_TIMESTAMPING) {
			tss = (void *)CMSG_DATA(cmsg);
		} else if (cmsg->cmsg_level == SOL_BLUETOOTH &&
					cmsg->cmsg_type == BT_SCM_ERROR) {
			serr = (void *)CMSG_DATA(cmsg);
		}
	}

	if (!tss) {
		tester_warn("SCM_TIMESTAMPING not found");
		return -EINVAL;
	}

	if (!serr) {
		tester_warn("BT_SCM_ERROR not found");
		return -EINVAL;
	}

	if (serr->ee_errno != ENOMSG ||
				serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
		tester_warn("BT_SCM_ERROR wrong for timestamping");
		return -EINVAL;
	}

	clock_gettime(CLOCK_REALTIME, &now);

	if (TS_NSEC(&now) < TS_NSEC(tss->ts) ||
			TS_NSEC(&now) > TS_NSEC(tss->ts) + SEC_NSEC(10)) {
		tester_warn("nonsense in timestamp");
		return -EINVAL;
	}

	if (data->pos >= data->count) {
		tester_warn("Too many timestamps");
		return -EINVAL;
	}

	/* Find first unreceived timestamp of the right type */
	for (i = 0; i < data->count; ++i) {
		if (data->expect[i].type >= 0xffff)
			continue;

		if (serr->ee_info == data->expect[i].type) {
			data->expect[i].type = 0xffff;
			break;
		}
	}
	if (i == data->count) {
		tester_warn("Bad timestamp type %u", serr->ee_info);
		return -EINVAL;
	}

	if ((data->so_timestamping & SOF_TIMESTAMPING_OPT_ID) &&
				serr->ee_data != data->expect[i].id) {
		tester_warn("Bad timestamp id %u", serr->ee_data);
		return -EINVAL;
	}

	tester_print("Got valid TX timestamp %u (type %u, id %u)", i,
						serr->ee_info, serr->ee_data);

	++data->pos;

	return data->count - data->pos;
}
