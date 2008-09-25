/*
 * Distributed under the terms of the GNU GPL version 2.
 * Copyright (c) 2007, 2008 The Board of Trustees of The Leland 
 * Stanford Junior University
 */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in6.h>
#include <asm/uaccess.h>
#include <linux/types.h>
#include <net/checksum.h>
#include "forward.h"
#include "datapath.h"
#include "chain.h"
#include "flow.h"

/* FIXME: do we need to use GFP_ATOMIC everywhere here? */

static int make_writable(struct sk_buff **);

static struct sk_buff *retrieve_skb(uint32_t id);
static void discard_skb(uint32_t id);

/* 'skb' was received on port 'p', which may be a physical switch port, the
 * local port, or a null pointer.  Process it according to 'chain'.  Returns 0
 * if successful, in which case 'skb' is destroyed, or -ESRCH if there is no
 * matching flow, in which case 'skb' still belongs to the caller. */
int run_flow_through_tables(struct sw_chain *chain, struct sk_buff *skb,
			    struct net_bridge_port *p)
{
	/* Ethernet address used as the destination for STP frames. */
	static const uint8_t stp_eth_addr[ETH_ALEN]
		= { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x01 };
	struct sw_flow_key key;
	struct sw_flow *flow;

	if (flow_extract(skb, p ? p->port_no : OFPP_NONE, &key)
	    && (chain->dp->flags & OFPC_FRAG_MASK) == OFPC_FRAG_DROP) {
		/* Drop fragment. */
		kfree_skb(skb);
		return 0;
	}
	if (p && p->config & (OFPPC_NO_RECV | OFPPC_NO_RECV_STP) &&
	    p->config & (compare_ether_addr(key.dl_dst, stp_eth_addr)
			? OFPPC_NO_RECV : OFPPC_NO_RECV_STP)) {
		kfree_skb(skb);
		return 0;
	}

	flow = chain_lookup(chain, &key);
	if (likely(flow != NULL)) {
		struct sw_flow_actions *sf_acts = rcu_dereference(flow->sf_acts);
		flow_used(flow, skb);
		execute_actions(chain->dp, skb, &key,
				sf_acts->actions, sf_acts->n_actions, 0);
		return 0;
	} else {
		return -ESRCH;
	}
}

/* 'skb' was received on port 'p', which may be a physical switch port, the
 * local port, or a null pointer.  Process it according to 'chain', sending it
 * up to the controller if no flow matches.  Takes ownership of 'skb'. */
void fwd_port_input(struct sw_chain *chain, struct sk_buff *skb,
		    struct net_bridge_port *p)
{
	if (run_flow_through_tables(chain, skb, p))
		dp_output_control(chain->dp, skb, fwd_save_skb(skb), 
				  chain->dp->miss_send_len,
				  OFPR_NO_MATCH);
}

static int do_output(struct datapath *dp, struct sk_buff *skb, size_t max_len,
		     int out_port, int ignore_no_fwd)
{
	if (!skb)
		return -ENOMEM;
	return (likely(out_port != OFPP_CONTROLLER)
		? dp_output_port(dp, skb, out_port, ignore_no_fwd)
		: dp_output_control(dp, skb, fwd_save_skb(skb),
					 max_len, OFPR_ACTION));
}

void execute_actions(struct datapath *dp, struct sk_buff *skb,
		     struct sw_flow_key *key,
		     const struct ofp_action *actions, int n_actions,
		     int ignore_no_fwd)
{
	/* Every output action needs a separate clone of 'skb', but the common
	 * case is just a single output action, so that doing a clone and
	 * then freeing the original skbuff is wasteful.  So the following code
	 * is slightly obscure just to avoid that. */
	int prev_port;
	size_t max_len=0;	 /* Initialze to make compiler happy */
	uint16_t eth_proto;
	int i;

	prev_port = -1;
	eth_proto = ntohs(key->dl_type);

	for (i = 0; i < n_actions; i++) {
		const struct ofp_action *a = &actions[i];

		if (prev_port != -1) {
			do_output(dp, skb_clone(skb, GFP_ATOMIC),
				  max_len, prev_port, ignore_no_fwd);
			prev_port = -1;
		}

		if (likely(a->type == htons(OFPAT_OUTPUT))) {
			prev_port = ntohs(a->arg.output.port);
			max_len = ntohs(a->arg.output.max_len);
		} else {
			if (!make_writable(&skb)) {
				if (net_ratelimit())
				    printk("make_writable failed\n");
				break;
			}
			skb = execute_setter(skb, eth_proto, key, a);
			if (!skb) {
				if (net_ratelimit())
					printk("execute_setter lost skb\n");
				return;
			}
		}
	}
	if (prev_port != -1)
		do_output(dp, skb, max_len, prev_port, ignore_no_fwd);
	else
		kfree_skb(skb);
}

/* Updates 'sum', which is a field in 'skb''s data, given that a 4-byte field
 * covered by the sum has been changed from 'from' to 'to'.  If set,
 * 'pseudohdr' indicates that the field is in the TCP or UDP pseudo-header.
 * Based on nf_proto_csum_replace4. */
static void update_csum(__sum16 *sum, struct sk_buff *skb,
			__be32 from, __be32 to, int pseudohdr)
{
	__be32 diff[] = { ~from, to };
	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		*sum = csum_fold(csum_partial((char *)diff, sizeof(diff),
				~csum_unfold(*sum)));
		if (skb->ip_summed == CHECKSUM_COMPLETE && pseudohdr)
			skb->csum = ~csum_partial((char *)diff, sizeof(diff),
						~skb->csum);
	} else if (pseudohdr)
		*sum = ~csum_fold(csum_partial((char *)diff, sizeof(diff),
				csum_unfold(*sum)));
}

static void modify_nh(struct sk_buff *skb, uint16_t eth_proto,
			uint8_t nw_proto, const struct ofp_action *a)
{
	if (eth_proto == ETH_P_IP) {
		struct iphdr *nh = ip_hdr(skb);
		uint32_t new, *field;

		new = a->arg.nw_addr;

		if (a->type == htons(OFPAT_SET_NW_SRC))
			field = &nh->saddr;
		else
			field = &nh->daddr;

		if (nw_proto == IPPROTO_TCP) {
			struct tcphdr *th = tcp_hdr(skb);
			update_csum(&th->check, skb, *field, new, 1);
		} else if (nw_proto == IPPROTO_UDP) {
			struct udphdr *th = udp_hdr(skb);
			update_csum(&th->check, skb, *field, new, 1);
		}
		update_csum(&nh->check, skb, *field, new, 0);
		*field = new;
	}
}

static void modify_th(struct sk_buff *skb, uint16_t eth_proto,
			uint8_t nw_proto, const struct ofp_action *a)
{
	if (eth_proto == ETH_P_IP) {
		uint16_t new, *field;

		new = a->arg.tp;

		if (nw_proto == IPPROTO_TCP) {
			struct tcphdr *th = tcp_hdr(skb);

			if (a->type == htons(OFPAT_SET_TP_SRC))
				field = &th->source;
			else
				field = &th->dest;

			update_csum(&th->check, skb, *field, new, 1);
			*field = new;
		} else if (nw_proto == IPPROTO_UDP) {
			struct udphdr *th = udp_hdr(skb);

			if (a->type == htons(OFPAT_SET_TP_SRC))
				field = &th->source;
			else
				field = &th->dest;

			update_csum(&th->check, skb, *field, new, 1);
			*field = new;
		}
	}
}

static struct sk_buff *vlan_pull_tag(struct sk_buff *skb)
{
	struct vlan_ethhdr *vh = vlan_eth_hdr(skb);
	struct ethhdr *eh;


	/* Verify we were given a vlan packet */
	if (vh->h_vlan_proto != htons(ETH_P_8021Q))
		return skb;

	memmove(skb->data + VLAN_HLEN, skb->data, 2 * VLAN_ETH_ALEN);

	eh = (struct ethhdr *)skb_pull(skb, VLAN_HLEN);

	skb->protocol = eh->h_proto;
	skb->mac_header += VLAN_HLEN;

	return skb;
}

static struct sk_buff *modify_vlan_tci(struct sk_buff *skb, 
		struct sw_flow_key *key, uint16_t tci, uint16_t mask)
{
	struct vlan_ethhdr *vh = vlan_eth_hdr(skb);

	if (key->dl_vlan != htons(OFP_VLAN_NONE)) {
		/* Modify vlan id, but maintain other TCI values */
		vh->h_vlan_TCI = (vh->h_vlan_TCI & ~(htons(mask))) | htons(tci);
	} else  {
		/* Add vlan header */

		/* xxx The vlan_put_tag function, doesn't seem to work
		 * xxx reliably when it attempts to use the hardware-accelerated
		 * xxx version.  We'll directly use the software version
		 * xxx until the problem can be diagnosed.
		 */
		skb = __vlan_put_tag(skb, tci);
		vh = vlan_eth_hdr(skb);
	}
	key->dl_vlan = vh->h_vlan_TCI & htons(VLAN_VID_MASK);

	return skb;
}

/* Mask for the priority bits in a vlan header.  The kernel doesn't
 * define this like it does for VID. */
#define VLAN_PCP_MASK 0xe000

struct sk_buff *execute_setter(struct sk_buff *skb, uint16_t eth_proto,
			struct sw_flow_key *key, const struct ofp_action *a)
{
	switch (ntohs(a->type)) {
	case OFPAT_SET_VLAN_VID: {
		uint16_t tci = ntohs(a->arg.vlan_vid);
		skb = modify_vlan_tci(skb, key, tci, VLAN_VID_MASK);
		break;
	}

	case OFPAT_SET_VLAN_PCP: {
		uint16_t tci = (uint16_t)a->arg.vlan_pcp << 13;
		skb = modify_vlan_tci(skb, key, tci, VLAN_PCP_MASK);
		break;
	}

	case OFPAT_STRIP_VLAN:
		vlan_pull_tag(skb);
		key->dl_vlan = htons(OFP_VLAN_NONE);
		break;

	case OFPAT_SET_DL_SRC: {
		struct ethhdr *eh = eth_hdr(skb);
		memcpy(eh->h_source, a->arg.dl_addr, sizeof eh->h_source);
		break;
	}
	case OFPAT_SET_DL_DST: {
		struct ethhdr *eh = eth_hdr(skb);
		memcpy(eh->h_dest, a->arg.dl_addr, sizeof eh->h_dest);
		break;
	}

	case OFPAT_SET_NW_SRC:
	case OFPAT_SET_NW_DST:
		modify_nh(skb, eth_proto, key->nw_proto, a);
		break;

	case OFPAT_SET_TP_SRC:
	case OFPAT_SET_TP_DST:
		modify_th(skb, eth_proto, key->nw_proto, a);
		break;
	
	default:
		if (net_ratelimit())
			printk("execute_setter: unknown action: %d\n", ntohs(a->type));
	}

	return skb;
}

static int
recv_hello(struct sw_chain *chain, const struct sender *sender,
	   const void *msg)
{
	return dp_send_hello(chain->dp, sender, msg);
}

static int
recv_features_request(struct sw_chain *chain, const struct sender *sender,
		      const void *msg) 
{
	return dp_send_features_reply(chain->dp, sender);
}

static int
recv_get_config_request(struct sw_chain *chain, const struct sender *sender,
			const void *msg)
{
	return dp_send_config_reply(chain->dp, sender);
}

static int
recv_set_config(struct sw_chain *chain, const struct sender *sender,
		const void *msg)
{
	const struct ofp_switch_config *osc = msg;
	int flags;

	flags = ntohs(osc->flags) & (OFPC_SEND_FLOW_EXP | OFPC_FRAG_MASK);
	if ((flags & OFPC_FRAG_MASK) != OFPC_FRAG_NORMAL
	    && (flags & OFPC_FRAG_MASK) != OFPC_FRAG_DROP) {
		flags = (flags & ~OFPC_FRAG_MASK) | OFPC_FRAG_DROP;
	}
	chain->dp->flags = flags;

	chain->dp->miss_send_len = ntohs(osc->miss_send_len);

	return 0;
}

static int
recv_packet_out(struct sw_chain *chain, const struct sender *sender,
		const void *msg)
{
	const struct ofp_packet_out *opo = msg;
	struct sk_buff *skb;
	struct vlan_ethhdr *mac;
	int nh_ofs;
	struct sw_flow_key key;
	int n_actions = ntohs(opo->n_actions);
	int act_len = n_actions * sizeof opo->actions[0];

	if (act_len > (ntohs(opo->header.length) - sizeof *opo)) {
		if (net_ratelimit()) 
			printk("message too short for number of actions\n");
		return -EINVAL;
	}

	if (ntohl(opo->buffer_id) == (uint32_t) -1) {
		int data_len = ntohs(opo->header.length) - sizeof *opo - act_len;

		/* FIXME: there is likely a way to reuse the data in msg. */
		skb = alloc_skb(data_len, GFP_ATOMIC);
		if (!skb)
			return -ENOMEM;

		/* FIXME?  We don't reserve NET_IP_ALIGN or NET_SKB_PAD since
		 * we're just transmitting this raw without examining anything
		 * at those layers. */
		memcpy(skb_put(skb, data_len), &opo->actions[n_actions], data_len);

		skb_set_mac_header(skb, 0);
		mac = vlan_eth_hdr(skb);
		if (likely(mac->h_vlan_proto != htons(ETH_P_8021Q)))
			nh_ofs = sizeof(struct ethhdr);
		else
			nh_ofs = sizeof(struct vlan_ethhdr);
		skb_set_network_header(skb, nh_ofs);
	} else {
		skb = retrieve_skb(ntohl(opo->buffer_id));
		if (!skb)
			return -ESRCH;
	}

	dp_set_origin(chain->dp, ntohs(opo->in_port), skb);

	flow_extract(skb, ntohs(opo->in_port), &key);
	execute_actions(chain->dp, skb, &key, opo->actions, n_actions, 1);

	return 0;
}

static int
recv_port_mod(struct sw_chain *chain, const struct sender *sender,
	      const void *msg)
{
	const struct ofp_port_mod *opm = msg;

	dp_update_port_flags(chain->dp, opm);

	return 0;
}

static int
recv_echo_request(struct sw_chain *chain, const struct sender *sender,
		  const void *msg) 
{
	return dp_send_echo_reply(chain->dp, sender, msg);
}

static int
recv_echo_reply(struct sw_chain *chain, const struct sender *sender,
		  const void *msg) 
{
	return 0;
}

static int
add_flow(struct sw_chain *chain, const struct ofp_flow_mod *ofm)
{
	int error = -ENOMEM;
	int i;
	int n_actions;
	struct sw_flow *flow;


	/* To prevent loops, make sure there's no action to send to the
	 * OFP_TABLE virtual port.
	 */
	n_actions = (ntohs(ofm->header.length) - sizeof *ofm) 
			/ sizeof *ofm->actions;
	for (i=0; i<n_actions; i++) {
		const struct ofp_action *a = &ofm->actions[i];

		if (a->type == htons(OFPAT_OUTPUT) 
					&& (a->arg.output.port == htons(OFPP_TABLE) 
						|| a->arg.output.port == htons(OFPP_NONE)
						|| a->arg.output.port == ofm->match.in_port)) {
			/* xxx Send fancy new error message? */
			goto error;
		}
	}

	/* Allocate memory. */
	flow = flow_alloc(n_actions, GFP_ATOMIC);
	if (flow == NULL)
		goto error;

	/* Fill out flow. */
	flow_extract_match(&flow->key, &ofm->match);
	flow->priority = flow->key.wildcards ? ntohs(ofm->priority) : -1;
	flow->idle_timeout = ntohs(ofm->idle_timeout);
	flow->hard_timeout = ntohs(ofm->hard_timeout);
	flow->used = jiffies;
	flow->init_time = jiffies;
	flow->byte_count = 0;
	flow->packet_count = 0;
	spin_lock_init(&flow->lock);
	memcpy(flow->sf_acts->actions, ofm->actions, 
			n_actions * sizeof *flow->sf_acts->actions);

	/* Act. */
	error = chain_insert(chain, flow);
	if (error)
		goto error_free_flow;
	error = 0;
	if (ntohl(ofm->buffer_id) != (uint32_t) -1) {
		struct sk_buff *skb = retrieve_skb(ntohl(ofm->buffer_id));
		if (skb) {
			struct sw_flow_key key;
			flow_used(flow, skb);
			flow_extract(skb, ntohs(ofm->match.in_port), &key);
			execute_actions(chain->dp, skb, &key, ofm->actions, n_actions, 0);
		}
		else
			error = -ESRCH;
	}
	return error;

error_free_flow:
	flow_free(flow);
error:
	if (ntohl(ofm->buffer_id) != (uint32_t) -1)
		discard_skb(ntohl(ofm->buffer_id));
	return error;
}

static int
mod_flow(struct sw_chain *chain, const struct ofp_flow_mod *ofm)
{
	int error = -ENOMEM;
	int i;
	int n_actions;
	struct sw_flow_key key;
	uint16_t priority;
	int strict;

	/* To prevent loops, make sure there's no action to send to the
	 * OFP_TABLE virtual port.
	 */
	n_actions = (ntohs(ofm->header.length) - sizeof *ofm) 
			/ sizeof *ofm->actions;
	for (i=0; i<n_actions; i++) {
		const struct ofp_action *a = &ofm->actions[i];

		if (a->type == htons(OFPAT_OUTPUT) 
					&& (a->arg.output.port == htons(OFPP_TABLE) 
						|| a->arg.output.port == htons(OFPP_NONE)
						|| a->arg.output.port == ofm->match.in_port)) {
			/* xxx Send fancy new error message? */
			goto error;
		}
	}

	flow_extract_match(&key, &ofm->match);
	priority = key.wildcards ? ntohs(ofm->priority) : -1;
	strict = (ofm->command == htons(OFPFC_MODIFY_STRICT)) ? 1 : 0;
	chain_modify(chain, &key, priority, strict, ofm->actions, n_actions);

	if (ntohl(ofm->buffer_id) != (uint32_t) -1) {
		struct sk_buff *skb = retrieve_skb(ntohl(ofm->buffer_id));
		if (skb) {
			struct sw_flow_key skb_key;
			flow_extract(skb, ntohs(ofm->match.in_port), &skb_key);
			execute_actions(chain->dp, skb, &skb_key, 
					ofm->actions, n_actions, 0);
		}
		else
			error = -ESRCH;
	}
	return error;

error:
	if (ntohl(ofm->buffer_id) != (uint32_t) -1)
		discard_skb(ntohl(ofm->buffer_id));
	return error;
}

static int
recv_flow(struct sw_chain *chain, const struct sender *sender, const void *msg)
{
	const struct ofp_flow_mod *ofm = msg;
	uint16_t command = ntohs(ofm->command);

	if (command == OFPFC_ADD) {
		return add_flow(chain, ofm);
	} else if ((command == OFPFC_MODIFY) || (command == OFPFC_MODIFY_STRICT)) {
		return mod_flow(chain, ofm);
	}  else if (command == OFPFC_DELETE) {
		struct sw_flow_key key;
		flow_extract_match(&key, &ofm->match);
		return chain_delete(chain, &key, 0, 0) ? 0 : -ESRCH;
	} else if (command == OFPFC_DELETE_STRICT) {
		struct sw_flow_key key;
		uint16_t priority;
		flow_extract_match(&key, &ofm->match);
		priority = key.wildcards ? ntohs(ofm->priority) : -1;
		return chain_delete(chain, &key, priority, 1) ? 0 : -ESRCH;
	} else {
		return -ENOTSUPP;
	}
}

/* 'msg', which is 'length' bytes long, was received across Netlink from
 * 'sender'.  Apply it to 'chain'. */
int
fwd_control_input(struct sw_chain *chain, const struct sender *sender,
		  const void *msg, size_t length)
{

	struct openflow_packet {
		size_t min_size;
		int (*handler)(struct sw_chain *, const struct sender *,
			       const void *);
	};

	static const struct openflow_packet packets[] = {
		[OFPT_HELLO] = {
			sizeof (struct ofp_header),
			recv_hello,
		},
		[OFPT_FEATURES_REQUEST] = {
			sizeof (struct ofp_header),
			recv_features_request,
		},
		[OFPT_GET_CONFIG_REQUEST] = {
			sizeof (struct ofp_header),
			recv_get_config_request,
		},
		[OFPT_SET_CONFIG] = {
			sizeof (struct ofp_switch_config),
			recv_set_config,
		},
		[OFPT_PACKET_OUT] = {
			sizeof (struct ofp_packet_out),
			recv_packet_out,
		},
		[OFPT_FLOW_MOD] = {
			sizeof (struct ofp_flow_mod),
			recv_flow,
		},
		[OFPT_PORT_MOD] = {
			sizeof (struct ofp_port_mod),
			recv_port_mod,
		},
		[OFPT_ECHO_REQUEST] = {
			sizeof (struct ofp_header),
			recv_echo_request,
		},
		[OFPT_ECHO_REPLY] = {
			sizeof (struct ofp_header),
			recv_echo_reply,
		},
	};

	struct ofp_header *oh;

	oh = (struct ofp_header *) msg;
	if (oh->version != OFP_VERSION
	    && oh->type != OFPT_HELLO
	    && oh->type != OFPT_ERROR
	    && oh->type != OFPT_ECHO_REQUEST
	    && oh->type != OFPT_ECHO_REPLY
	    && oh->type != OFPT_VENDOR)
	{
		dp_send_error_msg(chain->dp, sender, OFPET_BAD_REQUEST,
				  OFPBRC_BAD_VERSION, msg, length);
		return -EINVAL;
	}
	if (ntohs(oh->length) > length)
		return -EINVAL;

	if (oh->type < ARRAY_SIZE(packets)) {
		const struct openflow_packet *pkt = &packets[oh->type];
		if (pkt->handler) {
			if (length < pkt->min_size)
				return -EFAULT;
			return pkt->handler(chain, sender, msg);
		}
	}
	dp_send_error_msg(chain->dp, sender, OFPET_BAD_REQUEST,
			  OFPBRC_BAD_TYPE, msg, length);
	return -EINVAL;
}

/* Packet buffering. */

#define OVERWRITE_SECS	1
#define OVERWRITE_JIFFIES (OVERWRITE_SECS * HZ)

struct packet_buffer {
	struct sk_buff *skb;
	uint32_t cookie;
	unsigned long exp_jiffies;
};

static struct packet_buffer buffers[N_PKT_BUFFERS];
static unsigned int buffer_idx;
static DEFINE_SPINLOCK(buffer_lock);

uint32_t fwd_save_skb(struct sk_buff *skb)
{
	struct sk_buff *old_skb = NULL;
	struct packet_buffer *p;
	unsigned long int flags;
	uint32_t id;

	spin_lock_irqsave(&buffer_lock, flags);
	buffer_idx = (buffer_idx + 1) & PKT_BUFFER_MASK;
	p = &buffers[buffer_idx];
	if (p->skb) {
		/* Don't buffer packet if existing entry is less than
		 * OVERWRITE_SECS old. */
		if (time_before(jiffies, p->exp_jiffies)) {
			spin_unlock_irqrestore(&buffer_lock, flags);
			return -1;
		} else {
			/* Defer kfree_skb() until interrupts re-enabled. */
			old_skb = p->skb;
		}
	}
	/* Don't use maximum cookie value since the all-bits-1 id is
	 * special. */
	if (++p->cookie >= (1u << PKT_COOKIE_BITS) - 1)
		p->cookie = 0;
	skb_get(skb);
	p->skb = skb;
	p->exp_jiffies = jiffies + OVERWRITE_JIFFIES;
	id = buffer_idx | (p->cookie << PKT_BUFFER_BITS);
	spin_unlock_irqrestore(&buffer_lock, flags);

	if (old_skb)
		kfree_skb(old_skb);

	return id;
}

static struct sk_buff *retrieve_skb(uint32_t id)
{
	unsigned long int flags;
	struct sk_buff *skb = NULL;
	struct packet_buffer *p;

	spin_lock_irqsave(&buffer_lock, flags);
	p = &buffers[id & PKT_BUFFER_MASK];
	if (p->cookie == id >> PKT_BUFFER_BITS) {
		skb = p->skb;
		p->skb = NULL;
	} else {
		printk("cookie mismatch: %x != %x\n",
				id >> PKT_BUFFER_BITS, p->cookie);
	}
	spin_unlock_irqrestore(&buffer_lock, flags);

	return skb;
}

void fwd_discard_all(void) 
{
	int i;

	for (i = 0; i < N_PKT_BUFFERS; i++) {
		struct sk_buff *skb;
		unsigned long int flags;

		/* Defer kfree_skb() until interrupts re-enabled. */
		spin_lock_irqsave(&buffer_lock, flags);
		skb = buffers[i].skb;
		buffers[i].skb = NULL;
		spin_unlock_irqrestore(&buffer_lock, flags);

		kfree_skb(skb);
	}
}

static void discard_skb(uint32_t id)
{
	struct sk_buff *old_skb = NULL;
	unsigned long int flags;
	struct packet_buffer *p;

	spin_lock_irqsave(&buffer_lock, flags);
	p = &buffers[id & PKT_BUFFER_MASK];
	if (p->cookie == id >> PKT_BUFFER_BITS) {
		/* Defer kfree_skb() until interrupts re-enabled. */
		old_skb = p->skb;
		p->skb = NULL;
	}
	spin_unlock_irqrestore(&buffer_lock, flags);

	if (old_skb)
		kfree_skb(old_skb);
}

void fwd_exit(void)
{
	fwd_discard_all();
}

/* Utility functions. */

/* Makes '*pskb' writable, possibly copying it and setting '*pskb' to point to
 * the copy.
 * Returns 1 if successful, 0 on failure. */
static int
make_writable(struct sk_buff **pskb)
{
	/* Based on skb_make_writable() in net/netfilter/core.c. */
	struct sk_buff *nskb;

	/* Not exclusive use of packet?  Must copy. */
	if (skb_shared(*pskb) || skb_cloned(*pskb))
		goto copy_skb;

	return pskb_may_pull(*pskb, 40); /* FIXME? */

copy_skb:
	nskb = skb_copy(*pskb, GFP_ATOMIC);
	if (!nskb)
		return 0;
	BUG_ON(skb_is_nonlinear(nskb));

	/* Rest of kernel will get very unhappy if we pass it a
	   suddenly-orphaned skbuff */
	if ((*pskb)->sk)
		skb_set_owner_w(nskb, (*pskb)->sk);
	kfree_skb(*pskb);
	*pskb = nskb;
	return 1;
}
