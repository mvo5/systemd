/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sd-event.h"

#include "alloc-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "socket-forward.h"

#define SOCKET_FORWARD_BUFFER_SIZE (256 * 1024)

/* Unidirectional forwarder: copies data from read_fd to write_fd through a userspace buffer.
 * Each direction of a full-duplex SocketForward is handled by one of these. */
typedef struct SimplexForward {
        sd_event *event;

        int read_fd, write_fd;

        uint8_t *buffer;
        size_t buffer_start, buffer_end; /* pending data is buffer[buffer_start..buffer_end) */

        sd_event_source *read_event_source, *write_event_source;

        int (*on_done)(struct SimplexForward *fwd, int error, void *userdata);
        void *userdata;
} SimplexForward;

static SimplexForward* simplex_forward_free(SimplexForward *fwd) {
        if (!fwd)
                return NULL;

        sd_event_source_unref(fwd->read_event_source);
        sd_event_source_unref(fwd->write_event_source);

        safe_close(fwd->read_fd);
        safe_close(fwd->write_fd);

        free(fwd->buffer);

        sd_event_unref(fwd->event);

        return mfree(fwd);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(SimplexForward*, simplex_forward_free);

static bool simplex_forward_has_room(const SimplexForward *fwd) {
        assert(fwd);

        /* Pending data is compacted to the front of the buffer when the tail is full. Only do that once
         * half of the buffer has been written out, so that a slow writer doesn't make us move a nearly
         * full buffer for every few bytes it accepts. */
        return fwd->buffer_end < SOCKET_FORWARD_BUFFER_SIZE ||
                fwd->buffer_start >= SOCKET_FORWARD_BUFFER_SIZE / 2;
}

static ssize_t simplex_forward_read(SimplexForward *fwd) {
        assert(fwd);
        assert(fwd->read_fd >= 0);
        assert(simplex_forward_has_room(fwd));

        if (fwd->buffer_start == fwd->buffer_end)
                fwd->buffer_start = fwd->buffer_end = 0;
        else if (fwd->buffer_end >= SOCKET_FORWARD_BUFFER_SIZE) {
                memmove(fwd->buffer, fwd->buffer + fwd->buffer_start, fwd->buffer_end - fwd->buffer_start);
                fwd->buffer_end -= fwd->buffer_start;
                fwd->buffer_start = 0;
        }

        return read(fwd->read_fd, fwd->buffer + fwd->buffer_end, SOCKET_FORWARD_BUFFER_SIZE - fwd->buffer_end);
}

static int simplex_forward_shovel(SimplexForward *fwd) {
        bool shoveled;

        assert(fwd);

        do {
                ssize_t z;

                shoveled = false;

                if (fwd->read_fd >= 0 && fwd->write_fd >= 0 && simplex_forward_has_room(fwd)) {
                        z = simplex_forward_read(fwd);
                        if (z > 0) {
                                fwd->buffer_end += z;
                                shoveled = true;
                        } else if (z == 0 || ERRNO_IS_DISCONNECT(errno)) {
                                fwd->read_event_source = sd_event_source_unref(fwd->read_event_source);
                                fwd->read_fd = safe_close(fwd->read_fd);
                        } else if (!ERRNO_IS_TRANSIENT(errno))
                                return log_debug_errno(errno, "Failed to read from fd %i: %m", fwd->read_fd);
                }

                if (fwd->buffer_end > fwd->buffer_start && fwd->write_fd >= 0) {
                        z = write(fwd->write_fd, fwd->buffer + fwd->buffer_start, fwd->buffer_end - fwd->buffer_start);
                        if (z > 0) {
                                fwd->buffer_start += z;
                                shoveled = true;
                        } else if (z == 0 || ERRNO_IS_DISCONNECT(errno)) {
                                fwd->write_event_source = sd_event_source_unref(fwd->write_event_source);
                                fwd->write_fd = safe_close(fwd->write_fd);
                        } else if (!ERRNO_IS_TRANSIENT(errno))
                                return log_debug_errno(errno, "Failed to write to fd %i: %m", fwd->write_fd);
                }
        } while (shoveled);

        return 0;
}

static int simplex_forward_enable_event_sources(SimplexForward *fwd);

static int simplex_forward_traffic(SimplexForward *fwd) {
        int r;

        r = simplex_forward_shovel(fwd);
        if (r < 0)
                goto quit;

        /* Read side closed and all buffered data written? */
        if (fwd->read_fd < 0 && fwd->buffer_start >= fwd->buffer_end)
                goto quit;

        /* Write side closed? */
        if (fwd->write_fd < 0)
                goto quit;

        r = simplex_forward_enable_event_sources(fwd);
        if (r < 0)
                goto quit;

        return 1;

quit:
        fwd->read_event_source = sd_event_source_disable_unref(fwd->read_event_source);
        fwd->write_event_source = sd_event_source_disable_unref(fwd->write_event_source);
        return fwd->on_done(fwd, r, fwd->userdata);
}

static int simplex_forward_io_cb(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        SimplexForward *fwd = ASSERT_PTR(userdata);

        return simplex_forward_traffic(fwd);
}

static int simplex_forward_defer_cb(sd_event_source *s, void *userdata) {
        SimplexForward *fwd = ASSERT_PTR(userdata);

        return simplex_forward_traffic(fwd);
}

static int simplex_forward_enable_event_sources(SimplexForward *fwd) {
        bool can_read, can_write;
        int r;

        assert(fwd);

        can_read = simplex_forward_has_room(fwd);
        can_write = fwd->buffer_end > fwd->buffer_start;

        /* Event sources may have been unref'd by the shovel on EOF/disconnect */
        if (fwd->read_event_source) {
                r = sd_event_source_set_enabled(fwd->read_event_source, can_read ? SD_EVENT_ONESHOT : SD_EVENT_OFF);
                if (r < 0)
                        return log_debug_errno(r, "Failed to update read event source: %m");
        }

        if (fwd->write_event_source) {
                r = sd_event_source_set_enabled(fwd->write_event_source, can_write ? SD_EVENT_ONESHOT : SD_EVENT_OFF);
                if (r < 0)
                        return log_debug_errno(r, "Failed to update write event source: %m");
        }

        return 0;
}

static int simplex_forward_create_event_source(
                SimplexForward *fwd,
                sd_event_source **ret,
                int fd,
                uint32_t events) {

        int r;

        r = sd_event_add_io(fwd->event, ret, fd, events, simplex_forward_io_cb, fwd);
        if (r == -EPERM)
                /* fd is not pollable (e.g. regular file). Fall back to a defer event source
                 * which fires on each event loop iteration. This works because regular
                 * file are always ready for I/O so we don't need to poll. */
                r = sd_event_add_defer(fwd->event, ret, simplex_forward_defer_cb, fwd);

        return r;
}

static int simplex_forward_new(
                sd_event *event,
                int read_fd,
                int write_fd,
                int (*on_done)(SimplexForward *fwd, int error, void *userdata),
                void *userdata,
                SimplexForward **ret) {

        _cleanup_(simplex_forward_freep) SimplexForward *fwd = NULL;
        int r;

        assert(event);
        assert(read_fd >= 0);
        assert(write_fd >= 0);
        assert(read_fd != write_fd);
        assert(on_done);
        assert(ret);

        fwd = new(SimplexForward, 1);
        if (!fwd) {
                safe_close(read_fd);
                safe_close(write_fd);
                return log_oom_debug();
        }

        *fwd = (SimplexForward) {
                .event = sd_event_ref(event),
                .read_fd = read_fd,
                .write_fd = write_fd,
                .on_done = on_done,
                .userdata = userdata,
        };

        fwd->buffer = new(uint8_t, SOCKET_FORWARD_BUFFER_SIZE);
        if (!fwd->buffer)
                return log_oom_debug();

        /* All forwarding runs in one event loop, a single blocking read() or write() would stall every direction. */
        r = fd_nonblock(fwd->read_fd, true);
        if (r < 0)
                return r;

        r = fd_nonblock(fwd->write_fd, true);
        if (r < 0)
                return r;

        r = simplex_forward_create_event_source(fwd, &fwd->read_event_source, fwd->read_fd, EPOLLIN);
        if (r < 0)
                return r;

        r = simplex_forward_create_event_source(fwd, &fwd->write_event_source, fwd->write_fd, EPOLLOUT);
        if (r < 0)
                return r;

        r = simplex_forward_enable_event_sources(fwd);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(fwd);
        return 0;
}

/* Full-duplex forwarder from two SimplexForward instances */
struct SocketForward {
        SimplexForward *server_to_client;
        SimplexForward *client_to_server;

        socket_forward_done_t on_done;
        void *userdata;

        int first_error;
        unsigned directions_done;
};

SocketForward* socket_forward_free(SocketForward *sf) {
        if (!sf)
                return NULL;

        simplex_forward_free(sf->server_to_client);
        simplex_forward_free(sf->client_to_server);

        return mfree(sf);
}

static int socket_forward_direction_done(SimplexForward *fwd, int error, void *userdata) {
        SocketForward *sf = ASSERT_PTR(userdata);

        /* Half-close the write side so the remote end sees EOF. For sockets,
         * shutdown(SHUT_WR) sends FIN while keeping the fd open for the read side
         * (which belongs to the other direction's dup'd fd). For pipes/FIFOs,
         * shutdown() fails with ENOTSOCK - close the fd instead, which is the
         * only way to signal EOF on a pipe. */
        if (fwd->write_fd >= 0 && shutdown(fwd->write_fd, SHUT_WR) < 0) {
                if (errno == ENOTSOCK)
                        fwd->write_fd = safe_close(fwd->write_fd);
                else
                        log_debug_errno(errno, "Failed to shutdown write side of fd %d: %m, ignoring",
                                        fwd->write_fd);
        }

        if (error < 0 && sf->first_error >= 0)
                sf->first_error = error;

        sf->directions_done++;

        if (sf->directions_done >= 2)
                return sf->on_done(sf, sf->first_error, sf->userdata);

        return 0;
}

int socket_forward_new_pair(
                sd_event *event,
                int server_read_fd,
                int server_write_fd,
                int client_read_fd,
                int client_write_fd,
                socket_forward_done_t on_done,
                void *userdata,
                SocketForward **ret) {

        _cleanup_close_ int server_read_fd_close = server_read_fd,
                            server_write_fd_close = server_write_fd,
                            client_read_fd_close = client_read_fd,
                            client_write_fd_close = client_write_fd;
        _cleanup_(socket_forward_freep) SocketForward *sf = NULL;
        int r;

        assert(event);
        assert(server_read_fd >= 0);
        assert(server_write_fd >= 0);
        assert(client_read_fd >= 0);
        assert(client_write_fd >= 0);
        assert(server_read_fd != server_write_fd);
        assert(client_read_fd != client_write_fd);
        assert(server_read_fd != client_read_fd);
        assert(server_read_fd != client_write_fd);
        assert(server_write_fd != client_read_fd);
        assert(server_write_fd != client_write_fd);
        assert(on_done);
        assert(ret);

        sf = new(SocketForward, 1);
        if (!sf)
                return log_oom_debug();

        *sf = (SocketForward) {
                .on_done = on_done,
                .userdata = userdata,
        };

        r = simplex_forward_new(event,
                                TAKE_FD(server_read_fd_close), TAKE_FD(client_write_fd_close),
                                socket_forward_direction_done, sf,
                                &sf->server_to_client);
        if (r < 0)
                return r;

        r = simplex_forward_new(event,
                                TAKE_FD(client_read_fd_close), TAKE_FD(server_write_fd_close),
                                socket_forward_direction_done, sf,
                                &sf->client_to_server);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(sf);
        return 0;
}

int socket_forward_new(
                sd_event *event,
                int server_fd,
                int client_fd,
                socket_forward_done_t on_done,
                void *userdata,
                SocketForward **ret) {

        _cleanup_close_ int server_fd_close = server_fd, client_fd_close = client_fd,
                            server_write_fd = -EBADF, client_write_fd = -EBADF;

        assert(event);
        assert(server_fd >= 0);
        assert(client_fd >= 0);
        assert(on_done);
        assert(ret);

        server_write_fd = fcntl(server_fd, F_DUPFD_CLOEXEC, 3);
        if (server_write_fd < 0)
                return -errno;

        client_write_fd = fcntl(client_fd, F_DUPFD_CLOEXEC, 3);
        if (client_write_fd < 0)
                return -errno;

        return socket_forward_new_pair(
                        event,
                        TAKE_FD(server_fd_close), TAKE_FD(server_write_fd),
                        TAKE_FD(client_fd_close), TAKE_FD(client_write_fd),
                        on_done, userdata, ret);
}
