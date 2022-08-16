#include "gdbstub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arch.h"
#include "gdb_signal.h"

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

static char hexchars[] = "0123456789abcdef";
static void num_to_str(uint8_t *num, char *str, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        uint8_t ch = *(num + i);
        *(str + i * 2) = hexchars[ch >> 4];
        *(str + i * 2 + 1) = hexchars[ch & 0xf];
    }
}

static void process_reg_read(gdbstub_t *gdbstub, void *args)
{
    /* FIXME: yes, lots of memory copy again :( */
    char packet_str[MAX_PACKET_SIZE];

    /* FIXME: why should we pass a 64 bits value for 32 bits registers? */
    size_t reg_value;
    for (int i = 0; i < ARCH_REG_NUM; i++) {
        reg_value = gdbstub->ops->read_reg(args, i);
        num_to_str((uint8_t *) &reg_value,
                   packet_str + i * sizeof(reg_value) * 2, sizeof(reg_value));
    }
    packet_str[ARCH_REG_NUM * sizeof(reg_value) * 2] = '\0';
    conn_send_pktstr(&gdbstub->conn, packet_str);
}

void process_xfer(gdbstub_t *gdbstub, char *s)
{
    char *name = s;
    char *args = strchr(s, ':');
    if (args) {
        *args = '\0';
        args++;
    }
#ifdef DEBUG
    printf("xfer = %s %s\n", name, args);
#endif
    if (!strcmp(name, "features")) {
        /* FIXME: We should check the args */
        conn_send_pktstr(&gdbstub->conn, TAEGET_DESC);
    } else {
        conn_send_pktstr(&gdbstub->conn, "");
    }
}

static void process_query(gdbstub_t *gdbstub, char *payload)
{
    char *name = payload;
    char *args = strchr(payload, ':');
    if (args) {
        *args = '\0';
        args++;
    }
#ifdef DEBUG
    printf("query = %s %s\n", name, args);
#endif

    if (!strcmp(name, "Supported")) {
        /* TODO: We should do handshake correctly */
        conn_send_pktstr(&gdbstub->conn, "PacketSize=512;qXfer:features:read+");
    } else if (!strcmp(name, "Attached")) {
        /* assume attached to an existing process */
        conn_send_pktstr(&gdbstub->conn, "1");
    } else if (!strcmp(name, "Xfer")) {
        process_xfer(gdbstub, args);
    } else {
        conn_send_pktstr(&gdbstub->conn, "");
    }
}

static void process_vpacket(gdbstub_t *gdbstub, char *payload)
{
    char *name = payload;
    char *args = strchr(payload, ':');
    if (args) {
        *args = '\0';
        args++;
    }
#ifdef DEBUG
    printf("vpacket = %s %s\n", name, args);
#endif

    if (!strcmp("Cont?", name)) {
        conn_send_pktstr(&gdbstub->conn, "vCont;c;");
    } else {
        conn_send_pktstr(&gdbstub->conn, "");
    }
}

static event_t gdbstub_process_packet(gdbstub_t *gdbstub,
                                      packet_t *inpkt,
                                      void *args)
{
    assert(inpkt->data[0] == '$');
    /* TODO: check the checksum result */
    inpkt->data[inpkt->end_pos - CSUM_SIZE] = 0;
    uint8_t request = inpkt->data[1];
    char *payload = (char *) &inpkt->data[2];
    event_t event = EVENT_NONE;

    switch (request) {
    case 'c':
        event = EVENT_CONT;
        break;
    case 'g':
        process_reg_read(gdbstub, args);
        break;
    case 'q':
        process_query(gdbstub, payload);
        break;
    case 'v':
        process_vpacket(gdbstub, payload);
        break;
    case '?':
        conn_send_pktstr(&gdbstub->conn, "S05");
        break;
    default:
        conn_send_pktstr(&gdbstub->conn, "");
        break;
    }

    return event;
}

static action_t gdbstub_handle_event(gdbstub_t *gdbstub,
                                     event_t event,
                                     void *args)
{
    switch (event) {
    case EVENT_CONT:
        return gdbstub->ops->cont(args);
    default:
        return ACT_NONE;
    }
}

static void gdbstub_act_resume(gdbstub_t *gdbstub)
{
    char packet_str[32];
    sprintf(packet_str, "S%02x", GDB_SIGNAL_TRAP);
    conn_send_pktstr(&gdbstub->conn, packet_str);
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    while (true) {
        conn_recv_packet(&gdbstub->conn, &gdbstub->in);
        packet_t *pkt = pktbuf_pop_packet(&gdbstub->in);
#ifdef DEBUG
        printf("packet = %s\n", pkt->data);
#endif
        event_t event = gdbstub_process_packet(gdbstub, pkt, args);
        free(pkt);

        action_t act = gdbstub_handle_event(gdbstub, event, args);

        switch (act) {
        case ACT_RESUME:
            gdbstub_act_resume(gdbstub);
            break;
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
