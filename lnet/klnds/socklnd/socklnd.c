/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/klnds/socklnd/socklnd.c
 *
 * Author: Zach Brown <zab@zabbo.net>
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Eric Barton <eric@bartonsoftware.com>
 */

#include <linux/inetdevice.h>
#include "socklnd.h"
#include <linux/sunrpc/addr.h>

static const struct lnet_lnd the_ksocklnd;
struct ksock_nal_data ksocknal_data;

static struct ksock_interface *
ksocknal_ip2iface(struct lnet_ni *ni, struct sockaddr *addr)
{
	struct ksock_net *net = ni->ni_data;
	int i;
	struct ksock_interface *iface;

	for (i = 0; i < net->ksnn_ninterfaces; i++) {
		LASSERT(i < LNET_INTERFACES_NUM);
		iface = &net->ksnn_interfaces[i];

		if (rpc_cmp_addr((struct sockaddr *)&iface->ksni_addr, addr))
			return iface;
	}

	return NULL;
}

static struct ksock_interface *
ksocknal_index2iface(struct lnet_ni *ni, int index)
{
	struct ksock_net *net = ni->ni_data;
	int i;
	struct ksock_interface *iface;

	for (i = 0; i < net->ksnn_ninterfaces; i++) {
		LASSERT(i < LNET_INTERFACES_NUM);
		iface = &net->ksnn_interfaces[i];

		if (iface->ksni_index == index)
			return iface;
	}

	return NULL;
}

static int ksocknal_ip2index(struct sockaddr *addr, struct lnet_ni *ni)
{
	struct net_device *dev;
	int ret = -1;
	DECLARE_CONST_IN_IFADDR(ifa);

	if (addr->sa_family != AF_INET)
		/* No IPv6 support yet */
		return ret;

	rcu_read_lock();
	for_each_netdev(ni->ni_net_ns, dev) {
		int flags = dev_get_flags(dev);
		struct in_device *in_dev;

		if (flags & IFF_LOOPBACK) /* skip the loopback IF */
			continue;

		if (!(flags & IFF_UP))
			continue;

		in_dev = __in_dev_get_rcu(dev);
		if (!in_dev)
			continue;

		in_dev_for_each_ifa_rcu(ifa, in_dev) {
			if (ifa->ifa_local ==
			    ((struct sockaddr_in *)addr)->sin_addr.s_addr)
				ret = dev->ifindex;
		}
		endfor_ifa(in_dev);
		if (ret >= 0)
			break;
	}
	rcu_read_unlock();

	return ret;
}

static struct ksock_route *
ksocknal_create_route(struct sockaddr *addr)
{
	struct ksock_route *route;

	LIBCFS_ALLOC (route, sizeof (*route));
	if (route == NULL)
		return (NULL);

	refcount_set(&route->ksnr_refcount, 1);
	route->ksnr_peer = NULL;
	route->ksnr_retry_interval = 0;         /* OK to connect at any time */
	rpc_copy_addr((struct sockaddr *)&route->ksnr_addr, addr);
	rpc_set_port((struct sockaddr *)&route->ksnr_addr, rpc_get_port(addr));
	route->ksnr_myiface = -1;
	route->ksnr_scheduled = 0;
	route->ksnr_connecting = 0;
	route->ksnr_connected = 0;
	route->ksnr_deleted = 0;
	route->ksnr_conn_count = 0;
	route->ksnr_share_count = 0;

	return route;
}

void
ksocknal_destroy_route(struct ksock_route *route)
{
	LASSERT(refcount_read(&route->ksnr_refcount) == 0);

	if (route->ksnr_peer != NULL)
		ksocknal_peer_decref(route->ksnr_peer);

	LIBCFS_FREE (route, sizeof (*route));
}

static struct ksock_peer_ni *
ksocknal_create_peer(struct lnet_ni *ni, struct lnet_process_id id)
{
	int cpt = lnet_cpt_of_nid(id.nid, ni);
	struct ksock_net *net = ni->ni_data;
	struct ksock_peer_ni *peer_ni;

	LASSERT(id.nid != LNET_NID_ANY);
	LASSERT(id.pid != LNET_PID_ANY);
	LASSERT(!in_interrupt());

	if (!atomic_inc_unless_negative(&net->ksnn_npeers)) {
		CERROR("Can't create peer_ni: network shutdown\n");
		return ERR_PTR(-ESHUTDOWN);
	}

	LIBCFS_CPT_ALLOC(peer_ni, lnet_cpt_table(), cpt, sizeof(*peer_ni));
	if (!peer_ni) {
		atomic_dec(&net->ksnn_npeers);
		return ERR_PTR(-ENOMEM);
	}

	peer_ni->ksnp_ni = ni;
	peer_ni->ksnp_id = id;
	refcount_set(&peer_ni->ksnp_refcount, 1); /* 1 ref for caller */
	peer_ni->ksnp_closing = 0;
	peer_ni->ksnp_accepting = 0;
	peer_ni->ksnp_proto = NULL;
	peer_ni->ksnp_last_alive = 0;
	peer_ni->ksnp_zc_next_cookie = SOCKNAL_KEEPALIVE_PING + 1;

	INIT_LIST_HEAD(&peer_ni->ksnp_conns);
	INIT_LIST_HEAD(&peer_ni->ksnp_routes);
	INIT_LIST_HEAD(&peer_ni->ksnp_tx_queue);
	INIT_LIST_HEAD(&peer_ni->ksnp_zc_req_list);
	spin_lock_init(&peer_ni->ksnp_lock);

	return peer_ni;
}

void
ksocknal_destroy_peer(struct ksock_peer_ni *peer_ni)
{
	struct ksock_net *net = peer_ni->ksnp_ni->ni_data;

	CDEBUG (D_NET, "peer_ni %s %p deleted\n",
		libcfs_id2str(peer_ni->ksnp_id), peer_ni);

	LASSERT(refcount_read(&peer_ni->ksnp_refcount) == 0);
	LASSERT(peer_ni->ksnp_accepting == 0);
	LASSERT(list_empty(&peer_ni->ksnp_conns));
	LASSERT(list_empty(&peer_ni->ksnp_routes));
	LASSERT(list_empty(&peer_ni->ksnp_tx_queue));
	LASSERT(list_empty(&peer_ni->ksnp_zc_req_list));

	LIBCFS_FREE(peer_ni, sizeof(*peer_ni));

	/* NB a peer_ni's connections and routes keep a reference on their
	 * peer_ni until they are destroyed, so we can be assured that _all_
	 * state to do with this peer_ni has been cleaned up when its refcount
	 * drops to zero.
	 */
	if (atomic_dec_and_test(&net->ksnn_npeers))
		wake_up_var(&net->ksnn_npeers);
}

struct ksock_peer_ni *
ksocknal_find_peer_locked(struct lnet_ni *ni, struct lnet_process_id id)
{
	struct ksock_peer_ni *peer_ni;

	hash_for_each_possible(ksocknal_data.ksnd_peers, peer_ni,
			       ksnp_list, id.nid) {
		LASSERT(!peer_ni->ksnp_closing);

		if (peer_ni->ksnp_ni != ni)
			continue;

		if (peer_ni->ksnp_id.nid != id.nid ||
		    peer_ni->ksnp_id.pid != id.pid)
			continue;

		CDEBUG(D_NET, "got peer_ni [%p] -> %s (%d)\n",
		       peer_ni, libcfs_id2str(id),
		       refcount_read(&peer_ni->ksnp_refcount));
		return peer_ni;
	}
	return NULL;
}

struct ksock_peer_ni *
ksocknal_find_peer(struct lnet_ni *ni, struct lnet_process_id id)
{
	struct ksock_peer_ni *peer_ni;

	read_lock(&ksocknal_data.ksnd_global_lock);
	peer_ni = ksocknal_find_peer_locked(ni, id);
	if (peer_ni != NULL)			/* +1 ref for caller? */
		ksocknal_peer_addref(peer_ni);
	read_unlock(&ksocknal_data.ksnd_global_lock);

        return (peer_ni);
}

static void
ksocknal_unlink_peer_locked(struct ksock_peer_ni *peer_ni)
{
	int i;
	struct ksock_interface *iface;

	for (i = 0; i < peer_ni->ksnp_n_passive_ips; i++) {
		struct sockaddr_in sa = { .sin_family = AF_INET };
		LASSERT(i < LNET_INTERFACES_NUM);
		sa.sin_addr.s_addr = htonl(peer_ni->ksnp_passive_ips[i]);

		iface = ksocknal_ip2iface(peer_ni->ksnp_ni,
					  (struct sockaddr *)&sa);
		/*
		 * All IPs in peer_ni->ksnp_passive_ips[] come from the
		 * interface list, therefore the call must succeed.
		 */
		LASSERT(iface != NULL);

		CDEBUG(D_NET, "peer_ni=%p iface=%p ksni_nroutes=%d\n",
		       peer_ni, iface, iface->ksni_nroutes);
		iface->ksni_npeers--;
	}

	LASSERT(list_empty(&peer_ni->ksnp_conns));
	LASSERT(list_empty(&peer_ni->ksnp_routes));
	LASSERT(!peer_ni->ksnp_closing);
	peer_ni->ksnp_closing = 1;
	hlist_del(&peer_ni->ksnp_list);
	/* lose peerlist's ref */
	ksocknal_peer_decref(peer_ni);
}

static int
ksocknal_get_peer_info(struct lnet_ni *ni, int index,
		       struct lnet_process_id *id, __u32 *myip, __u32 *peer_ip,
		       int *port, int *conn_count, int *share_count)
{
	struct ksock_peer_ni *peer_ni;
	struct ksock_route *route;
	struct list_head *rtmp;
	int i;
	int j;
	int rc = -ENOENT;

	read_lock(&ksocknal_data.ksnd_global_lock);

	hash_for_each(ksocknal_data.ksnd_peers, i, peer_ni, ksnp_list) {

		if (peer_ni->ksnp_ni != ni)
			continue;

		if (peer_ni->ksnp_n_passive_ips == 0 &&
		    list_empty(&peer_ni->ksnp_routes)) {
			if (index-- > 0)
				continue;

			*id = peer_ni->ksnp_id;
			*myip = 0;
			*peer_ip = 0;
			*port = 0;
			*conn_count = 0;
			*share_count = 0;
			rc = 0;
			goto out;
		}

		for (j = 0; j < peer_ni->ksnp_n_passive_ips; j++) {
			if (index-- > 0)
				continue;

			*id = peer_ni->ksnp_id;
			*myip = peer_ni->ksnp_passive_ips[j];
			*peer_ip = 0;
			*port = 0;
			*conn_count = 0;
			*share_count = 0;
			rc = 0;
			goto out;
		}

		list_for_each(rtmp, &peer_ni->ksnp_routes) {
			if (index-- > 0)
				continue;

			route = list_entry(rtmp, struct ksock_route,
					   ksnr_list);

			*id = peer_ni->ksnp_id;
			if (route->ksnr_addr.ss_family == AF_INET) {
				struct sockaddr_in *sa =
					(void *)&route->ksnr_addr;
				rc = choose_ipv4_src(
					myip,
					route->ksnr_myiface,
					ntohl(sa->sin_addr.s_addr),
					ni->ni_net_ns);
				*peer_ip = ntohl(sa->sin_addr.s_addr);
				*port = ntohs(sa->sin_port);
			} else {
				*myip = 0xFFFFFFFF;
				*peer_ip = 0xFFFFFFFF;
				*port = 0;
				rc = -ENOTSUPP;
			}
			*conn_count = route->ksnr_conn_count;
			*share_count = route->ksnr_share_count;
			goto out;
		}
	}
out:
	read_unlock(&ksocknal_data.ksnd_global_lock);
	return rc;
}

static void
ksocknal_associate_route_conn_locked(struct ksock_route *route,
				     struct ksock_conn *conn)
{
	struct ksock_peer_ni *peer_ni = route->ksnr_peer;
	int type = conn->ksnc_type;
	struct ksock_interface *iface;
	int conn_iface =
		ksocknal_ip2index((struct sockaddr *)&conn->ksnc_myaddr,
				  route->ksnr_peer->ksnp_ni);

	conn->ksnc_route = route;
	ksocknal_route_addref(route);

	if (route->ksnr_myiface != conn_iface) {
		if (route->ksnr_myiface < 0) {
			/* route wasn't bound locally yet (the initial route) */
			CDEBUG(D_NET, "Binding %s %pIS to interface %d\n",
			       libcfs_id2str(peer_ni->ksnp_id),
			       &route->ksnr_addr,
			       conn_iface);
		} else {
			CDEBUG(D_NET,
			       "Rebinding %s %pIS from interface %d to %d\n",
			       libcfs_id2str(peer_ni->ksnp_id),
			       &route->ksnr_addr,
			       route->ksnr_myiface,
			       conn_iface);

			iface = ksocknal_index2iface(route->ksnr_peer->ksnp_ni,
						     route->ksnr_myiface);
			if (iface)
				iface->ksni_nroutes--;
		}
		route->ksnr_myiface = conn_iface;
		iface = ksocknal_index2iface(route->ksnr_peer->ksnp_ni,
					     route->ksnr_myiface);
		if (iface)
			iface->ksni_nroutes++;
	}

	route->ksnr_connected |= (1<<type);
	route->ksnr_conn_count++;

	/* Successful connection => further attempts can
	 * proceed immediately
	 */
	route->ksnr_retry_interval = 0;
}

static void
ksocknal_add_route_locked(struct ksock_peer_ni *peer_ni, struct ksock_route *route)
{
	struct list_head *tmp;
	struct ksock_conn *conn;
	struct ksock_route *route2;
	struct ksock_net *net = peer_ni->ksnp_ni->ni_data;

	LASSERT(!peer_ni->ksnp_closing);
	LASSERT(route->ksnr_peer == NULL);
	LASSERT(!route->ksnr_scheduled);
	LASSERT(!route->ksnr_connecting);
	LASSERT(route->ksnr_connected == 0);
	LASSERT(net->ksnn_ninterfaces > 0);

	/* LASSERT(unique) */
	list_for_each(tmp, &peer_ni->ksnp_routes) {
		route2 = list_entry(tmp, struct ksock_route, ksnr_list);

		if (rpc_cmp_addr((struct sockaddr *)&route2->ksnr_addr,
				 (struct sockaddr *)&route->ksnr_addr)) {
			CERROR("Duplicate route %s %pI4h\n",
			       libcfs_id2str(peer_ni->ksnp_id),
			       &route->ksnr_addr);
			LBUG();
		}
	}

	route->ksnr_peer = peer_ni;
	ksocknal_peer_addref(peer_ni);

	/* set the route's interface to the current net's interface */
	route->ksnr_myiface = net->ksnn_interfaces[0].ksni_index;
	net->ksnn_interfaces[0].ksni_nroutes++;

	/* peer_ni's routelist takes over my ref on 'route' */
	list_add_tail(&route->ksnr_list, &peer_ni->ksnp_routes);

	list_for_each(tmp, &peer_ni->ksnp_conns) {
		conn = list_entry(tmp, struct ksock_conn, ksnc_list);

		if (!rpc_cmp_addr((struct sockaddr *)&conn->ksnc_peeraddr,
				  (struct sockaddr *)&route->ksnr_addr))
			continue;

		ksocknal_associate_route_conn_locked(route, conn);
		/* keep going (typed routes) */
	}
}

static void
ksocknal_del_route_locked(struct ksock_route *route)
{
	struct ksock_peer_ni *peer_ni = route->ksnr_peer;
	struct ksock_interface *iface;
	struct ksock_conn *conn;
	struct ksock_conn *cnxt;

	LASSERT(!route->ksnr_deleted);

	/* Close associated conns */
	list_for_each_entry_safe(conn, cnxt, &peer_ni->ksnp_conns, ksnc_list) {
		if (conn->ksnc_route != route)
			continue;

		ksocknal_close_conn_locked(conn, 0);
	}

	if (route->ksnr_myiface >= 0) {
		iface = ksocknal_index2iface(route->ksnr_peer->ksnp_ni,
					     route->ksnr_myiface);
		if (iface)
			iface->ksni_nroutes--;
	}

	route->ksnr_deleted = 1;
	list_del(&route->ksnr_list);
	ksocknal_route_decref(route);		/* drop peer_ni's ref */

	if (list_empty(&peer_ni->ksnp_routes) &&
	    list_empty(&peer_ni->ksnp_conns)) {
		/* I've just removed the last route to a peer_ni with no active
		 * connections */
		ksocknal_unlink_peer_locked(peer_ni);
	}
}

int
ksocknal_add_peer(struct lnet_ni *ni, struct lnet_process_id id, __u32 ipaddr,
		  int port)
{
	struct list_head *tmp;
	struct ksock_peer_ni *peer_ni;
	struct ksock_peer_ni *peer2;
	struct ksock_route *route;
	struct ksock_route *route2;
	struct sockaddr_in sa = {.sin_family = AF_INET};

        if (id.nid == LNET_NID_ANY ||
            id.pid == LNET_PID_ANY)
                return (-EINVAL);

	/* Have a brand new peer_ni ready... */
	peer_ni = ksocknal_create_peer(ni, id);
	if (IS_ERR(peer_ni))
		return PTR_ERR(peer_ni);

	sa.sin_addr.s_addr = htonl(ipaddr);
	sa.sin_port = htons(port);
	route = ksocknal_create_route((struct sockaddr *)&sa);
        if (route == NULL) {
                ksocknal_peer_decref(peer_ni);
                return (-ENOMEM);
        }

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

        /* always called with a ref on ni, so shutdown can't have started */
	LASSERT(atomic_read(&((struct ksock_net *)ni->ni_data)->ksnn_npeers)
		>= 0);

	peer2 = ksocknal_find_peer_locked(ni, id);
	if (peer2 != NULL) {
		ksocknal_peer_decref(peer_ni);
		peer_ni = peer2;
	} else {
		/* peer_ni table takes my ref on peer_ni */
		hash_add(ksocknal_data.ksnd_peers, &peer_ni->ksnp_list, id.nid);
	}

	route2 = NULL;
	list_for_each(tmp, &peer_ni->ksnp_routes) {
		route2 = list_entry(tmp, struct ksock_route, ksnr_list);

		if (route2->ksnr_addr.ss_family == AF_INET &&
		    ((struct sockaddr_in *)&route2->ksnr_addr)->sin_addr.s_addr
		    == htonl(ipaddr))
			break;

		route2 = NULL;
	}
	if (route2 == NULL) {
		ksocknal_add_route_locked(peer_ni, route);
		route->ksnr_share_count++;
	} else {
		ksocknal_route_decref(route);
		route2->ksnr_share_count++;
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	return 0;
}

static void
ksocknal_del_peer_locked(struct ksock_peer_ni *peer_ni, __u32 ip)
{
	struct ksock_conn *conn;
	struct ksock_conn *cnxt;
	struct ksock_route *route;
	struct ksock_route *rnxt;
	int nshared;

	LASSERT(!peer_ni->ksnp_closing);

	/* Extra ref prevents peer_ni disappearing until I'm done with it */
	ksocknal_peer_addref(peer_ni);

	list_for_each_entry_safe(route, rnxt, &peer_ni->ksnp_routes,
				 ksnr_list) {
		/* no match */
		if (ip) {
			if (route->ksnr_addr.ss_family != AF_INET)
				continue;
			if (((struct sockaddr_in *)&route->ksnr_addr)
					->sin_addr.s_addr != htonl(ip))
				continue;
		}

		route->ksnr_share_count = 0;
		/* This deletes associated conns too */
		ksocknal_del_route_locked(route);
	}

	nshared = 0;
	list_for_each_entry(route, &peer_ni->ksnp_routes, ksnr_list)
		nshared += route->ksnr_share_count;

	if (nshared == 0) {
		/* remove everything else if there are no explicit entries
		 * left
		 */
		list_for_each_entry_safe(route, rnxt, &peer_ni->ksnp_routes,
					 ksnr_list) {
			/* we should only be removing auto-entries */
			LASSERT(route->ksnr_share_count == 0);
			ksocknal_del_route_locked(route);
		}

		list_for_each_entry_safe(conn, cnxt, &peer_ni->ksnp_conns,
					 ksnc_list)
			ksocknal_close_conn_locked(conn, 0);
	}

	ksocknal_peer_decref(peer_ni);
	/* NB peer_ni unlinks itself when last conn/route is removed */
}

static int
ksocknal_del_peer(struct lnet_ni *ni, struct lnet_process_id id, __u32 ip)
{
	LIST_HEAD(zombies);
	struct hlist_node *pnxt;
	struct ksock_peer_ni *peer_ni;
	int lo;
	int hi;
	int i;
	int rc = -ENOENT;

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	if (id.nid != LNET_NID_ANY) {
		lo = hash_min(id.nid, HASH_BITS(ksocknal_data.ksnd_peers));
		hi = lo;
	} else {
		lo = 0;
		hi = HASH_SIZE(ksocknal_data.ksnd_peers) - 1;
	}

	for (i = lo; i <= hi; i++) {
		hlist_for_each_entry_safe(peer_ni, pnxt,
					  &ksocknal_data.ksnd_peers[i],
					  ksnp_list) {
			if (peer_ni->ksnp_ni != ni)
				continue;

			if (!((id.nid == LNET_NID_ANY ||
			       peer_ni->ksnp_id.nid == id.nid) &&
			      (id.pid == LNET_PID_ANY ||
			       peer_ni->ksnp_id.pid == id.pid)))
				continue;

			ksocknal_peer_addref(peer_ni);	/* a ref for me... */

			ksocknal_del_peer_locked(peer_ni, ip);

			if (peer_ni->ksnp_closing &&
			    !list_empty(&peer_ni->ksnp_tx_queue)) {
				LASSERT(list_empty(&peer_ni->ksnp_conns));
				LASSERT(list_empty(&peer_ni->ksnp_routes));

				list_splice_init(&peer_ni->ksnp_tx_queue,
						 &zombies);
			}

			ksocknal_peer_decref(peer_ni);	/* ...till here */

			rc = 0;				/* matched! */
		}
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	ksocknal_txlist_done(ni, &zombies, -ENETDOWN);

	return rc;
}

static struct ksock_conn *
ksocknal_get_conn_by_idx(struct lnet_ni *ni, int index)
{
	struct ksock_peer_ni *peer_ni;
	struct ksock_conn *conn;
	struct list_head *ctmp;
	int i;

	read_lock(&ksocknal_data.ksnd_global_lock);

	hash_for_each(ksocknal_data.ksnd_peers, i, peer_ni, ksnp_list) {
		LASSERT(!peer_ni->ksnp_closing);

		if (peer_ni->ksnp_ni != ni)
			continue;

		list_for_each(ctmp, &peer_ni->ksnp_conns) {
			if (index-- > 0)
				continue;

			conn = list_entry(ctmp, struct ksock_conn,
					  ksnc_list);
			ksocknal_conn_addref(conn);
			read_unlock(&ksocknal_data.ksnd_global_lock);
			return conn;
		}
	}

	read_unlock(&ksocknal_data.ksnd_global_lock);
	return NULL;
}

static struct ksock_sched *
ksocknal_choose_scheduler_locked(unsigned int cpt)
{
	struct ksock_sched *sched = ksocknal_data.ksnd_schedulers[cpt];
	int i;

	if (sched->kss_nthreads == 0) {
		cfs_percpt_for_each(sched, i, ksocknal_data.ksnd_schedulers) {
			if (sched->kss_nthreads > 0) {
				CDEBUG(D_NET, "scheduler[%d] has no threads. selected scheduler[%d]\n",
				       cpt, sched->kss_cpt);
				return sched;
			}
		}
		return NULL;
	}

	return sched;
}

static int
ksocknal_local_ipvec(struct lnet_ni *ni, __u32 *ipaddrs)
{
	struct ksock_net *net = ni->ni_data;
	int i, j;
	int nip;

	read_lock(&ksocknal_data.ksnd_global_lock);

	nip = net->ksnn_ninterfaces;
	LASSERT(nip <= LNET_INTERFACES_NUM);

	for (i = 0, j = 0; i < nip; i++)
		if (net->ksnn_interfaces[i].ksni_addr.ss_family == AF_INET) {
			struct sockaddr_in *sa =
				(void *)&net->ksnn_interfaces[i].ksni_addr;

			ipaddrs[j] = ntohl(sa->sin_addr.s_addr);
			LASSERT(ipaddrs[j] != 0);
			j += 1;
		}
	nip = j;

	read_unlock(&ksocknal_data.ksnd_global_lock);
	/*
	 * Only offer interfaces for additional connections if I have
	 * more than one.
	 */
	return nip < 2 ? 0 : nip;
}

static int
ksocknal_match_peerip(struct ksock_interface *iface, __u32 *ips, int nips)
{
	int best_netmatch = 0;
	int best_xor = 0;
	int best = -1;
	int this_xor;
	int this_netmatch;
	int i;
	struct sockaddr_in *sa;
	__u32 ip;

	sa = (struct sockaddr_in *)&iface->ksni_addr;
	LASSERT(sa->sin_family == AF_INET);
	ip = ntohl(sa->sin_addr.s_addr);

	for (i = 0; i < nips; i++) {
		if (ips[i] == 0)
			continue;

		this_xor = ips[i] ^ ip;
		this_netmatch = ((this_xor & iface->ksni_netmask) == 0) ? 1 : 0;

		if (!(best < 0 ||
		      best_netmatch < this_netmatch ||
		      (best_netmatch == this_netmatch &&
		       best_xor > this_xor)))
			continue;

		best = i;
		best_netmatch = this_netmatch;
		best_xor = this_xor;
	}

	LASSERT(best >= 0);
	return best;
}

static int
ksocknal_select_ips(struct ksock_peer_ni *peer_ni, __u32 *peerips, int n_peerips)
{
	rwlock_t *global_lock = &ksocknal_data.ksnd_global_lock;
	struct ksock_net *net = peer_ni->ksnp_ni->ni_data;
	struct ksock_interface *iface;
	struct ksock_interface *best_iface;
	int n_ips;
	int i;
	int j;
	int k;
	u32 ip;
	u32 xor;
	int this_netmatch;
	int best_netmatch;
	int best_npeers;

        /* CAVEAT EMPTOR: We do all our interface matching with an
         * exclusive hold of global lock at IRQ priority.  We're only
         * expecting to be dealing with small numbers of interfaces, so the
         * O(n**3)-ness shouldn't matter */

        /* Also note that I'm not going to return more than n_peerips
         * interfaces, even if I have more myself */

	write_lock_bh(global_lock);

	LASSERT(n_peerips <= LNET_INTERFACES_NUM);
	LASSERT(net->ksnn_ninterfaces <= LNET_INTERFACES_NUM);

	/* Only match interfaces for additional connections
	 * if I have > 1 interface
	 */
	n_ips = (net->ksnn_ninterfaces < 2) ? 0 :
		min(n_peerips, net->ksnn_ninterfaces);

        for (i = 0; peer_ni->ksnp_n_passive_ips < n_ips; i++) {
                /*              ^ yes really... */

                /* If we have any new interfaces, first tick off all the
                 * peer_ni IPs that match old interfaces, then choose new
                 * interfaces to match the remaining peer_ni IPS.
                 * We don't forget interfaces we've stopped using; we might
                 * start using them again... */

		if (i < peer_ni->ksnp_n_passive_ips) {
			/* Old interface. */
			struct sockaddr_in sa = { .sin_family = AF_INET};

			sa.sin_addr.s_addr =
				htonl(peer_ni->ksnp_passive_ips[i]);
			best_iface = ksocknal_ip2iface(peer_ni->ksnp_ni,
						       (struct sockaddr *)&sa);

			/* peer_ni passive ips are kept up to date */
			LASSERT(best_iface != NULL);
                } else {
                        /* choose a new interface */
			struct sockaddr_in *sa;

                        LASSERT (i == peer_ni->ksnp_n_passive_ips);

                        best_iface = NULL;
                        best_netmatch = 0;
                        best_npeers = 0;

			for (j = 0; j < net->ksnn_ninterfaces; j++) {
				iface = &net->ksnn_interfaces[j];
				sa = (void *)&iface->ksni_addr;
				if (sa->sin_family != AF_INET)
					continue;
				ip = ntohl(sa->sin_addr.s_addr);

                                for (k = 0; k < peer_ni->ksnp_n_passive_ips; k++)
                                        if (peer_ni->ksnp_passive_ips[k] == ip)
                                                break;

                                if (k < peer_ni->ksnp_n_passive_ips) /* using it already */
                                        continue;

                                k = ksocknal_match_peerip(iface, peerips, n_peerips);
                                xor = (ip ^ peerips[k]);
                                this_netmatch = ((xor & iface->ksni_netmask) == 0) ? 1 : 0;

                                if (!(best_iface == NULL ||
                                      best_netmatch < this_netmatch ||
                                      (best_netmatch == this_netmatch &&
                                       best_npeers > iface->ksni_npeers)))
                                        continue;

                                best_iface = iface;
                                best_netmatch = this_netmatch;
                                best_npeers = iface->ksni_npeers;
                        }

			LASSERT(best_iface != NULL);

			best_iface->ksni_npeers++;
			sa = (void *)&best_iface->ksni_addr;
			ip = ntohl(sa->sin_addr.s_addr);
			peer_ni->ksnp_passive_ips[i] = ip;
			peer_ni->ksnp_n_passive_ips = i+1;
                }

                /* mark the best matching peer_ni IP used */
                j = ksocknal_match_peerip(best_iface, peerips, n_peerips);
                peerips[j] = 0;
        }

        /* Overwrite input peer_ni IP addresses */
        memcpy(peerips, peer_ni->ksnp_passive_ips, n_ips * sizeof(*peerips));

	write_unlock_bh(global_lock);

        return (n_ips);
}

static void
ksocknal_create_routes(struct ksock_peer_ni *peer_ni, int port,
                       __u32 *peer_ipaddrs, int npeer_ipaddrs)
{
	struct ksock_route		*newroute = NULL;
	rwlock_t		*global_lock = &ksocknal_data.ksnd_global_lock;
	struct lnet_ni *ni = peer_ni->ksnp_ni;
	struct ksock_net		*net = ni->ni_data;
	struct list_head	*rtmp;
	struct ksock_route		*route;
	struct ksock_interface	*iface;
	struct ksock_interface	*best_iface;
	int			best_netmatch;
	int			this_netmatch;
	int			best_nroutes;
	int			i;
	int			j;

        /* CAVEAT EMPTOR: We do all our interface matching with an
         * exclusive hold of global lock at IRQ priority.  We're only
         * expecting to be dealing with small numbers of interfaces, so the
         * O(n**3)-ness here shouldn't matter */

	write_lock_bh(global_lock);

        if (net->ksnn_ninterfaces < 2) {
		/* Only create additional connections
                 * if I have > 1 interface */
		write_unlock_bh(global_lock);
                return;
        }

	LASSERT(npeer_ipaddrs <= LNET_INTERFACES_NUM);

	for (i = 0; i < npeer_ipaddrs; i++) {
		if (newroute) {
			struct sockaddr_in *sa = (void *)&newroute->ksnr_addr;

			memset(sa, 0, sizeof(*sa));
			sa->sin_family = AF_INET;
			sa->sin_addr.s_addr = htonl(peer_ipaddrs[i]);
		} else {
			struct sockaddr_in sa = {.sin_family = AF_INET};

			write_unlock_bh(global_lock);

			sa.sin_addr.s_addr = htonl(peer_ipaddrs[i]);
			sa.sin_port = htons(port);
			newroute =
				ksocknal_create_route((struct sockaddr *)&sa);
			if (!newroute)
				return;

			write_lock_bh(global_lock);
		}

		if (peer_ni->ksnp_closing) {
			/* peer_ni got closed under me */
			break;
		}

		/* Already got a route? */
		route = NULL;
		list_for_each(rtmp, &peer_ni->ksnp_routes) {
			route = list_entry(rtmp, struct ksock_route, ksnr_list);

			if (rpc_cmp_addr(
				    (struct sockaddr *)&route->ksnr_addr,
				    (struct sockaddr *)&newroute->ksnr_addr))
				break;

			route = NULL;
		}
		if (route != NULL)
			continue;

		best_iface = NULL;
		best_nroutes = 0;
		best_netmatch = 0;

		LASSERT(net->ksnn_ninterfaces <= LNET_INTERFACES_NUM);

		/* Select interface to connect from */
		for (j = 0; j < net->ksnn_ninterfaces; j++) {
			__u32 iface_ip, route_ip;

			iface = &net->ksnn_interfaces[j];

			/* Using this interface already? */
			list_for_each(rtmp, &peer_ni->ksnp_routes) {
				route = list_entry(rtmp, struct ksock_route,
						   ksnr_list);

				if (route->ksnr_myiface == iface->ksni_index)
					break;

				route = NULL;
			}
			if (route != NULL)
				continue;
			if (iface->ksni_addr.ss_family != AF_INET)
				continue;
			if (newroute->ksnr_addr.ss_family != AF_INET)
				continue;

			iface_ip =
				ntohl(((struct sockaddr_in *)
				       &iface->ksni_addr)->sin_addr.s_addr);
			route_ip =
				ntohl(((struct sockaddr_in *)
				       &newroute->ksnr_addr)->sin_addr.s_addr);

			this_netmatch = (((iface_ip ^ route_ip) &
					  iface->ksni_netmask) == 0) ? 1 : 0;

			if (!(best_iface == NULL ||
			      best_netmatch < this_netmatch ||
			      (best_netmatch == this_netmatch &&
			       best_nroutes > iface->ksni_nroutes)))
				continue;

			best_iface = iface;
			best_netmatch = this_netmatch;
			best_nroutes = iface->ksni_nroutes;
		}

		if (best_iface == NULL)
			continue;

		newroute->ksnr_myiface = best_iface->ksni_index;
		best_iface->ksni_nroutes++;

		ksocknal_add_route_locked(peer_ni, newroute);
		newroute = NULL;
	}

	write_unlock_bh(global_lock);
	if (newroute != NULL)
		ksocknal_route_decref(newroute);
}

int
ksocknal_accept(struct lnet_ni *ni, struct socket *sock)
{
	struct ksock_connreq *cr;
	int rc;
	struct sockaddr_storage peer;

	rc = lnet_sock_getaddr(sock, true, &peer);
	LASSERT(rc == 0);		/* we succeeded before */

	LIBCFS_ALLOC(cr, sizeof(*cr));
	if (cr == NULL) {
		LCONSOLE_ERROR_MSG(0x12f,
				   "Dropping connection request from %pIS: memory exhausted\n",
				   &peer);
		return -ENOMEM;
	}

	lnet_ni_addref(ni);
	cr->ksncr_ni   = ni;
	cr->ksncr_sock = sock;

	spin_lock_bh(&ksocknal_data.ksnd_connd_lock);

	list_add_tail(&cr->ksncr_list, &ksocknal_data.ksnd_connd_connreqs);
	wake_up(&ksocknal_data.ksnd_connd_waitq);

	spin_unlock_bh(&ksocknal_data.ksnd_connd_lock);
	return 0;
}

static int
ksocknal_connecting(struct ksock_peer_ni *peer_ni, struct sockaddr *sa)
{
	struct ksock_route *route;

	list_for_each_entry(route, &peer_ni->ksnp_routes, ksnr_list) {
		if (rpc_cmp_addr((struct sockaddr *)&route->ksnr_addr, sa))
			return route->ksnr_connecting;
	}
	return 0;
}

int
ksocknal_create_conn(struct lnet_ni *ni, struct ksock_route *route,
		     struct socket *sock, int type)
{
	rwlock_t *global_lock = &ksocknal_data.ksnd_global_lock;
	LIST_HEAD(zombies);
	struct lnet_process_id peerid;
	struct list_head *tmp;
	u64 incarnation;
	struct ksock_conn *conn;
	struct ksock_conn *conn2;
	struct ksock_peer_ni *peer_ni = NULL;
	struct ksock_peer_ni *peer2;
	struct ksock_sched *sched;
	struct ksock_hello_msg *hello;
	int cpt;
	struct ksock_tx *tx;
	struct ksock_tx *txtmp;
	int rc;
	int rc2;
	int active;
	char *warn = NULL;

        active = (route != NULL);

        LASSERT (active == (type != SOCKLND_CONN_NONE));

        LIBCFS_ALLOC(conn, sizeof(*conn));
        if (conn == NULL) {
                rc = -ENOMEM;
                goto failed_0;
        }

        conn->ksnc_peer = NULL;
        conn->ksnc_route = NULL;
        conn->ksnc_sock = sock;
	/* 2 ref, 1 for conn, another extra ref prevents socket
	 * being closed before establishment of connection */
	refcount_set(&conn->ksnc_sock_refcount, 2);
	conn->ksnc_type = type;
	ksocknal_lib_save_callback(sock, conn);
	refcount_set(&conn->ksnc_conn_refcount, 1); /* 1 ref for me */

	conn->ksnc_rx_ready = 0;
	conn->ksnc_rx_scheduled = 0;

	INIT_LIST_HEAD(&conn->ksnc_tx_queue);
	conn->ksnc_tx_ready = 0;
	conn->ksnc_tx_scheduled = 0;
	conn->ksnc_tx_carrier = NULL;
	atomic_set (&conn->ksnc_tx_nob, 0);

	LIBCFS_ALLOC(hello, offsetof(struct ksock_hello_msg,
				     kshm_ips[LNET_INTERFACES_NUM]));
        if (hello == NULL) {
                rc = -ENOMEM;
                goto failed_1;
        }

        /* stash conn's local and remote addrs */
        rc = ksocknal_lib_get_conn_addrs (conn);
        if (rc != 0)
                goto failed_1;

        /* Find out/confirm peer_ni's NID and connection type and get the
         * vector of interfaces she's willing to let me connect to.
         * Passive connections use the listener timeout since the peer_ni sends
         * eagerly */

        if (active) {
                peer_ni = route->ksnr_peer;
                LASSERT(ni == peer_ni->ksnp_ni);

                /* Active connection sends HELLO eagerly */
                hello->kshm_nips = ksocknal_local_ipvec(ni, hello->kshm_ips);
                peerid = peer_ni->ksnp_id;

		write_lock_bh(global_lock);
                conn->ksnc_proto = peer_ni->ksnp_proto;
		write_unlock_bh(global_lock);

                if (conn->ksnc_proto == NULL) {
                         conn->ksnc_proto = &ksocknal_protocol_v3x;
#if SOCKNAL_VERSION_DEBUG
                         if (*ksocknal_tunables.ksnd_protocol == 2)
                                 conn->ksnc_proto = &ksocknal_protocol_v2x;
                         else if (*ksocknal_tunables.ksnd_protocol == 1)
                                 conn->ksnc_proto = &ksocknal_protocol_v1x;
#endif
                }

                rc = ksocknal_send_hello (ni, conn, peerid.nid, hello);
                if (rc != 0)
                        goto failed_1;
        } else {
                peerid.nid = LNET_NID_ANY;
                peerid.pid = LNET_PID_ANY;

                /* Passive, get protocol from peer_ni */
                conn->ksnc_proto = NULL;
        }

        rc = ksocknal_recv_hello (ni, conn, hello, &peerid, &incarnation);
        if (rc < 0)
                goto failed_1;

        LASSERT (rc == 0 || active);
        LASSERT (conn->ksnc_proto != NULL);
        LASSERT (peerid.nid != LNET_NID_ANY);

	cpt = lnet_cpt_of_nid(peerid.nid, ni);

	if (active) {
		ksocknal_peer_addref(peer_ni);
		write_lock_bh(global_lock);
	} else {
		peer_ni = ksocknal_create_peer(ni, peerid);
		if (IS_ERR(peer_ni)) {
			rc = PTR_ERR(peer_ni);
			goto failed_1;
		}

		write_lock_bh(global_lock);

		/* called with a ref on ni, so shutdown can't have started */
		LASSERT(atomic_read(&((struct ksock_net *)ni->ni_data)->ksnn_npeers) >= 0);

		peer2 = ksocknal_find_peer_locked(ni, peerid);
		if (peer2 == NULL) {
			/* NB this puts an "empty" peer_ni in the peer_ni
			 * table (which takes my ref) */
			hash_add(ksocknal_data.ksnd_peers,
				 &peer_ni->ksnp_list, peerid.nid);
		} else {
			ksocknal_peer_decref(peer_ni);
			peer_ni = peer2;
		}

                /* +1 ref for me */
                ksocknal_peer_addref(peer_ni);
                peer_ni->ksnp_accepting++;

		/* Am I already connecting to this guy?  Resolve in
		 * favour of higher NID...
		 */
		if (peerid.nid < ni->ni_nid &&
		    ksocknal_connecting(peer_ni, ((struct sockaddr *)
						  &conn->ksnc_peeraddr))) {
			rc = EALREADY;
			warn = "connection race resolution";
			goto failed_2;
		}
        }

        if (peer_ni->ksnp_closing ||
            (active && route->ksnr_deleted)) {
                /* peer_ni/route got closed under me */
                rc = -ESTALE;
                warn = "peer_ni/route removed";
                goto failed_2;
        }

	if (peer_ni->ksnp_proto == NULL) {
		/* Never connected before.
		 * NB recv_hello may have returned EPROTO to signal my peer_ni
		 * wants a different protocol than the one I asked for.
		 */
		LASSERT(list_empty(&peer_ni->ksnp_conns));

		peer_ni->ksnp_proto = conn->ksnc_proto;
		peer_ni->ksnp_incarnation = incarnation;
	}

	if (peer_ni->ksnp_proto != conn->ksnc_proto ||
	    peer_ni->ksnp_incarnation != incarnation) {
		/* peer_ni rebooted or I've got the wrong protocol version */
		ksocknal_close_peer_conns_locked(peer_ni, NULL, 0);

		peer_ni->ksnp_proto = NULL;
		rc = ESTALE;
		warn = peer_ni->ksnp_incarnation != incarnation ?
			"peer_ni rebooted" :
			"wrong proto version";
		goto failed_2;
	}

        switch (rc) {
        default:
                LBUG();
        case 0:
                break;
        case EALREADY:
                warn = "lost conn race";
                goto failed_2;
        case EPROTO:
                warn = "retry with different protocol version";
                goto failed_2;
        }

	/* Refuse to duplicate an existing connection, unless this is a
	 * loopback connection */
	if (!rpc_cmp_addr((struct sockaddr *)&conn->ksnc_peeraddr,
			  (struct sockaddr *)&conn->ksnc_myaddr)) {
		list_for_each(tmp, &peer_ni->ksnp_conns) {
			conn2 = list_entry(tmp, struct ksock_conn, ksnc_list);

			if (!rpc_cmp_addr(
				    (struct sockaddr *)&conn2->ksnc_peeraddr,
				    (struct sockaddr *)&conn->ksnc_peeraddr) ||
			    !rpc_cmp_addr(
				    (struct sockaddr *)&conn2->ksnc_myaddr,
				    (struct sockaddr *)&conn->ksnc_myaddr) ||
			    conn2->ksnc_type != conn->ksnc_type)
				continue;

                        /* Reply on a passive connection attempt so the peer_ni
                         * realises we're connected. */
                        LASSERT (rc == 0);
                        if (!active)
                                rc = EALREADY;

                        warn = "duplicate";
                        goto failed_2;
                }
        }

        /* If the connection created by this route didn't bind to the IP
         * address the route connected to, the connection/route matching
         * code below probably isn't going to work. */
        if (active &&
	    !rpc_cmp_addr((struct sockaddr *)&route->ksnr_addr,
			  (struct sockaddr *)&conn->ksnc_peeraddr)) {
		CERROR("Route %s %pIS connected to %pIS\n",
		       libcfs_id2str(peer_ni->ksnp_id),
		       &route->ksnr_addr,
		       &conn->ksnc_peeraddr);
        }

	/* Search for a route corresponding to the new connection and
	 * create an association.  This allows incoming connections created
	 * by routes in my peer_ni to match my own route entries so I don't
	 * continually create duplicate routes. */
	list_for_each(tmp, &peer_ni->ksnp_routes) {
		route = list_entry(tmp, struct ksock_route, ksnr_list);

		if (!rpc_cmp_addr((struct sockaddr *)&route->ksnr_addr,
				  (struct sockaddr *)&conn->ksnc_peeraddr))
			continue;

		ksocknal_associate_route_conn_locked(route, conn);
		break;
	}

	conn->ksnc_peer = peer_ni;                 /* conn takes my ref on peer_ni */
	peer_ni->ksnp_last_alive = ktime_get_seconds();
	peer_ni->ksnp_send_keepalive = 0;
	peer_ni->ksnp_error = 0;

	sched = ksocknal_choose_scheduler_locked(cpt);
	if (!sched) {
		CERROR("no schedulers available. node is unhealthy\n");
		goto failed_2;
	}
	/*
	 * The cpt might have changed if we ended up selecting a non cpt
	 * native scheduler. So use the scheduler's cpt instead.
	 */
	cpt = sched->kss_cpt;
        sched->kss_nconns++;
        conn->ksnc_scheduler = sched;

	conn->ksnc_tx_last_post = ktime_get_seconds();
	/* Set the deadline for the outgoing HELLO to drain */
	conn->ksnc_tx_bufnob = sock->sk->sk_wmem_queued;
	conn->ksnc_tx_deadline = ktime_get_seconds() +
				 ksocknal_timeout();
	smp_mb();   /* order with adding to peer_ni's conn list */

	list_add(&conn->ksnc_list, &peer_ni->ksnp_conns);
	ksocknal_conn_addref(conn);

	ksocknal_new_packet(conn, 0);

        conn->ksnc_zc_capable = ksocknal_lib_zc_capable(conn);

	/* Take packets blocking for this connection. */
	list_for_each_entry_safe(tx, txtmp, &peer_ni->ksnp_tx_queue, tx_list) {
		if (conn->ksnc_proto->pro_match_tx(conn, tx, tx->tx_nonblk) ==
		    SOCKNAL_MATCH_NO)
			continue;

		list_del(&tx->tx_list);
		ksocknal_queue_tx_locked(tx, conn);
	}

	write_unlock_bh(global_lock);

        /* We've now got a new connection.  Any errors from here on are just
         * like "normal" comms errors and we close the connection normally.
         * NB (a) we still have to send the reply HELLO for passive
	 *        connections,
         *    (b) normal I/O on the conn is blocked until I setup and call the
         *        socket callbacks.
         */

	CDEBUG(D_NET, "New conn %s p %d.x %pIS -> %pISp"
	       " incarnation:%lld sched[%d]\n",
	       libcfs_id2str(peerid), conn->ksnc_proto->pro_version,
	       &conn->ksnc_myaddr, &conn->ksnc_peeraddr,
	       incarnation, cpt);

	if (active) {
		/* additional routes after interface exchange? */
		ksocknal_create_routes(
			peer_ni,
			rpc_get_port((struct sockaddr *)&conn->ksnc_peeraddr),
			hello->kshm_ips, hello->kshm_nips);
        } else {
                hello->kshm_nips = ksocknal_select_ips(peer_ni, hello->kshm_ips,
                                                       hello->kshm_nips);
                rc = ksocknal_send_hello(ni, conn, peerid.nid, hello);
        }

	LIBCFS_FREE(hello, offsetof(struct ksock_hello_msg,
				    kshm_ips[LNET_INTERFACES_NUM]));

        /* setup the socket AFTER I've received hello (it disables
         * SO_LINGER).  I might call back to the acceptor who may want
         * to send a protocol version response and then close the
         * socket; this ensures the socket only tears down after the
         * response has been sent. */
        if (rc == 0)
                rc = ksocknal_lib_setup_sock(sock);

	write_lock_bh(global_lock);

        /* NB my callbacks block while I hold ksnd_global_lock */
        ksocknal_lib_set_callback(sock, conn);

        if (!active)
                peer_ni->ksnp_accepting--;

	write_unlock_bh(global_lock);

        if (rc != 0) {
		write_lock_bh(global_lock);
                if (!conn->ksnc_closing) {
                        /* could be closed by another thread */
                        ksocknal_close_conn_locked(conn, rc);
                }
		write_unlock_bh(global_lock);
        } else if (ksocknal_connsock_addref(conn) == 0) {
                /* Allow I/O to proceed. */
                ksocknal_read_callback(conn);
                ksocknal_write_callback(conn);
                ksocknal_connsock_decref(conn);
        }

        ksocknal_connsock_decref(conn);
        ksocknal_conn_decref(conn);
        return rc;

failed_2:
	if (!peer_ni->ksnp_closing &&
	    list_empty(&peer_ni->ksnp_conns) &&
	    list_empty(&peer_ni->ksnp_routes)) {
		list_splice_init(&peer_ni->ksnp_tx_queue, &zombies);
		ksocknal_unlink_peer_locked(peer_ni);
	}

	write_unlock_bh(global_lock);

        if (warn != NULL) {
                if (rc < 0)
                        CERROR("Not creating conn %s type %d: %s\n",
                               libcfs_id2str(peerid), conn->ksnc_type, warn);
                else
                        CDEBUG(D_NET, "Not creating conn %s type %d: %s\n",
                              libcfs_id2str(peerid), conn->ksnc_type, warn);
        }

        if (!active) {
                if (rc > 0) {
			/* Request retry by replying with CONN_NONE
                         * ksnc_proto has been set already */
                        conn->ksnc_type = SOCKLND_CONN_NONE;
                        hello->kshm_nips = 0;
                        ksocknal_send_hello(ni, conn, peerid.nid, hello);
                }

		write_lock_bh(global_lock);
                peer_ni->ksnp_accepting--;
		write_unlock_bh(global_lock);
        }

	/*
	 * If we get here without an error code, just use -EALREADY.
	 * Depending on how we got here, the error may be positive
	 * or negative. Normalize the value for ksocknal_txlist_done().
	 */
	rc2 = (rc == 0 ? -EALREADY : (rc > 0 ? -rc : rc));
	ksocknal_txlist_done(ni, &zombies, rc2);
        ksocknal_peer_decref(peer_ni);

failed_1:
	if (hello != NULL)
		LIBCFS_FREE(hello, offsetof(struct ksock_hello_msg,
					    kshm_ips[LNET_INTERFACES_NUM]));

	LIBCFS_FREE(conn, sizeof(*conn));

failed_0:
	sock_release(sock);
	return rc;
}

void
ksocknal_close_conn_locked(struct ksock_conn *conn, int error)
{
        /* This just does the immmediate housekeeping, and queues the
         * connection for the reaper to terminate.
         * Caller holds ksnd_global_lock exclusively in irq context */
	struct ksock_peer_ni *peer_ni = conn->ksnc_peer;
	struct ksock_route *route;
	struct ksock_conn *conn2;
	struct list_head *tmp;

	LASSERT(peer_ni->ksnp_error == 0);
	LASSERT(!conn->ksnc_closing);
	conn->ksnc_closing = 1;

	/* ksnd_deathrow_conns takes over peer_ni's ref */
	list_del(&conn->ksnc_list);

	route = conn->ksnc_route;
	if (route != NULL) {
		/* dissociate conn from route... */
		LASSERT(!route->ksnr_deleted);
		LASSERT((route->ksnr_connected & BIT(conn->ksnc_type)) != 0);

		conn2 = NULL;
		list_for_each(tmp, &peer_ni->ksnp_conns) {
			conn2 = list_entry(tmp, struct ksock_conn, ksnc_list);

			if (conn2->ksnc_route == route &&
			    conn2->ksnc_type == conn->ksnc_type)
				break;

			conn2 = NULL;
		}
		if (conn2 == NULL)
			route->ksnr_connected &= ~BIT(conn->ksnc_type);

		conn->ksnc_route = NULL;

		ksocknal_route_decref(route);	/* drop conn's ref on route */
	}

	if (list_empty(&peer_ni->ksnp_conns)) {
		/* No more connections to this peer_ni */

		if (!list_empty(&peer_ni->ksnp_tx_queue)) {
			struct ksock_tx *tx;

			LASSERT(conn->ksnc_proto == &ksocknal_protocol_v3x);

			/* throw them to the last connection...,
			 * these TXs will be send to /dev/null by scheduler */
			list_for_each_entry(tx, &peer_ni->ksnp_tx_queue,
					    tx_list)
				ksocknal_tx_prep(conn, tx);

			spin_lock_bh(&conn->ksnc_scheduler->kss_lock);
			list_splice_init(&peer_ni->ksnp_tx_queue,
					 &conn->ksnc_tx_queue);
			spin_unlock_bh(&conn->ksnc_scheduler->kss_lock);
		}

		/* renegotiate protocol version */
		peer_ni->ksnp_proto = NULL;
		/* stash last conn close reason */
		peer_ni->ksnp_error = error;

		if (list_empty(&peer_ni->ksnp_routes)) {
			/* I've just closed last conn belonging to a
			 * peer_ni with no routes to it */
			ksocknal_unlink_peer_locked(peer_ni);
		}
	}

	spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);

	list_add_tail(&conn->ksnc_list, &ksocknal_data.ksnd_deathrow_conns);
	wake_up(&ksocknal_data.ksnd_reaper_waitq);

	spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);
}

void
ksocknal_peer_failed(struct ksock_peer_ni *peer_ni)
{
	bool notify = false;
	time64_t last_alive = 0;

	/* There has been a connection failure or comms error; but I'll only
	 * tell LNET I think the peer_ni is dead if it's to another kernel and
	 * there are no connections or connection attempts in existence. */

	read_lock(&ksocknal_data.ksnd_global_lock);

	if ((peer_ni->ksnp_id.pid & LNET_PID_USERFLAG) == 0 &&
	     list_empty(&peer_ni->ksnp_conns) &&
	     peer_ni->ksnp_accepting == 0 &&
	     ksocknal_find_connecting_route_locked(peer_ni) == NULL) {
		notify = true;
		last_alive = peer_ni->ksnp_last_alive;
	}

	read_unlock(&ksocknal_data.ksnd_global_lock);

	if (notify)
		lnet_notify(peer_ni->ksnp_ni, peer_ni->ksnp_id.nid,
			    false, false, last_alive);
}

void
ksocknal_finalize_zcreq(struct ksock_conn *conn)
{
	struct ksock_peer_ni *peer_ni = conn->ksnc_peer;
	struct ksock_tx *tx;
	struct ksock_tx *tmp;
	LIST_HEAD(zlist);

	/* NB safe to finalize TXs because closing of socket will
	 * abort all buffered data */
	LASSERT(conn->ksnc_sock == NULL);

	spin_lock(&peer_ni->ksnp_lock);

	list_for_each_entry_safe(tx, tmp, &peer_ni->ksnp_zc_req_list, tx_zc_list) {
		if (tx->tx_conn != conn)
			continue;

		LASSERT(tx->tx_msg.ksm_zc_cookies[0] != 0);

		tx->tx_msg.ksm_zc_cookies[0] = 0;
		tx->tx_zc_aborted = 1;	/* mark it as not-acked */
		list_move(&tx->tx_zc_list, &zlist);
	}

	spin_unlock(&peer_ni->ksnp_lock);

	while (!list_empty(&zlist)) {
		tx = list_entry(zlist.next, struct ksock_tx, tx_zc_list);

		list_del(&tx->tx_zc_list);
		ksocknal_tx_decref(tx);
	}
}

void
ksocknal_terminate_conn(struct ksock_conn *conn)
{
	/* This gets called by the reaper (guaranteed thread context) to
	 * disengage the socket from its callbacks and close it.
	 * ksnc_refcount will eventually hit zero, and then the reaper will
	 * destroy it.
	 */
	struct ksock_peer_ni *peer_ni = conn->ksnc_peer;
	struct ksock_sched *sched = conn->ksnc_scheduler;
	bool failed = false;

	LASSERT(conn->ksnc_closing);

	/* wake up the scheduler to "send" all remaining packets to /dev/null */
	spin_lock_bh(&sched->kss_lock);

	/* a closing conn is always ready to tx */
	conn->ksnc_tx_ready = 1;

	if (!conn->ksnc_tx_scheduled &&
	    !list_empty(&conn->ksnc_tx_queue)) {
		list_add_tail(&conn->ksnc_tx_list,
			      &sched->kss_tx_conns);
		conn->ksnc_tx_scheduled = 1;
		/* extra ref for scheduler */
		ksocknal_conn_addref(conn);

		wake_up (&sched->kss_waitq);
	}

	spin_unlock_bh(&sched->kss_lock);

	/* serialise with callbacks */
	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	ksocknal_lib_reset_callback(conn->ksnc_sock, conn);

	/* OK, so this conn may not be completely disengaged from its
	 * scheduler yet, but it _has_ committed to terminate...
	 */
	conn->ksnc_scheduler->kss_nconns--;

	if (peer_ni->ksnp_error != 0) {
		/* peer_ni's last conn closed in error */
		LASSERT(list_empty(&peer_ni->ksnp_conns));
		failed = true;
		peer_ni->ksnp_error = 0;     /* avoid multiple notifications */
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	if (failed)
		ksocknal_peer_failed(peer_ni);

	/* The socket is closed on the final put; either here, or in
	 * ksocknal_{send,recv}msg().  Since we set up the linger2 option
	 * when the connection was established, this will close the socket
	 * immediately, aborting anything buffered in it. Any hung
	 * zero-copy transmits will therefore complete in finite time.
	 */
	ksocknal_connsock_decref(conn);
}

void
ksocknal_queue_zombie_conn(struct ksock_conn *conn)
{
	/* Queue the conn for the reaper to destroy */
	LASSERT(refcount_read(&conn->ksnc_conn_refcount) == 0);
	spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);

	list_add_tail(&conn->ksnc_list, &ksocknal_data.ksnd_zombie_conns);
	wake_up(&ksocknal_data.ksnd_reaper_waitq);

	spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);
}

void
ksocknal_destroy_conn(struct ksock_conn *conn)
{
	time64_t last_rcv;

	/* Final coup-de-grace of the reaper */
	CDEBUG (D_NET, "connection %p\n", conn);

	LASSERT(refcount_read(&conn->ksnc_conn_refcount) == 0);
	LASSERT(refcount_read(&conn->ksnc_sock_refcount) == 0);
	LASSERT (conn->ksnc_sock == NULL);
	LASSERT (conn->ksnc_route == NULL);
	LASSERT (!conn->ksnc_tx_scheduled);
	LASSERT (!conn->ksnc_rx_scheduled);
	LASSERT(list_empty(&conn->ksnc_tx_queue));

        /* complete current receive if any */
        switch (conn->ksnc_rx_state) {
        case SOCKNAL_RX_LNET_PAYLOAD:
                last_rcv = conn->ksnc_rx_deadline -
			   ksocknal_timeout();
		CERROR("Completing partial receive from %s[%d], ip %pISp, with error, wanted: %d, left: %d, last alive is %lld secs ago\n",
                       libcfs_id2str(conn->ksnc_peer->ksnp_id), conn->ksnc_type,
		       &conn->ksnc_peeraddr,
                       conn->ksnc_rx_nob_wanted, conn->ksnc_rx_nob_left,
		       ktime_get_seconds() - last_rcv);
		if (conn->ksnc_lnet_msg)
			conn->ksnc_lnet_msg->msg_health_status =
				LNET_MSG_STATUS_REMOTE_ERROR;
		lnet_finalize(conn->ksnc_lnet_msg, -EIO);
		break;
	case SOCKNAL_RX_LNET_HEADER:
		if (conn->ksnc_rx_started)
			CERROR("Incomplete receive of lnet header from %s, ip %pISp, with error, protocol: %d.x.\n",
			       libcfs_id2str(conn->ksnc_peer->ksnp_id),
			       &conn->ksnc_peeraddr,
			       conn->ksnc_proto->pro_version);
		break;
        case SOCKNAL_RX_KSM_HEADER:
                if (conn->ksnc_rx_started)
			CERROR("Incomplete receive of ksock message from %s, ip %pISp, with error, protocol: %d.x.\n",
			       libcfs_id2str(conn->ksnc_peer->ksnp_id),
			       &conn->ksnc_peeraddr,
			       conn->ksnc_proto->pro_version);
                break;
        case SOCKNAL_RX_SLOP:
                if (conn->ksnc_rx_started)
			CERROR("Incomplete receive of slops from %s, ip %pISp, with error\n",
			       libcfs_id2str(conn->ksnc_peer->ksnp_id),
			       &conn->ksnc_peeraddr);
               break;
        default:
                LBUG ();
                break;
        }

        ksocknal_peer_decref(conn->ksnc_peer);

        LIBCFS_FREE (conn, sizeof (*conn));
}

int
ksocknal_close_peer_conns_locked(struct ksock_peer_ni *peer_ni,
				 struct sockaddr *addr, int why)
{
	struct ksock_conn *conn;
	struct ksock_conn *cnxt;
	int count = 0;

	list_for_each_entry_safe(conn, cnxt, &peer_ni->ksnp_conns, ksnc_list) {
		if (!addr ||
		    rpc_cmp_addr(addr,
				 (struct sockaddr *)&conn->ksnc_peeraddr)) {
			count++;
			ksocknal_close_conn_locked(conn, why);
		}
	}

	return count;
}

int
ksocknal_close_conn_and_siblings(struct ksock_conn *conn, int why)
{
	struct ksock_peer_ni *peer_ni = conn->ksnc_peer;
	int count;

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	count = ksocknal_close_peer_conns_locked(
		peer_ni, (struct sockaddr *)&conn->ksnc_peeraddr, why);

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	return count;
}

int
ksocknal_close_matching_conns(struct lnet_process_id id, __u32 ipaddr)
{
	struct ksock_peer_ni *peer_ni;
	struct hlist_node *pnxt;
	int lo;
	int hi;
	int i;
	int count = 0;
	struct sockaddr_in sa = {.sin_family = AF_INET};

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	if (id.nid != LNET_NID_ANY) {
		lo = hash_min(id.nid, HASH_BITS(ksocknal_data.ksnd_peers));
		hi = lo;
	} else {
		lo = 0;
		hi = HASH_SIZE(ksocknal_data.ksnd_peers) - 1;
	}

	sa.sin_addr.s_addr = htonl(ipaddr);
	for (i = lo; i <= hi; i++) {
		hlist_for_each_entry_safe(peer_ni, pnxt,
					  &ksocknal_data.ksnd_peers[i],
					  ksnp_list) {

			if (!((id.nid == LNET_NID_ANY ||
			       id.nid == peer_ni->ksnp_id.nid) &&
			      (id.pid == LNET_PID_ANY ||
			       id.pid == peer_ni->ksnp_id.pid)))
				continue;

			count += ksocknal_close_peer_conns_locked(
				peer_ni,
				ipaddr ? (struct sockaddr *)&sa : NULL, 0);
		}
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	/* wildcards always succeed */
	if (id.nid == LNET_NID_ANY || id.pid == LNET_PID_ANY || ipaddr == 0)
		return 0;

	return (count == 0 ? -ENOENT : 0);
}

void
ksocknal_notify_gw_down(lnet_nid_t gw_nid)
{
	/* The router is telling me she's been notified of a change in
	 * gateway state....
	 */
	struct lnet_process_id id = {
		.nid	= gw_nid,
		.pid	= LNET_PID_ANY,
	};

	CDEBUG(D_NET, "gw %s down\n", libcfs_nid2str(gw_nid));

	/* If the gateway crashed, close all open connections... */
	ksocknal_close_matching_conns(id, 0);
	return;

	/* We can only establish new connections
	 * if we have autroutes, and these connect on demand. */
}

static void
ksocknal_push_peer(struct ksock_peer_ni *peer_ni)
{
	int index;
	int i;
	struct list_head *tmp;
	struct ksock_conn *conn;

        for (index = 0; ; index++) {
		read_lock(&ksocknal_data.ksnd_global_lock);

                i = 0;
                conn = NULL;

		list_for_each(tmp, &peer_ni->ksnp_conns) {
                        if (i++ == index) {
				conn = list_entry(tmp, struct ksock_conn,
						  ksnc_list);
                                ksocknal_conn_addref(conn);
                                break;
                        }
                }

		read_unlock(&ksocknal_data.ksnd_global_lock);

                if (conn == NULL)
                        break;

                ksocknal_lib_push_conn (conn);
                ksocknal_conn_decref(conn);
        }
}

static int
ksocknal_push(struct lnet_ni *ni, struct lnet_process_id id)
{
	int lo;
	int hi;
	int bkt;
	int rc = -ENOENT;

	if (id.nid != LNET_NID_ANY) {
		lo = hash_min(id.nid, HASH_BITS(ksocknal_data.ksnd_peers));
		hi = lo;
	} else {
		lo = 0;
		hi = HASH_SIZE(ksocknal_data.ksnd_peers) - 1;
	}

	for (bkt = lo; bkt <= hi; bkt++) {
		int peer_off; /* searching offset in peer_ni hash table */

		for (peer_off = 0; ; peer_off++) {
			struct ksock_peer_ni *peer_ni;
			int	      i = 0;

			read_lock(&ksocknal_data.ksnd_global_lock);
			hlist_for_each_entry(peer_ni,
					     &ksocknal_data.ksnd_peers[bkt],
					     ksnp_list) {
				if (!((id.nid == LNET_NID_ANY ||
				       id.nid == peer_ni->ksnp_id.nid) &&
				      (id.pid == LNET_PID_ANY ||
				       id.pid == peer_ni->ksnp_id.pid)))
					continue;

				if (i++ == peer_off) {
					ksocknal_peer_addref(peer_ni);
					break;
				}
			}
			read_unlock(&ksocknal_data.ksnd_global_lock);

			if (i <= peer_off) /* no match */
				break;

			rc = 0;
			ksocknal_push_peer(peer_ni);
			ksocknal_peer_decref(peer_ni);
		}
	}
	return rc;
}

static int
ksocknal_add_interface(struct lnet_ni *ni, __u32 ipaddress, __u32 netmask)
{
	struct ksock_net *net = ni->ni_data;
	struct ksock_interface *iface;
	struct sockaddr_in sa = { .sin_family = AF_INET };
	int rc;
	int i;
	int j;
	struct ksock_peer_ni *peer_ni;
	struct list_head *rtmp;
	struct ksock_route *route;

	if (ipaddress == 0 ||
	    netmask == 0)
		return -EINVAL;

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	sa.sin_addr.s_addr = htonl(ipaddress);
	iface = ksocknal_ip2iface(ni, (struct sockaddr *)&sa);
	if (iface != NULL) {
		/* silently ignore dups */
		rc = 0;
	} else if (net->ksnn_ninterfaces == LNET_INTERFACES_NUM) {
		rc = -ENOSPC;
	} else {
		iface = &net->ksnn_interfaces[net->ksnn_ninterfaces++];

		iface->ksni_index = ksocknal_ip2index((struct sockaddr *)&sa,
						      ni);
		rpc_copy_addr((struct sockaddr *)&iface->ksni_addr,
			      (struct sockaddr *)&sa);
		iface->ksni_netmask = netmask;
		iface->ksni_nroutes = 0;
		iface->ksni_npeers = 0;

		hash_for_each(ksocknal_data.ksnd_peers, i, peer_ni, ksnp_list) {
			for (j = 0; j < peer_ni->ksnp_n_passive_ips; j++)
				if (peer_ni->ksnp_passive_ips[j] == ipaddress)
					iface->ksni_npeers++;

			list_for_each(rtmp, &peer_ni->ksnp_routes) {
				route = list_entry(rtmp,
						   struct ksock_route,
						   ksnr_list);

				if (route->ksnr_myiface ==
					    iface->ksni_index)
					iface->ksni_nroutes++;
			}
		}

		rc = 0;
		/* NB only new connections will pay attention to the new
		 * interface!
		 */
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	return rc;
}

static void
ksocknal_peer_del_interface_locked(struct ksock_peer_ni *peer_ni,
				   __u32 ipaddr, int index)
{
	struct ksock_route *route;
	struct ksock_route *rnxt;
	struct ksock_conn *conn;
	struct ksock_conn *cnxt;
	int i;
	int j;

        for (i = 0; i < peer_ni->ksnp_n_passive_ips; i++)
                if (peer_ni->ksnp_passive_ips[i] == ipaddr) {
                        for (j = i+1; j < peer_ni->ksnp_n_passive_ips; j++)
                                peer_ni->ksnp_passive_ips[j-1] =
                                        peer_ni->ksnp_passive_ips[j];
                        peer_ni->ksnp_n_passive_ips--;
                        break;
                }

	list_for_each_entry_safe(route, rnxt, &peer_ni->ksnp_routes,
				 ksnr_list) {
		if (route->ksnr_myiface != index)
			continue;

		if (route->ksnr_share_count != 0) {
			/* Manually created; keep, but unbind */
			route->ksnr_myiface = -1;
		} else {
			ksocknal_del_route_locked(route);
		}
	}

	list_for_each_entry_safe(conn, cnxt, &peer_ni->ksnp_conns, ksnc_list)
		if (conn->ksnc_route->ksnr_myiface == index)
			ksocknal_close_conn_locked (conn, 0);
}

static int
ksocknal_del_interface(struct lnet_ni *ni, __u32 ipaddress)
{
	struct ksock_net *net = ni->ni_data;
	int rc = -ENOENT;
	struct hlist_node *nxt;
	struct ksock_peer_ni *peer_ni;
	u32 this_ip;
	struct sockaddr_in sa = {.sin_family = AF_INET };
	int index;
	int i;
	int j;

	sa.sin_addr.s_addr = htonl(ipaddress);
	index = ksocknal_ip2index((struct sockaddr *)&sa, ni);

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	for (i = 0; i < net->ksnn_ninterfaces; i++) {
		struct sockaddr_in *sa =
			(void *)&net->ksnn_interfaces[i].ksni_addr;

		if (sa->sin_family != AF_INET)
			continue;
		this_ip = ntohl(sa->sin_addr.s_addr);

		if (!(ipaddress == 0 ||
		      ipaddress == this_ip))
			continue;

		rc = 0;

		for (j = i+1; j < net->ksnn_ninterfaces; j++)
			net->ksnn_interfaces[j-1] =
				net->ksnn_interfaces[j];

		net->ksnn_ninterfaces--;

		hash_for_each_safe(ksocknal_data.ksnd_peers, j,
				   nxt, peer_ni, ksnp_list) {
			if (peer_ni->ksnp_ni != ni)
				continue;

			ksocknal_peer_del_interface_locked(peer_ni,
							   this_ip, index);
		}
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	return rc;
}

int
ksocknal_ctl(struct lnet_ni *ni, unsigned int cmd, void *arg)
{
	struct lnet_process_id id = {0};
        struct libcfs_ioctl_data *data = arg;
        int rc;

        switch(cmd) {
        case IOC_LIBCFS_GET_INTERFACE: {
		struct ksock_net *net = ni->ni_data;
		struct ksock_interface *iface;
		struct sockaddr_in *sa;

		read_lock(&ksocknal_data.ksnd_global_lock);

                if (data->ioc_count >= (__u32)net->ksnn_ninterfaces) {
                        rc = -ENOENT;
                } else {
                        rc = 0;
                        iface = &net->ksnn_interfaces[data->ioc_count];

			sa = (void *)&iface->ksni_addr;
			if (sa->sin_family == AF_INET)
				data->ioc_u32[0] = ntohl(sa->sin_addr.s_addr);
			else
				data->ioc_u32[0] = 0xFFFFFFFF;
			data->ioc_u32[1] = iface->ksni_netmask;
			data->ioc_u32[2] = iface->ksni_npeers;
			data->ioc_u32[3] = iface->ksni_nroutes;
		}

		read_unlock(&ksocknal_data.ksnd_global_lock);
                return rc;
        }

        case IOC_LIBCFS_ADD_INTERFACE:
                return ksocknal_add_interface(ni,
                                              data->ioc_u32[0], /* IP address */
                                              data->ioc_u32[1]); /* net mask */

        case IOC_LIBCFS_DEL_INTERFACE:
                return ksocknal_del_interface(ni,
                                              data->ioc_u32[0]); /* IP address */

        case IOC_LIBCFS_GET_PEER: {
                __u32            myip = 0;
                __u32            ip = 0;
                int              port = 0;
                int              conn_count = 0;
                int              share_count = 0;

                rc = ksocknal_get_peer_info(ni, data->ioc_count,
                                            &id, &myip, &ip, &port,
                                            &conn_count,  &share_count);
                if (rc != 0)
                        return rc;

                data->ioc_nid    = id.nid;
                data->ioc_count  = share_count;
                data->ioc_u32[0] = ip;
                data->ioc_u32[1] = port;
                data->ioc_u32[2] = myip;
                data->ioc_u32[3] = conn_count;
                data->ioc_u32[4] = id.pid;
                return 0;
        }

        case IOC_LIBCFS_ADD_PEER:
                id.nid = data->ioc_nid;
		id.pid = LNET_PID_LUSTRE;
                return ksocknal_add_peer (ni, id,
                                          data->ioc_u32[0], /* IP */
                                          data->ioc_u32[1]); /* port */

        case IOC_LIBCFS_DEL_PEER:
                id.nid = data->ioc_nid;
                id.pid = LNET_PID_ANY;
                return ksocknal_del_peer (ni, id,
                                          data->ioc_u32[0]); /* IP */

        case IOC_LIBCFS_GET_CONN: {
                int           txmem;
                int           rxmem;
                int           nagle;
		struct ksock_conn *conn = ksocknal_get_conn_by_idx(ni, data->ioc_count);
		struct sockaddr_in *psa = (void *)&conn->ksnc_peeraddr;
		struct sockaddr_in *mysa = (void *)&conn->ksnc_myaddr;

                if (conn == NULL)
                        return -ENOENT;

                ksocknal_lib_get_conn_tunables(conn, &txmem, &rxmem, &nagle);

                data->ioc_count  = txmem;
                data->ioc_nid    = conn->ksnc_peer->ksnp_id.nid;
                data->ioc_flags  = nagle;
		if (psa->sin_family == AF_INET)
			data->ioc_u32[0] = ntohl(psa->sin_addr.s_addr);
		else
			data->ioc_u32[0] = 0xFFFFFFFF;
		data->ioc_u32[1] = rpc_get_port((struct sockaddr *)
						&conn->ksnc_peeraddr);
		if (mysa->sin_family == AF_INET)
			data->ioc_u32[2] = ntohl(mysa->sin_addr.s_addr);
		else
			data->ioc_u32[2] = 0xFFFFFFFF;
		data->ioc_u32[3] = conn->ksnc_type;
		data->ioc_u32[4] = conn->ksnc_scheduler->kss_cpt;
                data->ioc_u32[5] = rxmem;
                data->ioc_u32[6] = conn->ksnc_peer->ksnp_id.pid;
                ksocknal_conn_decref(conn);
                return 0;
        }

        case IOC_LIBCFS_CLOSE_CONNECTION:
                id.nid = data->ioc_nid;
                id.pid = LNET_PID_ANY;
                return ksocknal_close_matching_conns (id,
                                                      data->ioc_u32[0]);

        case IOC_LIBCFS_REGISTER_MYNID:
                /* Ignore if this is a noop */
                if (data->ioc_nid == ni->ni_nid)
                        return 0;

                CERROR("obsolete IOC_LIBCFS_REGISTER_MYNID: %s(%s)\n",
                       libcfs_nid2str(data->ioc_nid),
                       libcfs_nid2str(ni->ni_nid));
                return -EINVAL;

        case IOC_LIBCFS_PUSH_CONNECTION:
                id.nid = data->ioc_nid;
                id.pid = LNET_PID_ANY;
                return ksocknal_push(ni, id);

        default:
                return -EINVAL;
        }
        /* not reached */
}

static void
ksocknal_free_buffers (void)
{
	LASSERT (atomic_read(&ksocknal_data.ksnd_nactive_txs) == 0);

	if (ksocknal_data.ksnd_schedulers != NULL)
		cfs_percpt_free(ksocknal_data.ksnd_schedulers);

	spin_lock(&ksocknal_data.ksnd_tx_lock);

	if (!list_empty(&ksocknal_data.ksnd_idle_noop_txs)) {
		LIST_HEAD(zlist);
		struct ksock_tx	*tx;

		list_splice_init(&ksocknal_data.ksnd_idle_noop_txs, &zlist);
		spin_unlock(&ksocknal_data.ksnd_tx_lock);

		while (!list_empty(&zlist)) {
			tx = list_entry(zlist.next, struct ksock_tx, tx_list);
			list_del(&tx->tx_list);
			LIBCFS_FREE(tx, tx->tx_desc_size);
		}
	} else {
		spin_unlock(&ksocknal_data.ksnd_tx_lock);
	}
}

static void
ksocknal_base_shutdown(void)
{
	struct ksock_sched *sched;
	struct ksock_peer_ni *peer_ni;
	int i;

	CDEBUG(D_MALLOC, "before NAL cleanup: kmem %lld\n",
	       libcfs_kmem_read());
	LASSERT (ksocknal_data.ksnd_nnets == 0);

	switch (ksocknal_data.ksnd_init) {
	default:
		LASSERT(0);
		/* fallthrough */

	case SOCKNAL_INIT_ALL:
	case SOCKNAL_INIT_DATA:
		hash_for_each(ksocknal_data.ksnd_peers, i, peer_ni, ksnp_list)
			LASSERT(0);

		LASSERT(list_empty(&ksocknal_data.ksnd_nets));
		LASSERT(list_empty(&ksocknal_data.ksnd_enomem_conns));
		LASSERT(list_empty(&ksocknal_data.ksnd_zombie_conns));
		LASSERT(list_empty(&ksocknal_data.ksnd_connd_connreqs));
		LASSERT(list_empty(&ksocknal_data.ksnd_connd_routes));

		if (ksocknal_data.ksnd_schedulers != NULL) {
			cfs_percpt_for_each(sched, i,
					    ksocknal_data.ksnd_schedulers) {

				LASSERT(list_empty(&sched->kss_tx_conns));
				LASSERT(list_empty(&sched->kss_rx_conns));
				LASSERT(list_empty(&sched->kss_zombie_noop_txs));
				LASSERT(sched->kss_nconns == 0);
			}
		}

		/* flag threads to terminate; wake and wait for them to die */
		ksocknal_data.ksnd_shuttingdown = 1;
		wake_up_all(&ksocknal_data.ksnd_connd_waitq);
		wake_up_all(&ksocknal_data.ksnd_reaper_waitq);

		if (ksocknal_data.ksnd_schedulers != NULL) {
			cfs_percpt_for_each(sched, i,
					    ksocknal_data.ksnd_schedulers)
					wake_up_all(&sched->kss_waitq);
		}

		wait_var_event_warning(&ksocknal_data.ksnd_nthreads,
				       atomic_read(&ksocknal_data.ksnd_nthreads) == 0,
				       "waiting for %d threads to terminate\n",
				       atomic_read(&ksocknal_data.ksnd_nthreads));

		ksocknal_free_buffers();

		ksocknal_data.ksnd_init = SOCKNAL_INIT_NOTHING;
		break;
	}

	CDEBUG(D_MALLOC, "after NAL cleanup: kmem %lld\n",
	       libcfs_kmem_read());

	module_put(THIS_MODULE);
}

static int
ksocknal_base_startup(void)
{
	struct ksock_sched *sched;
	int rc;
	int i;

	LASSERT(ksocknal_data.ksnd_init == SOCKNAL_INIT_NOTHING);
	LASSERT(ksocknal_data.ksnd_nnets == 0);

	memset(&ksocknal_data, 0, sizeof(ksocknal_data)); /* zero pointers */

	hash_init(ksocknal_data.ksnd_peers);

	rwlock_init(&ksocknal_data.ksnd_global_lock);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_nets);

	spin_lock_init(&ksocknal_data.ksnd_reaper_lock);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_enomem_conns);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_zombie_conns);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_deathrow_conns);
	init_waitqueue_head(&ksocknal_data.ksnd_reaper_waitq);

	spin_lock_init(&ksocknal_data.ksnd_connd_lock);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_connd_connreqs);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_connd_routes);
	init_waitqueue_head(&ksocknal_data.ksnd_connd_waitq);

	spin_lock_init(&ksocknal_data.ksnd_tx_lock);
	INIT_LIST_HEAD(&ksocknal_data.ksnd_idle_noop_txs);

	/* NB memset above zeros whole of ksocknal_data */

	/* flag lists/ptrs/locks initialised */
	ksocknal_data.ksnd_init = SOCKNAL_INIT_DATA;
	if (!try_module_get(THIS_MODULE))
		goto failed;

	/* Create a scheduler block per available CPT */
	ksocknal_data.ksnd_schedulers = cfs_percpt_alloc(lnet_cpt_table(),
							 sizeof(*sched));
	if (ksocknal_data.ksnd_schedulers == NULL)
		goto failed;

	cfs_percpt_for_each(sched, i, ksocknal_data.ksnd_schedulers) {
		int nthrs;

		/*
		 * make sure not to allocate more threads than there are
		 * cores/CPUs in teh CPT
		 */
		nthrs = cfs_cpt_weight(lnet_cpt_table(), i);
		if (*ksocknal_tunables.ksnd_nscheds > 0) {
			nthrs = min(nthrs, *ksocknal_tunables.ksnd_nscheds);
		} else {
			/*
			 * max to half of CPUs, assume another half should be
			 * reserved for upper layer modules
			 */
			nthrs = min(max(SOCKNAL_NSCHEDS, nthrs >> 1), nthrs);
		}

		sched->kss_nthreads_max = nthrs;
		sched->kss_cpt = i;

		spin_lock_init(&sched->kss_lock);
		INIT_LIST_HEAD(&sched->kss_rx_conns);
		INIT_LIST_HEAD(&sched->kss_tx_conns);
		INIT_LIST_HEAD(&sched->kss_zombie_noop_txs);
		init_waitqueue_head(&sched->kss_waitq);
        }

        ksocknal_data.ksnd_connd_starting         = 0;
        ksocknal_data.ksnd_connd_failed_stamp     = 0;
	ksocknal_data.ksnd_connd_starting_stamp   = ktime_get_real_seconds();
        /* must have at least 2 connds to remain responsive to accepts while
         * connecting */
        if (*ksocknal_tunables.ksnd_nconnds < SOCKNAL_CONND_RESV + 1)
                *ksocknal_tunables.ksnd_nconnds = SOCKNAL_CONND_RESV + 1;

        if (*ksocknal_tunables.ksnd_nconnds_max <
            *ksocknal_tunables.ksnd_nconnds) {
                ksocknal_tunables.ksnd_nconnds_max =
                        ksocknal_tunables.ksnd_nconnds;
        }

        for (i = 0; i < *ksocknal_tunables.ksnd_nconnds; i++) {
		char name[16];
		spin_lock_bh(&ksocknal_data.ksnd_connd_lock);
		ksocknal_data.ksnd_connd_starting++;
		spin_unlock_bh(&ksocknal_data.ksnd_connd_lock);


		snprintf(name, sizeof(name), "socknal_cd%02d", i);
		rc = ksocknal_thread_start(ksocknal_connd,
					   (void *)((uintptr_t)i), name);
		if (rc != 0) {
			spin_lock_bh(&ksocknal_data.ksnd_connd_lock);
			ksocknal_data.ksnd_connd_starting--;
			spin_unlock_bh(&ksocknal_data.ksnd_connd_lock);
                        CERROR("Can't spawn socknal connd: %d\n", rc);
                        goto failed;
                }
        }

	rc = ksocknal_thread_start(ksocknal_reaper, NULL, "socknal_reaper");
        if (rc != 0) {
                CERROR ("Can't spawn socknal reaper: %d\n", rc);
                goto failed;
        }

        /* flag everything initialised */
        ksocknal_data.ksnd_init = SOCKNAL_INIT_ALL;

        return 0;

 failed:
        ksocknal_base_shutdown();
        return -ENETDOWN;
}

static int
ksocknal_debug_peerhash(struct lnet_ni *ni)
{
	struct ksock_peer_ni *peer_ni;
	int i;

	read_lock(&ksocknal_data.ksnd_global_lock);

	hash_for_each(ksocknal_data.ksnd_peers, i, peer_ni, ksnp_list) {
		struct ksock_route *route;
		struct ksock_conn *conn;

		if (peer_ni->ksnp_ni != ni)
			continue;

		CWARN("Active peer_ni on shutdown: %s, ref %d, "
		      "closing %d, accepting %d, err %d, zcookie %llu, "
		      "txq %d, zc_req %d\n", libcfs_id2str(peer_ni->ksnp_id),
		      refcount_read(&peer_ni->ksnp_refcount),
		      peer_ni->ksnp_closing,
		      peer_ni->ksnp_accepting, peer_ni->ksnp_error,
		      peer_ni->ksnp_zc_next_cookie,
		      !list_empty(&peer_ni->ksnp_tx_queue),
		      !list_empty(&peer_ni->ksnp_zc_req_list));

		list_for_each_entry(route, &peer_ni->ksnp_routes, ksnr_list) {
			CWARN("Route: ref %d, schd %d, conn %d, cnted %d, del %d\n",
			      refcount_read(&route->ksnr_refcount),
			      route->ksnr_scheduled, route->ksnr_connecting,
			      route->ksnr_connected, route->ksnr_deleted);
		}

		list_for_each_entry(conn, &peer_ni->ksnp_conns, ksnc_list) {
			CWARN("Conn: ref %d, sref %d, t %d, c %d\n",
			      refcount_read(&conn->ksnc_conn_refcount),
			      refcount_read(&conn->ksnc_sock_refcount),
			      conn->ksnc_type, conn->ksnc_closing);
		}
		break;
	}

	read_unlock(&ksocknal_data.ksnd_global_lock);
	return 0;
}

void
ksocknal_shutdown(struct lnet_ni *ni)
{
	struct ksock_net *net = ni->ni_data;
	struct lnet_process_id anyid = {
		.nid = LNET_NID_ANY,
		.pid = LNET_PID_ANY,
	};
	int i;

	LASSERT(ksocknal_data.ksnd_init == SOCKNAL_INIT_ALL);
	LASSERT(ksocknal_data.ksnd_nnets > 0);

	/* prevent new peers */
	atomic_add(SOCKNAL_SHUTDOWN_BIAS, &net->ksnn_npeers);

	/* Delete all peers */
	ksocknal_del_peer(ni, anyid, 0);

	/* Wait for all peer_ni state to clean up */
	wait_var_event_warning(&net->ksnn_npeers,
			       atomic_read(&net->ksnn_npeers) ==
			       SOCKNAL_SHUTDOWN_BIAS,
			       "waiting for %d peers to disconnect\n",
			       ksocknal_debug_peerhash(ni) +
			       atomic_read(&net->ksnn_npeers) -
			       SOCKNAL_SHUTDOWN_BIAS);

	for (i = 0; i < net->ksnn_ninterfaces; i++) {
		LASSERT(net->ksnn_interfaces[i].ksni_npeers == 0);
		LASSERT(net->ksnn_interfaces[i].ksni_nroutes == 0);
	}

	list_del(&net->ksnn_list);
	LIBCFS_FREE(net, sizeof(*net));

	ksocknal_data.ksnd_nnets--;
	if (ksocknal_data.ksnd_nnets == 0)
		ksocknal_base_shutdown();
}

static int
ksocknal_search_new_ipif(struct ksock_net *net)
{
	int new_ipif = 0;
	int i;

	for (i = 0; i < net->ksnn_ninterfaces; i++) {
		char *ifnam = &net->ksnn_interfaces[i].ksni_name[0];
		char *colon = strchr(ifnam, ':');
		bool found  = false;
		struct ksock_net *tmp;
		int j;

		if (colon != NULL) /* ignore alias device */
			*colon = 0;

		list_for_each_entry(tmp, &ksocknal_data.ksnd_nets,
				    ksnn_list) {
			for (j = 0; !found && j < tmp->ksnn_ninterfaces; j++) {
				char *ifnam2 = &tmp->ksnn_interfaces[j].\
					ksni_name[0];
				char *colon2 = strchr(ifnam2, ':');

				if (colon2 != NULL)
					*colon2 = 0;

				found = strcmp(ifnam, ifnam2) == 0;
				if (colon2 != NULL)
					*colon2 = ':';
			}
			if (found)
				break;
		}

		new_ipif += !found;
		if (colon != NULL)
			*colon = ':';
	}

	return new_ipif;
}

static int
ksocknal_start_schedulers(struct ksock_sched *sched)
{
	int	nthrs;
	int	rc = 0;
	int	i;

	if (sched->kss_nthreads == 0) {
		if (*ksocknal_tunables.ksnd_nscheds > 0) {
			nthrs = sched->kss_nthreads_max;
		} else {
			nthrs = cfs_cpt_weight(lnet_cpt_table(),
					       sched->kss_cpt);
			nthrs = min(max(SOCKNAL_NSCHEDS, nthrs >> 1), nthrs);
			nthrs = min(SOCKNAL_NSCHEDS_HIGH, nthrs);
		}
		nthrs = min(nthrs, sched->kss_nthreads_max);
	} else {
		LASSERT(sched->kss_nthreads <= sched->kss_nthreads_max);
		/* increase two threads if there is new interface */
		nthrs = min(2, sched->kss_nthreads_max - sched->kss_nthreads);
	}

	for (i = 0; i < nthrs; i++) {
		long id;
		char name[20];

		id = KSOCK_THREAD_ID(sched->kss_cpt, sched->kss_nthreads + i);
		snprintf(name, sizeof(name), "socknal_sd%02d_%02d",
			 sched->kss_cpt, (int)KSOCK_THREAD_SID(id));

		rc = ksocknal_thread_start(ksocknal_scheduler,
					   (void *)id, name);
		if (rc == 0)
			continue;

		CERROR("Can't spawn thread %d for scheduler[%d]: %d\n",
		       sched->kss_cpt, (int) KSOCK_THREAD_SID(id), rc);
		break;
	}

	sched->kss_nthreads += i;
	return rc;
}

static int
ksocknal_net_start_threads(struct ksock_net *net, __u32 *cpts, int ncpts)
{
	int newif = ksocknal_search_new_ipif(net);
	int rc;
	int i;

	if (ncpts > 0 && ncpts > cfs_cpt_number(lnet_cpt_table()))
		return -EINVAL;

	for (i = 0; i < ncpts; i++) {
		struct ksock_sched *sched;
		int cpt = (cpts == NULL) ? i : cpts[i];

		LASSERT(cpt < cfs_cpt_number(lnet_cpt_table()));
		sched = ksocknal_data.ksnd_schedulers[cpt];

		if (!newif && sched->kss_nthreads > 0)
			continue;

		rc = ksocknal_start_schedulers(sched);
		if (rc != 0)
			return rc;
	}
	return 0;
}

int
ksocknal_startup(struct lnet_ni *ni)
{
	struct ksock_net *net;
	struct lnet_ioctl_config_lnd_cmn_tunables *net_tunables;
	struct ksock_interface *ksi = NULL;
	struct lnet_inetdev *ifaces = NULL;
	int i = 0;
	int rc;

        LASSERT (ni->ni_net->net_lnd == &the_ksocklnd);

        if (ksocknal_data.ksnd_init == SOCKNAL_INIT_NOTHING) {
                rc = ksocknal_base_startup();
                if (rc != 0)
                        return rc;
        }

	LIBCFS_ALLOC(net, sizeof(*net));
	if (net == NULL)
		goto fail_0;

	net->ksnn_incarnation = ktime_get_real_ns();
	ni->ni_data = net;
	net_tunables = &ni->ni_net->net_tunables;

	if (net_tunables->lct_peer_timeout == -1)
		net_tunables->lct_peer_timeout =
			*ksocknal_tunables.ksnd_peertimeout;

	if (net_tunables->lct_max_tx_credits == -1)
		net_tunables->lct_max_tx_credits =
			*ksocknal_tunables.ksnd_credits;

	if (net_tunables->lct_peer_tx_credits == -1)
		net_tunables->lct_peer_tx_credits =
			*ksocknal_tunables.ksnd_peertxcredits;

	if (net_tunables->lct_peer_tx_credits >
	    net_tunables->lct_max_tx_credits)
		net_tunables->lct_peer_tx_credits =
			net_tunables->lct_max_tx_credits;

	if (net_tunables->lct_peer_rtr_credits == -1)
		net_tunables->lct_peer_rtr_credits =
			*ksocknal_tunables.ksnd_peerrtrcredits;

	rc = lnet_inet_enumerate(&ifaces, ni->ni_net_ns);
	if (rc < 0)
		goto fail_1;

	if (!ni->ni_interfaces[0]) {
		struct sockaddr_in *sa;

		ksi = &net->ksnn_interfaces[0];
		sa = (void *)&ksi->ksni_addr;

		/* Use the first discovered interface */
		net->ksnn_ninterfaces = 1;
		ni->ni_dev_cpt = ifaces[0].li_cpt;
		memset(sa, 0, sizeof(*sa));
		sa->sin_family = AF_INET;
		sa->sin_addr.s_addr = htonl(ifaces[0].li_ipaddr);
		ksi->ksni_index = ksocknal_ip2index((struct sockaddr *)sa, ni);
		ksi->ksni_netmask = ifaces[0].li_netmask;
		strlcpy(ksi->ksni_name, ifaces[0].li_name,
			sizeof(ksi->ksni_name));
	} else {
		/* Before Multi-Rail ksocklnd would manage
		 * multiple interfaces with its own tcp bonding.
		 * If we encounter an old configuration using
		 * this tcp bonding approach then we need to
		 * handle more than one ni_interfaces.
		 *
		 * In Multi-Rail configuration only ONE ni_interface
		 * should exist. Each IP alias should be mapped to
		 * each 'struct net_ni'.
		 */
		for (i = 0; i < LNET_INTERFACES_NUM; i++) {
			int j;

			if (!ni->ni_interfaces[i])
				break;

			for (j = 0; j < LNET_INTERFACES_NUM;  j++) {
				if (i != j && ni->ni_interfaces[j] &&
				    strcmp(ni->ni_interfaces[i],
					   ni->ni_interfaces[j]) == 0) {
					rc = -EEXIST;
					CERROR("ksocklnd: found duplicate %s at %d and %d, rc = %d\n",
					       ni->ni_interfaces[i], i, j, rc);
					goto fail_1;
				}
			}

			for (j = 0; j < rc; j++) {
				struct sockaddr_in *sa;

				if (strcmp(ifaces[j].li_name,
					   ni->ni_interfaces[i]) != 0)
					continue;

				ksi =
				  &net->ksnn_interfaces[net->ksnn_ninterfaces];
				sa = (void *)&ksi->ksni_addr;
				ni->ni_dev_cpt = ifaces[j].li_cpt;
				memset(sa, 0, sizeof(*sa));
				sa->sin_family = AF_INET;
				sa->sin_addr.s_addr =
					htonl(ifaces[j].li_ipaddr);
				ksi->ksni_index = ksocknal_ip2index(
					(struct sockaddr *)sa, ni);
				ksi->ksni_netmask = ifaces[j].li_netmask;
				strlcpy(ksi->ksni_name, ifaces[j].li_name,
					sizeof(ksi->ksni_name));
				net->ksnn_ninterfaces++;
				break;
			}
		}
		/* ni_interfaces don't map to all network interfaces */
		if (!ksi || net->ksnn_ninterfaces != i) {
			CERROR("ksocklnd: requested %d but only %d interfaces found\n",
			       i, net->ksnn_ninterfaces);
			goto fail_1;
		}
	}

	/* call it before add it to ksocknal_data.ksnd_nets */
	rc = ksocknal_net_start_threads(net, ni->ni_cpts, ni->ni_ncpts);
	if (rc != 0)
		goto fail_1;

	LASSERT(ksi);
	LASSERT(ksi->ksni_addr.ss_family == AF_INET);
	ni->ni_nid = LNET_MKNID(
		LNET_NIDNET(ni->ni_nid),
		ntohl(((struct sockaddr_in *)
		       &ksi->ksni_addr)->sin_addr.s_addr));
	list_add(&net->ksnn_list, &ksocknal_data.ksnd_nets);

	ksocknal_data.ksnd_nnets++;

	return 0;

fail_1:
	LIBCFS_FREE(net, sizeof(*net));
fail_0:
	if (ksocknal_data.ksnd_nnets == 0)
		ksocknal_base_shutdown();

	return -ENETDOWN;
}


static void __exit ksocklnd_exit(void)
{
	lnet_unregister_lnd(&the_ksocklnd);
}

static const struct lnet_lnd the_ksocklnd = {
	.lnd_type		= SOCKLND,
	.lnd_startup		= ksocknal_startup,
	.lnd_shutdown		= ksocknal_shutdown,
	.lnd_ctl		= ksocknal_ctl,
	.lnd_send		= ksocknal_send,
	.lnd_recv		= ksocknal_recv,
	.lnd_notify_peer_down	= ksocknal_notify_gw_down,
	.lnd_accept		= ksocknal_accept,
};

static int __init ksocklnd_init(void)
{
	int rc;

	/* check ksnr_connected/connecting field large enough */
	BUILD_BUG_ON(SOCKLND_CONN_NTYPES > 4);
	BUILD_BUG_ON(SOCKLND_CONN_ACK != SOCKLND_CONN_BULK_IN);

	rc = ksocknal_tunables_init();
	if (rc != 0)
		return rc;

	lnet_register_lnd(&the_ksocklnd);

	return 0;
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("TCP Socket LNet Network Driver");
MODULE_VERSION("2.8.0");
MODULE_LICENSE("GPL");

module_init(ksocklnd_init);
module_exit(ksocklnd_exit);
