#include "gdbstub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gdbstub_init(gdbstub_t *gdbstub, struct target_ops *ops, char *s)
{
    if (s == NULL || ops == NULL)
        return false;

    gdbstub->ops = ops;

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

void gdbstub_process_packet(gdbstub_t *gdbstub, packet_t *inpkt)
{
    assert(inpkt->start[0] == '$');
    uint8_t request = inpkt->start[1];

    switch (request) {
    default:
        conn_send_pktstr(&gdbstub->conn, PKTSTR_EMPTY);
        break;
    }
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    while (true) {
        /* UNSAFE! the packet can only be valid in a limited lifetime
         * since it is a referenced of packet buffer */
        packet_t *pkt = conn_recv_packet(&gdbstub->conn);
#ifdef DEBUG
        *(pkt->end + 1) = 0;
        printf("packet = %s\n", pkt->start);
#endif
        gdbstub_process_packet(gdbstub, pkt);
        free(pkt);

        event_t e = EVENT_NONE;
        action_t act = gdbstub->ops->handle_event(e, args);

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
