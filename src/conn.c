#include "conn.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "utils/csum.h"
#include "utils/log.h"

static bool socket_poll(int socket_fd, int timeout, int events)
{
    struct pollfd pfd = (struct pollfd){
        .fd = socket_fd,
        .events = events,
    };

    return (poll(&pfd, 1, timeout) > 0) && (pfd.revents & events);
}

static bool socket_writable(int socket_fd, int timeout)
{
    return socket_poll(socket_fd, timeout, POLLOUT);
}

bool conn_init(conn_t *conn, char *addr_str, int port)
{
    if (pthread_mutex_init(&conn->send_mutex, NULL) != 0)
        return false;

    struct in_addr addr_ip;
    if (inet_aton(addr_str, &addr_ip) != 0) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = addr_ip.s_addr;
        addr.sin_port = htons(port);
        conn->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (conn->listen_fd < 0)
            goto mutex_fail;

        int optval = 1;
        if (setsockopt(conn->listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                       sizeof(optval)) < 0) {
            warn("Set sockopt fail.\n");
            goto fail;
        }

        if (bind(conn->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) <
            0) {
            warn("Bind fail.\n");
            goto fail;
        }
    } else {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr)); /* Zero before use */
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, addr_str, sizeof(addr.sun_path) - 1);
        unlink(addr_str);
        conn->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (conn->listen_fd < 0)
            goto mutex_fail;

        if (bind(conn->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) <
            0) {
            warn("Bind fail.\n");
            goto fail;
        }
    }

    if (listen(conn->listen_fd, 1) < 0) {
        warn("Listen fail.\n");
        goto fail;
    }

    conn->socket_fd = accept(conn->listen_fd, NULL, NULL);
    if (conn->socket_fd < 0) {
        warn("Accept fail.\n");
        goto fail;
    }

    return true;

fail:
    close(conn->listen_fd);
mutex_fail:
    pthread_mutex_destroy(&conn->send_mutex);
    return false;
}

void conn_send_str(conn_t *conn, char *str)
{
    size_t len = strlen(str);
    int total_waited = 0;

    pthread_mutex_lock(&conn->send_mutex);

    while (len > 0) {
        /* Use bounded poll timeout to prevent indefinite blocking.
         * This allows the system to recover from broken connections
         * and prevents priority inversion with reader thread. */
        if (!socket_writable(conn->socket_fd, CONN_SEND_POLL_MS)) {
            total_waited += CONN_SEND_POLL_MS;
            if (total_waited >= CONN_SEND_TIMEOUT_MS)
                break; /* Total timeout exceeded */
            continue;
        }

        ssize_t nwrite = write(conn->socket_fd, str, len);
        if (nwrite == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break; /* Fatal error */
        }
        str += nwrite;
        len -= nwrite;
        total_waited = 0; /* Reset timeout after successful write */
    }

    pthread_mutex_unlock(&conn->send_mutex);
}

void conn_send_pktstr(conn_t *conn, char *pktstr)
{
    char packet[MAX_SEND_PACKET_SIZE];
    size_t len = strlen(pktstr);

    /* 2: '$' + '#'
     * 2: checksum digits(maximum)
     * 1: '\0' */
    assert(len + 2 + CSUM_SIZE + 1 < MAX_SEND_PACKET_SIZE);

    packet[0] = '$';
    memcpy(packet + 1, pktstr, len);
    packet[len + 1] = '#';

    char csum_str[4];
    uint8_t csum = compute_checksum(pktstr, len);
    size_t csum_len = snprintf(csum_str, sizeof(csum_str) - 1, "%02x", csum);
    assert(csum_len == CSUM_SIZE);
    memcpy(packet + len + 2, csum_str, csum_len);
    packet[len + 2 + csum_len] = '\0';

#ifdef DEBUG
    printf("send packet = %s,", packet);
    printf(" checksum = %d\n", csum);
#endif
    conn_send_str(conn, packet);
}

bool conn_try_send_str(conn_t *conn, char *str)
{
    /* Try to acquire mutex without blocking.
     * This prevents priority inversion where the reader thread
     * blocks on send_mutex while trying to send ACKs. */
    if (pthread_mutex_trylock(&conn->send_mutex) != 0)
        return false; /* Mutex held by another thread */

    size_t len = strlen(str);
    bool success = true;

    /* Use short timeout for non-blocking send */
    while (len > 0) {
        if (!socket_writable(conn->socket_fd, CONN_SEND_POLL_MS)) {
            success = false;
            break;
        }

        ssize_t nwrite = write(conn->socket_fd, str, len);
        if (nwrite == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                success = false;
                break;
            }
            success = false;
            break; /* Fatal error */
        }
        str += nwrite;
        len -= nwrite;
    }

    pthread_mutex_unlock(&conn->send_mutex);
    return success && (len == 0);
}

void conn_close(conn_t *conn)
{
    close(conn->socket_fd);
    close(conn->listen_fd);
    pthread_mutex_destroy(&conn->send_mutex);
}
