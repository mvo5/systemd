/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/socket.h>

#include "alloc-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "socket-forward.h"

#define SOCKET_FORWARD_BUFFER_SIZE (256 * 1024)

struct SocketForward {
        sd_event *event;

        int server_read_fd, server_write_fd;
        int client_read_fd, client_write_fd;

        /* Track whether read and write fds are the same underlying fd (bidirectional socket).
         * When true, closing the read fd also invalidates the write fd. */
        bool server_bidirectional, client_bidirectional;

        int server_to_client_buffer[2]; /* a pipe */
        int client_to_server_buffer[2]; /* a pipe */

        size_t server_to_client_buffer_full, client_to_server_buffer_full;
        size_t server_to_client_buffer_size, client_to_server_buffer_size;

        /* When read_fd == write_fd (bidirectional socket) we use a single event source per side.
         * When they differ (e.g. stdin/stdout) we need separate sources for read and write. */
        sd_event_source *server_read_event_source, *server_write_event_source;
        sd_event_source *client_read_event_source, *client_write_event_source;

        socket_forward_done_t on_done;
        void *userdata;
};

SocketForward* socket_forward_free(SocketForward *sf) {
        if (!sf)
                return NULL;

        sd_event_source_unref(sf->server_read_event_source);
        sd_event_source_unref(sf->server_write_event_source);
        sd_event_source_unref(sf->client_read_event_source);
        sd_event_source_unref(sf->client_write_event_source);

        safe_close(sf->server_read_fd);
        if (!sf->server_bidirectional)
                safe_close(sf->server_write_fd);

        safe_close(sf->client_read_fd);
        if (!sf->client_bidirectional)
                safe_close(sf->client_write_fd);

        safe_close_pair(sf->server_to_client_buffer);
        safe_close_pair(sf->client_to_server_buffer);

        sd_event_unref(sf->event);

        return mfree(sf);
}

static int socket_forward_create_pipes(int buffer[static 2], size_t *ret_size) {
        int r;

        assert(buffer);
        assert(ret_size);

        if (buffer[0] >= 0)
                return 0;

        r = pipe2(buffer, O_CLOEXEC|O_NONBLOCK);
        if (r < 0)
                return log_debug_errno(errno, "Failed to allocate pipe buffer: %m");

        (void) fcntl(buffer[0], F_SETPIPE_SZ, SOCKET_FORWARD_BUFFER_SIZE);

        r = fcntl(buffer[0], F_GETPIPE_SZ);
        if (r < 0)
                return log_debug_errno(errno, "Failed to get pipe buffer size: %m");

        assert(r > 0);
        *ret_size = r;

        return 0;
}

/* Shovel data in one direction: from -> pipe buffer -> to.
 * On EOF or disconnect, marks the fd as -EBADF but does NOT close it -
 * the caller is responsible for actual fd lifecycle (half-close, shutdown, etc). */
static int socket_forward_shovel(
                int *from, int buffer[2], int *to,
                size_t *full, size_t *sz) {

        bool shoveled;

        assert(from);
        assert(buffer);
        assert(buffer[0] >= 0);
        assert(buffer[1] >= 0);
        assert(to);
        assert(full);
        assert(sz);

        do {
                ssize_t z;

                shoveled = false;

                if (*full < *sz && *from >= 0 && *to >= 0) {
                        z = splice(*from, NULL, buffer[1], NULL, *sz - *full, SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
                        if (z > 0) {
                                *full += z;
                                shoveled = true;
                        } else if (z == 0 || ERRNO_IS_DISCONNECT(errno))
                                *from = -EBADF;
                        else if (!ERRNO_IS_TRANSIENT(errno))
                                return log_debug_errno(errno, "Failed to splice: %m");
                }

                if (*full > 0 && *to >= 0) {
                        z = splice(buffer[0], NULL, *to, NULL, *full, SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
                        if (z > 0) {
                                *full -= z;
                                shoveled = true;
                        } else if (z == 0 || ERRNO_IS_DISCONNECT(errno))
                                *to = -EBADF;
                        else if (!ERRNO_IS_TRANSIENT(errno))
                                return log_debug_errno(errno, "Failed to splice: %m");
                }
        } while (shoveled);

        return 0;
}

static int socket_forward_enable_event_sources(SocketForward *sf);

static int socket_forward_traffic_cb(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        SocketForward *sf = ASSERT_PTR(userdata);
        int r;

        assert(fd >= 0);

        /* Save fd values before shoveling — the shovel marks fds as -EBADF on EOF
         * without closing them, so we need the original values for cleanup. */
        int server_read_fd_saved = sf->server_read_fd,
            client_write_fd_saved = sf->client_write_fd,
            client_read_fd_saved = sf->client_read_fd,
            server_write_fd_saved = sf->server_write_fd;

        /* Shovel server->client */
        r = socket_forward_shovel(
                        &sf->server_read_fd, sf->server_to_client_buffer, &sf->client_write_fd,
                        &sf->server_to_client_buffer_full, &sf->server_to_client_buffer_size);
        if (r < 0)
                goto quit;

        /* Shovel client->server */
        r = socket_forward_shovel(
                        &sf->client_read_fd, sf->client_to_server_buffer, &sf->server_write_fd,
                        &sf->client_to_server_buffer_full, &sf->client_to_server_buffer_size);
        if (r < 0)
                goto quit;

        /* Clean up read fds that hit EOF. For unidirectional fds, close the fd and drop the
         * event source. For bidirectional, the fd stays open for the write side. */
        if (sf->server_read_fd < 0 && server_read_fd_saved >= 0) {
                sf->server_read_event_source = sd_event_source_unref(sf->server_read_event_source);
                if (!sf->server_bidirectional)
                        safe_close(server_read_fd_saved);
        }
        if (sf->client_read_fd < 0 && client_read_fd_saved >= 0) {
                sf->client_read_event_source = sd_event_source_unref(sf->client_read_event_source);
                if (!sf->client_bidirectional)
                        safe_close(client_read_fd_saved);
        }
        /* Clean up write fds that hit disconnect */
        if (sf->client_write_fd < 0 && client_write_fd_saved >= 0) {
                sf->client_write_event_source = sd_event_source_unref(sf->client_write_event_source);
                if (!sf->client_bidirectional)
                        safe_close(client_write_fd_saved);
        }
        if (sf->server_write_fd < 0 && server_write_fd_saved >= 0) {
                sf->server_write_event_source = sd_event_source_unref(sf->server_write_event_source);
                if (!sf->server_bidirectional)
                        safe_close(server_write_fd_saved);
        }

        /* Handle half-close: once a direction's buffer is flushed, shut down the write channel
         * so the remote end receives a FIN. */

        /* server->client direction: buffer flushed, shut down client write */
        if (sf->server_read_fd < 0 && sf->server_to_client_buffer_full == 0 && sf->client_write_fd >= 0) {
                if (sf->client_bidirectional) {
                        (void) shutdown(sf->client_write_fd, SHUT_WR);
                        sf->client_write_fd = -EBADF;
                        /* Drop aliased event source pointer without unref — read side still owns it */
                        sf->client_write_event_source = NULL;
                } else {
                        sf->client_write_event_source = sd_event_source_unref(sf->client_write_event_source);
                        sf->client_write_fd = safe_close(sf->client_write_fd);
                }
        }

        /* client->server direction: buffer flushed, shut down server write */
        if (sf->client_read_fd < 0 && sf->client_to_server_buffer_full == 0 && sf->server_write_fd >= 0) {
                if (sf->server_bidirectional) {
                        (void) shutdown(sf->server_write_fd, SHUT_WR);
                        sf->server_write_fd = -EBADF;
                        sf->server_write_event_source = NULL;
                } else {
                        sf->server_write_event_source = sd_event_source_unref(sf->server_write_event_source);
                        sf->server_write_fd = safe_close(sf->server_write_fd);
                }
        }

        /* Check completion. A direction is done when its read side hit EOF and its
         * buffer is flushed. We quit when both directions are done, OR when one
         * direction is done and the other can't make progress (write destination closed). */
        bool server_to_client_done = sf->server_read_fd < 0 && sf->server_to_client_buffer_full == 0;
        bool client_to_server_done = sf->client_read_fd < 0 && sf->client_to_server_buffer_full == 0;

        if (server_to_client_done && client_to_server_done)
                goto quit;

        /* If one direction is done and the other's write destination is gone, the
         * remaining read side can never deliver data - quit to avoid a hang. */
        if (server_to_client_done && sf->server_write_fd < 0)
                goto quit;
        if (client_to_server_done && sf->client_write_fd < 0)
                goto quit;

        r = socket_forward_enable_event_sources(sf);
        if (r < 0)
                goto quit;

        return 0;

quit:
        if (sf->on_done)
                sf->on_done(sf, r, sf->userdata);

        return 0;
}

static int socket_forward_setup_event_source(
                SocketForward *sf,
                sd_event_source **source,
                int fd,
                uint32_t events) {

        assert(sf);
        assert(source);

        if (*source)
                /* Keep the source online - just update the event mask. set_io_events(0) clears
                 * the mask without disabling the source, which would prevent re-enablement. */
                return sd_event_source_set_io_events(*source, events);

        if (fd < 0 || events == 0)
                return 0;

        return sd_event_add_io(sf->event, source, fd, events, socket_forward_traffic_cb, sf);
}

static int socket_forward_enable_event_sources(SocketForward *sf) {
        uint32_t server_read_events = 0, server_write_events = 0,
                 client_read_events = 0, client_write_events = 0;
        int r;

        assert(sf);

        /* server->client: read from server, write to client */
        if (sf->server_to_client_buffer_full < sf->server_to_client_buffer_size && sf->server_read_fd >= 0)
                server_read_events |= EPOLLIN;
        if (sf->server_to_client_buffer_full > 0 && sf->client_write_fd >= 0)
                client_write_events |= EPOLLOUT;

        /* client->server: read from client, write to server */
        if (sf->client_to_server_buffer_full < sf->client_to_server_buffer_size && sf->client_read_fd >= 0)
                client_read_events |= EPOLLIN;
        if (sf->client_to_server_buffer_full > 0 && sf->server_write_fd >= 0)
                server_write_events |= EPOLLOUT;

        /* For bidirectional fds, combine into a single event source */
        if (sf->server_bidirectional) {
                r = socket_forward_setup_event_source(sf, &sf->server_read_event_source,
                                                      sf->server_read_fd,
                                                      server_read_events | server_write_events);
                if (r < 0)
                        return r;
        } else {
                r = socket_forward_setup_event_source(sf, &sf->server_read_event_source,
                                                      sf->server_read_fd, server_read_events);
                if (r < 0)
                        return r;

                r = socket_forward_setup_event_source(sf, &sf->server_write_event_source,
                                                      sf->server_write_fd, server_write_events);
                if (r < 0)
                        return r;
        }

        if (sf->client_bidirectional) {
                r = socket_forward_setup_event_source(sf, &sf->client_read_event_source,
                                                      sf->client_read_fd,
                                                      client_read_events | client_write_events);
                if (r < 0)
                        return r;
        } else {
                r = socket_forward_setup_event_source(sf, &sf->client_read_event_source,
                                                      sf->client_read_fd, client_read_events);
                if (r < 0)
                        return r;

                r = socket_forward_setup_event_source(sf, &sf->client_write_event_source,
                                                      sf->client_write_fd, client_write_events);
                if (r < 0)
                        return r;
        }

        return 0;
}

int socket_forward_new(
                sd_event *event,
                int server_read_fd,
                int server_write_fd,
                int client_read_fd,
                int client_write_fd,
                socket_forward_done_t on_done,
                void *userdata,
                SocketForward **ret) {

        _cleanup_(socket_forward_freep) SocketForward *sf = NULL;
        int r;

        assert(event);
        assert(server_read_fd >= 0);
        assert(server_write_fd >= 0);
        assert(client_read_fd >= 0);
        assert(client_write_fd >= 0);
        assert(ret);

        sf = new(SocketForward, 1);
        if (!sf) {
                safe_close(server_read_fd);
                if (server_write_fd != server_read_fd)
                        safe_close(server_write_fd);
                safe_close(client_read_fd);
                if (client_write_fd != client_read_fd)
                        safe_close(client_write_fd);
                return -ENOMEM;
        }

        *sf = (SocketForward) {
                .event = sd_event_ref(event),
                .server_read_fd = server_read_fd,
                .server_write_fd = server_write_fd,
                .server_bidirectional = server_read_fd == server_write_fd,
                .client_read_fd = client_read_fd,
                .client_write_fd = client_write_fd,
                .client_bidirectional = client_read_fd == client_write_fd,
                .server_to_client_buffer = EBADF_PAIR,
                .client_to_server_buffer = EBADF_PAIR,
                .on_done = on_done,
                .userdata = userdata,
        };

        r = socket_forward_create_pipes(sf->server_to_client_buffer, &sf->server_to_client_buffer_size);
        if (r < 0)
                return r;

        r = socket_forward_create_pipes(sf->client_to_server_buffer, &sf->client_to_server_buffer_size);
        if (r < 0)
                return r;

        r = socket_forward_enable_event_sources(sf);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(sf);
        return 0;
}
