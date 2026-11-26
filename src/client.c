/*
 * Author: Steven Barth <steven at midlink.org>
 *
 * Copyright 2015 Deutsche Telekom AG
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

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <alloca.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/mroute6.h>
#include <libubox/list.h>
#include <libubox/avl.h>

#include "client.h"

// A source filter as handed to the kernel: the group_filter and its list
struct client_group {
	struct group_filter f;
	struct sockaddr_storage sources[];
};

// Socket level of the family of a group
static int client_sol(const struct in6_addr *group)
{
	return IN6_IS_ADDR_V4MAPPED(group) ? SOL_IP : SOL_IPV6;
}

// Socket file descriptor of the family of a group
static int client_fd(const struct client *client, const struct in6_addr *group)
{
	return IN6_IS_ADDR_V4MAPPED(group) ? client->igmp_fd : client->mld_fd;
}

// Length of a group filter for the given number of sources
static size_t client_len(size_t cnt)
{
	return sizeof(struct group_filter) + cnt * sizeof(struct sockaddr_storage);
}

// Construct the MSFILTER for outgoing IGMP / MLD, returns its length
static size_t client_filter(struct client_group *filter, const struct client *client,
		const struct in6_addr *group, bool include,
		const struct in6_addr sources[], size_t cnt)
{
	int family = (IN6_IS_ADDR_V4MAPPED(group)) ? AF_INET : AF_INET6;
	size_t len = client_len(cnt);
	struct sockaddr_in *in_addr = (struct sockaddr_in*)&filter->f.gf_group;
	struct sockaddr_in6 *in6_addr = (struct sockaddr_in6*)&filter->f.gf_group;

	memset(filter, 0, len);
	filter->f.gf_interface = client->ifindex;
	filter->f.gf_fmode = include ? MCAST_INCLUDE : MCAST_EXCLUDE;
	filter->f.gf_group.ss_family = family;
	filter->f.gf_numsrc = cnt;

	if (family == AF_INET)
		client_unmap(&in_addr->sin_addr, group);
	else
		in6_addr->sin6_addr = *group;

	for (size_t i = 0; i < cnt; ++i) {
		filter->f.gf_slist[i].ss_family = family;

		in_addr = (struct sockaddr_in*)&filter->f.gf_slist[i];
		in6_addr = (struct sockaddr_in6*)&filter->f.gf_slist[i];

		if (family == AF_INET)
			client_unmap(&in_addr->sin_addr, &sources[i]);
		else
			in6_addr->sin6_addr = sources[i];
	}

	return len;
}

// Join a group with a source filter. A join without filter is a plain
// any-source membership, which the kernel records as exclude-without-sources.
int client_join(struct client *client, const struct in6_addr *group, bool include,
		const struct in6_addr sources[], size_t cnt)
{
	char addrbuf[INET6_ADDRSTRLEN];
	struct client_group *filter = alloca(client_len(cnt));
	size_t len = client_filter(filter, client, group, include, sources, cnt);
	int sol = client_sol(group);
	int fd = client_fd(client, group);

	inet_ntop(AF_INET6, group, addrbuf, sizeof(addrbuf));
	L_DEBUG("%s: %s on %d => %s (+%d sources)", __FUNCTION__, addrbuf,
			client->ifindex, (include) ? "include" : "exclude", (int)cnt);

	if (setsockopt(fd, sol, MCAST_JOIN_GROUP, &filter->f, sizeof(struct group_req))) {
		if (IN6_IS_ADDR_V4MAPPED(group) && errno == ENOBUFS)
			L_WARN("proxy: kernel denied joining multicast group. check igmp_max_memberships?");

		return -errno;
	}

	if (include || cnt > 0) {
		if (setsockopt(fd, sol, MCAST_MSFILTER, &filter->f, len)) {
			int err = -errno;

			// The join above succeeded, so the kernel holds an any-source
			// membership for this group. Drop it again: the caller does not
			// record a failed join, and a membership nobody knows about
			// cannot be left when the last listener goes.
			setsockopt(fd, sol, MCAST_LEAVE_GROUP, &filter->f, sizeof(struct group_req));
			return err;
		}
	}

	return 0;
}

// Replace the source filter of a membership that exists (RFC 3678: MSFILTER
// changes the filter of a group, the membership itself stays). A failure leaves
// the previous filter in place, which is what the caller still has recorded.
int client_update(struct client *client, const struct in6_addr *group, bool include,
		const struct in6_addr sources[], size_t cnt)
{
	char addrbuf[INET6_ADDRSTRLEN];
	struct client_group *filter = alloca(client_len(cnt));
	size_t len = client_filter(filter, client, group, include, sources, cnt);

	inet_ntop(AF_INET6, group, addrbuf, sizeof(addrbuf));
	L_DEBUG("%s: %s on %d => %s (+%d sources)", __FUNCTION__, addrbuf,
			client->ifindex, (include) ? "include" : "exclude", (int)cnt);

	if (setsockopt(client_fd(client, group), client_sol(group), MCAST_MSFILTER,
			&filter->f, len))
		return -errno;

	return 0;
}

// Leave a group
int client_leave(struct client *client, const struct in6_addr *group)
{
	char addrbuf[INET6_ADDRSTRLEN];
	struct client_group *filter = alloca(client_len(0));

	client_filter(filter, client, group, false, NULL, 0);

	inet_ntop(AF_INET6, group, addrbuf, sizeof(addrbuf));
	L_DEBUG("%s: %s on %d", __FUNCTION__, addrbuf, client->ifindex);

	if (setsockopt(client_fd(client, group), client_sol(group), MCAST_LEAVE_GROUP,
			&filter->f, sizeof(struct group_req)))
		return -errno;

	return 0;
}

// Initialize client-instance
int client_init(struct client *client, int ifindex)
{
	client->igmp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (client->igmp_fd < 0)
		return -errno;

	client->mld_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (client->mld_fd < 0) {
		close(client->igmp_fd);
		return -errno;
	}

	client->ifindex = ifindex;
	return 0;
}

// Cleanup client-instance
void client_deinit(struct client *client)
{
	if (client->ifindex) {
		close(client->igmp_fd);
		close(client->mld_fd);
		client->igmp_fd = -1;
		client->mld_fd = -1;
		client->ifindex = 0;
	}
}
