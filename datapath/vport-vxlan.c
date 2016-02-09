/*
 * Copyright (c) 2013 Nicira, Inc.
 * Copyright (c) 2013 Cisco Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>

#include <linux/in.h>
#include <linux/ip.h>
#include <linux/net.h>
#include <linux/rculist.h>
#include <linux/udp.h>

#include <net/icmp.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/ip_tunnels.h>
#include <net/rtnetlink.h>
#include <net/route.h>
#include <net/dsfield.h>
#include <net/inet_ecn.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/vxlan.h>

#include "datapath.h"
#include "vport.h"

/**
 * struct vxlan_port - Keeps track of open UDP ports
 * @vs: vxlan_sock created for the port.
 * @name: vport name.
 */
struct vxlan_port {
	struct vxlan_sock *vs;
	char name[IFNAMSIZ];
};

static inline struct vxlan_port *vxlan_vport(const struct vport *vport)
{
	return vport_priv(vport);
}

/* Called with rcu_read_lock and BH disabled.
    This function is used in linux call chain to receive vxlan packets.
    Then call ovs_vport_receive to further process packets.
    vxlan_rcv function is also defined in Linux/include/net/vxlan.h as follow:

    Line12 typedef void (vxlan_rcv_t)(struct vxlan_sock *vh, struct sk_buff *skb, __be32 key);

    This is to declare function, and linux call chain could register this function, and users
    could fill this function. Such as drivers/net/vxlan.c: line 2271 line 2324.
    */
static void vxlan_rcv(struct vxlan_sock *vs, struct sk_buff *skb, __be32 vx_vni)
{
	struct ovs_key_ipv4_tunnel tun_key;
	struct vport *vport = vs->data;
	struct iphdr *iph;
	__be64 key;

	/* Save outer tunnel values */
	iph = ip_hdr(skb);
	key = cpu_to_be64(ntohl(vx_vni) >> 8);
	ovs_flow_tun_key_init(&tun_key, iph, key, TUNNEL_KEY);
        /* ovs_flow_tun_key_init function is defined in openvswitch kernel module.
            refer to net/openvswitch/flow.h, line 52. */

	ovs_vport_receive(vport, skb, &tun_key);
}

static int vxlan_get_options(const struct vport *vport, struct sk_buff *skb)
{
	struct vxlan_port *vxlan_port = vxlan_vport(vport);
	__be16 dst_port = inet_sport(vxlan_port->vs->sock->sk);

	if (nla_put_u16(skb, OVS_TUNNEL_ATTR_DST_PORT, ntohs(dst_port)))
		return -EMSGSIZE;
	return 0;
}

static void vxlan_tnl_destroy(struct vport *vport)
{
	struct vxlan_port *vxlan_port = vxlan_vport(vport);

	vxlan_sock_release(vxlan_port->vs);

	ovs_vport_deferred_free(vport);
}

static struct vport *vxlan_tnl_create(const struct vport_parms *parms)
{
        /* Struct net is used for multi-namespace networking system.
            In ovs_dp_get_net, use read_pnet and net we register before to switch
            to the net we defined.
            The net is used in vxlan_sock_add. */
	struct net *net = ovs_dp_get_net(parms->dp);
        // struct nlattr is used as Netlink attributes, and it's tlv same as used in OVS
	struct nlattr *options = parms->options;
	struct vxlan_port *vxlan_port;
	struct vxlan_sock *vs;
	struct vport *vport;
	struct nlattr *a;
	u16 dst_port;
	int err;

	if (!options) {
		err = -EINVAL;
		goto error;
	}
	a = nla_find_nested(options, OVS_TUNNEL_ATTR_DST_PORT);
	if (a && nla_len(a) == sizeof(u16)) {
		dst_port = nla_get_u16(a);
	} else {
		/* Require destination port from userspace. */
		err = -EINVAL;
		goto error;
	}

	vport = ovs_vport_alloc(sizeof(struct vxlan_port),
				&ovs_vxlan_vport_ops, parms);
	if (IS_ERR(vport))
		return vport;

	vxlan_port = vxlan_vport(vport);
	strncpy(vxlan_port->name, parms->name, IFNAMSIZ);

        /* Construct vxlan socket, we use this socket to send packets.
            vxlan_rcv is use for receive vxlan packet, this is added on linux call chain. */
	vs = vxlan_sock_add(net, htons(dst_port), vxlan_rcv, vport, true, false);
	if (IS_ERR(vs)) {
		ovs_vport_free(vport);
		return (void *)vs;
	}
	vxlan_port->vs = vs;

	return vport;

error:
	return ERR_PTR(err);
}

static int vxlan_tnl_send(struct vport *vport, struct sk_buff *skb)
{
	struct net *net = ovs_dp_get_net(vport->dp);
	struct vxlan_port *vxlan_port = vxlan_vport(vport);
	__be16 dst_port = inet_sport(vxlan_port->vs->sock->sk);
	struct rtable *rt;
	__be16 src_port;
	__be32 saddr;
	__be16 df;
	int port_min;
	int port_max;
	int err;

	if (unlikely(!OVS_CB(skb)->tun_key)) {
		err = -EINVAL;
		goto error;
	}

	/* Route lookup */
	saddr = OVS_CB(skb)->tun_key->ipv4_src;
	rt = find_route(ovs_dp_get_net(vport->dp),
			&saddr,
			OVS_CB(skb)->tun_key->ipv4_dst,
			IPPROTO_UDP,
			OVS_CB(skb)->tun_key->ipv4_tos,
			skb->mark);
	if (IS_ERR(rt)) {
		err = PTR_ERR(rt);
		goto error;
	}

	df = OVS_CB(skb)->tun_key->tun_flags & TUNNEL_DONT_FRAGMENT ?
		htons(IP_DF) : 0;

	skb->local_df = 1;

	inet_get_local_port_range(net, &port_min, &port_max);
	src_port = vxlan_src_port(port_min, port_max, skb);

	err = vxlan_xmit_skb(vxlan_port->vs, rt, skb,
			     saddr, OVS_CB(skb)->tun_key->ipv4_dst,
			     OVS_CB(skb)->tun_key->ipv4_tos,
			     OVS_CB(skb)->tun_key->ipv4_ttl, df,
			     src_port, dst_port,
			     htonl(be64_to_cpu(OVS_CB(skb)->tun_key->tun_id) << 8));
	if (err < 0)
		ip_rt_put(rt);
error:
	return err;
}

static const char *vxlan_get_name(const struct vport *vport)
{
	struct vxlan_port *vxlan_port = vxlan_vport(vport);
	return vxlan_port->name;
}

const struct vport_ops ovs_vxlan_vport_ops = {
	.type		= OVS_VPORT_TYPE_VXLAN,
	.create		= vxlan_tnl_create,
	.destroy	= vxlan_tnl_destroy,
	.get_name	= vxlan_get_name,
	.get_options	= vxlan_get_options,
	.send		= vxlan_tnl_send,
};
