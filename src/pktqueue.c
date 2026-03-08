#include <errno.h>
#include <stdlib.h>

#include "pktqueue.h"

bool pktqueue_init(pktqueue_t *queue)
{
    queue->head = queue->tail = NULL;
    queue->shutdown = false;
    queue->interrupted = false;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
        return false;

    if (pthread_cond_init(&queue->cond, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return false;
    }

    return true;
}

void pktqueue_destroy(pktqueue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);

    /* Free all pending packets */
    pktqueue_node_t *node = queue->head;
    while (node) {
        pktqueue_node_t *next = node->next;
        free(node->pkt);
        free(node);
        node = next;
    }
    queue->head = NULL;
    queue->tail = NULL;

    pthread_mutex_unlock(&queue->mutex);

    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
}

bool pktqueue_push(pktqueue_t *queue, packet_t *pkt)
{
    pktqueue_node_t *node = malloc(sizeof(pktqueue_node_t));
    if (!node)
        return false; /* Caller retains ownership of pkt */

    node->pkt = pkt;
    node->next = NULL;

    pthread_mutex_lock(&queue->mutex);

    if (queue->tail) {
        queue->tail->next = node;
        queue->tail = node;
    } else {
        queue->head = node;
        queue->tail = node;
    }

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

packet_t *pktqueue_pop(pktqueue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);

    /* Wait until packet available, interrupted, or shutdown */
    while (!queue->head && !queue->shutdown && !queue->interrupted)
        pthread_cond_wait(&queue->cond, &queue->mutex);

    /* Return NULL on shutdown with empty queue */
    if (queue->shutdown && !queue->head) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* If only interrupted (no packet), return NULL but don't clear flag
     * The caller should check pktqueue_check_interrupt()
     */
    if (!queue->head) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    pktqueue_node_t *node = queue->head;
    queue->head = node->next;
    if (!queue->head)
        queue->tail = NULL;

    pthread_mutex_unlock(&queue->mutex);

    packet_t *pkt = node->pkt;
    free(node);
    return pkt;
}

void pktqueue_signal_shutdown(pktqueue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = true;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

void pktqueue_signal_interrupt(pktqueue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->interrupted = true;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

bool pktqueue_check_interrupt(pktqueue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    bool was_interrupted = queue->interrupted;
    queue->interrupted = false;
    pthread_mutex_unlock(&queue->mutex);
    return was_interrupted;
}

bool pktqueue_is_shutdown(pktqueue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    bool shutdown = queue->shutdown;
    pthread_mutex_unlock(&queue->mutex);
    return shutdown;
}
