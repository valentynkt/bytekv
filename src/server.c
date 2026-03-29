#include "server.h"
#include "command.h"
#include "event_loop.h"
#include "util.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS_FD 1024
#define FRAME_HDR_SIZE 4
#define WBUF_SIZE ((MSG_MAX + FRAME_HDR_SIZE) * 2)

typedef struct {
    bool active;
    char buf[MSG_MAX + FRAME_HDR_SIZE]; /* read buffer */
    size_t len;                         /* read buffer: bytes accumulated */
    char wbuf[WBUF_SIZE];               /* write buffer */
    size_t wlen;                        /* write buffer: total bytes queued */
    size_t woff;                        /* write buffer: bytes already sent */
} client_t;

typedef struct {
    client_t clients[MAX_CLIENTS_FD];
} server_state_t;

static void on_write(event_loop_t *el, int fd);
static void on_read(event_loop_t *el, int fd);
static void process_buffer(event_loop_t *el, int fd);

static void remove_client(event_loop_t *el, int fd)
{
    server_state_t *state = el->ctx;
    printf("client disconnected (fd=%d)\n", fd);
    el_remove(el, fd);
    close(fd);
    state->clients[fd] = (client_t){0};
}

static bool queue_write(event_loop_t *el, int fd, const char *data, size_t len)
{
    server_state_t *state = el->ctx;
    client_t *c = &state->clients[fd];

    if (c->wlen + len > WBUF_SIZE && c->woff > 0) {
        size_t pending = c->wlen - c->woff;
        memmove(c->wbuf, c->wbuf + c->woff, pending);
        c->wlen = pending;
        c->woff = 0;
    }

    if (c->wlen + len > WBUF_SIZE)
        return false;

    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;

    el_add_write(el, fd, on_write);
    return true;
}

static bool send_framed(event_loop_t *el, int fd, const char *payload, uint32_t payload_len)
{
    char frame[FRAME_HDR_SIZE + MSG_MAX];
    uint32_t net_len = htonl(payload_len);
    memcpy(frame, &net_len, FRAME_HDR_SIZE);
    memcpy(frame + FRAME_HDR_SIZE, payload, payload_len);
    return queue_write(el, fd, frame, FRAME_HDR_SIZE + payload_len);
}

static void process_buffer(event_loop_t *el, int fd)
{
    server_state_t *state = el->ctx;
    client_t *c = &state->clients[fd];

    while (c->len >= FRAME_HDR_SIZE) {
        uint32_t net_len;
        memcpy(&net_len, c->buf, FRAME_HDR_SIZE);
        uint32_t payload_len = ntohl(net_len);

        if (payload_len > MSG_MAX) {
            remove_client(el, fd);
            return;
        }
        if (c->len < FRAME_HDR_SIZE + payload_len)
            return;

        char resp[MSG_MAX];
        char *payload = c->buf + FRAME_HDR_SIZE;
        size_t rlen = command_execute(payload, payload_len, resp, sizeof(resp));
        if (!send_framed(el, fd, resp, (uint32_t)rlen))
            return;

        size_t frame_size = FRAME_HDR_SIZE + payload_len;
        memmove(c->buf, c->buf + frame_size, c->len - frame_size);
        c->len -= frame_size;
    }
}

static void on_write(event_loop_t *el, int fd)
{
    server_state_t *state = el->ctx;
    client_t *c = &state->clients[fd];

    ssize_t n = write(fd, c->wbuf + c->woff, c->wlen - c->woff);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        perror("write");
        remove_client(el, fd);
        return;
    }

    c->woff += n;

    if (c->woff == c->wlen) {
        c->woff = 0;
        c->wlen = 0;
        el_remove_write(el, fd);

        process_buffer(el, fd);
    }
}

static void on_read(event_loop_t *el, int fd)
{
    server_state_t *state = el->ctx;
    client_t *c = &state->clients[fd];

    if (!c->active)
        return;

    ssize_t n = read(fd, c->buf + c->len, sizeof(c->buf) - c->len);

    if (n == 0) {
        remove_client(el, fd);
        return;
    }
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        perror("read");
        remove_client(el, fd);
        return;
    }

    c->len += n;
    process_buffer(el, fd);
}

static void on_accept(event_loop_t *el, int server_fd)
{
    server_state_t *state = el->ctx;

    int fd = accept(server_fd, NULL, NULL);
    if (fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept");
        return;
    }
    if (fd >= MAX_CLIENTS_FD) {
        fprintf(stderr, "fd %d >= MAX_CLIENTS_FD, rejecting\n", fd);
        close(fd);
        return;
    }
    if (set_non_blocking(fd) == -1) {
        close(fd);
        return;
    }

    state->clients[fd] = (client_t){.active = true};
    printf("client connected (fd=%d)\n", fd);

    if (el_add(el, fd, on_read) == -1) {
        close(fd);
        state->clients[fd] = (client_t){0};
    }
}

int run_networking(void)
{
    int server_fd = create_listener();
    if (server_fd == -1)
        return EXIT_FAILURE;

    if (set_non_blocking(server_fd) == -1) {
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("[framing] listening on port %d\n", PORT);

    static server_state_t state;
    memset(&state, 0, sizeof(state));
    event_loop_t el;

    if (el_init(&el, &state) == -1) {
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (el_add(&el, server_fd, on_accept) == -1) {
        close(server_fd);
        el_cleanup(&el);
        return EXIT_FAILURE;
    }

    el_run(&el);

    for (int fd = 0; fd < MAX_CLIENTS_FD; fd++) {
        if (state.clients[fd].active)
            close(fd);
    }
    close(server_fd);
    el_cleanup(&el);
    return EXIT_FAILURE;
}
