/* Copyright (c) 2008 The Board of Trustees of The Leland Stanford
 * Junior University
 * 
 * We are making the OpenFlow specification and associated documentation
 * (Software) available for public use and benefit with the expectation
 * that others will use, modify and enhance the Software and contribute
 * those enhancements back to the community. However, since we would
 * like to make the Software available for broadest use, with as few
 * restrictions as possible permission is hereby granted, free of
 * charge, to any person obtaining a copy of this Software to deal in
 * the Software under the copyrights without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * The name and trademarks of copyright holder(s) may NOT be used in
 * advertising or publicity pertaining to the Software or any
 * derivatives without specific, written prior permission.
 */

#include <config.h>
#include "switch-flow.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "buffer.h"
#include "openflow.h"
#include "packets.h"
#include "timeval.h"

/* Internal function used to compare fields in flow. */
static inline int
flow_fields_match(const struct flow *a, const struct flow *b, uint16_t w,
                  uint32_t src_mask, uint32_t dst_mask)
{
    return ((w & OFPFW_IN_PORT || a->in_port == b->in_port)
            && (w & OFPFW_DL_VLAN || a->dl_vlan == b->dl_vlan)
            && (w & OFPFW_DL_SRC || eth_addr_equals(a->dl_src, b->dl_src))
            && (w & OFPFW_DL_DST || eth_addr_equals(a->dl_dst, b->dl_dst))
            && (w & OFPFW_DL_TYPE || a->dl_type == b->dl_type)
            && !((a->nw_src ^ b->nw_src) & src_mask)
            && !((a->nw_dst ^ b->nw_dst) & dst_mask)
            && (w & OFPFW_NW_PROTO || a->nw_proto == b->nw_proto)
            && (w & OFPFW_TP_SRC || a->tp_src == b->tp_src)
            && (w & OFPFW_TP_DST || a->tp_dst == b->tp_dst));
}

static uint32_t make_nw_mask(int n_wild_bits)
{
    n_wild_bits &= (1u << OFPFW_NW_SRC_BITS) - 1;
    return n_wild_bits < 32 ? htonl(~((1u << n_wild_bits) - 1)) : 0;
}

/* Returns nonzero if 'a' and 'b' match, that is, if their fields are equal
 * modulo wildcards in 'b', zero otherwise. */
inline int
flow_matches_1wild(const struct sw_flow_key *a, const struct sw_flow_key *b)
{
    return flow_fields_match(&a->flow, &b->flow, b->wildcards,
                             b->nw_src_mask, b->nw_dst_mask);
}

/* Returns nonzero if 'a' and 'b' match, that is, if their fields are equal
 * modulo wildcards in 'a' or 'b', zero otherwise. */
inline int
flow_matches_2wild(const struct sw_flow_key *a, const struct sw_flow_key *b)
{
    return flow_fields_match(&a->flow, &b->flow, a->wildcards | b->wildcards,
                             a->nw_src_mask & b->nw_src_mask,
                             a->nw_dst_mask & b->nw_dst_mask);
}

/* Returns nonzero if 't' (the table entry's key) and 'd' (the key 
 * describing the deletion) match, that is, if their fields are 
 * equal modulo wildcards, zero otherwise.  If 'strict' is nonzero, the
 * wildcards must match in both 't_key' and 'd_key'.  Note that the
 * table's wildcards are ignored unless 'strict' is set. */
int
flow_del_matches(const struct sw_flow_key *t, const struct sw_flow_key *d, int strict)
{
    if (strict && d->wildcards != t->wildcards) {
        return 0;
    }
    return flow_matches_1wild(t, d);
}

void
flow_extract_match(struct sw_flow_key* to, const struct ofp_match* from)
{
    to->wildcards = ntohl(from->wildcards) & OFPFW_ALL;
    to->flow.reserved = 0;
    to->flow.in_port = from->in_port;
    to->flow.dl_vlan = from->dl_vlan;
    memcpy(to->flow.dl_src, from->dl_src, ETH_ADDR_LEN);
    memcpy(to->flow.dl_dst, from->dl_dst, ETH_ADDR_LEN);
    to->flow.dl_type = from->dl_type;

    to->flow.nw_src = to->flow.nw_dst = to->flow.nw_proto = 0;
    to->flow.tp_src = to->flow.tp_dst = 0;

#define OFPFW_TP (OFPFW_TP_SRC | OFPFW_TP_DST)
#define OFPFW_NW (OFPFW_NW_SRC_MASK | OFPFW_NW_DST_MASK | OFPFW_NW_PROTO)
    if (to->wildcards & OFPFW_DL_TYPE) {
        /* Can't sensibly match on network or transport headers if the
         * data link type is unknown. */
        to->wildcards |= OFPFW_NW | OFPFW_TP;
    } else if (from->dl_type == htons(ETH_TYPE_IP)) {
        to->flow.nw_src   = from->nw_src;
        to->flow.nw_dst   = from->nw_dst;
        to->flow.nw_proto = from->nw_proto;

        if (to->wildcards & OFPFW_NW_PROTO) {
            /* Can't sensibly match on transport headers if the network
             * protocol is unknown. */
            to->wildcards |= OFPFW_TP;
        } else if (from->nw_proto == IPPROTO_TCP 
                || from->nw_proto == IPPROTO_UDP) {
            to->flow.tp_src = from->tp_src;
            to->flow.tp_dst = from->tp_dst;
        } else {
            /* Transport layer fields are undefined.  Mark them as
             * exact-match to allow such flows to reside in table-hash,
             * instead of falling into table-linear. */
            to->wildcards &= ~OFPFW_TP;
        }
    } else {
        /* Network and transport layer fields are undefined.  Mark them
         * as exact-match to allow such flows to reside in table-hash,
         * instead of falling into table-linear. */
        to->wildcards &= ~(OFPFW_NW | OFPFW_TP);
    }

	/* We set these late because code above adjusts to->wildcards. */
	to->nw_src_mask = make_nw_mask(to->wildcards >> OFPFW_NW_SRC_SHIFT);
	to->nw_dst_mask = make_nw_mask(to->wildcards >> OFPFW_NW_DST_SHIFT);
}

void
flow_fill_match(struct ofp_match* to, const struct sw_flow_key* from)
{
    to->wildcards = htonl(from->wildcards);
    to->in_port   = from->flow.in_port;
    to->dl_vlan   = from->flow.dl_vlan;
    memcpy(to->dl_src, from->flow.dl_src, ETH_ADDR_LEN);
    memcpy(to->dl_dst, from->flow.dl_dst, ETH_ADDR_LEN);
    to->dl_type   = from->flow.dl_type;
    to->nw_src        = from->flow.nw_src;
    to->nw_dst        = from->flow.nw_dst;
    to->nw_proto  = from->flow.nw_proto;
    to->tp_src        = from->flow.tp_src;
    to->tp_dst        = from->flow.tp_dst;
    to->pad           = 0;
}

/* Allocates and returns a new flow with 'n_actions' action, using allocation
 * flags 'flags'.  Returns the new flow or a null pointer on failure. */
struct sw_flow *
flow_alloc(int n_actions)
{
    struct sw_flow_actions *sfa;
    struct sw_flow *flow = malloc(sizeof *flow);
    if (!flow)
        return NULL;

    sfa = malloc(n_actions * sizeof sfa->actions[0]);
    if (!sfa) {
        free(flow);
        return NULL;
    }
    sfa->n_actions = n_actions;
    flow->sf_acts = sfa;
    return flow;
}

/* Frees 'flow' immediately. */
void
flow_free(struct sw_flow *flow)
{
    if (!flow) {
        return; 
    }
    free(flow->sf_acts);
    free(flow);
}

/* Copies 'actions' into a newly allocated structure for use by 'flow'
 * and frees the structure that defined the previous actions. */
void flow_replace_acts(struct sw_flow *flow, const struct ofp_action *actions,
        int n_actions)
{
    struct sw_flow_actions *sfa;
    int size = sizeof *sfa + (n_actions * sizeof sfa->actions[0]);

    sfa = malloc(size);
    if (unlikely(!sfa))
        return;

    sfa->n_actions = n_actions;
    memcpy(sfa->actions, actions, n_actions * sizeof sfa->actions[0]);

    free(flow->sf_acts);
    flow->sf_acts = sfa;

    return;
}

/* Prints a representation of 'key' to the kernel log. */
void
print_flow(const struct sw_flow_key *key)
{
    const struct flow *f = &key->flow;
    printf("wild%08x port%04x:vlan%04x mac%02x:%02x:%02x:%02x:%02x:%02x"
           "->%02x:%02x:%02x:%02x:%02x:%02x "
           "proto%04x ip%u.%u.%u.%u->%u.%u.%u.%u port%d->%d\n",
           key->wildcards, ntohs(f->in_port), ntohs(f->dl_vlan),
           f->dl_src[0], f->dl_src[1], f->dl_src[2],
           f->dl_src[3], f->dl_src[4], f->dl_src[5],
           f->dl_dst[0], f->dl_dst[1], f->dl_dst[2],
           f->dl_dst[3], f->dl_dst[4], f->dl_dst[5],
           ntohs(f->dl_type),
           ((unsigned char *)&f->nw_src)[0],
           ((unsigned char *)&f->nw_src)[1],
           ((unsigned char *)&f->nw_src)[2],
           ((unsigned char *)&f->nw_src)[3],
           ((unsigned char *)&f->nw_dst)[0],
           ((unsigned char *)&f->nw_dst)[1],
           ((unsigned char *)&f->nw_dst)[2],
           ((unsigned char *)&f->nw_dst)[3],
           ntohs(f->tp_src), ntohs(f->tp_dst));
}

bool flow_timeout(struct sw_flow *flow)
{
    time_t now = time_now();
    if (flow->idle_timeout != OFP_FLOW_PERMANENT
        && now > flow->used + flow->idle_timeout) {
        flow->reason = OFPER_IDLE_TIMEOUT;
        return true;
    } else if (flow->hard_timeout != OFP_FLOW_PERMANENT
               && now > flow->created + flow->hard_timeout) {
        flow->reason = OFPER_HARD_TIMEOUT;
        return true;
    } else {
        return false;
    }
}

void flow_used(struct sw_flow *flow, struct buffer *buffer)
{
    flow->used = time_now();
    flow->packet_count++;
    flow->byte_count += buffer->size;
}
