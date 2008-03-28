/* Copyright (C) 2007 Board of Trustees, Leland Stanford Jr. University.
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

#include "vconn.h"
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include "buffer.h"
#include "flow.h"
#include "openflow.h"
#include "poll-loop.h"
#include "util.h"

#define THIS_MODULE VLM_vconn
#include "vlog.h"

static struct vconn_class *vconn_classes[] = {
    &tcp_vconn_class,
    &ptcp_vconn_class,
#ifdef HAVE_NETLINK
    &netlink_vconn_class,
#endif
#ifdef HAVE_OPENSSL
    &ssl_vconn_class,
    &pssl_vconn_class,
#endif
};

/* Check the validity of the vconn class structures. */
static void
check_vconn_classes(void)
{
#ifndef NDEBUG
    size_t i;

    for (i = 0; i < ARRAY_SIZE(vconn_classes); i++) {
        struct vconn_class *class = vconn_classes[i];
        assert(class->name != NULL);
        assert(class->open != NULL);
        assert(class->close != NULL);
        assert(class->accept
               ? !class->recv && !class->send
               :  class->recv && class->send);
        assert(class->wait != NULL);
    }
#endif
}

/* Attempts to connect to an OpenFlow device.  'name' is a connection name in
 * the form "TYPE:ARGS", where TYPE is the vconn class's name and ARGS are
 * vconn class-specific.
 *
 * Returns 0 if successful, otherwise a positive errno value.  If successful,
 * stores a pointer to the new connection in '*vconnp', otherwise a null
 * pointer.  */
int
vconn_open(const char *name, struct vconn **vconnp)
{
    size_t prefix_len;
    size_t i;

    check_vconn_classes();

    prefix_len = strcspn(name, ":");
    if (prefix_len == strlen(name)) {
        fatal(0, "`%s' not correct format for peer name", name);
    }
    for (i = 0; i < ARRAY_SIZE(vconn_classes); i++) {
        struct vconn_class *class = vconn_classes[i];
        if (strlen(class->name) == prefix_len
            && !memcmp(class->name, name, prefix_len)) {
            char *suffix_copy = xstrdup(name + prefix_len + 1);
            int retval = class->open(name, suffix_copy, vconnp);
            free(suffix_copy);
            if (retval) {
                *vconnp = NULL;
            } else {
                assert((*vconnp)->connect_status != EAGAIN
                       || (*vconnp)->class->connect);
            }
            return retval;
        }
    }
    fatal(0, "unknown peer type `%.*s'", (int) prefix_len, name);
    abort();
}

int
vconn_open_block(const char *name, struct vconn **vconnp)
{
    struct vconn *vconn;
    int error;

    error = vconn_open(name, &vconn);
    while (error == EAGAIN) {
        vconn_connect_wait(vconn);
        poll_block();
        error = vconn_connect(vconn);
        assert(error != EINPROGRESS);
    }
    if (error) {
        vconn_close(vconn);
        *vconnp = NULL;
    } else {
        *vconnp = vconn;
    }
    return error;
}

/* Closes 'vconn'. */
void
vconn_close(struct vconn *vconn)
{
    if (vconn != NULL) {
        (vconn->class->close)(vconn);
    }
}

/* Returns true if 'vconn' is a passive vconn, that is, its purpose is to
 * wait for connections to arrive, not to transfer data.  Returns false if
 * 'vconn' is an active vconn, that is, its purpose is to transfer data, not
 * to wait for new connections to arrive. */
bool
vconn_is_passive(const struct vconn *vconn)
{
    return vconn->class->accept != NULL;
}

/* Tries to complete the connection on 'vconn', which must be an active
 * vconn.  If 'vconn''s connection is complete, returns 0 if the connection
 * was successful or a positive errno value if it failed.  If the
 * connection is still in progress, returns EAGAIN. */
int
vconn_connect(struct vconn *vconn)
{
    if (vconn->connect_status == EAGAIN) {
        vconn->connect_status = (vconn->class->connect)(vconn);
        assert(vconn->connect_status != EINPROGRESS);
    }
    return vconn->connect_status;
}

/* Tries to accept a new connection on 'vconn', which must be a passive vconn.
 * If successful, stores the new connection in '*new_vconn' and returns 0.
 * Otherwise, returns a positive errno value.
 *
 * vconn_accept will not block waiting for a connection.  If no connection is
 * ready to be accepted, it returns EAGAIN immediately. */
int
vconn_accept(struct vconn *vconn, struct vconn **new_vconn)
{
    int retval;

    retval = (vconn->class->accept)(vconn, new_vconn);

    if (retval) {
        *new_vconn = NULL;
    } else {
        assert((*new_vconn)->connect_status != EAGAIN
               || (*new_vconn)->class->connect);
    }
    return retval;
}

/* Tries to receive an OpenFlow message from 'vconn', which must be an active
 * vconn.  If successful, stores the received message into '*msgp' and returns
 * 0.  The caller is responsible for destroying the message with
 * buffer_delete().  On failure, returns a positive errno value and stores a
 * null pointer into '*msgp'.  On normal connection close, returns EOF.
 *
 * vconn_recv will not block waiting for a packet to arrive.  If no packets
 * have been received, it returns EAGAIN immediately. */
int
vconn_recv(struct vconn *vconn, struct buffer **msgp)
{
    int retval = vconn_connect(vconn);
    if (!retval) {
        retval = (vconn->class->recv)(vconn, msgp);
    }
    if (retval) {
        *msgp = NULL;
    }
    return retval;
}

/* Tries to queue 'msg' for transmission on 'vconn', which must be an active
 * vconn.  If successful, returns 0, in which case ownership of 'msg' is
 * transferred to the vconn.  Success does not guarantee that 'msg' has been or
 * ever will be delivered to the peer, only that it has been queued for
 * transmission.
 *
 * Returns a positive errno value on failure, in which case the caller
 * retains ownership of 'msg'.
 *
 * vconn_send will not block.  If 'msg' cannot be immediately accepted for
 * transmission, it returns EAGAIN immediately. */
int
vconn_send(struct vconn *vconn, struct buffer *msg)
{
    int retval = vconn_connect(vconn);
    if (!retval) {
        retval = (vconn->class->send)(vconn, msg);
    }
    return retval;
}

/* Same as vconn_send, except that it waits until 'msg' can be transmitted. */
int
vconn_send_block(struct vconn *vconn, struct buffer *msg)
{
    int retval;
    while ((retval = vconn_send(vconn, msg)) == EAGAIN) {
        vconn_send_wait(vconn);
        poll_block();
    }
    return retval;
}

void
vconn_wait(struct vconn *vconn, enum vconn_wait_type wait)
{
    int connect_status;

    assert(vconn_is_passive(vconn)
           ? wait == WAIT_ACCEPT || wait == WAIT_CONNECT
           : wait == WAIT_CONNECT || wait == WAIT_RECV || wait == WAIT_SEND);

    connect_status = vconn_connect(vconn);
    if (connect_status) {
        if (connect_status == EAGAIN) {
            wait = WAIT_CONNECT;
        } else {
            poll_immediate_wake();
            return;
        }
    }

    (vconn->class->wait)(vconn, wait);
}

void
vconn_connect_wait(struct vconn *vconn)
{
    vconn_wait(vconn, WAIT_CONNECT);
}

void
vconn_accept_wait(struct vconn *vconn)
{
    vconn_wait(vconn, WAIT_ACCEPT);
}

void
vconn_recv_wait(struct vconn *vconn)
{
    vconn_wait(vconn, WAIT_RECV);
}

void
vconn_send_wait(struct vconn *vconn)
{
    vconn_wait(vconn, WAIT_SEND);
}

struct buffer *
make_add_simple_flow(const struct flow *flow,
                     uint32_t buffer_id, uint16_t out_port)
{
    struct ofp_flow_mod *ofm;
    size_t size = sizeof *ofm + sizeof ofm->actions[0];
    struct buffer *out = buffer_new(size);
    ofm = buffer_put_uninit(out, size);
    memset(ofm, 0, size);
    ofm->header.version = OFP_VERSION;
    ofm->header.type = OFPT_FLOW_MOD;
    ofm->header.length = htons(size);
    ofm->match.wildcards = htons(0);
    ofm->match.in_port = flow->in_port;
    memcpy(ofm->match.dl_src, flow->dl_src, sizeof ofm->match.dl_src);
    memcpy(ofm->match.dl_dst, flow->dl_dst, sizeof ofm->match.dl_dst);
    ofm->match.dl_vlan = flow->dl_vlan;
    ofm->match.dl_type = flow->dl_type;
    ofm->match.nw_src = flow->nw_src;
    ofm->match.nw_dst = flow->nw_dst;
    ofm->match.nw_proto = flow->nw_proto;
    ofm->match.tp_src = flow->tp_src;
    ofm->match.tp_dst = flow->tp_dst;
    ofm->command = htons(OFPFC_ADD);
    ofm->max_idle = htons(60);
    ofm->buffer_id = htonl(buffer_id);
    ofm->group_id = htonl(0);
    ofm->actions[0].type = htons(OFPAT_OUTPUT);
    ofm->actions[0].arg.output.max_len = htons(0);
    ofm->actions[0].arg.output.port = htons(out_port);
    return out;
}

struct buffer *
make_unbuffered_packet_out(const struct buffer *packet,
                           uint16_t in_port, uint16_t out_port)
{
    struct ofp_packet_out *opo;
    size_t size = sizeof *opo + packet->size;
    struct buffer *out = buffer_new(size);
    opo = buffer_put_uninit(out, size);
    memset(opo, 0, sizeof *opo);
    opo->header.version = OFP_VERSION;
    opo->header.type = OFPT_PACKET_OUT;
    opo->header.length = htons(size);
    opo->buffer_id = htonl(UINT32_MAX);
    opo->in_port = htons(in_port);
    opo->out_port = htons(out_port);
    memcpy(opo->u.data, packet->data, packet->size);
    return out;
}

struct buffer *
make_buffered_packet_out(uint32_t buffer_id,
                         uint16_t in_port, uint16_t out_port)
{
    struct ofp_packet_out *opo;
    size_t size = sizeof *opo + sizeof opo->u.actions[0];
    struct buffer *out = buffer_new(size);
    opo = buffer_put_uninit(out, size);
    memset(opo, 0, size);
    opo->header.version = OFP_VERSION;
    opo->header.type = OFPT_PACKET_OUT;
    opo->header.length = htons(size);
    opo->buffer_id = htonl(buffer_id);
    opo->in_port = htons(in_port);
    opo->out_port = htons(out_port);
    opo->u.actions[0].type = htons(OFPAT_OUTPUT);
    opo->u.actions[0].arg.output.max_len = htons(0);
    opo->u.actions[0].arg.output.port = htons(out_port);
    return out;
}

