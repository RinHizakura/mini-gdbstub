#include "conn.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    pktbuf_init(&conn->in);

    conn->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->listen_fd < 0)
        return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addr_str);
    addr.sin_port = htons(port);
    if (bind(conn->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        goto fail;

    if (listen(conn->listen_fd, 1) < 0)
        goto fail;

    conn->socket_fd = accept(conn->listen_fd, NULL, NULL);
    if (conn->socket_fd < 0)
        goto fail;

    return true;

fail:
    close(conn->listen_fd);
    return false;
}

packet_t *conn_recv_packet(conn_t *conn)
{
    uint8_t buf[4096];

    /* TODO: read a full GDB packet and return to handle it */
    while (!pktbuf_is_complete(&conn->in) &&
           socket_readable(conn->socket_fd, -1)) {
        ssize_t nread = read(conn->socket_fd, buf, sizeof(buf));
        if (nread == -1)
            break;

        pktbuf_fill(&conn->in, buf, nread);
    }

    conn_send_pktstr(conn, PKTSTR_ACK);
    return pktbuf_top_packet(&conn->in);
}

bool conn_send_pktstr(conn_t *conn, char *pktstr)
{
    size_t len = strlen(pktstr);

    while (len > 0 && socket_writable(conn->socket_fd, -1)) {
        ssize_t nwrite = write(conn->socket_fd, pktstr, len);
        /* TODO */
    }
    return true;
}

void conn_close(conn_t *conn)
{
    close(conn->socket_fd);
    close(conn->listen_fd);
}
