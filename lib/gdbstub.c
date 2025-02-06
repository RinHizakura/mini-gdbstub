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

    pthread_t tid;
    bool async_io_enable;
    void *args;
};

static inline void async_io_enable(struct gdbstub_private *priv)
{
    __atomic_store_n(&priv->async_io_enable, true, __ATOMIC_RELAXED);
}

static inline void async_io_disable(struct gdbstub_private *priv)
{
    __atomic_store_n(&priv->async_io_enable, false, __ATOMIC_RELAXED);
}

static inline bool async_io_is_enable(struct gdbstub_private *priv)
{
    return __atomic_load_n(&priv->async_io_enable, __ATOMIC_RELAXED);
}

static volatile bool thread_stop = false;
static void *socket_reader(gdbstub_t *gdbstub)
{
    void *args = gdbstub->priv->args;

    /* This thread will only works when running the gdbstub routine,
     * which won't procees on any packets. In this case, we read packet
     * in another thread to be able to interrupt the gdbstub. */
    while (!__atomic_load_n(&thread_stop, __ATOMIC_RELAXED)) {
        if (async_io_is_enable(gdbstub->priv) &&
            conn_try_recv_intr(&gdbstub->priv->conn)) {
            gdbstub->ops->on_interrupt(args);
        }
    }

    return NULL;
}

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
    if (gdbstub->priv == NULL) {
        return false;
    }
    // This is a naive implementation to parse the string
    char *addr_str = strdup(s);
    char *port_str = strchr(addr_str, ':');
    int port = 0;
    if (addr_str == NULL) {
        free(addr_str);
        return false;
    }

    if (port_str != NULL) {
        *port_str = '\0';
        port_str += 1;

        if (sscanf(port_str, "%d", &port) <= 0) {
            free(addr_str);
            return false;
        }
    }

    if (!conn_init(&gdbstub->priv->conn, addr_str, port)) {
        free(addr_str);
        return false;
    }
    free(addr_str);

    return true;
}

#define SEND_ERR(gdbstub, err) conn_send_pktstr(&gdbstub->priv->conn, err)
#define SEND_EPERM(gdbstub) SEND_ERR(gdbstub, "E01")
#define SEND_EINVAL(gdbstub) SEND_ERR(gdbstub, "E22")

static void process_reg_read(gdbstub_t *gdbstub, void *args)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    size_t reg_value;
    size_t reg_sz = gdbstub->arch.reg_byte;

    assert(sizeof(reg_value) >= gdbstub->arch.reg_byte);

    for (int i = 0; i < gdbstub->arch.reg_num; i++) {
        int ret = gdbstub->ops->read_reg(args, i, &reg_value);
        if (!ret) {
            hex_to_str((uint8_t *) &reg_value, &packet_str[i * reg_sz * 2],
                       reg_sz);
        } else {
            sprintf(packet_str, "E%d", ret);
            break;
        }
    }
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static void process_reg_read_one(gdbstub_t *gdbstub, char *payload, void *args)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    int regno;
    size_t reg_sz = gdbstub->arch.reg_byte;
    size_t reg_value;

    assert(sscanf(payload, "%x", &regno) == 1);
    int ret = gdbstub->ops->read_reg(args, regno, &reg_value);
#ifdef DEBUG
    printf("reg read = regno %d data %lx\n", regno, reg_value);
#endif
    if (!ret) {
        hex_to_str((uint8_t *) &reg_value, packet_str, reg_sz);
    } else {
        sprintf(packet_str, "E%d", ret);
    }
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static void process_reg_write(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t reg_value = 0;
    size_t reg_sz = gdbstub->arch.reg_byte;

    assert(sizeof(reg_value) >= gdbstub->arch.reg_byte);

    for (int i = 0; i < gdbstub->arch.reg_num; i++) {
        str_to_hex(&payload[i * reg_sz * 2], (uint8_t *) &reg_value, reg_sz);
#ifdef DEBUG
        printf("reg write = regno %d data %lx\n", i, reg_value);
#endif
        int ret = gdbstub->ops->write_reg(args, i, reg_value);
        if (ret) {
            /* FIXME: Even if we fail to modify this register, some
             * registers could be writen before. This may not be
             * an expected behavior. */
            char packet_str[MAX_SEND_PACKET_SIZE];
            sprintf(packet_str, "E%d", ret);
            conn_send_pktstr(&gdbstub->priv->conn, packet_str);
            return;
        }
    }
    conn_send_pktstr(&gdbstub->priv->conn, "OK");
}

static void process_reg_write_one(gdbstub_t *gdbstub, char *payload, void *args)
{
    int regno;
    size_t data;
    char *regno_str = payload;
    char *data_str = strchr(payload, '=');
    if (data_str) {
        *data_str = '\0';
        data_str++;
    }

    assert(strlen(data_str) == gdbstub->arch.reg_byte * 2);
    assert(sizeof(data) >= gdbstub->arch.reg_byte);
    assert(sscanf(regno_str, "%x", &regno) == 1);
    str_to_hex(data_str, (uint8_t *) &data, gdbstub->arch.reg_byte);
#ifdef DEBUG
    printf("reg write = regno %d / data %lx\n", regno, data);
#endif
    int ret = gdbstub->ops->write_reg(args, regno, data);
    if (!ret) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else {
        char packet_str[MAX_SEND_PACKET_SIZE];
        sprintf(packet_str, "E%d", ret);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    }
}

static void process_mem_read(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t maddr, mlen;
    assert(sscanf(payload, "%lx,%lx", &maddr, &mlen) == 2);
#ifdef DEBUG
    printf("mem read = addr %lx / len %lx\n", maddr, mlen);
#endif
    char packet_str[MAX_SEND_PACKET_SIZE];

    uint8_t *mval = malloc(mlen);
    int ret = gdbstub->ops->read_mem(args, maddr, mlen, mval);
    if (!ret) {
        hex_to_str(mval, packet_str, mlen);
    } else {
        sprintf(packet_str, "E%d", ret);
    }
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
    int ret = gdbstub->ops->write_mem(args, maddr, mlen, mval);

    if (!ret) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else {
        char packet_str[MAX_SEND_PACKET_SIZE];
        sprintf(packet_str, "E%d", ret);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    }
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
        /* check the args */
        char *action =  strtok(args, ":");
        assert(strcmp(action, "read") == 0);
        char *annex = strtok(NULL, ":");
        assert(strcmp(annex, "target.xml") == 0);

        char buf[MAX_SEND_PACKET_SIZE];
        int offset = 0, length = 0;
        sscanf(strtok(NULL, ":"), "%x,%x", &offset, &length);

        int total_len = strlen(gdbstub->arch.target_desc);
        int payload_length = MAX_DATA_PAYLOAD > length ? length : MAX_DATA_PAYLOAD; 

        // Determine if the remaining data fits within the buffer
        buf[0]=(total_len - offset < payload_length ) ? 'l' : 'm';
        snprintf(buf + 1 ,payload_length ,"%s", gdbstub->arch.target_desc+offset);

        conn_send_pktstr(&gdbstub->priv->conn, buf);
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
    }
}

static void process_query(gdbstub_t *gdbstub, char *payload, void *args)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    char *name = payload;
    char *qargs = strchr(payload, ':');
    if (qargs) {
        *qargs = '\0';
        qargs++;
    }
#ifdef DEBUG
    printf("query = %s %s\n", name, qargs);
#endif

    if (!strcmp(name, "C")) {
        if (gdbstub->ops->get_cpu != NULL) {
            int cpuid = gdbstub->ops->get_cpu(args);
            sprintf(packet_str, "QC%04d", cpuid);
            conn_send_pktstr(&gdbstub->priv->conn, packet_str);
        }
        else
            conn_send_pktstr(&gdbstub->priv->conn, "");
    } else if (!strcmp(name, "Supported")) {
        if (gdbstub->arch.target_desc != NULL)
            conn_send_pktstr(&gdbstub->priv->conn,
                             "PacketSize=1024;qXfer:features:read+");
        else
            conn_send_pktstr(&gdbstub->priv->conn, "PacketSize=1024");
    } else if (!strcmp(name, "Attached")) {
        /* assume attached to an existing process */
        conn_send_pktstr(&gdbstub->priv->conn, "1");
    } else if (!strcmp(name, "Xfer")) {
        process_xfer(gdbstub, qargs);
    } else if (!strcmp(name, "Symbol")) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else if (!strcmp(name, "fThreadInfo")) {
        /* Assume at least 1 CPU if user didn't specific
         * the CPU counts */
        int smp = gdbstub->arch.smp ? gdbstub->arch.smp : 1;
        char *ptr;
        char cpuid_str[6];

        /* Make assumption on the CPU counts, so
         * that we can use the buffer very simply. */
        assert(smp < 10000);

        packet_str[0] = 'm';
        ptr = packet_str + 1;
        for (int cpuid = 0; cpuid < smp; cpuid++)
        {
            sprintf(cpuid_str, "%04d,", cpuid);
            memcpy(ptr, cpuid_str, 5);
            ptr += 5;
        }
        *ptr = 0;
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    } else if (!strcmp(name, "sThreadInfo")) {
        conn_send_pktstr(&gdbstub->priv->conn, "l");
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
    }
}

static inline gdb_event_t process_vcont(gdbstub_t *gdbstub, char *args)
{
    gdb_event_t event = EVENT_NONE;

    switch (args[0]) {
    case 'c':
        if (gdbstub->ops->cont != NULL)
            event = EVENT_CONT;
        else
            SEND_EPERM(gdbstub);
        break;
    case 's':
        if (gdbstub->ops->stepi != NULL)
            event = EVENT_STEP;
        else
            SEND_EPERM(gdbstub);
        break;
    default:
        SEND_EPERM(gdbstub);
        break;
    }

    return event;
}

#define VCONT_DESC "vCont;%s%s"
static inline void process_vcont_support(gdbstub_t *gdbstub)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    char *str_s = (gdbstub->ops->stepi == NULL) ? "" : "s;S;";
    char *str_c = (gdbstub->ops->cont == NULL) ? "" : "c;C;";
    sprintf(packet_str, VCONT_DESC, str_s, str_c);

    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static gdb_event_t process_vpacket(gdbstub_t *gdbstub, char *payload)
{
    gdb_event_t event = EVENT_NONE;
    char *name = payload;
    char *args = strchr(payload, ';');
    if (args) {
        *args = '\0';
        args++;
    }
#ifdef DEBUG
    printf("vpacket = %s %s\n", name, args);
#endif

    if (!strcmp("Cont", name))
        event = process_vcont(gdbstub, args);
    else if (!strcmp("Cont?", name))
        process_vcont_support(gdbstub);
    else
        conn_send_pktstr(&gdbstub->priv->conn, "");

    return event;
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
        SEND_EINVAL(gdbstub);
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
        SEND_EINVAL(gdbstub);
}

static void process_set_cpu(gdbstub_t *gdbstub,
                            char *payload,
                            void *args)
{
    int cpuid;
    /* We don't support deprecated Hc packet, GDB
     * should send only send vCont;c and vCont;s here. */
    if (payload[0] == 'g') {
        assert(sscanf(payload, "g%d", &cpuid) == 1);
        gdbstub->ops->set_cpu(args, cpuid);
    }
    conn_send_pktstr(&gdbstub->priv->conn, "OK");
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
    case 'g':
        if (gdbstub->ops->read_reg != NULL) {
            process_reg_read(gdbstub, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'm':
        if (gdbstub->ops->read_mem != NULL) {
            process_mem_read(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'p':
        if (gdbstub->ops->read_reg != NULL) {
            process_reg_read_one(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'q':
        process_query(gdbstub, payload, args);
        break;
    case 'v':
        event = process_vpacket(gdbstub, payload);
        break;
    case 'z':
        if (gdbstub->ops->del_bp != NULL) {
            process_del_break_points(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case '?':
        conn_send_pktstr(&gdbstub->priv->conn, "S05");
        break;
    case 'D':
        event = EVENT_DETACH;
        break;
    case 'G':
        if (gdbstub->ops->write_reg != NULL) {
            process_reg_write(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'H':
        if (gdbstub->ops->set_cpu != NULL) {
            process_set_cpu(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'M':
        if (gdbstub->ops->write_mem != NULL) {
            process_mem_write(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'P':
        if (gdbstub->ops->write_reg != NULL) {
            process_reg_write_one(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'T':
        /* FIXME: Assume all CPUs are alive here, any exception case
         * that user may want to handle? */
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
        break;
    case 'X':
        if (gdbstub->ops->write_mem != NULL) {
            /* It is important for xwrite to know the end position of packet,
             * because there're escape characters which block us interpreting
             * the packet as a string just like other packets do. */
            process_mem_xwrite(gdbstub, payload,
                               &inpkt->data[inpkt->end_pos - CSUM_SIZE], args);
        } else {
            SEND_EPERM(gdbstub);
        }
        break;
    case 'Z':
        if (gdbstub->ops->set_bp != NULL) {
            process_set_break_points(gdbstub, payload, args);
        } else {
            SEND_EPERM(gdbstub);
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
    gdb_action_t act = ACT_NONE;

    switch (event) {
    case EVENT_CONT:
        async_io_enable(gdbstub->priv);
        act = gdbstub->ops->cont(args);
        async_io_disable(gdbstub->priv);
        break;
    case EVENT_STEP:
        act = gdbstub->ops->stepi(args);
        break;
    case EVENT_DETACH:
        act = ACT_SHUTDOWN;
        break;
    default:
        break;
    }

    return act;
}

static void gdbstub_act_resume(gdbstub_t *gdbstub)
{
    char packet_str[32];
    sprintf(packet_str, "S%02x", GDB_SIGNAL_TRAP);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    // Bring the user-provided argument in the gdbstub_t structure
    gdbstub->priv->args = args;

    /* Create a thread to receive interrupt when running the gdbstub op */
    if (gdbstub->ops->on_interrupt != NULL && gdbstub->priv->tid == 0) {
        async_io_disable(gdbstub->priv);
        pthread_create(&gdbstub->priv->tid, NULL, (void *) socket_reader,
                       (void *) gdbstub);
    }

    while (true) {
        conn_recv_packet(&gdbstub->priv->conn);
        packet_t *pkt = conn_pop_packet(&gdbstub->priv->conn);
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
    /* Use thread ID to make sure the thread was created */
    if (gdbstub->priv->tid != 0) {
        __atomic_store_n(&thread_stop, true, __ATOMIC_RELAXED);
        pthread_join(gdbstub->priv->tid, NULL);
    }

    conn_close(&gdbstub->priv->conn);
    free(gdbstub->priv);
}
