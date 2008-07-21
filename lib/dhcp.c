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
#include "dhcp.h"
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include "buffer.h"
#include "dynamic-string.h"

#define THIS_MODULE VLM_dhcp
#include "vlog.h"

/* Information about a DHCP argument type. */
struct arg_type {
    const char *name;           /* Name. */
    size_t size;                /* Number of bytes per argument. */
};

static struct arg_type types[] = {
#define DHCP_ARG(NAME, SIZE) [DHCP_ARG_##NAME] = {#NAME, SIZE},
    DHCP_ARGS
#undef DHCP_ARG
};

/* Information about a DHCP option. */
struct option_class {
    const char *name;           /* Name. */
    enum dhcp_arg_type type;    /* Argument type. */
    size_t min_args;            /* Minimum number of arguments. */
    size_t max_args;            /* Maximum number of arguments. */
};

static struct option_class classes[DHCP_N_OPTIONS] = {
    [0 ... 255] = {NULL, DHCP_ARG_UINT8, 0, SIZE_MAX},
#define DHCP_OPT(NAME, CODE, TYPE, MIN, MAX) \
    [CODE] = {#NAME, DHCP_ARG_##TYPE, MIN, MAX},
    DHCP_OPTS
#undef DHCP_OPT
};

static void copy_data(struct dhcp_msg *);

const char *
dhcp_type_name(enum dhcp_msg_type type)
{
    switch (type) {
#define DHCP_MSG(NAME, VALUE) case NAME: return #NAME;
        DHCP_MSGS
#undef DHCP_MSG
    }
    return "<<unknown DHCP message type>>";
}

/* Initializes 'msg' as a DHCP message.  The message should be freed with
 * dhcp_msg_uninit() when it is no longer needed. */
void
dhcp_msg_init(struct dhcp_msg *msg)
{
    memset(msg, 0, sizeof *msg);
}

/* Frees the contents of 'msg'.  The caller is responsible for freeing 'msg',
 * if necessary. */
void
dhcp_msg_uninit(struct dhcp_msg *msg)
{
    if (msg) {
        free(msg->data);
    }
}

/* Initializes 'dst' as a copy of 'src'.  'dst' (and 'src') should be freed
 * with dhcp_msg_uninit() when it is no longer needed. */
void
dhcp_msg_copy(struct dhcp_msg *dst, const struct dhcp_msg *src)
{
    *dst = *src;
    dst->data_allocated = src->data_used;
    dst->data_used = 0;
    dst->data = xmalloc(dst->data_allocated);
    copy_data(dst);
}

static void
prealloc_data(struct dhcp_msg *msg, size_t n)
{
    size_t needed = msg->data_used + n;
    if (needed > msg->data_allocated) {
        uint8_t *old_data = msg->data;
        msg->data_allocated = MAX(needed * 2, 64);
        msg->data = xmalloc(msg->data_allocated);
        if (old_data) {
            copy_data(msg);
            free(old_data);
        }
    }
}

static void *
append_data(struct dhcp_msg *msg, const void *data, size_t n)
{
    uint8_t *p = &msg->data[msg->data_used];
    memcpy(p, data, n);
    msg->data_used += n;
    return p;
}

static void
copy_data(struct dhcp_msg *msg)
{
    int code;

    msg->data_used = 0;
    for (code = 0; code < DHCP_N_OPTIONS; code++) {
        struct dhcp_option *opt = &msg->options[code];
        if (opt->data) {
            assert(msg->data_used + opt->n <= msg->data_allocated);
            opt->data = append_data(msg, opt->data, opt->n);
        }
    }
}

/* Appends the 'n' bytes in 'data' to the DHCP option in 'msg' represented by
 * 'code' (which must be in the range 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put(struct dhcp_msg *msg, int code,
             const void *data, size_t n)
{
    struct dhcp_option *opt;
    if (code == DHCP_CODE_PAD || code == DHCP_CODE_END) {
        return;
    }

    opt = &msg->options[code];
    prealloc_data(msg, n + opt->n);
    if (opt->n) {
        if (&msg->data[msg->data_used - opt->n] != opt->data) {
            opt->data = append_data(msg, opt->data, opt->n);
        }
        append_data(msg, data, n);
    } else {
        opt->data = append_data(msg, data, n);
    }
    opt->n += n;
}

/* Appends the boolean value 'b', as a octet with value 0 (false) or 1 (true),
 * to the DHCP option in 'msg' represented by 'code' (which must be in the
 * range 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_bool(struct dhcp_msg *msg, int code, bool b_)
{
    char b = !!b_;
    dhcp_msg_put(msg, code, &b, 1);
}

/* Appends the number of seconds 'secs', as a 32-bit number in network byte
 * order, to the DHCP option in 'msg' represented by 'code' (which must be in
 * the range 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_secs(struct dhcp_msg *msg, int code, uint32_t secs_)
{
    uint32_t secs = htonl(secs_);
    dhcp_msg_put(msg, code, &secs, sizeof secs);
}

/* Appends the IP address 'ip', as a 32-bit number in network byte order, to
 * the DHCP option in 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_ip(struct dhcp_msg *msg, int code, uint32_t ip)
{
    dhcp_msg_put(msg, code, &ip, sizeof ip);
}

/* Appends the ASCII string 'string', to the DHCP option in 'msg' represented
 * by 'code' (which must be in the range 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_string(struct dhcp_msg *msg, int code, const char *string)
{
    dhcp_msg_put(msg, code, string, strlen(string));
}

/* Appends octet 'x' to DHCP option in 'msg' represented by 'code' (which must
 * be in the range 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_uint8(struct dhcp_msg *msg, int code, uint8_t x)
{
    dhcp_msg_put(msg, code, &x, sizeof x);
}

/* Appends the 'n' octets in 'data' to DHCP option in 'msg' represented by
 * 'code' (which must be in the range 0...DHCP_N_OPTIONS). */
void dhcp_msg_put_uint8_array(struct dhcp_msg *msg, int code,
                              const uint8_t data[], size_t n)
{
    dhcp_msg_put(msg, code, data, n);
}

/* Appends the 16-bit value in 'x', in network byte order, to DHCP option in
 * 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_uint16(struct dhcp_msg *msg, int code, uint16_t x_)
{
    uint16_t x = htons(x_);
    dhcp_msg_put(msg, code, &x, sizeof x);
}


/* Appends the 'n' 16-bit values in 'data', in network byte order, to DHCP
 * option in 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS). */
void
dhcp_msg_put_uint16_array(struct dhcp_msg *msg, int code,
                          const uint16_t data[], size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        dhcp_msg_put_uint16(msg, code, data[i]);
    }
}

/* Returns a pointer to the 'size' bytes starting at byte offset 'offset' in
 * the DHCP option in 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS).  If the option has fewer than 'offset + size' bytes,
 * returns a null pointer. */
const void *
dhcp_msg_get(const struct dhcp_msg *msg, int code,
             size_t offset, size_t size)
{
    const struct dhcp_option *opt = &msg->options[code];
    return offset + size <= opt->n ? (const char *) opt->data + offset : NULL;
}

/* Stores in '*out' the boolean value at byte offset 'offset' in the DHCP
 * option in 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS).  Returns true if successful, false if the option has
 * fewer than 'offset + 1' bytes. */
bool
dhcp_msg_get_bool(const struct dhcp_msg *msg, int code, size_t offset,
                  bool *out)
{
    const uint8_t *uint8 = dhcp_msg_get(msg, code, offset, sizeof *uint8);
    if (uint8) {
        *out = *uint8 != 0;
        return true;
    } else {
        return false;
    }
}

/* Stores in '*out' the 32-bit count of seconds at offset 'offset' (in
 * 4-byte increments) in the DHCP option in 'msg' represented by 'code'
 * (which must be in the range 0...DHCP_N_OPTIONS).  The value is converted to
 * native byte order.  Returns true if successful, false if the option has
 * fewer than '4 * (offset + 1)' bytes. */
bool
dhcp_msg_get_secs(const struct dhcp_msg *msg, int code, size_t offset,
                  uint32_t *out)
{
    const uint32_t *uint32 = dhcp_msg_get(msg, code, offset * sizeof *uint32,
                                          sizeof *uint32);
    if (uint32) {
        *out = ntohl(*uint32);
        return true;
    } else {
        return false;
    }
}

/* Stores in '*out' the IP address at offset 'offset' (in 4-byte increments) in
 * the DHCP option in 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS).  The IP address is stored in network byte order.
 * Returns true if successful, false if the option has fewer than '4 * (offset
 * + 1)' bytes. */
bool
dhcp_msg_get_ip(const struct dhcp_msg *msg, int code,
                size_t offset, uint32_t *out)
{
    const uint32_t *uint32 = dhcp_msg_get(msg, code, offset * sizeof *uint32,
                                          sizeof *uint32);
    if (uint32) {
        *out = *uint32;
        return true;
    } else {
        return false;
    }
}

/* Returns the string in the DHCP option in 'msg' represented by 'code' (which
 * must be in the range 0...DHCP_N_OPTIONS).  The caller is responsible for
 * freeing the string with free().
 *
 * If 'msg' has no option represented by 'code', returns a null pointer.  (If
 * the option was specified but had no content, then an empty string is
 * returned, not a null pointer.) */
char *
dhcp_msg_get_string(const struct dhcp_msg *msg, int code)
{
    const struct dhcp_option *opt = &msg->options[code];
    return opt->data ? xmemdup0(opt->data, opt->n) : NULL;
}

/* Stores in '*out' the octet at byte offset 'offset' in the DHCP option in
 * 'msg' represented by 'code' (which must be in the range 0...DHCP_N_OPTIONS).
 * Returns true if successful, false if the option has fewer than 'offset + 1'
 * bytes. */
bool
dhcp_msg_get_uint8(const struct dhcp_msg *msg, int code,
                   size_t offset, uint8_t *out)
{
    const uint8_t *uint8 = dhcp_msg_get(msg, code, offset, sizeof *uint8);
    if (uint8) {
        *out = *uint8;
        return true;
    } else {
        return false;
    }
}

/* Stores in '*out' the 16-bit value at offset 'offset' (in 2-byte units) in
 * the DHCP option in 'msg' represented by 'code' (which must be in the range
 * 0...DHCP_N_OPTIONS).  The value is converted to native byte order.  Returns
 * true if successful, false if the option has fewer than '2 * (offset + 1)'
 * bytes. */
bool
dhcp_msg_get_uint16(const struct dhcp_msg *msg, int code,
                    size_t offset, uint16_t *out)
{
    const uint16_t *uint16 = dhcp_msg_get(msg, code, offset * sizeof *uint16,
                                          sizeof *uint16);
    if (uint16) {
        *out = ntohs(*uint16);
        return true;
    } else {
        return false;
    }
}

/* Appends a string representation of 'opt', which has the given 'code', to
 * 'ds'. */
const char *
dhcp_option_to_string(const struct dhcp_option *opt, int code, struct ds *ds)
{
    struct option_class *class = &classes[code];
    const struct arg_type *type = &types[class->type];
    size_t offset;

    ds_put_char(ds, ' ');
    if (class->name) {
        const char *cp;
        for (cp = class->name; *cp; cp++) {
            unsigned char c = *cp;
            ds_put_char(ds, c == '_' ? '-' : tolower(c));
        }
    } else {
        ds_put_format(ds, "option-%d", code);
    }
    ds_put_char(ds, '=');

    if (class->type == DHCP_ARG_STRING) {
        ds_put_char(ds, '"');
    }
    for (offset = 0; offset + type->size <= opt->n; offset += type->size) {
        const void *p = (const char *) opt->data + offset;
        const uint8_t *uint8 = p;
        const uint32_t *uint32 = p;
        const uint16_t *uint16 = p;
        const char *cp = p;
        unsigned char c;
        unsigned int secs;

        if (offset && class->type != DHCP_ARG_STRING) {
            ds_put_cstr(ds, class->type == DHCP_ARG_UINT8 ? ":" : ", ");
        }
        switch (class->type) {
        case DHCP_ARG_FIXED:
            NOT_REACHED();
        case DHCP_ARG_IP:
            ds_put_format(ds, IP_FMT, IP_ARGS(uint32));
            break;
        case DHCP_ARG_UINT8:
            ds_put_format(ds, "%02"PRIx8, *uint8);
            break;
        case DHCP_ARG_UINT16:
            ds_put_format(ds, "%"PRIu16, ntohs(*uint16));
            break;
        case DHCP_ARG_UINT32:
            ds_put_format(ds, "%"PRIu32, ntohl(*uint32));
            break;
        case DHCP_ARG_SECS:
            secs = ntohl(*uint32);
            if (secs >= 86400) {
                ds_put_format(ds, "%ud", secs / 86400);
                secs %= 86400;
            }
            if (secs >= 3600) {
                ds_put_format(ds, "%uh", secs / 3600);
                secs %= 3600;
            }
            if (secs >= 60) {
                ds_put_format(ds, "%umin", secs / 60);
                secs %= 60;
            }
            if (secs > 0 || *uint32 == 0) {
                ds_put_format(ds, "%us", secs);
            }
            break;
        case DHCP_ARG_STRING:
            c = *cp;
            if (isprint(c) && (!isspace(c) || c == ' ') && c != '\\') {
                ds_put_char(ds, *cp);
            } else {
                ds_put_format(ds, "\\%03o", (int) c);
            }
            break;
        case DHCP_ARG_BOOLEAN:
            if (*uint8 == 0) {
                ds_put_cstr(ds, "false");
            } else if (*uint8 == 1) {
                ds_put_cstr(ds, "true");
            } else {
                ds_put_format(ds, "**%"PRIu8"**", *uint8);
            }
            break;
        }
    }
    if (class->type == DHCP_ARG_STRING) {
        ds_put_char(ds, '"');
    }
    if (offset != opt->n) {
        if (offset) {
            ds_put_cstr(ds, ", ");
        }
        ds_put_cstr(ds, "**leftovers:");
        for (; offset < opt->n; offset++) {
            const void *p = (const char *) opt->data + offset;
            const uint8_t *uint8 = p;
            ds_put_format(ds, " %"PRIu8, *uint8);
        }
        ds_put_cstr(ds, "**");
    }
    return ds_cstr(ds);
}

/* Replaces 'ds' by a string representation of 'msg'. */
const char *
dhcp_msg_to_string(const struct dhcp_msg *msg, struct ds *ds)
{
    int code;

    ds_clear(ds);
    ds_put_format(ds, "%s %s xid=%08"PRIx32" secs=%"PRIu16,
                  (msg->op == DHCP_BOOTREQUEST ? "BOOTREQUEST"
                   : msg->op == DHCP_BOOTREPLY ? "BOOTREPLY"
                   : "<<bad DHCP op>>"),
                  dhcp_type_name(msg->type), msg->xid, msg->secs);
    if (msg->flags) {
        ds_put_cstr(ds, " flags=");
        if (msg->flags & DHCP_FLAGS_BROADCAST) {
            ds_put_cstr(ds, "[BROADCAST]");
        }
        if (msg->flags & DHCP_FLAGS_MBZ) {
            ds_put_format(ds, "[0x%04"PRIx16"]", msg->flags & DHCP_FLAGS_MBZ);
        }
    }
    if (msg->ciaddr) {
        ds_put_format(ds, " ciaddr="IP_FMT, IP_ARGS(&msg->ciaddr));
    }
    if (msg->yiaddr) {
        ds_put_format(ds, " yiaddr="IP_FMT, IP_ARGS(&msg->yiaddr));
    }
    if (msg->siaddr) {
        ds_put_format(ds, " siaddr="IP_FMT, IP_ARGS(&msg->siaddr));
    }
    if (msg->giaddr) {
        ds_put_format(ds, " giaddr="IP_FMT, IP_ARGS(&msg->giaddr));
    }
    ds_put_format(ds, " chaddr="ETH_ADDR_FMT, ETH_ADDR_ARGS(msg->chaddr));

    for (code = 0; code < DHCP_N_OPTIONS; code++) {
        const struct dhcp_option *opt = &msg->options[code];
        if (opt->data) {
            dhcp_option_to_string(opt, code, ds);
        }
    }
    return ds_cstr(ds);
}

static void
parse_options(struct dhcp_msg *msg, const char *name, void *data, size_t size,
              int option_offset)
{
    struct buffer b;

    b.data = data;
    b.size = size;
    for (;;) {
        uint8_t *code, *len;
        void *payload;

        code = buffer_try_pull(&b, 1);
        if (!code || *code == DHCP_CODE_END) {
            break;
        } else if (*code == DHCP_CODE_PAD) {
            continue;
        }

        len = buffer_try_pull(&b, 1);
        if (!len) {
            VLOG_DBG("reached end of %s expecting length byte", name);
            break;
        }

        payload = buffer_try_pull(&b, *len);
        if (!payload) {
            VLOG_DBG("expected %"PRIu8" bytes of option-%"PRIu8" payload "
                     "with only %zu bytes of %s left",
                     *len, *code, b.size, name);
            break;
        }
        dhcp_msg_put(msg, *code + option_offset, payload, *len);
    }
}

static void
validate_options(struct dhcp_msg *msg)
{
    int code;

    for (code = 0; code < DHCP_N_OPTIONS; code++) {
        struct dhcp_option *opt = &msg->options[code];
        struct option_class *class = &classes[code];
        struct arg_type *type = &types[class->type];
        if (opt->data) {
            size_t n_elems = opt->n / type->size;
            size_t remainder = opt->n % type->size;
            bool ok = true;
            if (remainder) {
                VLOG_DBG("%s option has %zu %zu-byte %s arguments with "
                         "%zu bytes left over",
                         class->name, n_elems, type->size,
                         type->name, remainder);
                ok = false;
            }
            if (n_elems < class->min_args || n_elems > class->max_args) {
                VLOG_DBG("%s option has %zu %zu-byte %s arguments but "
                         "between %zu and %zu are required",
                         class->name, n_elems, type->size, type->name,
                         class->min_args, class->max_args);
                ok = false;
            }
            if (!ok) {
                struct ds ds = DS_EMPTY_INITIALIZER;
                VLOG_DBG("%s option contains: %s",
                         class->name, dhcp_option_to_string(opt, code, &ds));
                ds_destroy(&ds);

                opt->n = 0;
                opt->data = NULL;
            }
        }
    }
}

/* Attempts to parse 'b' as a DHCP message.  If successful, initializes '*msg'
 * to the parsed message and returns 0.  Otherwise, returns a positive errno
 * value and '*msg' is indeterminate. */
int
dhcp_parse(struct dhcp_msg *msg, const struct buffer *b_)
{
    struct buffer b = *b_;
    struct dhcp_header *dhcp;
    uint32_t *cookie;
    uint8_t type;
    char *vendor_class;

    dhcp = buffer_try_pull(&b, sizeof *dhcp);
    if (!dhcp) {
        VLOG_DBG("buffer too small for DHCP header (%zu bytes)", b.size);
        goto error;
    }

    if (dhcp->op != DHCP_BOOTREPLY && dhcp->op != DHCP_BOOTREQUEST) {
        VLOG_DBG("invalid DHCP op (%"PRIu8")", dhcp->op);
        goto error;
    }
    if (dhcp->htype != ARP_HRD_ETHERNET) {
        VLOG_DBG("invalid DHCP htype (%"PRIu8")", dhcp->htype);
        goto error;
    }
    if (dhcp->hlen != ETH_ADDR_LEN) {
        VLOG_DBG("invalid DHCP hlen (%"PRIu8")", dhcp->hlen);
        goto error;
    }

    dhcp_msg_init(msg);
    msg->op = dhcp->op;
    msg->xid = ntohl(dhcp->xid);
    msg->secs = ntohs(dhcp->secs);
    msg->flags = ntohs(dhcp->flags);
    msg->ciaddr = dhcp->ciaddr;
    msg->yiaddr = dhcp->yiaddr;
    msg->siaddr = dhcp->siaddr;
    msg->giaddr = dhcp->giaddr;
    memcpy(msg->chaddr, dhcp->chaddr, ETH_ADDR_LEN);

    cookie = buffer_try_pull(&b, sizeof cookie);
    if (cookie) {
        if (ntohl(*cookie) == DHCP_OPTS_COOKIE) {
            uint8_t overload;

            parse_options(msg, "options", b.data, b.size, 0);
            if (dhcp_msg_get_uint8(msg, DHCP_CODE_OPTION_OVERLOAD,
                                   0, &overload)) {
                if (overload & 1) {
                    parse_options(msg, "file", dhcp->file, sizeof dhcp->file,
                                  0);
                }
                if (overload & 2) {
                    parse_options(msg, "sname",
                                  dhcp->sname, sizeof dhcp->sname, 0);
                }
            }
        } else {
            VLOG_DBG("bad DHCP options cookie: %08"PRIx32, ntohl(*cookie));
        }
    } else {
        VLOG_DBG("DHCP packet has no options");
    }

    vendor_class = dhcp_msg_get_string(msg, DHCP_CODE_VENDOR_CLASS);
    if (vendor_class && !strcmp(vendor_class, "OpenFlow")) {
        parse_options(msg, "vendor-specific",
                      msg->options[DHCP_CODE_VENDOR_SPECIFIC].data,
                      msg->options[DHCP_CODE_VENDOR_SPECIFIC].n,
                      DHCP_VENDOR_OFS);
    }
    free(vendor_class);

    validate_options(msg);
    if (!dhcp_msg_get_uint8(msg, DHCP_CODE_DHCP_MSG_TYPE, 0, &type)) {
        VLOG_DBG("missing DHCP message type");
        dhcp_msg_uninit(msg);
        goto error;
    }
    msg->type = type;
    return 0;

error:
    if (VLOG_IS_DBG_ENABLED()) {
        struct ds ds;

        ds_init(&ds);
        ds_put_hex_dump(&ds, b_->data, b_->size, 0, true);
        VLOG_DBG("invalid DHCP message dump:\n%s", ds_cstr(&ds));

        ds_clear(&ds);
        dhcp_msg_to_string(msg, &ds);
        VLOG_DBG("partially dissected DHCP message: %s", ds_cstr(&ds));

        ds_destroy(&ds);
    }
    return EPROTO;
}

static void
put_option_chunk(struct buffer *b, uint8_t code, void *data, size_t n)
{
    uint8_t header[2];

    assert(n < 256);
    header[0] = code;
    header[1] = n;
    buffer_put(b, header, sizeof header);
    buffer_put(b, data, n);
}

static void
put_option(struct buffer *b, uint8_t code, void *data, size_t n)
{
    if (data) {
        if (n) {
            /* Divide the data into chunks of 255 bytes or less.  Make
             * intermediate chunks multiples of 8 bytes in case the
             * recipient validates a chunk at a time instead of the
             * concatenated value. */
            uint8_t *p = data;
            while (n) {
                size_t chunk = n > 255 ? 248 : n;
                put_option_chunk(b, code, p, chunk);
                p += chunk;
                n -= chunk;
            }
        } else {
            /* Option should be present but carry no data. */
            put_option_chunk(b, code, NULL, 0);
        }
    }
}

/* Appends to 'b' the DHCP message represented by 'msg'. */
void
dhcp_assemble(const struct dhcp_msg *msg, struct buffer *b)
{
    const uint8_t end = DHCP_CODE_END;
    uint32_t cookie = htonl(DHCP_OPTS_COOKIE);
    struct buffer vnd_data;
    struct dhcp_header dhcp;
    int i;

    memset(&dhcp, 0, sizeof dhcp);
    dhcp.op = msg->op;
    dhcp.htype = ARP_HRD_ETHERNET;
    dhcp.hlen = ETH_ADDR_LEN;
    dhcp.hops = 0;
    dhcp.xid = htonl(msg->xid);
    dhcp.secs = htons(msg->secs);
    dhcp.flags = htons(msg->flags);
    dhcp.ciaddr = msg->ciaddr;
    dhcp.yiaddr = msg->yiaddr;
    dhcp.siaddr = msg->siaddr;
    dhcp.giaddr = msg->giaddr;
    memcpy(dhcp.chaddr, msg->chaddr, ETH_ADDR_LEN);
    buffer_put(b, &dhcp, sizeof dhcp);
    buffer_put(b, &cookie, sizeof cookie);

    /* Put DHCP message type first.  (The ordering is not required but it
     * seems polite.) */
    if (msg->type) {
        uint8_t type = msg->type;
        put_option(b, DHCP_CODE_DHCP_MSG_TYPE, &type, 1);
    }

    /* Put the standard options. */
    for (i = 0; i < DHCP_VENDOR_OFS; i++) {
        const struct dhcp_option *option = &msg->options[i];
        put_option(b, i, option->data, option->n);
    }

    /* Assemble vendor specific option and put it. */
    buffer_init(&vnd_data, 0);
    for (i = DHCP_VENDOR_OFS; i < DHCP_N_OPTIONS; i++) {
        const struct dhcp_option *option = &msg->options[i];
        put_option(&vnd_data, i - DHCP_VENDOR_OFS, option->data, option->n);
    }
    if (vnd_data.size) {
        put_option(b, DHCP_CODE_VENDOR_SPECIFIC, vnd_data.data, vnd_data.size);
    }
    buffer_uninit(&vnd_data);

    /* Put end-of-options option. */
    buffer_put(b, &end, sizeof end);
}

