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

#include "chain.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include "switch-flow.h"
#include "table.h"

#define THIS_MODULE VLM_chain
#include "vlog.h"

/* Set of tables chained together in sequence from cheap to expensive. */
#define CHAIN_MAX_TABLES 4
struct sw_chain {
    int n_tables;
    struct sw_table *tables[CHAIN_MAX_TABLES];
};

/* Attempts to append 'table' to the set of tables in 'chain'.  Returns 0 or
 * negative error.  If 'table' is null it is assumed that table creation failed
 * due to out-of-memory. */
static int add_table(struct sw_chain *chain, struct sw_table *table)
{
    if (table == NULL)
        return -ENOMEM;
    if (chain->n_tables >= CHAIN_MAX_TABLES) {
        VLOG_ERR("too many tables in chain\n");
        table->destroy(table);
        return -ENOBUFS;
    }
    chain->tables[chain->n_tables++] = table;
    return 0;
}

/* Creates and returns a new chain.  Returns NULL if the chain cannot be
 * created. */
struct sw_chain *chain_create(void)
{
    struct sw_chain *chain = calloc(1, sizeof *chain);
    if (chain == NULL)
        return NULL;

    if (add_table(chain, table_mac_create(TABLE_MAC_NUM_BUCKETS, 
                                          TABLE_MAC_MAX_FLOWS))
        || add_table(chain, table_hash2_create(0x1EDC6F41, TABLE_HASH_MAX_FLOWS,
                                               0x741B8CD7, TABLE_HASH_MAX_FLOWS))
        || add_table(chain, table_linear_create(TABLE_LINEAR_MAX_FLOWS))) {
        chain_destroy(chain);
        return NULL;
    }

    return chain;
}

/* Searches 'chain' for a flow matching 'key', which must not have any wildcard
 * fields.  Returns the flow if successful, otherwise a null pointer. */
struct sw_flow *
chain_lookup(struct sw_chain *chain, const struct sw_flow_key *key)
{
    int i;

    assert(!key->wildcards);
    for (i = 0; i < chain->n_tables; i++) {
        struct sw_table *t = chain->tables[i];
        struct sw_flow *flow = t->lookup(t, key);
        if (flow)
            return flow;
    }
    return NULL;
}

/* Inserts 'flow' into 'chain', replacing any duplicate flow.  Returns 0 if
 * successful or a negative error.
 *
 * If successful, 'flow' becomes owned by the chain, otherwise it is retained
 * by the caller. */
int
chain_insert(struct sw_chain *chain, struct sw_flow *flow)
{
    int i;

    for (i = 0; i < chain->n_tables; i++) {
        struct sw_table *t = chain->tables[i];
        if (t->insert(t, flow))
            return 0;
    }

    return -ENOBUFS;
}

/* Deletes from 'chain' any and all flows that match 'key'.  Returns the number
 * of flows that were deleted.
 *
 * Expensive in the general case as currently implemented, since it requires
 * iterating through the entire contents of each table for keys that contain
 * wildcards.  Relatively cheap for fully specified keys. */
int
chain_delete(struct sw_chain *chain, const struct sw_flow_key *key, int strict)
{
    int count = 0;
    int i;

    for (i = 0; i < chain->n_tables; i++) {
        struct sw_table *t = chain->tables[i];
        count += t->delete(t, key, strict);
    }

    return count;

}

/* Deletes timed-out flow entries from all the tables in 'chain' and appends
 * the deleted flows to 'deleted'.
 *
 * Expensive as currently implemented, since it iterates through the entire
 * contents of each table. */
void
chain_timeout(struct sw_chain *chain, struct list *deleted)
{
    int i;

    for (i = 0; i < chain->n_tables; i++) {
        struct sw_table *t = chain->tables[i];
        t->timeout(t, deleted);
    }
}

/* Destroys 'chain', which must not have any users. */
void
chain_destroy(struct sw_chain *chain)
{
    int i;

    for (i = 0; i < chain->n_tables; i++) {
        struct sw_table *t = chain->tables[i];
        t->destroy(t);
    }
    free(chain);
}

/* Prints statistics for each of the tables in 'chain'. */
void
chain_print_stats(struct sw_chain *chain)
{
    int i;

    printf("\n");
    for (i = 0; i < chain->n_tables; i++) {
        struct sw_table *t = chain->tables[i];
        struct sw_table_stats stats;
        t->stats(t, &stats);
        printf("%s: %lu/%lu flows\n",
               stats.name, stats.n_flows, stats.max_flows);
    }
}
