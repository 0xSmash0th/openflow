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

#ifndef VCONN_H
#define VCONN_H 1

#include <stdbool.h>
#include <stdint.h>

struct buffer;
struct flow;
struct pollfd;

/* Client interface. */

/* Virtual connection to an OpenFlow device. */
struct vconn {
    struct vconn_class *class;
};

/* What kind of operation do we want to perform? */
enum {
    WANT_ACCEPT = 1 << 0,          /* Want to accept a new connection. */
    WANT_RECV = 1 << 1,            /* Want to receive a message. */
    WANT_SEND = 1 << 2             /* Want to send a message. */
};

int vconn_open(const char *name, struct vconn **);
void vconn_close(struct vconn *);
bool vconn_is_passive(const struct vconn *);
bool vconn_prepoll(struct vconn *, int want, struct pollfd *);
void vconn_postpoll(struct vconn *, short int *revents);
int vconn_accept(struct vconn *, struct vconn **);
int vconn_recv(struct vconn *, struct buffer **);
int vconn_send(struct vconn *, struct buffer *);
int vconn_send_wait(struct vconn *, struct buffer *);

struct buffer *make_add_simple_flow(const struct flow *,
                                    uint32_t buffer_id, uint16_t out_port);
struct buffer *make_buffered_packet_out(uint32_t buffer_id,
                                        uint16_t in_port, uint16_t out_port);
struct buffer *make_unbuffered_packet_out(const struct buffer *packet,
                                          uint16_t in_port, uint16_t out_port);

/* Provider interface. */

struct vconn_class {
    /* Prefix for connection names, e.g. "nl", "tcp". */
    const char *name;

    /* Attempts to connect to an OpenFlow device.  'name' is the full
     * connection name provided by the user, e.g. "nl:0", "tcp:1.2.3.4".  This
     * name is useful for error messages but must not be modified.
     * 
     * 'suffix' is a copy of 'name' following the colon and may be modified.
     *
     * Returns 0 if successful, otherwise a positive errno value.  If
     * successful, stores a pointer to the new connection in '*vconnp'. */
    int (*open)(const char *name, char *suffix, struct vconn **vconnp);

    /* Closes 'vconn' and frees associated memory. */
    void (*close)(struct vconn *vconn);

    /* Called by the main loop before calling poll(), this function must
     * initialize 'pfd->fd' and 'pfd->events' appropriately so that poll() will
     * wake up when the connection becomes available for the operations
     * specified in 'want'.  The prepoll function may also set bits in 'pfd' to
     * allow for internal processing.
     *
     * Should return false normally.  May return true to indicate that no
     * blocking should happen in poll() because the connection is available for
     * some operation specified in 'want' but that status cannot be detected
     * via poll() and thus poll() could block forever otherwise. */
    bool (*prepoll)(struct vconn *, int want, struct pollfd *pfd);

    /* Called by the main loop after calling poll(), this function may perform
     * any internal processing needed by the connection.  It is provided with
     * the vconn file descriptor's status in '*revents', as reported by poll().
     *
     * The postpoll function should adjust '*revents' to reflect the status of
     * the connection from the caller's point of view: that is, upon return
     * '*revents & POLLIN' should indicate that a packet is (potentially) ready
     * to be read (for an active vconn) or a new connection is ready to be
     * accepted (for a passive vconn) and '*revents & POLLOUT' should indicate
     * that a packet is (potentially) ready to be written.
     *
     * This function may be a null pointer in a vconn class that has no use for
     * it, that is, if the vconn does not need to do any internal processing
     * and poll's revents out properly reflects the vconn's status.  */
    void (*postpoll)(struct vconn *, short int *revents);

    /* Tries to accept a new connection on 'vconn', which must be a passive
     * vconn.  If successful, stores the new connection in '*new_vconnp' and
     * returns 0.  Otherwise, returns a positive errno value.
     *
     * The accept function must not block waiting for a connection.  If no
     * connection is ready to be accepted, it should return EAGAIN.
     *
     * Nonnull iff this is a passive vconn (one that accepts connection and
     * does not transfer data). */
    int (*accept)(struct vconn *vconn, struct vconn **new_vconnp);

    /* Tries to receive an OpenFlow message from 'vconn', which must be an
     * active vconn.  If successful, stores the received message into '*msgp'
     * and returns 0.  The caller is responsible for destroying the message
     * with buffer_delete().  On failure, returns a positive errno value and
     * stores a null pointer into '*msgp'.
     *
     * If the connection has been closed in the normal fashion, returns EOF.
     *
     * The recv function must not block waiting for a packet to arrive.  If no
     * packets have been received, it should return EAGAIN.
     *
     * Nonnull iff this is an active vconn (one that transfers data and does
     * not accept connections). */
    int (*recv)(struct vconn *vconn, struct buffer **msgp);

    /* Tries to queue 'msg' for transmission on 'vconn', which must be an
     * active vconn.  If successful, returns 0, in which case ownership of
     * 'msg' is transferred to the vconn.  Success does not guarantee that
     * 'msg' has been or ever will be delivered to the peer, only that it has
     * been queued for transmission.
     *
     * Returns a positive errno value on failure, in which case the caller
     * retains ownership of 'msg'.
     *
     * The send function must not block.  If 'msg' cannot be immediately
     * accepted for transmission, it should return EAGAIN.
     *
     * Nonnull iff this is an active vconn (one that transfers data and does
     * not accept connections). */
    int (*send)(struct vconn *vconn, struct buffer *msg);
};

extern struct vconn_class tcp_vconn_class;
extern struct vconn_class ptcp_vconn_class;
#ifdef HAVE_OPENSSL
extern struct vconn_class ssl_vconn_class;
extern struct vconn_class pssl_vconn_class;
#endif
#ifdef HAVE_NETLINK
extern struct vconn_class netlink_vconn_class;
#endif

#endif /* vconn.h */
