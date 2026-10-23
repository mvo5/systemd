/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sd-event.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "memory-util.h"
#include "random-util.h"
#include "socket-forward.h"
#include "tests.h"
#include "time-util.h"
#include "tmpfile-util.h"

#define DATA_SIZE (1024U * 1024U)

/* Writes data into *fd in small chunks, then signals EOF: shutdown() on sockets, close() on everything else. */
typedef struct Feeder {
        int *fd;
        const uint8_t *data;
        size_t size, offset;
        sd_event_source *source;
} Feeder;

/* Reads everything from a dup of fd until EOF. The dup is needed since sd-event does not allow two event
 * sources for the same fd, and the Feeder may already watch it. */
typedef struct Collector {
        sd_event *event;
        int fd, dup_fd;
        uint8_t *data;
        size_t size;
        bool eof;
        sd_event_source *source;
        struct Collector *start_on_eof;
} Collector;

typedef struct Done {
        bool done;
        int error;
} Done;

static int feeder_io(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Feeder *f = ASSERT_PTR(userdata);
        ssize_t n;

        n = write(fd, f->data + f->offset, MIN(f->size - f->offset, 4096U));
        if (n < 0) {
                assert_se(errno == EAGAIN);
                return 0;
        }

        f->offset += n;
        if (f->offset < f->size)
                return 0;

        f->source = sd_event_source_disable_unref(f->source);

        if (shutdown(*f->fd, SHUT_WR) < 0) {
                assert_se(errno == ENOTSOCK);
                *f->fd = safe_close(*f->fd);
        }

        return 0;
}

static void feeder_start(Feeder *f, sd_event *event, int *fd, const uint8_t *data, size_t size) {
        *f = (Feeder) {
                .fd = fd,
                .data = data,
                .size = size,
        };

        assert_se(fd_nonblock(*fd, true) >= 0);
        assert_se(sd_event_add_io(event, &f->source, *fd, EPOLLOUT, feeder_io, f) >= 0);
}

static void feeder_done(Feeder *f) {
        f->source = sd_event_source_unref(f->source);
}

static void collector_start(Collector *c);

static int collector_io(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Collector *c = ASSERT_PTR(userdata);
        uint8_t buf[4096];
        ssize_t n;

        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
                assert_se(errno == EAGAIN);
                return 0;
        }

        if (n == 0) {
                c->eof = true;
                c->source = sd_event_source_disable_unref(c->source);

                if (c->start_on_eof)
                        collector_start(c->start_on_eof);

                return 0;
        }

        assert_se(GREEDY_REALLOC(c->data, c->size + n));
        memcpy(c->data + c->size, buf, n);
        c->size += n;

        return 0;
}

static void collector_start(Collector *c) {
        c->dup_fd = fcntl(c->fd, F_DUPFD_CLOEXEC, 3);
        assert_se(c->dup_fd >= 0);
        assert_se(fd_nonblock(c->dup_fd, true) >= 0);
        assert_se(sd_event_add_io(c->event, &c->source, c->dup_fd, EPOLLIN, collector_io, c) >= 0);
}

static void collector_done(Collector *c) {
        c->source = sd_event_source_unref(c->source);
        c->dup_fd = safe_close(c->dup_fd);
        c->data = mfree(c->data);
}

static void collector_verify(const Collector *c, const uint8_t *data, size_t size) {
        assert_se(c->eof);
        assert_se(c->size == size);
        assert_se(memcmp_safe(c->data, data, size) == 0);
}

static int on_forward_done(SocketForward *sf, int error, void *userdata) {
        Done *d = ASSERT_PTR(userdata);

        d->done = true;
        d->error = error;

        return 0;
}

/* The forwarder is done once it has closed or shut down its own fds, our collectors see the resulting EOF
 * one event loop iteration later. */
static void run_until_done(sd_event *event, Done *d, Collector *a, Collector *b) {
        usec_t deadline = usec_add(now(CLOCK_MONOTONIC), 30 * USEC_PER_SEC);

        while (!d->done || !a->eof || !b->eof) {
                assert_se(now(CLOCK_MONOTONIC) < deadline);
                assert_se(sd_event_run(event, USEC_PER_SEC) >= 0);
        }

        assert_se(d->error == 0);
}

static uint8_t* random_data(void) {
        uint8_t *p;

        assert_se(p = new(uint8_t, DATA_SIZE));
        random_bytes(p, DATA_SIZE);

        return p;
}

TEST(sockets) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(socket_forward_freep) SocketForward *sf = NULL;
        _cleanup_free_ uint8_t *to_server = random_data(), *to_client = random_data();
        _cleanup_close_ int server_peer = -EBADF, client_peer = -EBADF;
        _cleanup_(feeder_done) Feeder feed_server = {}, feed_client = {};
        int server[2], client[2];
        Done done = {};

        assert_se(sd_event_new(&event) >= 0);
        assert_se(socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, server) >= 0);
        assert_se(socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, client) >= 0);
        server_peer = server[1];
        client_peer = client[1];

        _cleanup_(collector_done) Collector collect_server = { .event = event, .fd = server_peer, .dup_fd = -EBADF },
                                            collect_client = { .event = event, .fd = client_peer, .dup_fd = -EBADF };

        assert_se(socket_forward_new(event, server[0], client[0], on_forward_done, &done, &sf) >= 0);

        feeder_start(&feed_server, event, &client_peer, to_server, DATA_SIZE);
        feeder_start(&feed_client, event, &server_peer, to_client, DATA_SIZE);
        collector_start(&collect_server);
        collector_start(&collect_client);

        run_until_done(event, &done, &collect_server, &collect_client);

        collector_verify(&collect_server, to_server, DATA_SIZE);
        collector_verify(&collect_client, to_client, DATA_SIZE);
}

/* One direction is not drained at all until the other one has fully completed. The stalled direction
 * must not block the event loop. */
TEST(sockets_backpressure) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(socket_forward_freep) SocketForward *sf = NULL;
        _cleanup_free_ uint8_t *to_server = random_data(), *to_client = random_data();
        _cleanup_close_ int server_peer = -EBADF, client_peer = -EBADF;
        _cleanup_(feeder_done) Feeder feed_server = {}, feed_client = {};
        int server[2], client[2];
        Done done = {};

        assert_se(sd_event_new(&event) >= 0);
        assert_se(socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, server) >= 0);
        assert_se(socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, client) >= 0);
        server_peer = server[1];
        client_peer = client[1];

        _cleanup_(collector_done) Collector collect_server = { .event = event, .fd = server_peer, .dup_fd = -EBADF },
                                            collect_client = { .event = event, .fd = client_peer, .dup_fd = -EBADF, .start_on_eof = &collect_server };

        assert_se(socket_forward_new(event, server[0], client[0], on_forward_done, &done, &sf) >= 0);

        feeder_start(&feed_server, event, &client_peer, to_server, DATA_SIZE);
        feeder_start(&feed_client, event, &server_peer, to_client, DATA_SIZE);
        collector_start(&collect_client);

        run_until_done(event, &done, &collect_server, &collect_client);

        collector_verify(&collect_server, to_server, DATA_SIZE);
        collector_verify(&collect_client, to_client, DATA_SIZE);
}

TEST(pipes) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(socket_forward_freep) SocketForward *sf = NULL;
        _cleanup_free_ uint8_t *to_server = random_data(), *to_client = random_data();
        _cleanup_close_ int server_in_w = -EBADF, server_out_r = -EBADF, client_in_w = -EBADF, client_out_r = -EBADF;
        _cleanup_(feeder_done) Feeder feed_server = {}, feed_client = {};
        int server_in[2], server_out[2], client_in[2], client_out[2];
        Done done = {};

        assert_se(sd_event_new(&event) >= 0);
        assert_se(pipe2(server_in, O_CLOEXEC) >= 0);
        assert_se(pipe2(server_out, O_CLOEXEC) >= 0);
        assert_se(pipe2(client_in, O_CLOEXEC) >= 0);
        assert_se(pipe2(client_out, O_CLOEXEC) >= 0);
        server_in_w = server_in[1];
        server_out_r = server_out[0];
        client_in_w = client_in[1];
        client_out_r = client_out[0];

        _cleanup_(collector_done) Collector collect_server = { .event = event, .fd = server_out_r, .dup_fd = -EBADF },
                                            collect_client = { .event = event, .fd = client_out_r, .dup_fd = -EBADF };

        assert_se(socket_forward_new_pair(
                                  event,
                                  server_in[0], server_out[1],
                                  client_in[0], client_out[1],
                                  on_forward_done, &done, &sf) >= 0);

        feeder_start(&feed_server, event, &client_in_w, to_server, DATA_SIZE);
        feeder_start(&feed_client, event, &server_in_w, to_client, DATA_SIZE);
        collector_start(&collect_server);
        collector_start(&collect_client);

        run_until_done(event, &done, &collect_server, &collect_client);

        collector_verify(&collect_server, to_server, DATA_SIZE);
        collector_verify(&collect_client, to_client, DATA_SIZE);
}

/* Regular files cannot be polled, the forwarder falls back to a defer event source for them. */
TEST(regular_file) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(socket_forward_freep) SocketForward *sf = NULL;
        _cleanup_free_ uint8_t *to_client = random_data();
        _cleanup_close_ int server_out_r = -EBADF, client_out_r = -EBADF;
        _cleanup_(unlink_tempfilep) char path[] = "/tmp/test-socket-forward.XXXXXX";
        int file_fd, server_out[2], client_in[2], client_out[2];
        Done done = {};

        assert_se(sd_event_new(&event) >= 0);

        file_fd = mkostemp_safe(path);
        assert_se(file_fd >= 0);
        assert_se(write(file_fd, to_client, DATA_SIZE) == (ssize_t) DATA_SIZE);
        assert_se(lseek(file_fd, 0, SEEK_SET) == 0);

        assert_se(pipe2(server_out, O_CLOEXEC) >= 0);
        assert_se(pipe2(client_in, O_CLOEXEC) >= 0);
        assert_se(pipe2(client_out, O_CLOEXEC) >= 0);
        server_out_r = server_out[0];
        client_out_r = client_out[0];
        client_in[1] = safe_close(client_in[1]); /* immediate EOF in the client to server direction */

        _cleanup_(collector_done) Collector collect_server = { .event = event, .fd = server_out_r, .dup_fd = -EBADF },
                                            collect_client = { .event = event, .fd = client_out_r, .dup_fd = -EBADF };

        assert_se(socket_forward_new_pair(
                                  event,
                                  file_fd, server_out[1],
                                  client_in[0], client_out[1],
                                  on_forward_done, &done, &sf) >= 0);

        collector_start(&collect_server);
        collector_start(&collect_client);

        run_until_done(event, &done, &collect_server, &collect_client);

        collector_verify(&collect_server, NULL, 0);
        collector_verify(&collect_client, to_client, DATA_SIZE);
}

DEFINE_TEST_MAIN(LOG_DEBUG);
