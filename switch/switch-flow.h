/* Copyright (C) 2008 Board of Trustees, Leland Stanford Jr. University.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef SWITCH_FLOW_H
#define SWITCH_FLOW_H 1

#include <time.h>
#include "flow.h"
#include "list.h"

struct ofp_match;

/* Identification data for a flow. */
struct sw_flow_key {
    struct flow flow;           /* Flow data (in network byte order). */
    uint32_t wildcards;         /* Wildcard fields (in host byte order). */
};

/* Maximum number of actions in a single flow entry. */
#define MAX_ACTIONS 16

struct sw_flow {
    struct sw_flow_key key;

    uint32_t group_id;          /* Flow group ID (for QoS). */
    uint16_t max_idle;          /* Idle time before discarding (seconds). */
    time_t created;             /* When the flow was created. */
    time_t timeout;             /* When the flow expires (if idle). */
    uint64_t packet_count;      /* Number of packets seen. */
    uint64_t byte_count;        /* Number of bytes seen. */
    struct list node;

    /* Actions (XXX probably most flows have only a single action). */
    unsigned int n_actions;
    struct ofp_action *actions;
};

int flow_matches(const struct sw_flow_key *, const struct sw_flow_key *);
int flow_del_matches(const struct sw_flow_key *, const struct sw_flow_key *, 
                     int);
struct sw_flow *flow_alloc(int n_actions);
void flow_free(struct sw_flow *);
void flow_deferred_free(struct sw_flow *);
void flow_extract_match(struct sw_flow_key* to, const struct ofp_match* from);
void flow_fill_match(struct ofp_match* to, const struct sw_flow_key* from);

void print_flow(const struct sw_flow_key *);
int flow_timeout(struct sw_flow *flow);
void flow_used(struct sw_flow *flow, struct buffer *buffer);

#endif /* switch-flow.h */
