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

#include <errno.h>
#include <string.h>
#include <libubox/list.h>
#include <libubox/avl.h>

#include "querier.h"
#include "client.h"
#include "mrib.h"
#include "proxy.h"

struct proxy {
	struct list_head head;
	int ifindex;
	struct mrib_user mrib;
	struct querier querier;
	struct client client;
	struct avl_tree applied;
	enum proxy_flags flags;
	struct proxy_options opts;
};

struct proxy_downlink {
	struct querier_user_iface iface;
	struct mrib_user mrib;
	struct proxy *proxy;
};

// Set of sources under construction while aggregating downlink states
struct proxy_sources {
	struct in6_addr *addrs;
	size_t cnt;
	size_t cap;
};

// Filter last handed to the kernel for one group of a proxy. An entry exists
// only while the group is joined on the uplink.
struct proxy_applied {
	struct avl_node node;
	struct in6_addr addr;
	bool include;
	struct proxy_sources sources;
};

// Listener state of one downlink for one group
struct proxy_state {
	const struct group *group;
	bool include;
	bool any;
};

static struct list_head proxies = LIST_HEAD_INIT(proxies);

static struct proxy_downlink *proxy_downlink_of(struct querier_user *user)
{
	struct querier_user_iface *iface = container_of(user, struct querier_user_iface, user);
	return container_of(iface, struct proxy_downlink, iface);
}

// Remove and cleanup a downlink
static void proxy_remove_downlink(struct proxy_downlink *downlink)
{
	mrib_detach_user(&downlink->mrib);
	querier_detach(&downlink->iface);
	free(downlink);
}

// Match scope of a multicast-group against proxy scope-filter
static bool proxy_match_scope(enum proxy_flags flags, const struct in6_addr *addr)
{
	unsigned scope = 0;
	if (IN6_IS_ADDR_V4MAPPED(addr)) {
		if (addr->s6_addr[12] == 239 && addr->s6_addr[13] == 255)
			scope = PROXY_REALMLOCAL;
		else if (addr->s6_addr[12] == 239 && (addr->s6_addr[13] & 0xfc) == 192)
			scope = PROXY_ORGLOCAL;
		else if (addr->s6_addr[12] == 224 && addr->s6_addr[13] == 0 && addr->s6_addr[14] == 0)
			scope = 2;
		else
			scope = PROXY_GLOBAL;
	} else {
		scope = addr->s6_addr[1] & 0xf;
	}
	return scope >= (flags & _PROXY_SCOPEMASK);
}

// Test a source against the source networks of a proxy. A proxy without
// networks of the family of the source routes every source.
static bool proxy_match_source(const struct proxy *proxy, const struct in6_addr *source)
{
	bool v4 = IN6_IS_ADDR_V4MAPPED(source);
	bool family_listed = false;

	for (size_t i = 0; i < proxy->opts.altnets_cnt; ++i) {
		const struct proxy_altnet *net = &proxy->opts.altnets[i];
		if (IN6_IS_ADDR_V4MAPPED(&net->addr) != v4)
			continue;

		family_listed = true;
		if (prefix_contains(&net->addr, net->plen, source))
			return true;
	}

	return !family_listed;
}

// Test and set multicast route (called by mrib on detection of new source)
static void proxy_mrib(struct mrib_user *mrib, const struct in6_addr *group,
		const struct in6_addr *source, mrib_filter *filter)
{
	struct proxy *proxy = container_of(mrib, struct proxy, mrib);
	if (!proxy_match_scope(proxy->flags, group) || !proxy_match_source(proxy, source))
		return;

	omgp_time_t now = omgp_time();
	struct querier_user *user;
	list_for_each_entry(user, &proxy->querier.ifaces, head) {
		if (groups_includes_group(user->groups, group, source, now)) {
			struct querier_user_iface *iface = container_of(user, struct querier_user_iface, user);
			struct proxy_downlink *downlink = container_of(iface, struct proxy_downlink, iface);
			mrib_filter_add(filter, &downlink->mrib);
		}
	}
}

// Append a source to a source set, ignoring duplicates
static bool proxy_sources_add(struct proxy_sources *set, const struct in6_addr *addr)
{
	for (size_t i = 0; i < set->cnt; ++i)
		if (IN6_ARE_ADDR_EQUAL(&set->addrs[i], addr))
			return true;

	if (set->cnt == set->cap) {
		size_t cap = (set->cap > 0) ? set->cap * 2 : QUERIER_MAX_SOURCE;
		struct in6_addr *addrs = realloc(set->addrs, cap * sizeof(*addrs));
		if (!addrs)
			return false;

		set->addrs = addrs;
		set->cap = cap;
	}

	set->addrs[set->cnt++] = *addr;
	return true;
}

// Remove a source from a source set
static void proxy_sources_del(struct proxy_sources *set, const struct in6_addr *addr)
{
	for (size_t i = 0; i < set->cnt; ++i)
		if (IN6_ARE_ADDR_EQUAL(&set->addrs[i], addr)) {
			set->addrs[i] = set->addrs[--set->cnt];
			return;
		}
}

// Reduce a source set to the sources another downlink has in its list
static void proxy_sources_intersect(struct proxy_sources *set,
		const struct group *group, omgp_time_t now)
{
	size_t keep = 0;

	for (size_t i = 0; i < set->cnt; ++i) {
		struct group_source *source;
		bool found = false;

		group_for_each_active_source(source, group, now) {
			if (IN6_ARE_ADDR_EQUAL(&source->addr, &set->addrs[i])) {
				found = true;
				break;
			}
		}

		if (found)
			set->addrs[keep++] = set->addrs[i];
	}

	set->cnt = keep;
}

// Test if a source set contains a source
static bool proxy_sources_has(const struct proxy_sources *set, const struct in6_addr *addr)
{
	for (size_t i = 0; i < set->cnt; ++i)
		if (IN6_ARE_ADDR_EQUAL(&set->addrs[i], addr))
			return true;

	return false;
}

// Group comparator for the tree of applied filters
static int proxy_applied_cmp(const void *k1, const void *k2, __unused void *ptr)
{
	return memcmp(k1, k2, sizeof(struct in6_addr));
}

// Free the tree of applied filters
static void proxy_applied_clear(struct proxy *proxy)
{
	struct proxy_applied *applied, *n;

	avl_for_each_element_safe(&proxy->applied, applied, node, n) {
		avl_delete(&proxy->applied, &applied->node);
		free(applied->sources.addrs);
		free(applied);
	}
}

// Test if the kernel already holds the given filter for a group
static bool proxy_applied_test(const struct proxy_applied *applied,
		bool include, const struct proxy_sources *set)
{
	if (!applied || applied->include != include || applied->sources.cnt != set->cnt)
		return false;

	for (size_t i = 0; i < set->cnt; ++i)
		if (!proxy_sources_has(&applied->sources, &set->addrs[i]))
			return false;

	return true;
}

// Forget the filter of a group that is no longer joined
static void proxy_applied_drop(struct proxy *proxy, struct proxy_applied *applied)
{
	avl_delete(&proxy->applied, &applied->node);
	free(applied->sources.addrs);
	free(applied);
}

// Remember the filter handed to the kernel for a group
static void proxy_applied_store(struct proxy *proxy, const struct in6_addr *group,
		bool include, const struct proxy_sources *set)
{
	struct proxy_applied *applied = avl_find_element(&proxy->applied, group, applied, node);
	struct in6_addr *addrs = NULL;

	if (!applied) {
		if (!(applied = calloc(1, sizeof(*applied))))
			return;

		applied->addr = *group;
		applied->node.key = &applied->addr;
		avl_insert(&proxy->applied, &applied->node);
	}

	if (set->cnt > 0) {
		if (!(addrs = malloc(set->cnt * sizeof(*addrs)))) {
			// Forget the entry, so that the next event is applied again
			proxy_applied_drop(proxy, applied);
			return;
		}

		memcpy(addrs, set->addrs, set->cnt * sizeof(*addrs));
	}

	free(applied->sources.addrs);
	applied->sources.addrs = addrs;
	applied->sources.cnt = set->cnt;
	applied->sources.cap = set->cnt;
	applied->include = include;
}

// Get the listener state of a downlink for a group, false if it has no listeners
static bool proxy_downlink_state(struct proxy_downlink *downlink,
		const struct in6_addr *group, omgp_time_t now, struct proxy_state *state)
{
	const struct group *g = groups_get(downlink->iface.user.groups, group);
	struct group_source *source;
	size_t cnt = 0;

	if (!g)
		return false;

	state->group = g;

	// A downlink with IGMPv1/v2 reporters records EXCLUDE without sources
	// (RFC 3376 7.2.1), which asks for any source.
	if (g->compat_v1_until > now || g->compat_v2_until > now) {
		state->include = false;
		state->any = true;
		return true;
	}

	state->include = group_is_included(g, now);

	group_for_each_active_source(source, g, now)
		++cnt;

	// INCLUDE without sources is the "no sources wanted" state of RFC 3376 /
	// RFC 3810: this downlink does not want the group at all. It turns up when
	// the last listener leaves and after a TO_IN without sources, and joining
	// the group upstream for it would be wrong twice: it is not a request for
	// any source, and RFC 4607 does not allow an EXCLUDE join for an SSM group.
	if (state->include && cnt == 0)
		return false;

	// EXCLUDE without exceptions asks for every source
	state->any = (cnt == 0);

	return true;
}

// Aggregate the listener states of all downlinks of a proxy for one group
// (RFC 4605 3.2.2). Returns 0 or -ENOMEM if the aggregate did not fit;
// listeners tells whether any downlink wants the group at all.
static int proxy_aggregate(struct proxy *proxy, const struct in6_addr *group,
		bool *include, struct proxy_sources *set, bool *listeners)
{
	omgp_time_t now = omgp_time();
	struct querier_user *user;
	struct group_source *source;
	size_t ifaces = 0, cnt = 0;
	bool first = true;

	list_for_each_entry(user, &proxy->querier.ifaces, head)
		++ifaces;

	// Read every downlink's state once and aggregate the snapshot
	struct proxy_state states[ifaces ? ifaces : 1];

	list_for_each_entry(user, &proxy->querier.ifaces, head) {
		if (proxy_downlink_state(proxy_downlink_of(user), group, now, &states[cnt]))
			++cnt;
	}

	set->cnt = 0;
	*include = true;
	*listeners = (cnt > 0);

	// A downlink that takes the group from every source decides it for all of
	// them: the union of the receive sets is "any source" as well (RFC 4605
	// 3.2.2). This is where INCLUDE from one downlink and EXCLUDE from another
	// end up, without a filter change between them.
	for (size_t i = 0; i < cnt; ++i) {
		if (states[i].any) {
			*include = false;
			return 0;
		}

		if (!states[i].include)
			*include = false;
	}

	if (!cnt)
		return 0;

	if (*include) {
		// INCLUDE: union of the include lists
		for (size_t i = 0; i < cnt; ++i) {
			if (!states[i].include)
				continue;

			group_for_each_active_source(source, states[i].group, now)
				if (!proxy_sources_add(set, &source->addr))
					return -ENOMEM;
		}

		return 0;
	}

	// EXCLUDE: intersection of the exception lists
	for (size_t i = 0; i < cnt; ++i) {
		if (states[i].include)
			continue;

		if (first) {
			group_for_each_active_source(source, states[i].group, now)
				if (!proxy_sources_add(set, &source->addr))
					return -ENOMEM;
			first = false;
		} else {
			proxy_sources_intersect(set, states[i].group, now);
		}
	}

	// A source that an INCLUDE downlink asks for is not excluded after all
	for (size_t i = 0; i < cnt; ++i) {
		if (!states[i].include)
			continue;

		group_for_each_active_source(source, states[i].group, now)
			proxy_sources_del(set, &source->addr);
	}

	return 0;
}

// Recompute the upstream membership of a group from all downlinks
static void proxy_update_group(struct proxy *proxy, const struct in6_addr *group)
{
	struct proxy_sources set = {0};
	struct proxy_applied *applied;
	bool include = true;
	bool listeners = false;

	// Match the proxy's scope, not a downlink's: proxy_set() sets a downlink's
	// flags only after querier_attach() has announced every group that exists
	// on that interface, so a group joined without this check would never be
	// left again.
	if (!proxy_match_scope(proxy->flags, group))
		return;

	if (proxy_aggregate(proxy, group, &include, &set, &listeners)) {
		L_WARN("%s: failed to aggregate downlinks, keeping the membership", __FUNCTION__);
		goto out;
	}

	applied = avl_find_element(&proxy->applied, group, applied, node);

	if (!listeners) {
		// No downlink wants the group any more: the only case that leaves it
		// on the upstream.
		if (applied && client_leave(&proxy->client, group) == 0)
			proxy_applied_drop(proxy, applied);
	} else if (!applied) {
		if (client_join(&proxy->client, group, include, set.addrs, set.cnt) == 0)
			proxy_applied_store(proxy, group, include, &set);
	} else if (!proxy_applied_test(applied, include, &set)) {
		// The membership exists, only its filter changed. MSFILTER replaces
		// the filter in place (RFC 3678), so the uplink does not see a leave
		// and the players already watching keep their stream.
		if (client_update(&proxy->client, group, include, set.addrs, set.cnt) == 0)
			proxy_applied_store(proxy, group, include, &set);
	}

out:
	free(set.addrs);

	// The kernel reports a source only once, when its first packet has no
	// route. A route that exists must follow the membership changes.
	mrib_refresh(&proxy->mrib, group, 128);
}

// Update proxy state (called from querier on change of combined group-state)
static void proxy_trigger(struct querier_user_iface *user, const struct in6_addr *group,
		bool include, const struct in6_addr *sources, size_t len)
{
	struct proxy_downlink *downlink = container_of(user, struct proxy_downlink, iface);

	// The aggregate is read from every downlink, so the state announced here
	// (include, sources, len) is not used.
	proxy_update_group(downlink->proxy, group);
}

// Remove proxy with given name
static int proxy_unset(struct proxy *proxyp)
{
	bool found = false;
	struct proxy *proxy, *n;
	list_for_each_entry_safe(proxy, n, &proxies, head) {
		if ((proxyp && proxy == proxyp) ||
				(!proxyp && (proxy->flags & _PROXY_UNUSED))) {
			mrib_detach_user(&proxy->mrib);

			struct querier_user *user, *n;
			list_for_each_entry_safe(user, n, &proxy->querier.ifaces, head) {
				struct querier_user_iface *i = container_of(user, struct querier_user_iface, user);
				proxy_remove_downlink(container_of(i, struct proxy_downlink, iface));
			}

			querier_deinit(&proxy->querier);
			// After the querier, so that its last announcements can leave the
			// groups on the upstream socket.
			client_deinit(&proxy->client);
			proxy_applied_clear(proxy);
			list_del(&proxy->head);
			free(proxy);
			found = true;
		}
	}
	return (found) ? 0 : -ENOENT;
}

// Add / update proxy
int proxy_set(int uplink, const int downlinks[], size_t downlinks_cnt, enum proxy_flags flags,
		const struct proxy_options *opts)
{
	struct proxy *proxy = NULL, *p;
	list_for_each_entry(p, &proxies, head)
		if (p->ifindex == uplink)
			proxy = p;

	if (proxy && (downlinks_cnt == 0 ||
			((proxy->flags & _PROXY_SCOPEMASK) != (flags & _PROXY_SCOPEMASK)))) {
		proxy_unset(proxy);
		proxy = NULL;
	}

	if (downlinks_cnt <= 0)
		return 0;

	if (!proxy) {
		if (!(proxy = calloc(1, sizeof(*proxy))))
			return -ENOMEM;

		if ((flags & _PROXY_SCOPEMASK) == 0)
			flags |= PROXY_GLOBAL;

		proxy->flags = flags;
		proxy->ifindex = uplink;
		if (opts)
			proxy->opts = *opts;
		querier_init(&proxy->querier);
		avl_init(&proxy->applied, proxy_applied_cmp, false, NULL);
		list_add(&proxy->head, &proxies);
		// querier_attach() can call proxy_trigger() before it returns, so the
		// upstream client has to exist before any downlink is attached.
		if (client_init(&proxy->client, uplink))
			goto err;
		if (mrib_attach_user(&proxy->mrib, uplink, proxy_mrib))
			goto err;
	}

	struct querier_user *user, *n;
	list_for_each_entry_safe(user, n, &proxy->querier.ifaces, head) {
		struct querier_user_iface *iface = container_of(user, struct querier_user_iface, user);

		size_t i;
		for (i = 0; i < downlinks_cnt && downlinks[i] == iface->iface->ifindex; ++i);
		if (i == downlinks_cnt)
			proxy_remove_downlink(container_of(iface, struct proxy_downlink, iface));
	}

	for (size_t i = 0; i < downlinks_cnt; ++i) {
		bool found = false;
		struct querier_user *user;
		list_for_each_entry(user, &proxy->querier.ifaces, head) {
			struct querier_user_iface *iface = container_of(user, struct querier_user_iface, user);
			if (iface->iface->ifindex == downlinks[i]) {
				found = true;
				break;
			}
		}

		if (found)
			continue;

		struct proxy_downlink *downlink = calloc(1, sizeof(*downlink));
		if (!downlink)
			goto err;

		downlink->proxy = proxy;

		if (mrib_attach_user(&downlink->mrib, downlinks[i], NULL))
			goto downlink_err2;

		if (querier_attach(&downlink->iface, &proxy->querier, downlinks[i], proxy_trigger,
				&proxy->opts.querier))
			goto downlink_err1;

		continue;

downlink_err1:
		mrib_detach_user(&downlink->mrib);
downlink_err2:
		free(downlink);
		goto err;
	}

	return 0;

err:
	proxy_unset(proxy);
	return -errno;
}

// Mark all flushable proxies as unused
void proxy_update(bool all)
{
	struct proxy *proxy;
	list_for_each_entry(proxy, &proxies, head)
		if (all || (proxy->flags & PROXY_FLUSHABLE))
			proxy->flags |= _PROXY_UNUSED;
}


// Flush all unused proxies
void proxy_flush(void)
{
	proxy_unset(NULL);
}
