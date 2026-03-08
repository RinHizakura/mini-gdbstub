#ifndef PKTQUEUE_H
#define PKTQUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include "packet.h"

/* Thread-safe packet queue for inter-thread packet handoff.
 *
 * Reader thread pushes complete packets; main thread pops and processes.
 * This eliminates the race condition where both threads call recv() on
 * the same socket.
 */

typedef struct pktqueue_node {
    packet_t *pkt;
    struct pktqueue_node *next;
} pktqueue_node_t;

typedef struct {
    pktqueue_node_t *head;
    pktqueue_node_t *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond; /* for blocking pop */
    bool shutdown;       /* signal reader to stop */
    bool interrupted;    /* interrupt character received */
} pktqueue_t;

/* Initialize packet queue. Returns true on success. */
bool pktqueue_init(pktqueue_t *queue);

/* Destroy packet queue and free all pending packets. */
void pktqueue_destroy(pktqueue_t *queue);

/* Push a packet to the queue (called by reader thread).
 * Takes ownership of pkt on success - caller must not free it.
 * Returns true on success, false on allocation failure (pkt NOT freed).
 */
bool pktqueue_push(pktqueue_t *queue, packet_t *pkt);

/* Pop a packet from the queue (called by main thread).
 * Blocks until a packet is available or shutdown is signaled.
 * Returns NULL on shutdown, otherwise returns packet (caller must free).
 */
packet_t *pktqueue_pop(pktqueue_t *queue);

/* Signal shutdown to unblock any waiting pop operation. */
void pktqueue_signal_shutdown(pktqueue_t *queue);

/* Signal that an interrupt character (0x03) was received.
 * This wakes up any blocked pop to allow interrupt handling.
 */
void pktqueue_signal_interrupt(pktqueue_t *queue);

/* Check and clear the interrupt flag. Returns true if interrupted. */
bool pktqueue_check_interrupt(pktqueue_t *queue);

/* Check if shutdown was signaled. */
bool pktqueue_is_shutdown(pktqueue_t *queue);

#endif
