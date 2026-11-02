/* Unit test for the pktbuf ring buffer. Build and run with `make check`. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packet.h"

/* Allocation failure injection via -Wl,--wrap=realloc,--wrap=calloc */
static bool fail_alloc;
void *__real_realloc(void *ptr, size_t size);
void *__real_calloc(size_t n, size_t size);
void *__wrap_realloc(void *ptr, size_t size)
{
    return fail_alloc ? NULL : __real_realloc(ptr, size);
}
void *__wrap_calloc(size_t n, size_t size)
{
    return fail_alloc ? NULL : __real_calloc(n, size);
}

static int capacity(const pktbuf_t *b)
{
    return 1 << b->cap;
}

/* Write n bytes to a pipe and fill from it exactly once. Pipe writes up to
 * PIPE_BUF are atomic, so a single fill sees all n bytes when they fit. */
static ssize_t fill_once(pktbuf_t *b, const void *s, size_t n)
{
    int p[2];
    assert(pipe(p) == 0);
    assert(write(p[1], s, n) == (ssize_t) n);
    close(p[1]);
    ssize_t r = pktbuf_fill_from_file(b, p[0]);
    close(p[0]);
    return r;
}

static void feed(pktbuf_t *b, const char *s)
{
    size_t n = strlen(s);
    assert(fill_once(b, s, n) == (ssize_t) n);
}

static void expect(pktbuf_t *b, const char *want)
{
    assert(pktbuf_is_complete(b));
    packet_t *pkt = pktbuf_pop_packet(b);
    assert(pkt);
    assert(strcmp((char *) pkt->data, want) == 0);
    assert(pkt->end_pos == (int) strlen(want) - 1);
    free(pkt);
}

/* Build "$" + fill*(n-4) + "#00": a packet of exactly n bytes */
static char *make_packet(int n, char fill)
{
    char *s = malloc(n + 1);
    memset(s, fill, n);
    s[0] = '$';
    memcpy(s + n - 3, "#00", 4);
    return s;
}

static void test_init_failure(void)
{
    pktbuf_t b;
    fail_alloc = true;
    assert(!pktbuf_init(&b));
    fail_alloc = false;
    pktbuf_destroy(&b);
}

static void test_framing(pktbuf_t *b)
{
    /* garbage prefix + back-to-back packets */
    feed(b, "+++$g#67$m0,4#fd");
    expect(b, "$g#67");
    expect(b, "$m0,4#fd");
    assert(!pktbuf_is_complete(b) && b->size == 0 && b->head == 0);

    /* pop without a complete packet */
    assert(pktbuf_pop_packet(b) == NULL);

    /* EOF leaves the buffer untouched */
    assert(fill_once(b, "", 0) == 0 && b->size == 0);

    /* packet arriving one piece at a time, including a split checksum */
    feed(b, "$abc");
    assert(!pktbuf_is_complete(b)); /* no '#' */
    feed(b, "#");
    assert(!pktbuf_is_complete(b)); /* end_pos > size */
    feed(b, "0");
    assert(!pktbuf_is_complete(b)); /* end_pos == size */
    feed(b, "1");
    expect(b, "$abc#01");

    /* '#' inside the payload is not the end of the packet */
    feed(b, "$X0,1:#");
    assert(!pktbuf_is_complete(b));

    /* trailing bytes without '$' are dropped and the buffer resets */
    feed(b, "ab");
    expect(b, "$X0,1:#ab");
    feed(b, "junk");
    assert(!pktbuf_is_complete(b) && b->size == 0 && b->head == 0);
}

static void test_wrap(pktbuf_t *b)
{
    int cap = capacity(b);
    assert(b->head == 0 && b->size == 0);

    /* park head at cap - 4 without filling the buffer */
    char *pkt = make_packet(cap - 4, 'x');
    feed(b, pkt);
    assert(pktbuf_is_complete(b) && b->end_pos == cap - 5);
    free(pktbuf_pop_packet(b));
    free(pkt);
    assert(b->head == cap - 4 && b->size == 0);

    /* 7 bytes into 4 free bytes at the tail + 3 at the front: one readv */
    feed(b, "$Hg0#df");
    assert(b->head + b->size > cap);

    /* data already wraps, so the free run must be clamped to size */
    feed(b, "$c#63");
    assert(b->size == 12);

    /* '#' search and pop copy both cross the physical boundary */
    expect(b, "$Hg0#df");
    expect(b, "$c#63");
    assert(b->head == 8 && b->size == 0);
    assert(capacity(b) == cap); /* never grew */

    /* INTR_CHAR scanning via pktbuf_at over the last nread bytes */
    feed(b, "\x03");
    assert(pktbuf_at(b, b->size - 1) == INTR_CHAR);
    assert(!pktbuf_is_complete(b)); /* reset head to 0 */
}

/* Fill to exactly capacity, then deliver the rest so the buffer grows */
static void test_grow(pktbuf_t *b, int head)
{
    int cap = capacity(b);
    assert(b->head == head && b->size == 0);

    char *pkt = make_packet(cap + 3, 'y');
    assert(fill_once(b, pkt, cap) == cap);
    assert(b->size == cap && !pktbuf_is_complete(b));

    /* allocation failure: fill reports an error, buffer unchanged */
    fail_alloc = true;
    assert(fill_once(b, pkt + cap, 3) == -1);
    fail_alloc = false;
    assert(capacity(b) == cap && b->size == cap);

    assert(fill_once(b, pkt + cap, 3) == 3);
    assert(capacity(b) == cap * 2);
    assert(pktbuf_is_complete(b));

    /* allocation failure on pop leaves the packet in place */
    fail_alloc = true;
    assert(pktbuf_pop_packet(b) == NULL);
    fail_alloc = false;

    packet_t *out = pktbuf_pop_packet(b);
    assert(out && out->end_pos == cap + 2);
    assert(memcmp(out->data, pkt, cap + 3) == 0 && out->data[cap + 3] == 0);
    free(out);
    free(pkt);
    assert(b->size == 0);

    /* reset head to 0 for the next scenario */
    feed(b, "z");
    assert(!pktbuf_is_complete(b) && b->head == 0);
}

int main(void)
{
    test_init_failure();

    pktbuf_t b;
    assert(pktbuf_init(&b));
    test_framing(&b);
    test_wrap(&b);
    test_grow(&b, 0); /* grow with contiguous data */

    /* grow while the data wraps: park head at 3 first */
    int cap = capacity(&b);
    char *pkt = make_packet(cap - 1, 'w');
    feed(&b, pkt);
    assert(pktbuf_is_complete(&b));
    free(pktbuf_pop_packet(&b));
    free(pkt);
    assert(b.head == cap - 1);
    feed(&b, "$a#00");
    feed(&b, "$b#00");
    expect(&b, "$a#00");
    expect(&b, "$b#00");
    assert(b.head == 9 && b.head + b.size < cap);
    test_grow(&b, 9);

    pktbuf_destroy(&b);
    puts("test_pktbuf: ok");
    return 0;
}
