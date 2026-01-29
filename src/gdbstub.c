#include "gdbstub.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include "conn.h"
#include "gdb_signal.h"
#include "packet.h"
#include "regbuf.h"
#include "utils/csum.h"
#include "utils/translate.h"

/* Maximum memory operation size to prevent DoS via huge allocations */
#define MAX_MEM_XFER_SIZE (MAX_DATA_PAYLOAD / 2)

/* Maximum SMP count for thread info formatting */
#define MAX_SMP_COUNT 10000

struct gdbstub_private {
    conn_t conn;
    regbuf_t regbuf;

    pthread_t tid;
    bool async_io_enable;
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

static volatile bool thread_stop = false;
static void *socket_reader(gdbstub_t *gdbstub)
{
    void *args = gdbstub->priv->args;
    int socket_fd = gdbstub->priv->conn.socket_fd;

    fd_set readfds;
    struct timeval timeout;

    while (!__atomic_load_n(&thread_stop, __ATOMIC_RELAXED)) {
        if (!async_io_is_enable(gdbstub->priv)) {
            usleep(10000);
            continue;
        }

        FD_ZERO(&readfds);
        FD_SET(socket_fd, &readfds);

        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int result = select(socket_fd + 1, &readfds, NULL, NULL, &timeout);

        if (result > 0 && FD_ISSET(socket_fd, &readfds)) {
            char ch;
            ssize_t nread = read(socket_fd, &ch, 1);
            if (nread == 1 && ch == INTR_CHAR) {
                gdbstub->ops->on_interrupt(args);
            }
        } else if (result < 0) {
            perror("select error in socket_reader");
            break;
        }
    }

    return NULL;
}

bool gdbstub_init(gdbstub_t *gdbstub,
                  struct target_ops *ops,
                  arch_info_t arch,
                  const char *s)
{
    char *addr_str = NULL;

    if (!s || !ops)
        return false;

    memset(gdbstub, 0, sizeof(gdbstub_t));
    gdbstub->ops = ops;
    gdbstub->arch = arch;
    gdbstub->priv = calloc(1, sizeof(struct gdbstub_private));
    if (!gdbstub->priv)
        return false;

    /* Precompute total register storage size (register sizes are constant) */
    gdbstub->priv->total_reg_bytes = 0;
    for (int i = 0; i < arch.reg_num; i++)
        gdbstub->priv->total_reg_bytes += ops->get_reg_bytes(i);

    /* Parse address string - check strdup result immediately */
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

    if (!conn_init(&gdbstub->priv->conn, addr_str, port))
        goto conn_fail;

    free(addr_str);
    return true;

conn_fail:
    regbuf_destroy(&gdbstub->priv->regbuf);
addr_fail:
    free(addr_str);
priv_fail:
    free(gdbstub->priv);
    gdbstub->priv = NULL;
    return false;
}

#define SEND_ERR(gdbstub, err) conn_send_pktstr(&gdbstub->priv->conn, err)
#define SEND_EPERM(gdbstub) SEND_ERR(gdbstub, "E01")
#define SEND_EFAULT(gdbstub) SEND_ERR(gdbstub, "E0e")
#define SEND_EINVAL(gdbstub) SEND_ERR(gdbstub, "E16")
#define SEND_ENOMEM(gdbstub) SEND_ERR(gdbstub, "E0c")

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

    for (int i = 0; i < gdbstub->arch.reg_num; i++) {
        size_t reg_sz = gdbstub->ops->get_reg_bytes(i);

        /* Check for buffer overflow before writing */
        if (offset + reg_sz * 2 >= MAX_SEND_PACKET_SIZE) {
            SEND_ENOMEM(gdbstub);
            return;
        }

        void *reg_value = regbuf_get(&gdbstub->priv->regbuf, reg_sz);
        if (!reg_value) {
            SEND_ENOMEM(gdbstub);
            return;
        }

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
            conn_send_pktstr(&gdbstub->priv->conn, packet_str);
            return;
        }
    }

    packet_str[offset] = '\0';
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

    /* Validate register number */
    if (regno < 0 || regno >= gdbstub->arch.reg_num) {
        SEND_EINVAL(gdbstub);
        return;
    }

    size_t reg_sz = gdbstub->ops->get_reg_bytes(regno);
    void *reg_value = regbuf_get(&gdbstub->priv->regbuf, reg_sz);
    if (!reg_value) {
        SEND_ENOMEM(gdbstub);
        return;
    }

    int ret = gdbstub->ops->read_reg(args, regno, reg_value);
#ifdef DEBUG
    char debug_hex[MAX_SEND_PACKET_SIZE];
    hex_to_str((uint8_t *) reg_value, debug_hex, reg_sz);
    printf("reg read = regno %d data 0x%s (size %zu)\n", regno, debug_hex,
           reg_sz);
#endif
    if (!ret) {
        hex_to_str((uint8_t *) reg_value, packet_str, reg_sz);
    } else {
        snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
    }
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
        conn_send_pktstr(&gdbstub->priv->conn, "E22"); /* EINVAL */
        return;
    }

    /* Allocate storage for new values and backup (for rollback) */
    uint8_t *new_values = malloc(total_reg_bytes);
    uint8_t *backup_values = malloc(total_reg_bytes);
    if (!new_values || !backup_values) {
        free(new_values);
        free(backup_values);
        conn_send_pktstr(&gdbstub->priv->conn, "E12"); /* ENOMEM */
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
            char packet_str[16];
            snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
            conn_send_pktstr(&gdbstub->priv->conn, packet_str);
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
        char packet_str[16];
        sprintf(packet_str, "E%d", error_code);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
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

    /* Validate register number */
    if (regno < 0 || regno >= gdbstub->arch.reg_num) {
        SEND_EINVAL(gdbstub);
        return;
    }

    size_t reg_sz = gdbstub->ops->get_reg_bytes(regno);
    void *data = regbuf_get(&gdbstub->priv->regbuf, reg_sz);
    if (!data) {
        SEND_ENOMEM(gdbstub);
        return;
    }

    /* Validate data string length */
    if (strlen(data_str) != reg_sz * 2) {
        SEND_EINVAL(gdbstub);
        return;
    }

    str_to_hex(data_str, (uint8_t *) data, reg_sz);
#ifdef DEBUG
    printf("reg write = regno %d data 0x%s (size %zu)\n", regno, data_str,
           reg_sz);
#endif

    int ret = gdbstub->ops->write_reg(args, regno, data);

    if (!ret) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else {
        char packet_str[16];
        snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    }
}

static void process_mem_read(gdbstub_t *gdbstub, char *payload, void *args)
{
    size_t maddr, mlen;

    if (sscanf(payload, "%zx,%zx", &maddr, &mlen) != 2) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Enforce size limit to prevent DoS */
    if (mlen > MAX_MEM_XFER_SIZE || mlen == 0) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("mem read = addr %zx / len %zx\n", maddr, mlen);
#endif
    char packet_str[MAX_SEND_PACKET_SIZE];

    uint8_t *mval = malloc(mlen);
    if (!mval) {
        SEND_ENOMEM(gdbstub);
        return;
    }

    int ret = gdbstub->ops->read_mem(args, maddr, mlen, mval);
    if (!ret) {
        hex_to_str(mval, packet_str, mlen);
    } else {
        snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
    }
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
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

    if (sscanf(payload, "%zx,%zx", &maddr, &mlen) != 2) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Enforce size limit to prevent DoS */
    if (mlen > MAX_MEM_XFER_SIZE || mlen == 0) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Validate content length matches mlen */
    if (strlen(content) != mlen * 2) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("mem write = addr %zx / len %zx\n", maddr, mlen);
    printf("mem write = content %s\n", content);
#endif
    uint8_t *mval = malloc(mlen);
    if (!mval) {
        SEND_ENOMEM(gdbstub);
        return;
    }

    str_to_hex(content, mval, mlen);
    int ret = gdbstub->ops->write_mem(args, maddr, mlen, mval);

    if (!ret) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else {
        char packet_str[16];
        snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
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

    if (!content) {
        SEND_EINVAL(gdbstub);
        return;
    }

    *content = '\0';
    content++;

    if (sscanf(payload, "%zx,%zx", &maddr, &mlen) != 2) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Enforce size limit to prevent DoS */
    if (mlen > MAX_MEM_XFER_SIZE) {
        SEND_EINVAL(gdbstub);
        return;
    }

    int unescaped_len = unescape(content, (char *) packet_end);
    if (unescaped_len != (int) mlen) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("mem xwrite = addr %zx / len %zx\n", maddr, mlen);
    for (size_t i = 0; i < mlen; i++) {
        printf("\tmem xwrite, byte %zu: %x\n", i, (unsigned char) content[i]);
    }
#endif

    int ret = gdbstub->ops->write_mem(args, maddr, mlen, content);
    if (!ret) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else {
        char packet_str[16];
        snprintf(packet_str, sizeof(packet_str), "E%02x", ret & 0xff);
        conn_send_pktstr(&gdbstub->priv->conn, packet_str);
    }
}


static void process_xfer(gdbstub_t *gdbstub, char *s)
{
    char *name = s;
    char *xfer_args = strchr(s, ':');
    if (xfer_args) {
        *xfer_args = '\0';
        xfer_args++;
    }
#ifdef DEBUG
    printf("xfer = %s %s\n", name, xfer_args);
#endif
    if (!strcmp(name, "features") && gdbstub->arch.target_desc != NULL) {
        /* Parse: read:target.xml:offset,length */
        char *action = strtok(xfer_args, ":");
        if (!action || strcmp(action, "read")) {
            conn_send_pktstr(&gdbstub->priv->conn, "");
            return;
        }

        char *annex = strtok(NULL, ":");
        if (!annex || strcmp(annex, "target.xml")) {
            conn_send_pktstr(&gdbstub->priv->conn, "");
            return;
        }

        char *offset_str = strtok(NULL, ":");
        if (!offset_str) {
            conn_send_pktstr(&gdbstub->priv->conn, "");
            return;
        }

        int offset = 0, length = 0;
        if (sscanf(offset_str, "%x,%x", &offset, &length) != 2) {
            conn_send_pktstr(&gdbstub->priv->conn, "");
            return;
        }

        int total_len = strlen(gdbstub->arch.target_desc);

        /* Validate offset */
        if (offset < 0 || offset >= total_len) {
            conn_send_pktstr(&gdbstub->priv->conn, "l");
            return;
        }

        /* Clamp length to remaining bytes and buffer size */
        int remaining = total_len - offset;
        int max_payload = MAX_SEND_PACKET_SIZE - 2; /* Reserve for 'l'/'m' + NUL
                                                     */
        int payload_length = length;
        if (payload_length > remaining)
            payload_length = remaining;
        if (payload_length > max_payload)
            payload_length = max_payload;

        char buf[MAX_SEND_PACKET_SIZE];
        buf[0] = (remaining <= payload_length) ? 'l' : 'm';
        memcpy(buf + 1, gdbstub->arch.target_desc + offset, payload_length);
        buf[payload_length + 1] = '\0';

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
            snprintf(packet_str, sizeof(packet_str), "QC%04d", cpuid);
            conn_send_pktstr(&gdbstub->priv->conn, packet_str);
        } else
            conn_send_pktstr(&gdbstub->priv->conn, "");
    } else if (!strcmp(name, "Supported")) {
        /* Advertise supported features:
         * - PacketSize: max packet size
         * - qXfer:features:read+: target description XML support
         * - hwbreak+: hardware breakpoint support (Z1 packets)
         * - swbreak+: software breakpoint support (Z0 packets)
         */
        if (gdbstub->arch.target_desc != NULL)
            conn_send_pktstr(
                &gdbstub->priv->conn,
                "PacketSize=1024;qXfer:features:read+;hwbreak+;swbreak+");
        else
            conn_send_pktstr(&gdbstub->priv->conn,
                             "PacketSize=1024;hwbreak+;swbreak+");
    } else if (!strcmp(name, "Attached")) {
        /* assume attached to an existing process */
        conn_send_pktstr(&gdbstub->priv->conn, "1");
    } else if (!strcmp(name, "Xfer")) {
        process_xfer(gdbstub, qargs);
    } else if (!strcmp(name, "Symbol")) {
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    } else if (!strcmp(name, "fThreadInfo")) {
        /* Assume at least 1 CPU if user didn't specific the CPU counts */
        int smp = gdbstub->arch.smp ? gdbstub->arch.smp : 1;

        /* Enforce SMP limit to prevent buffer overflow */
        if (smp >= MAX_SMP_COUNT) {
            smp = MAX_SMP_COUNT - 1;
        }

        char *ptr;
        char cpuid_str[6];

        packet_str[0] = 'm';
        ptr = packet_str + 1;
        for (int cpuid = 0; cpuid < smp; cpuid++) {
            snprintf(cpuid_str, sizeof(cpuid_str), "%04d,", cpuid);
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

/* Process vCont action (single action only, no thread selectors)
 *
 * Currently supported:
 *   'c' - continue
 *   's' - step
 *
 * TODO: Multi-action support (e.g., "c:all;s:tid") requires:
 * - Loop through ';'-separated actions
 * - Parse thread selector after ':'
 * - Call set_cpu() for each thread-specific action
 */
static inline gdb_event_t process_vcont(gdbstub_t *gdbstub, char *args)
{
    gdb_event_t event = EVENT_NONE;

    /* Handle NULL or empty args */
    if (!args || !args[0]) {
        SEND_EINVAL(gdbstub);
        return EVENT_NONE;
    }

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
        /* Reject unsupported actions (including 'C'/'S' with signal) */
        SEND_EPERM(gdbstub);
        break;
    }

    return event;
}

#define VCONT_DESC "vCont;%s%s"

/* Report vCont actions supported by this stub
 *
 * This stub advertises only 'c' (continue) and 's' (step), matching
 * the behavior of other hardware emulators.
 *
 * NOT advertised:
 * - 'C' (continue with signal) / 'S' (step with signal)
 *   Reason: Signal support is for process debugging, not hardware emulation
 *
 * - Thread selectors (e.g., "c:tid", "s:all")
 *   Reason: Current implementation processes single action only
 *   set_cpu() must be called separately via 'H' packet
 */
static inline void process_vcont_support(gdbstub_t *gdbstub)
{
    char packet_str[MAX_SEND_PACKET_SIZE];
    /* Only advertise 'c' and 's' (no signal support for hardware emulation) */
    const char *str_s = (gdbstub->ops->stepi == NULL) ? "" : "s;";
    const char *str_c = (gdbstub->ops->cont == NULL) ? "" : "c;";
    snprintf(packet_str, sizeof(packet_str), VCONT_DESC, str_s, str_c);

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

    if (sscanf(payload, "%zx,%zx,%zx", &type, &addr, &kind) != 3) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Validate breakpoint type */
    if (type > WP_ACCESS) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("remove breakpoints = type %zx addr %zx kind %zx\n", type, addr,
           kind);
#endif

    bool ret = gdbstub->ops->del_bp(args, addr, kind, type);
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

    if (sscanf(payload, "%zx,%zx,%zx", &type, &addr, &kind) != 3) {
        SEND_EINVAL(gdbstub);
        return;
    }

    /* Validate breakpoint type */
    if (type > WP_ACCESS) {
        SEND_EINVAL(gdbstub);
        return;
    }

#ifdef DEBUG
    printf("set breakpoints = type %zx addr %zx kind %zx\n", type, addr, kind);
#endif

    bool ret = gdbstub->ops->set_bp(args, addr, kind, type);
    if (ret)
        conn_send_pktstr(&gdbstub->priv->conn, "OK");
    else
        SEND_EINVAL(gdbstub);
}

static void process_set_cpu(gdbstub_t *gdbstub, char *payload, void *args)
{
    int cpuid;
    /* We don't support deprecated Hc packet, GDB
     * should send only send vCont;c and vCont;s here. */
    if (payload[0] == 'g') {
        if (sscanf(payload, "g%d", &cpuid) != 1) {
            SEND_EINVAL(gdbstub);
            return;
        }
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
    /* Validate packet framing */
    if (inpkt->data[0] != '$')
        return EVENT_NONE;

    /* Validate checksum - reject packet on failure instead of crashing */
    if (!packet_csum_verify(inpkt)) {
#ifdef DEBUG
        printf("checksum verification failed\n");
#endif
        return EVENT_NONE;
    }

    /* After checking the checksum result, ignore those bytes */
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
    snprintf(packet_str, sizeof(packet_str), "S%02x", GDB_SIGNAL_TRAP);
    conn_send_pktstr(&gdbstub->priv->conn, packet_str);
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    /* Bring the user-provided argument in the gdbstub_t structure */
    gdbstub->priv->args = args;

    /* Create a thread to receive interrupt when running the gdbstub op */
    if (gdbstub->ops->on_interrupt != NULL && gdbstub->priv->tid == 0) {
        async_io_disable(gdbstub->priv);
        int ret = pthread_create(&gdbstub->priv->tid, NULL,
                                 (void *) socket_reader, (void *) gdbstub);
        if (ret != 0) {
            perror("pthread_create failed");
            return false;
        }
    }

    while (true) {
        conn_recv_packet(&gdbstub->priv->conn);
        packet_t *pkt = conn_pop_packet(&gdbstub->priv->conn);

        /* Handle NULL packet (connection error) */
        if (!pkt)
            return false;

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
