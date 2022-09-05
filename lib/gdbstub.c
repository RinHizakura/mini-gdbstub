#include "gdbstub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conn.h"
#include "gdb_signal.h"
#include "utils/csum.h"
#include "utils/translate.h"

struct gdbstub_private {
    conn_t conn;
    pktbuf_t in;
};

bool gdbstub_init(gdbstub_t *gdbstub,
                  struct target_ops *ops,
                  arch_info_t arch,
                  char *s)
{
    if (s == NULL || ops == NULL)
        return false;

    memset(gdbstub, 0, sizeof(gdbstub_t));
    gdbstub->ops = ops;
    gdbstub->arch = arch;
    gdbstub->priv = calloc(1, sizeof(struct gdbstub_private));
    pktbuf_init(&gdbstub->priv->in);

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

    if (!conn_init(&gdbstub->priv->conn, addr_str, port)) {
        free(addr_str);
        return false;
    }

    free(addr_str);
    return true;
}

static void process_reg_read(gdbstub_t *gdbstub, void *args)
{
    /* FIXME: yes, lots of memory copy again :( */
    char packet_str[MAX_PACKET_SIZE];
    size_t reg_value;
    size_t reg_sz = gdbstub->arch.reg_byte;

    assert(sizeof(reg_value) >= gdbstub->arch.reg_byte);

    for (int i = 0; i < gdbstub->arch.reg_num; i++) {
        reg_value = gdbstub->ops->read_reg(args, i);
        /* FIXME: we may have to consider the endian */
        hex_to_str((uint8_t *) &reg_value, &packet_str[i * reg_sz * 2], reg_sz);
    }
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static void process_mem_read(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t maddr, mlen;
    assert(sscanf(payload, "%lx,%lx", &maddr, &mlen) == 2);
#ifdef DEBUG
    printf("mem read = addr %lx / len %lx\n", maddr, mlen);
#endif
    char packet_str[MAX_PACKET_SIZE];

    uint8_t *mval = malloc(mlen);
    gdbstub->ops->read_mem(args, maddr, mlen, mval);
    hex_to_str(mval, packet_str, mlen);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    free(mval);
}

static void process_mem_write(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t maddr, mlen;
    char *content = strchr(payload, ':');
    if (content) {
        *content = '\0';
        content++;
    }
    assert(sscanf(payload, "%lx,%lx", &maddr, &mlen) == 2);
#ifdef DEBUG
    printf("mem write = addr %lx / len %lx\n", maddr, mlen);
    printf("mem write = content %s\n", content);
#endif
    uint8_t *mval = malloc(mlen);
    str_to_hex(content, mval, mlen);
    gdbstub->ops->write_mem(args, maddr, mlen, mval);
    conn_send_pktstr(&gdbstub->priv->conn, "OK");
    free(mval);
}

static void process_mem_xwrite(gdbstub_t *gdbstub,
                               char *payload,
                               uint8_t *packet_end,
                               void *args)
{
    size_t maddr, mlen;
    char *content = strchr(payload, ':');
    if (content) {
        *content = '\0';
        content++;
    }
    assert(sscanf(payload, "%lx,%lx", &maddr, &mlen) == 2);
    assert(unescape(content, (char *) packet_end) == (int) mlen);
#ifdef DEBUG
    printf("mem xwrite = addr %lx / len %lx\n", maddr, mlen);
    for (size_t i = 0; i < mlen; i++) {
        printf("\tmem xwrite, byte %ld: %x\n", i, content[i]);
    }
#endif

    gdbstub->ops->write_mem(args, maddr, mlen, content);
    conn_send_pktstr(&gdbstub->priv->conn, "OK");
}
#define TARGET_DESC \
    "l<target version=\"1.0\"><architecture>%s</architecture></target>"

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
    if (!strcmp(name, "features") && gdbstub->arch.target_desc != NULL) {
        /* FIXME: We should check the args */
        char buf[MAX_PACKET_SIZE];
        sprintf(buf, TARGET_DESC, gdbstub->arch.target_desc);
        conn_send_pktstr(&gdbstub->priv->conn, buf);
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
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
        if (gdbstub->arch.target_desc != NULL)
            conn_send_pktstr(&gdbstub->priv->conn,
                             "PacketSize=1024;qXfer:features:read+");
        else
            conn_send_pktstr(&gdbstub->priv->conn, "PacketSize=1024");
    } else if (!strcmp(name, "Attached")) {
        /* assume attached to an existing process */
        conn_send_pktstr(&gdbstub->priv->conn, "1");
    } else if (!strcmp(name, "Xfer")) {
        process_xfer(gdbstub, args);
    } else if (!strcmp(name, "Symbol")) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
    }
}

#define VCONT_DESC "vCont;%s%s"
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
        char packet_str[MAX_PACKET_SIZE];
        char *str_s = (gdbstub->ops->stepi == NULL) ? "" : "s;";
        char *str_c = (gdbstub->ops->cont == NULL) ? "" : "c;";
        sprintf(packet_str, VCONT_DESC, str_s, str_c);

        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
    }
}

static void process_del_break_points(gdbstub_t *gdbstub,
                                     char *payload,
                                     void *args)
{
    size_t type, addr, kind;
    assert(sscanf(payload, "%zx,%zx,%zx", &type, &addr, &kind) == 3);

#ifdef DEBUG
    printf("remove breakpoints = %zx %zx %zx\n", type, addr, kind);
#endif

    bool ret = gdbstub->ops->del_bp(args, addr, type);
    if (ret)
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    else
        conn_send_pktstr(&gdbstub->priv->conn, "E01");
}

static void process_set_break_points(gdbstub_t *gdbstub,
                                     char *payload,
                                     void *args)
{
    size_t type, addr, kind;
    assert(sscanf(payload, "%zx,%zx,%zx", &type, &addr, &kind) == 3);

#ifdef DEBUG
    printf("set breakpoints = %zx %zx %zx\n", type, addr, kind);
#endif

    bool ret = gdbstub->ops->set_bp(args, addr, type);
    if (ret)
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    else
        conn_send_pktstr(&gdbstub->priv->conn, "E01");
}


static bool packet_csum_verify(packet_t *inpkt)
{
    /* We add extra 1 for leading '$' and minus extra 1 for trailing '#' */
    uint8_t csum_rslt = compute_checksum((char *) inpkt->data + 1,
                                         inpkt->end_pos - CSUM_SIZE - 1);
    uint8_t csum_expected;
    str_to_hex((char *) &inpkt->data[inpkt->end_pos - CSUM_SIZE + 1],
               &csum_expected, sizeof(uint8_t));
#ifdef DEBUG
    printf("csum rslt = %x / csum expected = %s / ", csum_rslt,
           &inpkt->data[inpkt->end_pos - CSUM_SIZE + 1]);
    printf("csum expected = %x \n", csum_expected);
#endif
    return csum_rslt == csum_expected;
}

static gdb_event_t gdbstub_process_packet(gdbstub_t *gdbstub,
                                          packet_t *inpkt,
                                          void *args)
{
    assert(inpkt->data[0] == '$');
    assert(packet_csum_verify(inpkt));

    /* After checking the checksum result, ignore those bytes */
    inpkt->data[inpkt->end_pos - CSUM_SIZE] = 0;
    uint8_t request = inpkt->data[1];
    char *payload = (char *) &inpkt->data[2];
    gdb_event_t event = EVENT_NONE;

    switch (request) {
    case 'c':
        if (gdbstub->ops->cont != NULL) {
            event = EVENT_CONT;
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case 'g':
        if (gdbstub->ops->read_reg != NULL) {
            process_reg_read(gdbstub, args);
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case 'm':
        if (gdbstub->ops->read_mem != NULL) {
            process_mem_read(gdbstub, payload, args);
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case 'q':
        process_query(gdbstub, payload);
        break;
    case 's':
        if (gdbstub->ops->stepi != NULL) {
            event = EVENT_STEP;
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case 'v':
        process_vpacket(gdbstub, payload);
        break;
    case 'z':
        if (gdbstub->ops->del_bp != NULL) {
            process_del_break_points(gdbstub, payload, args);
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case '?':
        conn_send_pktstr(&gdbstub->priv->conn, "S05");
        break;
    case 'D':
        event = EVENT_DETACH;
        break;
    case 'M':
        if (gdbstub->ops->write_mem != NULL) {
            process_mem_write(gdbstub, payload, args);
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case 'X':
        if (gdbstub->ops->write_mem != NULL) {
            /* It is important for xwrite to know the end position of packet,
             * because there're escape characters which block us interpreting
             * the packet as a string just like other packets do. */
            process_mem_xwrite(gdbstub, payload,
                               &inpkt->data[inpkt->end_pos - CSUM_SIZE], args);
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    case 'Z':
        if (gdbstub->ops->set_bp != NULL) {
            process_set_break_points(gdbstub, payload, args);
        } else {
            conn_send_pktstr(&gdbstub->priv->conn, "");
        }
        break;
    default:
        conn_send_pktstr(&gdbstub->priv->conn, "");
        break;
    }

    return event;
}

static gdb_action_t gdbstub_handle_event(gdbstub_t *gdbstub,
                                         gdb_event_t event,
                                         void *args)
{
    switch (event) {
    case EVENT_CONT:
        return gdbstub->ops->cont(args);
    case EVENT_STEP:
        return gdbstub->ops->stepi(args);
    case EVENT_DETACH:
        return ACT_SHUTDOWN;
    default:
        return ACT_NONE;
    }
}

static void gdbstub_act_resume(gdbstub_t *gdbstub)
{
    char packet_str[32];
    sprintf(packet_str, "S%02x", GDB_SIGNAL_TRAP);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    while (true) {
        conn_recv_packet(&gdbstub->priv->conn, &gdbstub->priv->in);
        packet_t *pkt = pktbuf_pop_packet(&gdbstub->priv->in);
#ifdef DEBUG
        printf("packet = %s\n", pkt->data);
#endif
        gdb_event_t event = gdbstub_process_packet(gdbstub, pkt, args);
        free(pkt);

        gdb_action_t act = gdbstub_handle_event(gdbstub, event, args);
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
    conn_close(&gdbstub->priv->conn);
    free(gdbstub->priv);
}
