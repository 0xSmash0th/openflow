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

#include "buffer.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

/* Initializes 'b' as an empty buffer that contains the 'allocated' bytes of
 * memory starting at 'base'.
 *
 * 'base' should ordinarily be the first byte of a region obtained from
 * malloc(), but in circumstances where it can be guaranteed that 'b' will
 * never need to be expanded or freed, it can be a pointer into arbitrary
 * memory. */
void
buffer_use(struct buffer *b, void *base, size_t allocated)
{
    b->base = b->data = base;
    b->allocated = allocated;
    b->size = 0;
    b->next = NULL;
}

/* Initializes 'b' as a buffer with an initial capacity of 'size' bytes. */
void
buffer_init(struct buffer *b, size_t size)
{
    buffer_use(b, size ? xmalloc(size) : NULL, size);
}

/* Frees memory that 'b' points to. */
void
buffer_uninit(struct buffer *b) 
{
    if (b) {
        free(b->base);
    }
}

/* Frees memory that 'b' points to and allocates a new buffer */
void
buffer_reinit(struct buffer *b, size_t size)
{
    buffer_uninit(b);
    buffer_init(b, size);
}

/* Creates and returns a new buffer with an initial capacity of 'size'
 * bytes. */
struct buffer *
buffer_new(size_t size)
{
    struct buffer *b = xmalloc(sizeof *b);
    buffer_init(b, size);
    return b;
}

/* Frees memory that 'b' points to, as well as 'b' itself. */
void
buffer_delete(struct buffer *b) 
{
    if (b) {
        buffer_uninit(b);
        free(b);
    }
}

/* Returns the number of bytes of headroom in 'b', that is, the number of bytes
 * of unused space in buffer 'b' before the data that is in use.  (Most
 * commonly, the data in a buffer is at its beginning, and thus the buffer's
 * headroom is 0.) */
size_t
buffer_headroom(struct buffer *b) 
{
    return b->data - b->base;
}

/* Returns the number of bytes that may be appended to the tail end of buffer
 * 'b' before the buffer must be reallocated. */
size_t
buffer_tailroom(struct buffer *b) 
{
    return buffer_end(b) - buffer_tail(b);
}

/* Ensures that 'b' has room for at least 'size' bytes at its tail end,
 * reallocating and copying its data if necessary. */
void
buffer_reserve_tailroom(struct buffer *b, size_t size) 
{
    if (size > buffer_tailroom(b)) {
        size_t headroom = buffer_headroom(b);
        size_t new_allocated = b->allocated + MAX(size, 64);
        void *new_base = xmalloc(new_allocated);
        memcpy(new_base, b->base, b->allocated);
        free(b->base);
        b->base = new_base;
        b->allocated = new_allocated;
        b->data = new_base + headroom;
    }
}

/* Appends 'size' bytes of data to the tail end of 'b', reallocating and
 * copying its data if necessary.  Returns a pointer to the first byte of the
 * new data, which is left uninitialized. */
void *
buffer_put_uninit(struct buffer *b, size_t size) 
{
    void *p;
    buffer_reserve_tailroom(b, size);
    p = buffer_tail(b);
    b->size += size;
    return p;
}

/* Appends the 'size' bytes of data in 'p' to the tail end of 'b'.  Data in 'b'
 * is reallocated and copied if necessary. */
void
buffer_put(struct buffer *b, const void *p, size_t size) 
{
    memcpy(buffer_put_uninit(b, size), p, size);
}

/* If 'b' contains at least 'offset + size' bytes of data, returns a pointer to
 * byte 'offset'.  Otherwise, returns a null pointers. */
void *
buffer_at(const struct buffer *b, size_t offset, size_t size) 
{
    return offset + size <= b->size ? (char *) b->data + offset : NULL;
}

/* Returns a pointer to byte 'offset' in 'b', which must contain at least
 * 'offset + size' bytes of data. */
void *
buffer_at_assert(const struct buffer *b, size_t offset, size_t size) 
{
    assert(offset + size <= b->size);
    return ((char *) b->data) + offset;
}

/* Returns the byte following the last byte of data in use in 'b'. */
void *
buffer_tail(const struct buffer *b) 
{
    return (char *) b->data + b->size;
}

/* Returns the byte following the last byte allocated for use (but not
 * necessarily in use) by 'b'. */
void *
buffer_end(const struct buffer *b) 
{
    return (char *) b->base + b->allocated;
}

/* Clears any data from 'b'. */
void
buffer_clear(struct buffer *b) 
{
    b->data = b->base;
    b->size = 0;
}

/* Removes 'size' bytes from the head end of 'b', which must contain at least
 * 'size' bytes of data. */
void
buffer_pull(struct buffer *b, size_t size) 
{
    assert(b->size >= size);
    b->data += size;
    b->size -= size;
}
