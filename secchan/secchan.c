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

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "command-line.h"
#include "compiler.h"
#include "fault.h"
#include "util.h"
#include "vconn.h"
#include "vlog-socket.h"
#include "openflow.h"

#include "vlog.h"
#define THIS_MODULE VLM_secchan

static void parse_options(int argc, char *argv[]);
static void usage(void) NO_RETURN;

static bool reliable = true;

struct half {
    const char *name;
    struct vconn *vconn;
    struct pollfd *pollfd;
    struct buffer *rxbuf;
};

static void reconnect(struct half *);

int
main(int argc, char *argv[])
{
    struct half halves[2];
    struct pollfd pollfds[2 + 1];
    struct vlog_server *vlog_server;
    int retval;
    int i;

    set_program_name(argv[0]);
    register_fault_handlers();
    vlog_init();
    parse_options(argc, argv);

    if (argc - optind != 2) {
        fatal(0, "exactly two peer arguments required; use --help for usage");
    }

    retval = vlog_server_listen(NULL, &vlog_server);
    if (retval) {
        fatal(retval, "Could not listen for vlog connections");
    }

    for (i = 0; i < 2; i++) {
        halves[i].name = argv[optind + i];
        halves[i].vconn = NULL;
        halves[i].pollfd = &pollfds[i];
        halves[i].rxbuf = NULL;
        reconnect(&halves[i]);
    }
    for (;;) {
        /* Wait until there's something to do. */
        for (i = 0; i < 2; i++) {
            struct half *this = &halves[i];
            struct half *peer = &halves[!i];
            int want = 0;
            if (peer->rxbuf) {
                want |= WANT_SEND;
            }
            if (!this->rxbuf) {
                want |= WANT_RECV;
            }
            this->pollfd->fd = -1;
            this->pollfd->events = 0;
            vconn_prepoll(this->vconn, want, this->pollfd);
        }
        if (vlog_server) {
            pollfds[2].fd = vlog_server_get_fd(vlog_server);
            pollfds[2].events = POLLIN;
        }
        do {
            retval = poll(pollfds, 2 + (vlog_server != NULL), -1);
        } while (retval < 0 && errno == EINTR);
        if (retval <= 0) {
            fatal(retval < 0 ? errno : 0, "poll");
        }

        /* Let each connection deal with any pending operations. */
        for (i = 0; i < 2; i++) {
            struct half *this = &halves[i];
            vconn_postpoll(this->vconn, &this->pollfd->revents);
            if (this->pollfd->revents & POLLERR) {
                this->pollfd->revents |= POLLIN | POLLOUT;
            }
        }
        if (vlog_server && pollfds[2].revents) {
            vlog_server_poll(vlog_server);
        }

        /* Do as much work as we can without waiting. */
        for (i = 0; i < 2; i++) {
            struct half *this = &halves[i];
            struct half *peer = &halves[!i];

            if (this->pollfd->revents & POLLIN && !this->rxbuf) {
                retval = vconn_recv(this->vconn, &this->rxbuf);
                if (retval && retval != EAGAIN) {
                    VLOG_DBG("%s: recv: closing connection: %s",
                             this->name, strerror(retval));
                    reconnect(this);
                    break;
                }
            }

            if (peer->pollfd->revents & POLLOUT && this->rxbuf) {
                retval = vconn_send(peer->vconn, this->rxbuf);
                if (!retval) {
                    this->rxbuf = NULL;
                } else if (retval != EAGAIN) {
                    VLOG_DBG("%s: send: closing connection: %s",
                             peer->name, strerror(retval));
                    reconnect(peer); 
                    break;
                }
            } 
        }
    }

    return 0;
}

static void
reconnect(struct half *this) 
{
    int backoff;
    
    if (this->vconn != NULL) {
        if (!reliable) {
            fatal(0, "%s: connection dropped", this->name);
        }

        VLOG_WARN("%s: connection dropped, reconnecting", this->name);
        vconn_close(this->vconn);
        this->vconn = NULL;
        buffer_delete(this->rxbuf);
        this->rxbuf = NULL;
    }
    this->pollfd->revents = POLLIN | POLLOUT;

    for (backoff = 1; ; backoff = MIN(backoff * 2, 60)) {
        int retval = vconn_open(this->name, &this->vconn);
        if (!retval) {
            VLOG_WARN("%s: connected", this->name);
            if (vconn_is_passive(this->vconn)) {
                fatal(0, "%s: passive vconn not supported in control path",
                      this->name);
            }
            return;
        }

        if (!reliable) {
            fatal(0, "%s: connection failed", this->name);
        }
        VLOG_WARN("%s: connection failed (%s), reconnecting",
                  this->name, strerror(errno));
        sleep(backoff);
    }
}

static void
parse_options(int argc, char *argv[]) 
{
    static struct option long_options[] = {
        {"unreliable",  no_argument, 0, 'u'},
        {"verbose",     optional_argument, 0, 'v'},
        {"help",        no_argument, 0, 'h'},
        {"version",     no_argument, 0, 'V'},
        {0, 0, 0, 0},
    };
    char *short_options = long_options_to_short_options(long_options);
    
    for (;;) {
        int indexptr;
        int c;

        c = getopt_long(argc, argv, short_options, long_options, &indexptr);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'u':
            reliable = false;
            break;

        case 'h':
            usage();

        case 'V':
            printf("%s "VERSION" compiled "__DATE__" "__TIME__"\n", argv[0]);
            exit(EXIT_SUCCESS);

        case 'v':
            vlog_set_verbosity(optarg);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);
}

static void
usage(void)
{
    printf("%s: Secure Channel\n"
           "usage: %s [OPTIONS] nl:DP_ID tcp:HOST:[PORT]\n"
           "\nConnects to local datapath DP_ID via Netlink and \n"
           "controller on HOST via TCP to PORT (default: %d).\n"
           "\nNetworking options:\n"
           "  -u, --unreliable        do not reconnect after connections drop\n"
           "\nOther options:\n"
           "  -v, --verbose           set maximum verbosity level\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n",
           program_name, program_name, OFP_TCP_PORT);
    exit(EXIT_SUCCESS);
}
