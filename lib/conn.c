#include "conn.h"
#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

static bool socket_readable(int socket_fd, int timeout)
{
    return socket_poll(socket_fd, timeout, POLLIN);
}

static bool socket_writable(int socket_fd, int timeout)
{
    return socket_poll(socket_fd, timeout, POLLOUT);
}

bool conn_init(conn_t *conn, char *addr_str, int port)
{
    conn->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->listen_fd < 0)
        return false;

    int optval = 1;
    if (setsockopt(conn->listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        warn("Set sockopt fail.\n");
        goto fail;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addr_str);
    addr.sin_port = htons(port);
    if (bind(conn->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        warn("Bind fail.\n");
        goto fail;
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
    return false;
}

void conn_recv_packet(conn_t *conn, pktbuf_t *pktbuf)
{
    while (!pktbuf_is_complete(pktbuf) &&
           socket_readable(conn->socket_fd, -1)) {
        ssize_t nread = pktbuf_fill_from_file(pktbuf, conn->socket_fd);
        if (nread == -1)
            break;
    }

    /* There must exists a complete packet in packet buffer after the loop */
    assert(pktbuf->end_pos != -1);
    conn_send_str(conn, STR_ACK);
}

void conn_send_str(conn_t *conn, char *str)
{
    size_t len = strlen(str);

    while (len > 0 && socket_writable(conn->socket_fd, -1)) {
        ssize_t nwrite = write(conn->socket_fd, str, len);
        if (nwrite == -1)
            break;
        len -= nwrite;
    }
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

void conn_close(conn_t *conn)
{
    close(conn->socket_fd);
    close(conn->listen_fd);
}
