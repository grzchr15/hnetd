/*
 * Author: Steven Barth <steven@midlink.org>
 *
 * Copyright (c) 2014-2015 cisco Systems, Inc.
 */

#include "pd.h"
#include "hnetd.h"
#include "prefix_utils.h"
#include "hncp_pa.h"

#include <libubox/list.h>
#include <libubox/uloop.h>
#include <libubox/usock.h>
#include <libubox/ustream.h>
#include <sys/socket.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

//Time given to PA to provide first lease (even temporary)
#define PD_PA_TIMEOUT 5000

struct pd {
	struct uloop_fd fd;
	hncp_pa hncp_pa;
	struct list_head handles;
};


struct pd_handle {
	struct list_head head;
	struct ustream_fd fd;
	bool established;
	bool done;
	struct uloop_timeout timeout;
	hpa_lease lease;
	struct list_head prefixes;
	struct pd *pd;
};

struct pd_prefix {
	struct list_head le;
	struct prefix prefix;
	hnetd_time_t valid_until, preferred_until;
};

// TCP transmission has ended, either because of success or timeout or other error
static void pd_handle_done(struct ustream *s)
{
	struct pd_handle *c = container_of(s, struct pd_handle, fd.stream);
	if(c->done) //Prevent multiple dones
		return;

	c->done = 1;

	if (c->established)
		hpa_pd_del_lease(c->pd->hncp_pa, c->lease);

	close(c->fd.fd.fd);
	ustream_free(&c->fd.stream);
	list_del(&c->head);
	free(c);
}

//PA did not replied in time
static void pd_handle_timeout(struct uloop_timeout *to)
{
	struct pd_handle *c = container_of(to, struct pd_handle, timeout);
	pd_handle_done(&c->fd.stream);
}

// Update
static void pd_handle_update(struct pd_handle *c)
{
	struct pd_prefix *p;
	hnetd_time_t now = hnetd_time();

	uloop_timeout_cancel(&c->timeout);
	bool keep = false;
	bool sent = false;
	list_for_each_entry(p, &c->prefixes, le) {
		hnetd_time_t valid = (p->valid_until > now)?
				(p->valid_until - now) /HNETD_TIME_PER_SECOND:0;
		hnetd_time_t preferred = (p->preferred_until > now)?
				(p->preferred_until - now) / HNETD_TIME_PER_SECOND:0;

		if (valid > UINT32_MAX)
			valid = UINT32_MAX;

		if (preferred > UINT32_MAX)
			preferred = UINT32_MAX;

		if(!c->fd.fd.error)
			ustream_printf(&c->fd.stream, "%s,%"PRId64",%"PRId64"\n",
				PREFIX_REPR_C(&p->prefix), preferred, valid);

		sent = true;

		if (valid > 0)
			keep = true;
	}

	if (sent && !c->fd.fd.error) {
		ustream_write(&c->fd.stream, "\n", 1, false);
		ustream_write_pending(&c->fd.stream);
	}

	if (!keep)
		pd_handle_done(&c->fd.stream);
}

static void pd_cb(const struct in6_addr *prefix, uint8_t plen,
		hnetd_time_t valid_until, hnetd_time_t preferred_until,
		__unused const char *dhcp_data, __unused size_t dhcp_len,
		void *priv)
{
	struct pd_handle *c = (struct pd_handle *)priv;
	struct pd_prefix *p;
	list_for_each_entry(p, &c->prefixes, le) {
		if(!memcmp(&p->prefix.prefix, prefix, sizeof(struct in6_addr)) &&
				p->prefix.plen == plen)
			goto found;
	}
	if(!preferred_until || !(p = malloc(sizeof(*p))))
		return;
	p->prefix.prefix = *prefix;
	p->prefix.plen = plen;
	list_add(&p->le, &c->prefixes);
found:
	p->preferred_until = preferred_until;
	p->valid_until = valid_until;
	pd_handle_update(c);
	//todo: Would be better to send a diff instead of a complete dump
	if(!preferred_until) {
		list_del(&p->le);
		free(p);
	}
}

// More data was received from TCP connection
static void pd_handle_data(struct ustream *s, __unused int bytes_new)
{
	struct pd_handle *c = container_of(s, struct pd_handle, fd.stream);
	int pending;
	char *data = ustream_get_read_buf(s, &pending);
	char *end = memmem(data, pending, "\n\n", 2);

	uint8_t hint = 62;

	if (!c->established && end) {
		end += 2;
		end[-1] = 0;

		char *saveptr, *line;
		char *seed = strtok_r(data, "\n", &saveptr);
		// We don't care about the first line

		if ((line = strtok_r(NULL, "\n", &saveptr)) &&
				(line = strtok_r(NULL, "/", &saveptr)) &&
				(line = strtok_r(NULL, ",", &saveptr))) {
			hint = atoi(line);
		}

		if (hint > 64)
			hint = 64;

		if(!(c->lease = hpa_pd_add_lease(c->pd->hncp_pa, seed, hint, pd_cb, c))){
			pd_handle_done(s);
		} else {
			c->timeout.cb = pd_handle_timeout;
			c->timeout.pending = 0;
			uloop_timeout_set(&c->timeout, PD_PA_TIMEOUT);
			c->established = true;
		}
	}
}


static void pd_accept(struct uloop_fd *fd, __unused unsigned int events)
{
	struct pd *pd = container_of(fd, struct pd, fd);
	for (;;) {
		int sock = accept(fd->fd, NULL, 0);
		if (sock < 0) {
			if (errno == EWOULDBLOCK)
				break;
			else
				continue;
		}

		struct pd_handle *handle = calloc(1, sizeof(*handle));
		INIT_LIST_HEAD(&handle->prefixes);
		handle->pd = pd;

		handle->fd.stream.notify_read = pd_handle_data;
		handle->fd.stream.notify_state = pd_handle_done;

		ustream_fd_init(&handle->fd, sock);
		list_add(&handle->head, &pd->handles);
	}
}


struct pd* pd_create(hncp_pa hncp_pa, const char *path)
{
	L_DEBUG("Initialize HNCP Downstream PD on Unix socket %s", path);
	unlink(path);
	int sock = usock(USOCK_TCP | USOCK_SERVER | USOCK_UNIX, path, NULL);
	if (sock < 0)
		return NULL;

	struct pd *pd = calloc(1, sizeof(*pd));
	INIT_LIST_HEAD(&pd->handles);
	pd->hncp_pa = hncp_pa;
	pd->fd.fd = sock;
	pd->fd.cb = pd_accept;

	uloop_fd_add(&pd->fd, ULOOP_READ | ULOOP_EDGE_TRIGGER);

	return pd;
}


void pd_destroy(struct pd *pd)
{
	while (!list_empty(&pd->handles))
		pd_handle_done(&list_first_entry(&pd->handles, struct pd_handle, head)->fd.stream);

	close(pd->fd.fd);
	free(pd);
}
