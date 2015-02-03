/*
 * Author: Steven Barth <steven@midlink.org>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 */

#include <syslog.h>
#include <errno.h>
#include <assert.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <resolv.h>

#include <sys/un.h>
#include <sys/socket.h>
#include <libubox/usock.h>
#include <libubox/blobmsg_json.h>

#include "dhcpv6.h"
#include "dhcp.h"
#include "platform.h"
#include "iface.h"
#include "prefix_utils.h"
#include "hncp_dump.h"
#include "dncp_trust.h"

static char backend[] = "/usr/sbin/hnetd-backend";
static const char *hnetd_pd_socket = NULL;
static void ipc_handle(struct uloop_fd *fd, __unused unsigned int events);
static int ipc_ifupdown(const char *method, int argc, char* const argv[]);
static struct uloop_fd ipcsock = { .cb = ipc_handle };
static const char *ipcpath = "/var/run/hnetd.sock";
static const char *ipcpath_client = "/var/run/hnetd-client%d.sock";
static dncp hncp = NULL;
static struct platform_rpc_method *hnet_rpc_methods[PLATFORM_RPC_MAX];
static size_t rpc_methods_cnt = 0;

struct platform_iface {
	pid_t dhcpv4;
	pid_t dhcpv6;
};

int platform_init(dncp hncp_in, __unused struct pa_data *data, const char *pd_socket)
{
	hncp = hncp_in;
	hnetd_pd_socket = pd_socket;

	unlink(ipcpath);
	ipcsock.fd = usock(USOCK_UNIX | USOCK_SERVER | USOCK_UDP, ipcpath, NULL);
	if (ipcsock.fd < 0) {
		L_ERR("Unable to create IPC socket");
		return 3;
	}
	uloop_fd_add(&ipcsock, ULOOP_EDGE_TRIGGER | ULOOP_READ);
	return 0;
}

int platform_rpc_register(struct platform_rpc_method *m)
{
	if (rpc_methods_cnt >= PLATFORM_RPC_MAX)
		return -ENOBUFS;

	hnet_rpc_methods[rpc_methods_cnt++] = m;
	return 0;
}

int platform_rpc_cli(const char *method, struct blob_attr *in)
{
	char sockaddr[108]; //Client address
	struct sockaddr_un serveraddr; //Server sockaddr
	int ret = 0;
	serveraddr.sun_family = AF_UNIX;
	strcpy(serveraddr.sun_path, ipcpath);

	snprintf(sockaddr, 107, ipcpath_client, getpid());
	unlink(sockaddr);
	int sock = usock(USOCK_UNIX | USOCK_SERVER | USOCK_UDP, sockaddr, NULL);
	if (sock < 0) {
		perror("Failed to open socket");
		return 2;
	}

	struct blob_buf b = {NULL, NULL, 0, NULL};
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "command", method);

	struct blob_attr *a;
	unsigned rem;
	blobmsg_for_each_attr(a, in, rem)
		blobmsg_add_blob(&b, a);


	if (sendto(sock, blob_data(b.head), blob_len(b.head), 0,
			(struct sockaddr *)&serveraddr, sizeof(serveraddr)) > 0) {
		struct __packed {
			struct blob_attr hdr;
			uint8_t buf[1024*128];
		} resp;

		struct timeval tv = {3, 0};
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		ret = 4;
		ssize_t rcvlen = recv(sock, resp.buf, sizeof(resp.buf), 0);
		if (rcvlen >= 0) {
			resp.hdr.id_len = 0;
			blob_set_raw_len(&resp.hdr, rcvlen + sizeof(resp.hdr));
			char *json = blobmsg_format_json_indent(&resp.hdr, true, true);
			if (json) {
				puts(json);
				ret = 0;
			}
		}

		if (ret > 0)
			perror("Failed to retrieve from hnetd");

	} else {
		perror("Failed to send to hnetd");
		ret = 3;
	}

	unlink(sockaddr);
	return ret;
}

int platform_rpc_multicall(int argc, char *const argv[])
{
	char *method = strstr(argv[0], "hnet-");
	if (method) {
		method += 5;

		if (!strcmp(method, "ifresolve")) {
			if (!argv[1])
				return 1;

			int ifindex = if_nametoindex(argv[1]);
			if (ifindex) {
				printf("%i\n", ifindex);
				return 0;
			} else {
				return 2;
			}
		} else if (!strcmp(method, "call")) {
			if (argc < 3)
				return 1;

			struct blob_buf b = {NULL, NULL, 0, NULL};
			blob_buf_init(&b, 0);

			if (!blobmsg_add_json_from_string(&b, argv[2])) {
				fprintf(stderr, "Failed to parse input data: %s\n", argv[2]);
				return 1;
			}

			return platform_rpc_cli(argv[1], b.head);
		} else if (!strcmp(method, "ifup") || !strcmp(method, "ifdown")) {
			if (argc < 2)
				return 1;
			return ipc_ifupdown(method, argc, argv);
		} else {
			size_t i;
			for (i = 0; i < rpc_methods_cnt && strcmp(hnet_rpc_methods[i]->name, method); ++i);
			if (i < rpc_methods_cnt && hnet_rpc_methods[i]->main)
				return hnet_rpc_methods[i]->main(hnet_rpc_methods[i], argc, argv);
		}
	}

	return -1;
}

// Run platform script
static pid_t platform_run(char *argv[])
{
	pid_t pid = vfork();
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(128);
	}
	return pid;
}

//
static void platform_call(char *argv[])
{
	pid_t pid = platform_run(argv);
	waitpid(pid, NULL, 0);
}

// Constructor for openwrt-specific interface part
void platform_iface_new(struct iface *c, __unused const char *handle)
{
	char *argv_dhcpv4[] = {backend, "dhcpv4client", c->ifname, NULL};
	char *argv_dhcpv6[] = {backend, "dhcpv6client", c->ifname, NULL};
	assert(c->platform == NULL);

	struct platform_iface *iface = calloc(1, sizeof(*iface));
	if (!(c->flags & IFACE_FLAG_INTERNAL) ||
			(c->flags & IFACE_FLAG_HYBRID) == IFACE_FLAG_HYBRID) {
		iface->dhcpv4 = platform_run(argv_dhcpv4);
		iface->dhcpv6 = platform_run(argv_dhcpv6);
	}

	c->platform = iface;
}

// Destructor for openwrt-specific interface part
void platform_iface_free(struct iface *c)
{
	struct platform_iface *iface = c->platform;
	if (iface) {
		if (iface->dhcpv4)
			kill(iface->dhcpv4, SIGTERM);

		if (iface->dhcpv6)
			kill(iface->dhcpv6, SIGTERM);

		free(iface);
		c->platform = NULL;
	}
}


void platform_set_internal(struct iface *c, bool internal)
{
	char *argv[] = {backend, !internal ? "setfilter" : "unsetfilter",
			c->ifname, NULL};
	platform_call(argv);
}


void platform_filter_prefix(struct iface *c, const struct prefix *p, bool enable)
{
	char abuf[PREFIX_MAXBUFFLEN];
	prefix_ntop(abuf, sizeof(abuf), p, true);
	char *argv[] = {backend, (enable) ? "newblocked" : "delblocked",
			c->ifname, abuf, NULL};
	platform_call(argv);
}


void platform_set_address(struct iface *c, struct iface_addr *a, bool enable)
{
	hnetd_time_t now = hnetd_time();
	char abuf[PREFIX_MAXBUFFLEN], pbuf[10] = "", vbuf[10] = "", cbuf[10] = "";
	prefix_ntop(abuf, sizeof(abuf), &a->prefix, false);

	if (!IN6_IS_ADDR_V4MAPPED(&a->prefix.prefix)) {
		hnetd_time_t valid = (a->valid_until - now) / HNETD_TIME_PER_SECOND;
		if (valid <= 0)
			enable = false;
		else if (valid > UINT32_MAX)
			valid = UINT32_MAX;

		hnetd_time_t preferred = (a->preferred_until - now) / HNETD_TIME_PER_SECOND;
		if (preferred < 0)
			preferred = 0;
		else if (preferred > UINT32_MAX)
			preferred = UINT32_MAX;

		snprintf(pbuf, sizeof(pbuf), "%u", (unsigned)preferred);
		snprintf(vbuf, sizeof(vbuf), "%u", (unsigned)valid);
	}

	uint8_t *oend = &a->dhcpv6_data[a->dhcpv6_len], *odata;
	uint16_t olen, otype;
	dhcpv6_for_each_option(a->dhcpv6_data, oend, otype, olen, odata) {
#ifdef EXT_PREFIX_CLASS
		if (otype == DHCPV6_OPT_PREFIX_CLASS && olen == 2) {
			uint16_t class = (uint16_t)odata[0] << 8 | (uint16_t)odata[1];
			snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)class);
		}
#endif
	}

	char *argv[] = {backend, (enable) ? "newaddr" : "deladdr",
			c->ifname, abuf, pbuf, vbuf, cbuf, NULL};
	platform_call(argv);
}


void platform_set_snat(struct iface *c, const struct prefix *p)
{
	char sbuf[INET_ADDRSTRLEN], pbuf[PREFIX_MAXBUFFLEN], prefix[3] = {0};
	inet_ntop(AF_INET, &c->v4_saddr, sbuf, sizeof(sbuf));
	prefix_ntop(pbuf, sizeof(pbuf), p, true);

	if (!c->designatedv4)
		snprintf(prefix, sizeof(prefix), "%d", c->v4_prefix);

	char *argv[] = {backend, (p && c->v4_saddr.s_addr) ? "newnat" : "delnat",
			c->ifname, sbuf, pbuf, prefix, NULL};
	platform_call(argv);
}


void platform_set_route(struct iface *c, struct iface_route *route, bool enable)
{
	char from[PREFIX_MAXBUFFLEN];
	char to[PREFIX_MAXBUFFLEN];
	char via[INET6_ADDRSTRLEN];
	char metric[10];

	prefix_ntop(to, sizeof(to), &route->to, true);

	if (!IN6_IS_ADDR_V4MAPPED(&route->to.prefix))
		inet_ntop(AF_INET6, &route->via, via, sizeof(via));
	else
		inet_ntop(AF_INET, &route->via.s6_addr[12], via, sizeof(via));

	if (!IN6_IS_ADDR_V4MAPPED(&route->to.prefix))
		prefix_ntop(from, sizeof(from), &route->from, true);
	else
		from[0] = 0;

	snprintf(metric, sizeof(metric), "%u", route->metric);

	char *argv[] = {backend, (enable) ? "newroute" : "delroute",
			c->ifname, to, via, metric,
			(from[0]) ? from : NULL, NULL};
	platform_call(argv);
}


void platform_set_dhcp(struct iface *c, enum hncp_link_elected elected)
{
	if (elected & (HNCP_LINK_LEGACY | HNCP_LINK_PREFIXDEL | HNCP_LINK_HOSTNAMES | HNCP_LINK_STATELESS)) {
		char *argv[] = {backend, "startdhcp", c->ifname,
				(elected & HNCP_LINK_LEGACY) ? "1" : "",
				(elected & (HNCP_LINK_PREFIXDEL | HNCP_LINK_HOSTNAMES)) ? "1" : "",
				(elected & HNCP_LINK_PREFIXDEL) ? (char*)hnetd_pd_socket : "", NULL};
		platform_call(argv);
	} else {
		char *argv[] = {backend, "stopdhcp", c->ifname, NULL};
		platform_call(argv);
	}
}


void platform_restart_dhcpv4(struct iface *c)
{
	struct platform_iface *iface = c->platform;
	if (iface) {
		char metricbuf[16];
		snprintf(metricbuf, sizeof(metricbuf), "%i", 1000 + if_nametoindex(c->ifname));

		if (iface->dhcpv4)
			kill(iface->dhcpv4, SIGTERM);

		char *argv_dhcpv4[] = {backend, "dhcpv4client", c->ifname,
				(c->designatedv4) ? "0" : "1", metricbuf, NULL};

		iface->dhcpv4 = platform_run(argv_dhcpv4);
	}
}


void platform_set_prefix_route(const struct prefix *p, bool enable)
{
	char buf[PREFIX_MAXBUFFLEN];
	prefix_ntop(buf, sizeof(buf), p, true);
	char *argv[] = {backend, (enable) ? "newprefixroute" : "delprefixroute", buf, NULL};
	platform_call(argv);
}


void platform_set_dhcpv6_send(struct iface *c, const void *dhcpv6_data, size_t len, const void *dhcp_data, size_t len4)
{
	// DNS options
	const size_t dns_max = 4;
	size_t dns_cnt = 0;
	struct in6_addr dns[dns_max];

	const size_t domainbuf_size = 8 + dns_max * 256;
	char domainbuf[domainbuf_size];
	strcpy(domainbuf, "SEARCH=");
	iface_get_fqdn(c->ifname, domainbuf + strlen(domainbuf), 256);
	size_t domainbuf_len = strlen(domainbuf);

	// Add per interface DHCPv6 options
	uint8_t *oend = ((uint8_t*)dhcpv6_data) + len, *odata;
	uint16_t olen, otype;
	dhcpv6_for_each_option(dhcpv6_data, oend, otype, olen, odata) {
		if (otype == DHCPV6_OPT_DNS_SERVERS) {
			size_t cnt = olen / sizeof(*dns);
			if (cnt + dns_cnt > dns_max)
				cnt = dns_max - dns_cnt;

			memcpy(&dns[dns_cnt], odata, cnt * sizeof(*dns));
			dns_cnt += cnt;
		} else if (otype == DHCPV6_OPT_DNS_DOMAIN) {
			uint8_t *oend = &odata[olen];
			while (odata < oend) {
				domainbuf[domainbuf_len++] = ' ';
				int l = dn_expand(odata, oend, odata, &domainbuf[domainbuf_len],
						domainbuf_size - domainbuf_len);
				if (l <= 0)
					break;
				domainbuf_len = strlen(domainbuf);
				odata += l;
			}
		}
	}

	// DNS options
	size_t dns4_cnt = 0;
	struct in_addr dns4[dns_max];

	// Add per interface DHCPv6 options
	uint8_t *o4end = ((uint8_t*)dhcp_data) + len4;
	struct dhcpv4_option *opt;
	dhcpv4_for_each_option(dhcp_data, o4end, opt) {
		if (opt->type == DHCPV4_OPT_DNSSERVER) {
			size_t cnt = opt->len / sizeof(*dns4);
			if (cnt + dns4_cnt > dns_max)
				cnt = dns_max - dns_cnt;

			memcpy(&dns4[dns4_cnt], opt->data, cnt * sizeof(*dns4));
			dns4_cnt += cnt;
		}
	}

	pid_t pid = fork();
	if (pid == 0) {
		char *argv[] = {backend, "setdhcpv6", c->ifname, NULL};

		char *dnsbuf = malloc((dns_cnt + dns4_cnt) * INET6_ADDRSTRLEN + 5);
		strcpy(dnsbuf, "DNS=");
		size_t dnsbuflen = strlen(dnsbuf);

		char *rawbuf = malloc(c->dhcpv6_len_out * 2 + 10);
		strncpy(rawbuf, "PASSTHRU=", 10);

		dhcpv6_for_each_option(c->dhcpv6_data_out, ((uint8_t*)c->dhcpv6_data_out) + c->dhcpv6_len_out, otype, olen, odata)
			if (otype != DHCPV6_OPT_DNS_SERVERS && otype != DHCPV6_OPT_DNS_DOMAIN)
				hexlify(rawbuf + strlen(rawbuf), &odata[-4], olen + 4);

		char *radefaultbuf = malloc(16);
		snprintf(radefaultbuf, 16, "RA_DEFAULT=%d", (c->flags & IFACE_FLAG_ULA_DEFAULT) ? 1 : 0);

		for (size_t i = 0; i < dns_cnt; ++i) {
			inet_ntop(AF_INET6, &dns[i], &dnsbuf[dnsbuflen], INET6_ADDRSTRLEN);
			dnsbuflen = strlen(dnsbuf);
			dnsbuf[dnsbuflen++] = ' ';
		}

		for (size_t i = 0; i < dns4_cnt; ++i) {
			inet_ntop(AF_INET, &dns4[i], &dnsbuf[dnsbuflen], INET_ADDRSTRLEN);
			dnsbuflen = strlen(dnsbuf);
			dnsbuf[dnsbuflen++] = ' ';
		}

		if (dns_cnt || dns4_cnt)
			dnsbuf[dnsbuflen - 1] = 0;

		char guestbuf[10];
		sprintf(guestbuf, "GUEST=%s",
			(c->flags & IFACE_FLAG_GUEST) == IFACE_FLAG_GUEST ?
			"1": "");
		putenv(guestbuf);
		putenv(dnsbuf);
		putenv(domainbuf);
		putenv(rawbuf);
		putenv(radefaultbuf);

		execv(argv[0], argv);
		_exit(128);
	}
	waitpid(pid, NULL, 0);
}

enum ipc_option {
	OPT_COMMAND,
	OPT_IFNAME,
	OPT_HANDLE,
	OPT_PREFIX,
	OPT_IPV4SOURCE,
	OPT_DNS,
	OPT_MODE,
	OPT_LINK_ID,
	OPT_IFACE_ID,
	OPT_IP6_PLEN,
	OPT_IP4_PLEN,
	OPT_DISABLE_PA,
	OPT_PASSTHRU,
	OPT_ULA_DEFAULT_ROUTER,
	OPT_KEEPALIVE_INTERVAL,
	OPT_TRICKLE_K,
	OPT_DNSNAME,
	OPT_MAX
};

struct blobmsg_policy ipc_policy[] = {
	[OPT_COMMAND] = {"command", BLOBMSG_TYPE_STRING},
	[OPT_IFNAME] = {"ifname", BLOBMSG_TYPE_STRING},
	[OPT_HANDLE] = {"handle", BLOBMSG_TYPE_STRING},
	[OPT_PREFIX] = {"prefix", BLOBMSG_TYPE_ARRAY},
	[OPT_IPV4SOURCE] = {"ipv4source", BLOBMSG_TYPE_STRING},
	[OPT_DNS] = {"dns", BLOBMSG_TYPE_ARRAY},
	[OPT_MODE] = {"mode", BLOBMSG_TYPE_STRING},
	[OPT_LINK_ID] = {"link_id", BLOBMSG_TYPE_STRING},
	[OPT_IFACE_ID] = {"iface_id", BLOBMSG_TYPE_ARRAY},
	[OPT_IP6_PLEN] = {"ip6assign", BLOBMSG_TYPE_STRING},
	[OPT_IP4_PLEN] = {"ip4assign", BLOBMSG_TYPE_STRING},
	[OPT_DISABLE_PA] = {"disable_pa", BLOBMSG_TYPE_BOOL},
	[OPT_PASSTHRU] = {"passthru", BLOBMSG_TYPE_STRING},
	[OPT_ULA_DEFAULT_ROUTER] = {"ula_default_router", BLOBMSG_TYPE_BOOL},
	[OPT_KEEPALIVE_INTERVAL] = { .name = "keepalive_interval", .type = BLOBMSG_TYPE_INT32 },
	[OPT_TRICKLE_K] = { .name = "trickle_k", .type = BLOBMSG_TYPE_INT32 },
	[OPT_DNSNAME] = { .name = "dnsname", .type = BLOBMSG_TYPE_STRING},
};

enum ipc_prefix_option {
	PREFIX_ADDRESS,
	PREFIX_EXCLUDED,
	PREFIX_PREFERRED,
	PREFIX_VALID,
	PREFIX_CLASS,
	PREFIX_MAX
};

struct blobmsg_policy ipc_prefix_policy[] = {
	[PREFIX_ADDRESS] = {"address", BLOBMSG_TYPE_STRING},
	[PREFIX_EXCLUDED] = {"excluded", BLOBMSG_TYPE_STRING},
	[PREFIX_PREFERRED] = {"preferred", BLOBMSG_TYPE_INT32},
	[PREFIX_VALID] = {"valid", BLOBMSG_TYPE_INT32},
	[PREFIX_CLASS] = {"class", BLOBMSG_TYPE_INT32}
};

// Multicall handler for hnet-ifup/hnet-ifdown
int ipc_ifupdown(const char *method, int argc, char *const argv[])
{
	struct blob_buf b = {NULL, NULL, 0, NULL};
	blob_buf_init(&b, 0);

	bool external = false;
	void *p;
	char *buf;
	char *entry;

	int c, i;
	while ((c = getopt(argc, argv, "c:dp:l:i:m:n:uk:P:")) > 0) {
		switch(c) {
		case 'c':
			blobmsg_add_string(&b, "mode", optarg);
			break;

		case 'p':
			buf = strdup(optarg);
			p = blobmsg_open_array(&b, "prefix");
			for (entry = strtok(buf, ", "); entry; entry = strtok(NULL, ", "))
				blobmsg_add_string(&b, NULL, entry);
			blobmsg_close_array(&b, p);
			free(buf);
			break;

		case 'l':
			blobmsg_add_string(&b, "link_id", optarg);
			break;

		case 'i':
			buf = strdup(optarg);
			p = blobmsg_open_array(&b, "iface_id");
			for (entry = strtok(buf, ","); entry; entry = strtok(NULL, ","))
				blobmsg_add_string(&b, NULL, entry);
			blobmsg_close_array(&b, p);
			free(buf);
			break;

		case 'm':
			blobmsg_add_string(&b, "ip6assign", optarg);
			break;

		case 'n':
			blobmsg_add_string(&b, "ip4assign", optarg);
			break;

		case 'd':
			blobmsg_add_u8(&b, "disable_pa", 1);
			break;

		case 'u':
			blobmsg_add_u8(&b, "ula_default_router", 1);
			break;
		case 'k':
			if(sscanf(optarg, "%d", &i) == 1)
				blobmsg_add_u32(&b, "trickle_k", i);
			break;
		case 'P':
			if(sscanf(optarg, "%d", &i) == 1)
				blobmsg_add_u32(&b, "keepalive_interval", i);
			break;
		}
	}

	blobmsg_add_string(&b, "ifname", argv[optind]);

	if (!external)
		blobmsg_add_string(&b, "handle", argv[optind]);

	return platform_rpc_cli(method, b.head);
}

struct prefix zeros_64_prefix = { .prefix = { .s6_addr = {}}, .plen = 64 } ;

// Handle internal IPC message
static void ipc_handle(struct uloop_fd *fd, __unused unsigned int events)
{
	struct __packed {
		struct blob_attr hdr;
		uint8_t buf[1024*128];
	} req;

	ssize_t len;
	struct sockaddr_un sender;
	socklen_t sender_len = sizeof(sender);
	struct blob_attr *tb[OPT_MAX];

	while ((len = recvfrom(fd->fd, req.buf, sizeof(req.buf), MSG_DONTWAIT,
			(struct sockaddr*)&sender, &sender_len)) >= 0) {
		blob_set_raw_len(&req.hdr, len + sizeof(req.hdr));
		blobmsg_parse(ipc_policy, OPT_MAX, tb, req.buf, len);
		if(!tb[OPT_COMMAND])
			continue;

		const char *cmd = blobmsg_get_string(tb[OPT_COMMAND]);
		L_DEBUG("Handling ipc command %s", cmd);

		size_t i;
		for (i = 0; i < rpc_methods_cnt && strcmp(hnet_rpc_methods[i]->name, cmd); ++i);
		if (i < rpc_methods_cnt && hnet_rpc_methods[i]->cb) {
			struct blob_buf b = {NULL, NULL, 0, NULL};
			blob_buf_init(&b, 0);

			int ret = hnet_rpc_methods[i]->cb(hnet_rpc_methods[i], &req.hdr, &b);
			if (ret)
				blobmsg_add_u32(&b, "error", ret);

			sendto(fd->fd, blob_data(b.head), blob_len(b.head), MSG_DONTWAIT,
					(struct sockaddr *)&sender, sender_len);

			blob_buf_free(&b);
			continue;
		}


		if (!tb[OPT_IFNAME]) {
			sendto(fd->fd, NULL, 0, MSG_DONTWAIT, (struct sockaddr *)&sender, sender_len);
			continue;
		}

		const char *ifname = blobmsg_get_string(tb[OPT_IFNAME]);
		struct iface *c = iface_get(ifname);
		L_DEBUG("ipc_handle cmd:%s ifname:%s iface:%p", cmd, ifname, c);
		if (!strcmp(cmd, "ifup")) {
			iface_flags flags = 0;

			if (tb[OPT_MODE]) {
				const char *mode = blobmsg_get_string(tb[OPT_MODE]);
				if (!strcmp(mode, "adhoc"))
					flags |= IFACE_FLAG_ADHOC;
				else if (!strcmp(mode, "guest"))
					flags |= IFACE_FLAG_GUEST;
				else if (!strcmp(mode, "hybrid"))
					flags |= IFACE_FLAG_HYBRID;
				else if (!strcmp(mode, "leaf"))
					flags |= IFACE_FLAG_LEAF;
				else if (!strcmp(mode, "external"))
					tb[OPT_HANDLE] = NULL;
				else if (strcmp(mode, "auto"))
					L_WARN("Unknown mode '%s' for interface %s: falling back to auto", mode, ifname);
			}

			if (tb[OPT_DISABLE_PA] && blobmsg_get_bool(tb[OPT_DISABLE_PA]))
				flags |= IFACE_FLAG_DISABLE_PA;

			if (tb[OPT_ULA_DEFAULT_ROUTER] && blobmsg_get_bool(tb[OPT_ULA_DEFAULT_ROUTER]))
				flags |= IFACE_FLAG_ULA_DEFAULT;

			struct iface *iface = iface_create(ifname, tb[OPT_HANDLE] == NULL ? NULL :
					blobmsg_get_string(tb[OPT_HANDLE]), flags);

			if (iface && tb[OPT_PREFIX]) {
				struct blob_attr *k;
				unsigned rem;

				blobmsg_for_each_attr(k, tb[OPT_PREFIX], rem) {
					struct prefix p;
					if (blobmsg_type(k) == BLOBMSG_TYPE_STRING &&
							prefix_pton(blobmsg_get_string(k), &p) == 1)
						iface_add_chosen_prefix(iface, &p);
				}
			}

			unsigned link_id, link_mask = 8;
			if (iface && tb[OPT_LINK_ID] && sscanf(
						blobmsg_get_string(tb[OPT_LINK_ID]),
						"%x/%u", &link_id, &link_mask) >= 1)
					iface_set_link_id(iface, link_id, link_mask);

			if (iface && tb[OPT_IFACE_ID]) {
				struct blob_attr *k;
				unsigned rem;

				blobmsg_for_each_attr(k, tb[OPT_IFACE_ID], rem) {
					if (blobmsg_type(k) == BLOBMSG_TYPE_STRING) {
						char astr[55], fstr[55];
						struct prefix filter, addr;
						int res = sscanf(blobmsg_get_string(k), "%54s %54s", astr, fstr);
						if(res <= 0 || !prefix_pton(astr, &addr) || (res > 1 && !prefix_pton(fstr, &filter))) {
							L_ERR("Incorrect iface_id syntax %s", blobmsg_get_string(k));
							continue;
						}
						if(addr.plen == 128 && prefix_contains(&zeros_64_prefix, &addr))
							addr.plen = 64;
						if(res == 1)
							filter.plen = 0;
						iface_add_addrconf(iface, &addr.prefix, 128 - addr.plen, &filter);
					}
				}
			}

			unsigned ip6_plen;
			if(iface && tb[OPT_IP6_PLEN]
				       && sscanf(blobmsg_get_string(tb[OPT_IP6_PLEN]), "%u", &ip6_plen) == 1
				       && ip6_plen <= 128) {
				iface->ip6_plen = ip6_plen;
			}

			unsigned ip4_plen;
			if(iface && tb[OPT_IP4_PLEN]
				       && sscanf(blobmsg_get_string(tb[OPT_IP4_PLEN]), "%u", &ip4_plen) == 1
				       && ip4_plen <= 128) {
				iface->ip4_plen = ip4_plen;
			}

			dncp_link_conf conf;
			if(c && tb[OPT_KEEPALIVE_INTERVAL] && (conf = dncp_if_find_conf_by_name(hncp, c->ifname))) {
				conf->keepalive_interval = (((hnetd_time_t) blobmsg_get_u32(tb[OPT_KEEPALIVE_INTERVAL])) * HNETD_TIME_PER_SECOND) / 1000;
			}

			if(c && tb[OPT_TRICKLE_K] && (conf = dncp_if_find_conf_by_name(hncp, c->ifname)))
				conf->trickle_k = (int) blobmsg_get_u32(tb[OPT_TRICKLE_K]);
			if(c && tb[OPT_DNSNAME] && (conf = dncp_if_find_conf_by_name(hncp, c->ifname)))
				strncpy(conf->dnsname, blobmsg_get_string(tb[OPT_DNSNAME]), sizeof(conf->dnsname));

		} else if (!c) {
			L_ERR("invalid interface - command:%s ifname:%s",
			      cmd, ifname);
		} else if (!strcmp(cmd, "ifdown")) {
			iface_remove(c);
		} else if (!strcmp(cmd, "enable_ipv4_uplink")) {
			struct in_addr ipv4source = {INADDR_ANY};
			const size_t dns_max = 4;
			size_t dns_cnt = 0;
			struct __packed {
				uint8_t type;
				uint8_t len;
				struct in_addr addr[dns_max];
			} dns;

			if (tb[OPT_IPV4SOURCE])
				inet_pton(AF_INET, blobmsg_get_string(tb[OPT_IPV4SOURCE]), &ipv4source);

			if (tb[OPT_DNS]) {
				struct blob_attr *k;
				unsigned rem;

				blobmsg_for_each_attr(k, tb[OPT_DNS], rem) {
					if (dns_cnt >= dns_max || blobmsg_type(k) != BLOBMSG_TYPE_STRING ||
							inet_pton(AF_INET, blobmsg_data(k), &dns.addr[dns_cnt]) < 1)
						continue;

					++dns_cnt;
				}
			}

			if (dns_cnt) {
				dns.type = DHCPV4_OPT_DNSSERVER;
				dns.len = 4 * dns_cnt;
			}

			iface_update_ipv4_uplink(c);
			iface_add_dhcp_received(c, &dns, ((uint8_t*)&dns.addr[dns_cnt]) - ((uint8_t*)&dns));
			iface_set_ipv4_uplink(c, &ipv4source, 24);
			iface_commit_ipv4_uplink(c);
		} else if (!strcmp(cmd, "disable_ipv4_uplink")) {
			iface_update_ipv4_uplink(c);
			iface_commit_ipv4_uplink(c);
		} else if (!strcmp(cmd, "enable_ipv6_uplink")) {
			hnetd_time_t now = hnetd_time();
			iface_update_ipv6_uplink(c);

			struct blob_attr *k;
			unsigned rem;
			blobmsg_for_each_attr(k, tb[OPT_PREFIX], rem) {
				hnetd_time_t valid = HNETD_TIME_MAX, preferred = HNETD_TIME_MAX;

				struct prefix addr = {IN6ADDR_ANY_INIT, 0};
				struct prefix ex = {IN6ADDR_ANY_INIT, 0};
				struct blob_attr *tb[PREFIX_MAX];
				blobmsg_parse(ipc_prefix_policy, PREFIX_MAX, tb,
						blobmsg_data(k), blobmsg_data_len(k));

				if (!tb[PREFIX_ADDRESS] || !prefix_pton(blobmsg_get_string(tb[PREFIX_ADDRESS]), &addr))
					continue;

				if (tb[PREFIX_EXCLUDED])
					prefix_pton(blobmsg_get_string(tb[PREFIX_EXCLUDED]), &ex);

				if (tb[PREFIX_PREFERRED])
					preferred = now + blobmsg_get_u32(tb[PREFIX_PREFERRED]) * HNETD_TIME_PER_SECOND;

				if (tb[PREFIX_VALID])
					valid = now + blobmsg_get_u32(tb[PREFIX_VALID]) * HNETD_TIME_PER_SECOND;

				void *data = NULL;
				size_t len = 0;

#ifdef EXT_PREFIX_CLASS
				struct dhcpv6_prefix_class pclass = {
					.type = htons(DHCPV6_OPT_PREFIX_CLASS),
					.len = htons(2),
					.class = htons(atoi(blobmsg_get_string(a)))
				};

				if ((a = tb[PREFIX_CLASS])) {
					data = &pclass;
					len = sizeof(pclass);
				}
#endif
				iface_add_delegated(c, &addr, (ex.plen) ? &ex : NULL, valid, preferred, data, len);
			}


			if (tb[OPT_PASSTHRU]) {
				size_t buflen = blobmsg_data_len(tb[OPT_PASSTHRU]) / 2;
				uint8_t *buf = malloc(buflen);
				if (buf) {
					unhexlify(buf, buflen, blobmsg_get_string(tb[OPT_PASSTHRU]));
					iface_add_dhcpv6_received(c, buf, buflen);
					free(buf);
				}
			}

			iface_commit_ipv6_uplink(c);
		} else if (!strcmp(cmd, "disable_ipv6_uplink")) {
			iface_update_ipv6_uplink(c);
			iface_commit_ipv6_uplink(c);
		}

		//Send an empty response
		sendto(fd->fd, NULL, 0, MSG_DONTWAIT, (struct sockaddr *)&sender, sender_len);
	}
}

