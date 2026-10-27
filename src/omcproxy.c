/*
 * Copyright 2015 Steven Barth <steven at midlink.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <libubox/uloop.h>
#include <libubox/blobmsg.h>

#include "omcproxy.h"
#include "proxy.h"

enum {
	PROXY_ATTR_SOURCE,
	PROXY_ATTR_SCOPE,
	PROXY_ATTR_DEST,
	PROXY_ATTR_MAX,
};

static struct blobmsg_policy proxy_policy[PROXY_ATTR_MAX] = {
	[PROXY_ATTR_SOURCE] = { .name = "source", .type = BLOBMSG_TYPE_STRING },
	[PROXY_ATTR_SCOPE] = { .name = "scope", .type = BLOBMSG_TYPE_STRING },
	[PROXY_ATTR_DEST] = { .name = "dest", .type = BLOBMSG_TYPE_ARRAY },
};

static int handle_proxy_set(void *data, size_t len)
{
	struct blob_attr *tb[PROXY_ATTR_MAX], *c;
	blobmsg_parse(proxy_policy, PROXY_ATTR_MAX, tb, data, len);

	const char *name = ((c = tb[PROXY_ATTR_SOURCE])) ? blobmsg_get_string(c) : NULL;
	const char *downlinks[32];
	size_t downlinks_cnt = 0;
	enum proxy_flags flags = 0;

	if (!name)
		return -EINVAL;

	if ((c = tb[PROXY_ATTR_SCOPE])) {
		const char *scope = blobmsg_get_string(c);
		if (!strcmp(scope, "global"))
			flags = PROXY_GLOBAL;
		else if (!strcmp(scope, "organization"))
			flags = PROXY_ORGLOCAL;
		else if (!strcmp(scope, "site"))
			flags = PROXY_SITELOCAL;
		else if (!strcmp(scope, "admin"))
			flags = PROXY_ADMINLOCAL;
		else if (!strcmp(scope, "realm"))
			flags = PROXY_REALMLOCAL;

		if (!flags) {
			L_WARN("%s(%s): invalid scope (%s)", __FUNCTION__, name, scope);
			return -EINVAL;
		}
	}

	if ((c = tb[PROXY_ATTR_DEST])) {
		struct blob_attr *d;
		unsigned rem;
		blobmsg_for_each_attr(d, c, rem) {
			if (downlinks_cnt >= 32) {
				L_WARN("%s(%s): maximum number of destinations exceeded", __FUNCTION__, name);
				return -EINVAL;
			}

			if (blobmsg_type(d) != BLOBMSG_TYPE_STRING) {
				return -EINVAL;
			}

			downlinks[downlinks_cnt++] = blobmsg_get_string(d);
		}
	}

	return proxy_set(name, downlinks, downlinks_cnt, flags);
}

static struct uloop_fd rtnl_fd = { .fd = -1 };

static void rtnl_event(struct uloop_fd *fd, unsigned int events)
{
	char buf[8192];
	ssize_t len;

	if (!(events & ULOOP_READ))
		return;

	while ((len = recv(fd->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		struct nlmsghdr *nlh;
		int remaining = (int)len;

		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, remaining);
			nlh = NLMSG_NEXT(nlh, remaining)) {
			if (nlh->nlmsg_type == NLMSG_DONE)
				break;
			if (nlh->nlmsg_type == NLMSG_ERROR)
				continue;
			if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK)
				proxy_reconcile_all();
		}
	}
}

static int rtnl_init(void)
{
	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_LINK,
	};

	rtnl_fd.fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
	if (rtnl_fd.fd < 0)
		return -errno;

	if (bind(rtnl_fd.fd, (struct sockaddr *)&addr, sizeof(addr))) {
		int ret = -errno;
		close(rtnl_fd.fd);
		rtnl_fd.fd = -1;
		return ret;
	}

	rtnl_fd.cb = rtnl_event;
	uloop_fd_add(&rtnl_fd, ULOOP_READ);
	return 0;
}

static void rtnl_deinit(void)
{
	if (rtnl_fd.fd < 0)
		return;

	uloop_fd_delete(&rtnl_fd);
	close(rtnl_fd.fd);
	rtnl_fd.fd = -1;
}

static void handle_signal(__unused int signal)
{
	uloop_end();
}

static void usage(const char *arg) {
	fprintf(stderr, "Usage: %s [options] <proxy1> [<proxy2>] [...]\n"
			"\nProxy examples:\n"
			"eth1,eth2\n"
			"eth1,eth2,eth3,scope=organization\n"
			"\nProxy options (each option may only occur once):\n"
			"	<interface>			interfaces to proxy (first is uplink)\n"
			"	scope=<scope>			minimum multicast scope to proxy\n"
			"		[global,organization,site,admin,realm] (default: global)\n"
			"\nOptions:\n"
			"	-v				verbose logging\n"
			"	-h				show this help\n",
	arg);
}

int main(int argc, char **argv) {
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	openlog("omcproxy", LOG_PERROR | LOG_PID, LOG_DAEMON);
	setlogmask(LOG_UPTO(L_LEVEL));

	if (getuid()) {
		L_ERR("must be run as root!");
		return 2;
	}

	uloop_init();
	bool start = true;

	if (rtnl_init()) {
		L_ERR("failed to initialize rtnetlink listener: %s", strerror(errno));
		start = false;
	}

	for (ssize_t i = 1; i < argc; ++i) {
		const char *source = NULL;
		const char *scope = NULL;
		struct blob_buf b = {NULL, NULL, 0, NULL};

		if (!strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 1;
		} else if (!strncmp(argv[i], "-v", 2)) {
			int log_level;
			if ((log_level = atoi(&argv[i][2])) <= 0)
				log_level = LOG_DEBUG;
			setlogmask(LOG_UPTO(log_level));
			continue;
		}


		blob_buf_init(&b, 0);

		void *k = blobmsg_open_array(&b, "dest");
		for (char *c = strtok(argv[i], ","); c; c = strtok(NULL, ",")) {
			if (!strncmp(c, "scope=", 6)) {
				scope = &c[6];
			} else if (!source) {
				source = c;
			} else {
				blobmsg_add_string(&b, NULL, c);
			}
		}
		blobmsg_close_array(&b, k);

		if (source)
			blobmsg_add_string(&b, "source", source);

		if (scope)
			blobmsg_add_string(&b, "scope", scope);

		if (handle_proxy_set(blob_data(b.head), blob_len(b.head))) {
			fprintf(stderr, "failed to setup proxy: %s\n", argv[i]);
			start = false;
		}

		blob_buf_free(&b);
	}

	if (argc < 2) {
		usage(argv[0]);
		start = false;
	}

	if (start)
		uloop_run();

	proxy_update(true);
	proxy_flush();
	rtnl_deinit();

	uloop_done();
	return 0;
}
