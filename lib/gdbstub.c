#include "gdbstub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG

bool gdbstub_init(gdbstub_t *gdbstub, struct target_ops *ops, char *s)
{
    if (s == NULL || ops == NULL)
        return false;

    gdbstub->ops = ops;
    pktbuf_init(&gdbstub->in);

    // This is a naive implementation to parse the string
    char *addr_str = strdup(s);
    char *port_str = strchr(addr_str, ':');
    if (addr_str == NULL || port_str == NULL) {
        free(addr_str);
        return false;
    }

    *port_str = '\0';
    port_str += 1;

    int port;
    if (sscanf(port_str, "%d", &port) <= 0) {
        free(addr_str);
        return false;
    }

    if (!conn_init(&gdbstub->conn, addr_str, port)) {
        free(addr_str);
        return false;
    }

    free(addr_str);
    return true;
}

static void process_query(gdbstub_t *gdbstub, char *payload)
{
#ifdef DEBUG
    printf("payload = %s\n", payload);
#endif
    char *name = payload;
    char *args = strchr(payload, ':');
    if (args) {
        *args = '\0';
        args++;
    }

    if (!strcmp(name, "Supported")) {
        /* TODO: We should do handshake correctly */
        conn_send_pktstr(&gdbstub->conn, "PacketSize=512");
    } else {
        conn_send_pktstr(&gdbstub->conn, "");
    }
}

static void gdbstub_process_packet(gdbstub_t *gdbstub, packet_t *inpkt)
{
    assert(inpkt->data[0] == '$');
    /* TODO: check the checksum result */
    *(inpkt->end - CSUM_SIZE) = 0;
    uint8_t request = inpkt->data[1];
    char *payload = (char *) &inpkt->data[2];

    switch (request) {
    case 'q':
        process_query(gdbstub, payload);
        break;
    default:
        conn_send_pktstr(&gdbstub->conn, "");
        break;
    }
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    while (true) {
        /* UNSAFE! the packet can only be valid in a limited lifetime
         * since it is a referenced of packet buffer */
        conn_recv_packet(&gdbstub->conn, &gdbstub->in);
        packet_t *pkt = pktbuf_pop_packet(&gdbstub->in);
#ifdef DEBUG
        printf("packet = %s\n", pkt->data);
#endif
        gdbstub_process_packet(gdbstub, pkt);
        free(pkt);

        event_t e = EVENT_NONE;
        // action_t act = gdbstub->ops->handle_event(e, args);
        action_t act = ACT_CONT;

        switch (act) {
        case ACT_SHUTDOWN:
            return true;
        default:
            break;
        }
    }

    return false;
}

void gdbstub_close(gdbstub_t *gdbstub)
{
    conn_close(&gdbstub->conn);
}
