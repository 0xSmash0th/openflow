/*-
 * Copyright (c) 2008, 2009
 *      The Board of Trustees of The Leland Stanford Junior University
 *
 * We are making the OpenFlow specification and associated documentation
 * (Software) available for public use and benefit with the expectation that
 * others will use, modify and enhance the Software and contribute those
 * enhancements back to the community. However, since we would like to make the
 * Software available for broadest use, with as few restrictions as possible
 * permission is hereby granted, free of charge, to any person obtaining a copy
 * of this Software to deal in the Software under the copyrights without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * The name and trademarks of copyright holder(s) may NOT be used in
 * advertising or publicity pertaining to the Software or any derivatives
 * without specific, written prior permission.
 */

#ifndef HATABLE_NF2_NF2_OPENFLOW_H_
#define HATABLE_NF2_NF2_OPENFLOW_H_

#define OPENFLOW_NF2_EXACT_TABLE_SIZE	32768

#pragma pack(push)		/* push current alignment to stack */
#pragma pack(1)			/* set alignment to 1 byte boundary */

#define NF2_OF_ENTRY_WORD_LEN	8
struct nf2_of_entry {
	uint16_t transp_dst;
	uint16_t transp_src;
	uint8_t ip_proto;
	uint32_t ip_dst;
	uint32_t ip_src;
	uint16_t eth_type;
	uint8_t eth_dst[6];
	uint8_t eth_src[6];
	uint8_t src_port;
	uint16_t vlan_id;
	uint16_t pad;
};

typedef union nf2_of_entry_wrap {
	struct nf2_of_entry entry;
	uint32_t raw[NF2_OF_ENTRY_WORD_LEN];
} nf2_of_entry_wrap;

typedef nf2_of_entry_wrap nf2_of_mask_wrap;
#define NF2_OF_MASK_WORD_LEN	8

struct nf2_of_action {
	uint16_t forward_bitmask;
	uint16_t nf2_action_flag;
	uint16_t vlan_id;
	uint8_t vlan_pcp;
	uint8_t eth_src[6];
	uint8_t eth_dst[6];
	uint32_t ip_src;
	uint32_t ip_dst;
	uint16_t transp_src;
	uint16_t transp_dst;
	uint8_t reserved[19];
};

#define NF2_OF_ACTION_WORD_LEN	10
typedef union nf2_of_action_wrap {
	struct nf2_of_action action;
	uint32_t raw[10];
} nf2_of_action_wrap;

struct nf2_of_exact_counters {
	uint32_t pkt_count:25;
	uint8_t last_seen:7;
	uint32_t byte_count;
};

#define NF2_OF_EXACT_COUNTERS_WORD_LEN	2
typedef union nf2_of_exact_counters_wrap {
	struct nf2_of_exact_counters counters;
	uint32_t raw[NF2_OF_EXACT_COUNTERS_WORD_LEN];
} nf2_of_exact_counters_wrap;

#pragma pack(pop)		/* XXX: Restore original alignment from stack */

void nf2_reset_card(struct net_device *);
int nf2_write_of_wildcard(struct net_device *, int, nf2_of_entry_wrap *,
			  nf2_of_mask_wrap *, nf2_of_action_wrap *);
int nf2_write_of_exact(struct net_device *, int, nf2_of_entry_wrap *,
		       nf2_of_action_wrap *);
int nf2_modify_write_of_wildcard(struct net_device *, int, nf2_of_entry_wrap *,
				 nf2_of_mask_wrap *, nf2_of_action_wrap *);
int nf2_modify_write_of_exact(struct net_device *, int, nf2_of_action_wrap *);
unsigned int nf2_get_exact_packet_count(struct net_device *, int);
unsigned int nf2_get_exact_byte_count(struct net_device *, int);
unsigned int nf2_get_wildcard_packet_count(struct net_device *, int);
unsigned int nf2_get_wildcard_byte_count(struct net_device *, int);
unsigned long int nf2_get_matched_count(struct net_device *);
unsigned long int nf2_get_missed_count(struct net_device *);

#endif
