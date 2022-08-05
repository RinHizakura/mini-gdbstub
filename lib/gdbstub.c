#include "gdbstub.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool gdbstub_init(gdbstub_t *gdbstub, struct target_ops *ops, char *s)
{
    if (s == NULL || ops == NULL)
        return false;

    gdbstub->ops = ops;

    // This is a naive implementation to parse the string
    char *addr_str = strdup(s);
    char *port_str = strchr(addr_str, ':');
    if (addr_str == NULL || port_str == NULL)
        goto fail_1;

    *port_str = '\0';
    port_str += 1;

    gdbstub->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gdbstub->listen_fd < 0)
        goto fail_1;

    struct sockaddr_in addr;
    int port;
    if (sscanf(port_str, "%d", &port) <= 0)
        goto fail_2;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addr_str);
    addr.sin_port = htons(port);
    if (bind(gdbstub->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        goto fail_2;

    if (listen(gdbstub->listen_fd, 1) < 0)
        goto fail_2;

    gdbstub->socket_fd = accept(gdbstub->listen_fd, NULL, NULL);
    if (gdbstub->socket_fd < 0)
        goto fail_2;

    return true;

fail_2:
    close(gdbstub->listen_fd);
fail_1:
    free(addr_str);
    return false;
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    while (true) {
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
    close(gdbstub->socket_fd);
    close(gdbstub->listen_fd);
}
