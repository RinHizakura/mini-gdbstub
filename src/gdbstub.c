#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conn.h"
#include "gdb_signal.h"
#include "gdbstub.h"
#include "packet.h"
#include "pktqueue.h"
#include "regbuf.h"
#include "utils/csum.h"
#include "utils/log.h"
#include "utils/translate.h"

/* Poll timeout for reader thread (milliseconds) */
#define READER_POLL_TIMEOUT_MS 100

/* Each CPU takes at most 5 characters ("xxxx,") in the fThreadInfo reply */
#define MAX_SMP ((MAX_DATA_PAYLOAD - 1) / 5)

struct gdbstub_private {
    conn_t conn;
    regbuf_t regbuf;
    pktqueue_t pktqueue;

    pthread_t tid;
    volatile bool thread_stop; /* Per-instance thread control */
    bool reader_running;       /* Explicit flag for thread state */
    bool async_io_enable;
    bool interrupted;   /* Ctrl-C arrived while the target was running */
    char last_stop[64]; /* most recent stop reply, replayed for '?' */
    void *args;

    /* Cached register size totals (computed once at init) */
    size_t total_reg_bytes;
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

/* Reader thread: sole owner of all recv() calls on the socket.
 *
 * This thread reads from the socket, assembles complete packets,
 * pushes them to the queue, and detects interrupt characters (0x03).
 */
static void *socket_reader(void *arg)
{
    gdbstub_t *gdbstub = (gdbstub_t *) arg;
    struct gdbstub_private *priv = gdbstub->priv;
    int socket_fd = priv->conn.socket_fd;
    pktbuf_t pktbuf;

    if (!pktbuf_init(&pktbuf)) {
        pktqueue_signal_shutdown(&priv->pktqueue);
        return NULL;
    }

    struct pollfd pfd = {.fd = socket_fd, .events = POLLIN};

    while (!__atomic_load_n(&priv->thread_stop, __ATOMIC_RELAXED)) {
        int result = poll(&pfd, 1, READER_POLL_TIMEOUT_MS);

        if (result < 0) {
            if (errno == EINTR)
                continue;
            perror("poll error in socket_reader");
            break;
        }

        if (result == 0)
            continue; /* Timeout, check thread_stop */

        if (!(pfd.revents & POLLIN))
            continue;

        /* Read available data into buffer */
        ssize_t nread = pktbuf_fill_from_file(&pktbuf, socket_fd);

        if (nread == 0) {
            /* EOF - clean disconnect */
            break;
        }

        if (nread < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            /* Fatal error: ECONNRESET, EPIPE, etc. */
            break;
        }

        /* Process complete packets */
        while (pktbuf_is_complete(&pktbuf)) {
            packet_t *pkt = pktbuf_pop_packet(&pktbuf);
            /* The main thread acknowledges once it has verified the
             * checksum; on allocation failure the packet is dropped and GDB
             * retransmits after its timeout. */
            if (pkt && !pktqueue_push(&priv->pktqueue, pkt))
                free(pkt);
        }

        /* Framing tallied any interrupt characters found between packets */
        if (pktbuf.interrupts) {
            pktbuf.interrupts = 0;
            if (async_io_is_enable(priv) && gdbstub->ops->on_interrupt)
                gdbstub->ops->on_interrupt(priv->args);
            __atomic_store_n(&priv->interrupted, true, __ATOMIC_RELAXED);
        }
    }

    pktbuf_destroy(&pktbuf);
    pktqueue_signal_shutdown(&priv->pktqueue);
    return NULL;
}

bool gdbstub_init(gdbstub_t *gdbstub,
                  struct target_ops *ops,
                  arch_info_t arch,
                  char *s)
{
    char *addr_str = NULL;

    if (s == NULL || ops == NULL)
        return false;

    memset(gdbstub, 0, sizeof(gdbstub_t));
    gdbstub->ops = ops;
    gdbstub->arch = arch;
    gdbstub->priv = calloc(1, sizeof(struct gdbstub_private));
    if (gdbstub->priv == NULL)
        return false;

    /* Precompute total register storage size (register sizes are constant) */
    gdbstub->priv->total_reg_bytes = 0;
    for (int i = 0; i < arch.reg_num; i++)
        gdbstub->priv->total_reg_bytes += ops->get_reg_bytes(i);

    /* The g reply and the fThreadInfo list each have to fit in one packet */
    if (gdbstub->priv->total_reg_bytes * 2 > MAX_DATA_PAYLOAD ||
        arch.smp > MAX_SMP)
        goto priv_fail;

    /* Parse address string (format: "host:port" or "path") */
    addr_str = strdup(s);
    if (!addr_str)
        goto priv_fail;

    char *port_str = strchr(addr_str, ':');
    int port = 0;

    if (port_str != NULL) {
        *port_str = '\0';
        port_str += 1;

        if (sscanf(port_str, "%d", &port) <= 0)
            goto addr_fail;
    }

    if (!regbuf_init(&gdbstub->priv->regbuf))
        goto addr_fail;

    if (!pktqueue_init(&gdbstub->priv->pktqueue))
        goto regbuf_fail;

    if (!conn_init(&gdbstub->priv->conn, addr_str, port))
        goto pktqueue_fail;

    free(addr_str);
    return true;

pktqueue_fail:
    pktqueue_destroy(&gdbstub->priv->pktqueue);
regbuf_fail:
    regbuf_destroy(&gdbstub->priv->regbuf);
addr_fail:
    free(addr_str);
priv_fail:
    free(gdbstub->priv);
    gdbstub->priv = NULL;
    return false;
}

/* An error reply carries the errno as two hex digits: "E NN" */
static void send_errno(gdbstub_t *gdbstub, int err)
{
    char packet_str[8];
    snprintf(packet_str, sizeof(packet_str), "E%02x", err & 0xff);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}
#define SEND_EPERM(gdbstub) send_errno(gdbstub, EPERM)
#define SEND_EINVAL(gdbstub) send_errno(gdbstub, EINVAL)

/* A target that does not say how many CPUs it has gets one */
static inline int smp_count(gdbstub_t *gdbstub)
{
    return gdbstub->arch.smp ? gdbstub->arch.smp : 1;
}

/* GDB thread ids are hex and 1-based: thread N is CPU N-1. The reserved
 * ids 0 (any thread) and -1 (all threads) map to cpuid -1. */
static bool thread_to_cpu(gdbstub_t *gdbstub, const char *str, int *cpuid)
{
    char *end;
    long tid = strtol(str, &end, 16);
    if (end == str || tid < -1 || tid > smp_count(gdbstub))
        return false;
    *cpuid = tid > 0 ? tid - 1 : -1;
    return true;
}

/* Stop reply: "T05<reason>" plus the stopping thread when the target
 * can name it, so GDB does not have to guess which CPU stopped. */
static void send_stop_reply(gdbstub_t *gdbstub,
                            void *args,
                            int sig,
                            const char *reason)
{
    char *packet_str = gdbstub->priv->last_stop;
    size_t size = sizeof(gdbstub->priv->last_stop);
    int n = snprintf(packet_str, size, "T%02x%s", sig, reason);
    if (gdbstub->ops->get_cpu != NULL)
        snprintf(packet_str + n, size - n, "thread:%x;",
                 gdbstub->ops->get_cpu(args) + 1);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static gdb_event_t process_cont(gdbstub_t *gdbstub)
{
    gdb_event_t event = EVENT_NONE;

    if (gdbstub->ops->cont != NULL)
        event = EVENT_CONT;
    else
        SEND_EPERM(gdbstub);

    return event;
}

static gdb_event_t process_stepi(gdbstub_t *gdbstub)
{
    gdb_event_t event = EVENT_NONE;

    if (gdbstub->ops->stepi != NULL)
        event = EVENT_STEP;
    else
        SEND_EPERM(gdbstub);

    return event;
}

static void process_reg_read(gdbstub_t *gdbstub, void *args)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    size_t offset = 0;

    packet_str[0] = '\0';
    for (int i = 0; i < gdbstub->arch.reg_num; i++) {
        size_t reg_sz = gdbstub->ops->get_reg_bytes(i);
        void *reg_value = regbuf_get(&gdbstub->priv->regbuf, reg_sz);

        int ret = gdbstub->ops->read_reg(args, i, reg_value);
#ifdef DEBUG
        char debug_hex[MAX_SEND_PACKET_SIZE];
        hex_to_str((uint8_t *) reg_value, debug_hex, reg_sz);
        printf("reg read = regno %d data 0x%s (size %zu)\n", i, debug_hex,
               reg_sz);
#endif
        if (!ret) {
            hex_to_str((uint8_t *) reg_value, &packet_str[offset], reg_sz);
            offset += reg_sz * 2;
        } else {
            snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
            break;
        }
    }

    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static void process_reg_read_one(gdbstub_t *gdbstub, char *payload, void *args)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    int regno;

    if (sscanf(payload, "%x", &regno) != 1) {
        SEND_EINVAL(gdbstub);
        return;
    }
    size_t reg_sz = gdbstub->ops->get_reg_bytes(regno);
    void *reg_value = regbuf_get(&gdbstub->priv->regbuf, reg_sz);

    int ret = gdbstub->ops->read_reg(args, regno, reg_value);
#ifdef DEBUG
    char debug_hex[MAX_SEND_PACKET_SIZE];
    hex_to_str((uint8_t *) reg_value, debug_hex, reg_sz);
    printf("reg read = regno %d data 0x%s (size %zu)\n", regno, debug_hex,
           reg_sz);
#endif
    if (ret) {
        send_errno(gdbstub, ret);
        return;
    }
    hex_to_str((uint8_t *) reg_value, packet_str, reg_sz);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

static void process_reg_write(gdbstub_t *gdbstub, char *payload, void *args)
{
    int reg_num = gdbstub->arch.reg_num;

    /* Use cached total_reg_bytes computed at init time */
    size_t total_reg_bytes = gdbstub->priv->total_reg_bytes;
    size_t expected_hex_len = total_reg_bytes * 2;

    if (total_reg_bytes == 0) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
        return;
    }

    /* Validate payload length matches expected register count */
    size_t payload_len = strlen(payload);
    if (payload_len != expected_hex_len) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Allocate storage for new values and backup (for rollback) */
    uint8_t *new_values = malloc(total_reg_bytes);
    uint8_t *backup_values = malloc(total_reg_bytes);
    if (!new_values || !backup_values) {
        free(new_values);
        free(backup_values);
        send_errno(gdbstub, ENOMEM);
        return;
    }

    /* Parse all new values and save current values for rollback */
    size_t payload_offset = 0;
    size_t storage_offset = 0;
    for (int i = 0; i < reg_num; i++) {
        size_t reg_sz = gdbstub->ops->get_reg_bytes(i);

        /* Parse new value from payload */
        str_to_hex(&payload[payload_offset], &new_values[storage_offset],
                   reg_sz);

        /* Save current value for potential rollback */
        int ret =
            gdbstub->ops->read_reg(args, i, &backup_values[storage_offset]);
        if (ret) {
            /* Cannot read current state; abort without modifying */
            free(new_values);
            free(backup_values);
            send_errno(gdbstub, ret);
            return;
        }

#ifdef DEBUG
        char debug_hex[MAX_SEND_PACKET_SIZE];
        hex_to_str(&new_values[storage_offset], debug_hex, reg_sz);
        printf("reg write = regno %d data 0x%s (size %zu)\n", i, debug_hex,
               reg_sz);
#endif
        payload_offset += reg_sz * 2;
        storage_offset += reg_sz;
    }

    /* Commit all registers atomically */
    storage_offset = 0;
    int failed_regno = -1;
    int error_code = 0;
    for (int i = 0; i < reg_num; i++) {
        size_t reg_sz = gdbstub->ops->get_reg_bytes(i);
        int ret = gdbstub->ops->write_reg(args, i, &new_values[storage_offset]);
        if (ret) {
            failed_regno = i;
            error_code = ret;
            break;
        }
        storage_offset += reg_sz;
    }

    /* Rollback on failure */
    if (failed_regno >= 0) {
        /* Restore all registers written before the failure */
        storage_offset = 0;
        for (int i = 0; i < failed_regno; i++) {
            size_t reg_sz = gdbstub->ops->get_reg_bytes(i);
            gdbstub->ops->write_reg(args, i, &backup_values[storage_offset]);
            storage_offset += reg_sz;
        }
        free(new_values);
        free(backup_values);
        send_errno(gdbstub, error_code);
        return;
    }

    free(new_values);
    free(backup_values);
    conn_send_pktstr(&gdbstub->priv->conn, "OK");
}

static void process_reg_write_one(gdbstub_t *gdbstub, char *payload, void *args)
{
    int regno;
    char *regno_str = payload;
    char *data_str = strchr(payload, '=');
    if (!data_str) {
        SEND_EINVAL(gdbstub);
        return;
    }
    *data_str = '\0';
    data_str++;

    if (sscanf(regno_str, "%x", &regno) != 1) {
        SEND_EINVAL(gdbstub);
        return;
    }
    size_t reg_sz = gdbstub->ops->get_reg_bytes(regno);
    if (strlen(data_str) != reg_sz * 2) {
        SEND_EINVAL(gdbstub);
        return;
    }
    void *data = regbuf_get(&gdbstub->priv->regbuf, reg_sz);

    str_to_hex(data_str, (uint8_t *) data, reg_sz);
#ifdef DEBUG
    printf("reg write = regno %d data 0x%s (size %zu)\n", regno, data_str,
           reg_sz);
#endif

    int ret = gdbstub->ops->write_reg(args, regno, data);
    if (ret)
        send_errno(gdbstub, ret);
    else
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
}

static void process_mem_read(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t maddr, mlen;
    /* The hex-encoded reply has to fit in a single packet */
    if (sscanf(payload, "%zx,%zx", &maddr, &mlen) != 2 ||
        mlen > MAX_DATA_PAYLOAD / 2) {
        SEND_EINVAL(gdbstub);
        return;
    }
#ifdef DEBUG
    printf("mem read = addr %zx / len %zx\n", maddr, mlen);
#endif
    char packet_str[MAX_SEND_PACKET_SIZE];

    uint8_t *mval = malloc(mlen);
    int ret = gdbstub->ops->read_mem(args, maddr, mlen, mval);
    if (ret) {
        send_errno(gdbstub, ret);
    } else {
        hex_to_str(mval, packet_str, mlen);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    }
    free(mval);
}

static void process_mem_write(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t maddr, mlen;
    char *content = strchr(payload, ':');
    if (!content) {
        SEND_EINVAL(gdbstub);
        return;
    }
    *content = '\0';
    content++;
    if (sscanf(payload, "%zx,%zx", &maddr, &mlen) != 2 ||
        strlen(content) != mlen * 2) {
        SEND_EINVAL(gdbstub);
        return;
    }
#ifdef DEBUG
    printf("mem write = addr %zx / len %zx\n", maddr, mlen);
    printf("mem write = content %s\n", content);
#endif
    uint8_t *mval = malloc(mlen);
    str_to_hex(content, mval, mlen);
    int ret = gdbstub->ops->write_mem(args, maddr, mlen, mval);
    if (ret)
        send_errno(gdbstub, ret);
    else
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
    if (!content) {
        SEND_EINVAL(gdbstub);
        return;
    }
    *content = '\0';
    content++;
    if (sscanf(payload, "%zx,%zx", &maddr, &mlen) != 2 ||
        unescape(content, (char *) packet_end) != (int) mlen) {
        SEND_EINVAL(gdbstub);
        return;
    }
#ifdef DEBUG
    printf("mem xwrite = addr %zx / len %zx\n", maddr, mlen);
    for (size_t i = 0; i < mlen; i++) {
        printf("\tmem xwrite, byte %ld: %x\n", i, content[i]);
    }
#endif

    int ret = gdbstub->ops->write_mem(args, maddr, mlen, content);
    if (ret)
        send_errno(gdbstub, ret);
    else
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
}


static void process_xfer(gdbstub_t *gdbstub, char *s)
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
        /* qXfer:features:read:target.xml:offset,length */
        char *action = args ? strtok(args, ":") : NULL;
        char *annex = strtok(NULL, ":");
        char *range = strtok(NULL, ":");
        if (!action || strcmp(action, "read")) {
            conn_send_pktstr(&gdbstub->priv->conn, "");
            return;
        }
        if (!annex || strcmp(annex, "target.xml")) {
            send_errno(gdbstub, 0); /* E00: no such annex */
            return;
        }

        int offset = 0, length = 0;
        int total_len = strlen(gdbstub->arch.target_desc);
        if (!range || sscanf(range, "%x,%x", &offset, &length) != 2 ||
            offset < 0 || length < 0) {
            SEND_EINVAL(gdbstub);
            return;
        }

        /* Reading at or past the end yields an empty final chunk */
        int remaining = offset < total_len ? total_len - offset : 0;
        length = length < remaining ? length : remaining;

        /* The 'm'/'l' marker takes the first byte; the escaped data follows */
        char buf[MAX_SEND_PACKET_SIZE];
        int sent = escape(gdbstub->arch.target_desc + offset, length, buf + 1,
                          MAX_DATA_PAYLOAD - 1);
        buf[0] = offset + sent < total_len ? 'm' : 'l';
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
        /* A target without get_cpu has a single CPU, thread 1 */
        int cpuid = gdbstub->ops->get_cpu ? gdbstub->ops->get_cpu(args) : 0;
        sprintf(packet_str, "QC%x", cpuid + 1);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    } else if (!strcmp(name, "Supported")) {
        snprintf(packet_str, sizeof(packet_str),
                 "PacketSize=%x;QStartNoAckMode+%s", MAX_DATA_PAYLOAD,
                 gdbstub->arch.target_desc ? ";qXfer:features:read+" : "");
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    } else if (!strcmp(name, "Attached")) {
        /* assume attached to an existing process */
        conn_send_pktstr(&gdbstub->priv->conn, "1");
    } else if (!strcmp(name, "Xfer")) {
        process_xfer(gdbstub, qargs);
    } else if (!strcmp(name, "Symbol")) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else if (!strcmp(name, "fThreadInfo")) {
        char *ptr = packet_str;

        *ptr++ = 'm';
        for (int cpuid = 0; cpuid < smp_count(gdbstub); cpuid++)
            ptr += snprintf(ptr, packet_str + sizeof(packet_str) - ptr, "%x,",
                            cpuid + 1);
        ptr[-1] = '\0'; /* drop the trailing comma */
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    } else if (!strcmp(name, "sThreadInfo")) {
        conn_send_pktstr(&gdbstub->priv->conn, "l");
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
    }
}

static void process_general_set(gdbstub_t *gdbstub, char *payload)
{
    char *name = payload;
    char *qargs = strchr(payload, ':');
    if (qargs) {
        *qargs = '\0';
        qargs++;
    }
#ifdef DEBUG
    printf("general set = %s %s\n", name, qargs);
#endif

    if (!strcmp(name, "StartNoAckMode")) {
        gdbstub->priv->conn.no_ack_mode = true;
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
#ifdef DEBUG
        printf("No-ack mode enabled\n");
#endif
    } else {
        conn_send_pktstr(&gdbstub->priv->conn, "");
    }
}

/* Process vCont action (single action only, no thread selectors)
 *
 * 'C sig' / 'S sig' are treated as 'c' / 's': a bare-metal target has no
 * signals to deliver, and GDB only uses vCont at all when the stub accepts
 * all four actions.
 *
 * TODO: Multi-action support (e.g., "c:all;s:tid") requires:
 * - Loop through ';'-separated actions
 * - Parse thread selector after ':'
 * - Call set_cpu() for each thread-specific action
 */
static inline gdb_event_t process_vcont(gdbstub_t *gdbstub, char *args)
{
    gdb_event_t event = EVENT_NONE;

    if (!args) {
        SEND_EINVAL(gdbstub);
        return event;
    }

    switch (args[0]) {
    case 'c':
    case 'C':
        if (gdbstub->ops->cont != NULL)
            event = EVENT_CONT;
        else
            SEND_EPERM(gdbstub);
        break;
    case 's':
    case 'S':
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

/* GDB disables vCont unless c, C, s and S are all advertised, so only
 * claim support when the target can both continue and step. */
static inline void process_vcont_support(gdbstub_t *gdbstub)
{
    bool ok = gdbstub->ops->cont != NULL && gdbstub->ops->stepi != NULL;
    conn_send_pktstr(&gdbstub->priv->conn, ok ? "vCont;c;C;s;S" : "");
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

/* Reply to a Z/z packet: OK, empty for an unsupported type, or an error */
static void send_bp_reply(gdbstub_t *gdbstub, int ret)
{
    if (ret == 0)
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    else if (ret == ENOTSUP)
        conn_send_pktstr(&gdbstub->priv->conn, "");
    else
        send_errno(gdbstub, ret);
}

/* Watchpoints are only usable when the target can report a hit, so treat
 * Z2-Z4 as unsupported for a target without wp_hit. */
static bool bp_type_supported(gdbstub_t *gdbstub, size_t type)
{
    return type < WP_WRITE || gdbstub->ops->wp_hit != NULL;
}

static void process_del_break_points(gdbstub_t *gdbstub,
                                     char *payload,
                                     void *args)
{
    size_t type, addr, kind;
    if (sscanf(payload, "%zx,%zx,%zx", &type, &addr, &kind) != 3) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("remove breakpoints = %zx %zx %zx\n", type, addr, kind);
#endif

    int ret = bp_type_supported(gdbstub, type)
                  ? gdbstub->ops->del_bp(args, addr, kind, type)
                  : ENOTSUP;
    send_bp_reply(gdbstub, ret);
}

static void process_set_break_points(gdbstub_t *gdbstub,
                                     char *payload,
                                     void *args)
{
    size_t type, addr, kind;
    if (sscanf(payload, "%zx,%zx,%zx", &type, &addr, &kind) != 3) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("set breakpoints = %zx %zx %zx\n", type, addr, kind);
#endif

    int ret = bp_type_supported(gdbstub, type)
                  ? gdbstub->ops->set_bp(args, addr, kind, type)
                  : ENOTSUP;
    send_bp_reply(gdbstub, ret);
}

static void process_set_cpu(gdbstub_t *gdbstub, char *payload, void *args)
{
    int cpuid;
    if ((payload[0] != 'g' && payload[0] != 'c') ||
        !thread_to_cpu(gdbstub, payload + 1, &cpuid)) {
        SEND_EINVAL(gdbstub);
        return;
    }
    /* Hc only arrives when GDB cannot use vCont; the thread it names is
     * then whatever Hg already selected, so only Hg switches the CPU. */
    if (payload[0] == 'g' && cpuid >= 0 && gdbstub->ops->set_cpu != NULL)
        gdbstub->ops->set_cpu(args, cpuid);
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

    inpkt->data[inpkt->end_pos - CSUM_SIZE] = 0;
    uint8_t request = inpkt->data[1];
    char *payload = (char *) &inpkt->data[2];
    gdb_event_t event = EVENT_NONE;

    switch (request) {
    case 'c':
        event = process_cont(gdbstub);
        break;
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
    case 'Q':
        process_general_set(gdbstub, payload);
        break;
    case 's':
        event = process_stepi(gdbstub);
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
        /* Report the last stop; before any resume the target sits in a trap */
        if (gdbstub->priv->last_stop[0])
            conn_send_pktstr(&gdbstub->priv->conn, gdbstub->priv->last_stop);
        else
            send_stop_reply(gdbstub, args, GDB_SIGNAL_TRAP, "");
        break;
    case 'D':
        /* Send OK before detaching per GDB Remote Serial Protocol */
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
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
        process_set_cpu(gdbstub, payload, args);
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
    case 'T': {
        /* Every CPU is alive; anything else is not a thread */
        int cpuid;
        if (thread_to_cpu(gdbstub, payload, &cpuid) && cpuid >= 0)
            conn_send_pktstr(&gdbstub->priv->conn, "OK");
        else
            send_errno(gdbstub, ESRCH);
        break;
    }
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
        /* Clear any interrupt left over from before this run, so a set
         * flag afterwards can only mean a Ctrl-C arrived while running */
        __atomic_store_n(&gdbstub->priv->interrupted, false, __ATOMIC_RELAXED);
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

static void gdbstub_act_resume(gdbstub_t *gdbstub, void *args)
{
    char reason[32] = "";
    size_t addr;
    bp_type_t type;

    /* GDB only treats a stop as a watchpoint hit when the stop reply
     * carries the triggered address, so ask the target first. */
    if (gdbstub->ops->wp_hit != NULL &&
        gdbstub->ops->wp_hit(args, &addr, &type)) {
        const char *kind = type == WP_READ     ? "rwatch"
                           : type == WP_ACCESS ? "awatch"
                                               : "watch";
        snprintf(reason, sizeof(reason), "%s:%zx;", kind, addr);
    }
    /* A Ctrl-C from GDB stops the target with SIGINT, anything else is a
     * trap (breakpoint, watchpoint or single step) */
    int sig = __atomic_exchange_n(&gdbstub->priv->interrupted, false,
                                  __ATOMIC_RELAXED)
                  ? GDB_SIGNAL_INT
                  : GDB_SIGNAL_TRAP;
    send_stop_reply(gdbstub, args, sig, reason);
}

/* Maximum consecutive checksum failures before disconnecting */
#define CONN_MAX_FAILURES 50

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    /* Store user-provided argument in the gdbstub_t structure */
    gdbstub->priv->args = args;

    /* Create reader thread - it's the sole owner of all socket recv() calls.
     * This eliminates the race condition where both threads read the socket. */
    if (!gdbstub->priv->reader_running) {
        gdbstub->priv->thread_stop = false;
        async_io_disable(gdbstub->priv);
        int rc = pthread_create(&gdbstub->priv->tid, NULL, socket_reader,
                                (void *) gdbstub);
        if (rc != 0) {
            perror("pthread_create failed");
            return false;
        }
        gdbstub->priv->reader_running = true;
    }

    conn_t *conn = &gdbstub->priv->conn;

    while (true) {
        /* Blocks until a packet arrives; NULL means the reader thread shut
         * down because the connection closed */
        packet_t *pkt = pktqueue_pop(&gdbstub->priv->pktqueue);
        if (!pkt)
            return true;

        /* Verify checksum before processing */
        bool csum_ok = packet_csum_verify(pkt);
        if (!conn->no_ack_mode)
            conn_send_str(conn, csum_ok ? STR_ACK : STR_NACK);

        if (!csum_ok) {
            free(pkt);

            conn->failure_count++;
            if (conn->failure_count >= CONN_MAX_FAILURES) {
                warn("Too many consecutive failures (%d), disconnecting\n",
                     conn->failure_count);
                return false;
            }
            continue; /* Discard packet and wait for retransmission */
        }

        /* Checksum OK - reset failure counter */
        conn->failure_count = 0;

#ifdef DEBUG
        printf("packet = %s\n", pkt->data);
#endif
        gdb_event_t event = gdbstub_process_packet(gdbstub, pkt, args);
        free(pkt);

        gdb_action_t act = gdbstub_handle_event(gdbstub, event, args);
        switch (act) {
        case ACT_RESUME:
            gdbstub_act_resume(gdbstub, args);
            break;
        case ACT_SHUTDOWN:
            return true;
        default:
            break;
        }
    }
}

void gdbstub_close(gdbstub_t *gdbstub)
{
    /* Signal reader thread to stop and wait for it */
    if (gdbstub->priv->reader_running) {
        __atomic_store_n(&gdbstub->priv->thread_stop, true, __ATOMIC_RELAXED);
        pktqueue_signal_shutdown(&gdbstub->priv->pktqueue);
        pthread_join(gdbstub->priv->tid, NULL);
        gdbstub->priv->reader_running = false;
    }

    pktqueue_destroy(&gdbstub->priv->pktqueue);
    regbuf_destroy(&gdbstub->priv->regbuf);
    conn_close(&gdbstub->priv->conn);
    free(gdbstub->priv);
}
