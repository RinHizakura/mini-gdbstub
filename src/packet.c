#include "packet.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#define CAPACITY(pktbuf) (1 << (pktbuf)->cap)
#define MASK(pktbuf) (CAPACITY(pktbuf) - 1)

static void pktbuf_clear(pktbuf_t *pktbuf)
{
    pktbuf->end_pos = -1;
    pktbuf->size = 0;
    pktbuf->head = 0;
}

#define DEFAULT_CAP (10)
bool pktbuf_init(pktbuf_t *pktbuf)
{
    pktbuf->cap = DEFAULT_CAP;
    pktbuf->data = calloc(1, CAPACITY(pktbuf) * sizeof(uint8_t));
    pktbuf->interrupts = 0;
    pktbuf_clear(pktbuf);

    return pktbuf->data != NULL;
}

/* Find the first logical offset in [from, size) holding c, or -1 */
static int pktbuf_find(const pktbuf_t *pktbuf, int from, uint8_t c)
{
    for (int i = from; i < pktbuf->size; i++) {
        if (pktbuf_at(pktbuf, i) == c)
            return i;
    }
    return -1;
}

/* Drop the first n logical bytes */
static void pktbuf_consume(pktbuf_t *pktbuf, int n)
{
    pktbuf->head = (pktbuf->head + n) & MASK(pktbuf);
    pktbuf->size -= n;
}

/* Copy the first len logical bytes to dst, handling wrap-around */
static void pktbuf_copy(const pktbuf_t *pktbuf, uint8_t *dst, int len)
{
    int first = CAPACITY(pktbuf) - pktbuf->head;
    if (first > len)
        first = len;
    memcpy(dst, pktbuf->data + pktbuf->head, first);
    memcpy(dst + first, pktbuf->data, len - first);
}

static bool pktbuf_grow(pktbuf_t *pktbuf)
{
    int old_capacity = CAPACITY(pktbuf);
    uint8_t *data = realloc(pktbuf->data, old_capacity * 2);
    if (!data)
        return false;
    pktbuf->data = data;
    pktbuf->cap++;

    /* Bytes that wrapped to the front now continue after the old end */
    int wrapped = pktbuf->head + pktbuf->size - old_capacity;
    if (wrapped > 0)
        memcpy(pktbuf->data + old_capacity, pktbuf->data, wrapped);
    return true;
}

ssize_t pktbuf_fill_from_file(pktbuf_t *pktbuf, int fd)
{
    assert(CAPACITY(pktbuf) >= pktbuf->size);

    /* enlarge the buffer to read from file when it is full */
    if (CAPACITY(pktbuf) == pktbuf->size && !pktbuf_grow(pktbuf))
        return -1;

    /* Free space may be split by the physical end of the array: read both
     * runs at once with readv(). */
    int tail = (pktbuf->head + pktbuf->size) & MASK(pktbuf);
    int left = CAPACITY(pktbuf) - pktbuf->size;
    int first = CAPACITY(pktbuf) - tail;
    if (first > left)
        first = left;
    struct iovec iov[2] = {
        {.iov_base = pktbuf->data + tail, .iov_len = first},
        {.iov_base = pktbuf->data, .iov_len = left - first},
    };
    ssize_t nread = readv(fd, iov, iov[1].iov_len ? 2 : 1);
    if (nread > 0)
        pktbuf->size += nread;

    return nread;
}

/* 0x03 only means "interrupt" outside packet framing: inside an X packet it
 * is the escaped form of '#'. Count it in the logical range [from, to). */
static void pktbuf_count_interrupts(pktbuf_t *pktbuf, int from, int to)
{
    for (int i = from; i < to; i++) {
        if (pktbuf_at(pktbuf, i) == INTR_CHAR)
            pktbuf->interrupts++;
    }
}

bool pktbuf_is_complete(pktbuf_t *pktbuf)
{
    /* skip to the head of next packet */
    int head = pktbuf_find(pktbuf, 0, '$');
    if (head < 0) {
        pktbuf_count_interrupts(pktbuf, 0, pktbuf->size);
        pktbuf_clear(pktbuf);
        return false;
    }
    pktbuf_count_interrupts(pktbuf, 0, head);
    pktbuf_consume(pktbuf, head);

    /* check the end of the buffer */
    int end_pos = pktbuf_find(pktbuf, 1, '#');
    if (end_pos < 0)
        return false;

    end_pos += CSUM_SIZE;
    if (end_pos >= pktbuf->size)
        return false;

    pktbuf->end_pos = end_pos;
    return true;
}

packet_t *pktbuf_pop_packet(pktbuf_t *pktbuf)
{
    if (pktbuf->end_pos == -1)
        return NULL;

    int pkt_size = pktbuf->end_pos + 1;
    packet_t *pkt = calloc(1, sizeof(packet_t) + pkt_size + 1);
    if (!pkt)
        return NULL;
    pktbuf_copy(pktbuf, pkt->data, pkt_size);
    pkt->end_pos = pktbuf->end_pos;
    pkt->data[pkt_size] = 0;

    pktbuf_consume(pktbuf, pkt_size);
    pktbuf->end_pos = -1;
    return pkt;
}

void pktbuf_destroy(pktbuf_t *pktbuf)
{
    free(pktbuf->data);
}
