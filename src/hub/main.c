/*
 * hub: accepts sensord clients on $CS370_RUN_DIR/hub.sock, validates every
 * record, and forwards the valid ones to the store on store.sock, holding up
 * to 256 records while the store is down (docs/DESIGN.md section 1, IPC
 * contract). Per-source liveness and status.json arrive in a later change.
 *
 * Exit codes: 0 after SIGTERM/SIGINT, 1 on a startup or poll failure.
 */
#define _GNU_SOURCE /* pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "queue.h"
#include "record.h"

#define MAX_CLIENTS  8
#define NS_PER_MS    1000000ull
#define HEARTBEAT_NS (30ull * 1000000000ull) /* CLAUDE.md: heartbeat within 60 s */
#define DROP_LOG_NS  (10ull * 1000000000ull) /* at most one drop line per 10 s */

struct hub {
    int lfd;
    int clients[MAX_CLIENTS];
    int store_fd;
    struct hub_queue q;
    struct cs370_backoff backoff;
    uint64_t next_connect_ns;
    uint64_t next_heartbeat_ns;
    uint64_t last_drop_log_ns;
    uint64_t logged_dropped;
    uint64_t rejected;
    char hub_path[PATH_MAX];
    char store_path[PATH_MAX];
};

static struct hub hub; /* static: the queue is 8 KiB */
static int sig_pipe[2] = {-1, -1};

/* Self-pipe: the handler only writes one byte, the poll loop does the rest. */
static void on_signal(int sig)
{
    int e = errno;
    unsigned char b = (unsigned char)sig;

    /* Nonblocking; if the pipe is full a wakeup is already pending. */
    if (write(sig_pipe[1], &b, 1) < 0) {
        /* nothing async-signal-safe left to do */
    }
    errno = e;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "hub: clock_gettime: %s\n", strerror(errno));
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void close_fd(int *fd, const char *what)
{
    if (*fd >= 0 && close(*fd) != 0)
        fprintf(stderr, "hub: close %s: %s\n", what, strerror(errno));
    *fd = -1;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);

    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        fprintf(stderr, "hub: fcntl O_NONBLOCK: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void schedule_connect(struct hub *h, uint64_t now)
{
    h->next_connect_ns = now + cs370_backoff_next(&h->backoff) * NS_PER_MS;
}

/* Logs the drop total, at most once per DROP_LOG_NS unless forced. */
static void log_drops(struct hub *h, uint64_t now, int force)
{
    if (h->q.dropped == h->logged_dropped)
        return;
    if (!force && h->logged_dropped && now - h->last_drop_log_ns < DROP_LOG_NS)
        return;
    fprintf(stderr, "hub store drop n=%" PRIu64 "\n", h->q.dropped);
    h->logged_dropped = h->q.dropped;
    h->last_drop_log_ns = now;
}

static void store_lost(struct hub *h, const char *why, uint64_t now)
{
    fprintf(stderr, "hub store lost: %s, queued=%zu\n", why, h->q.len);
    close_fd(&h->store_fd, "store");
    cs370_backoff_reset(&h->backoff);
    schedule_connect(h, now);
}

/* Sends queued records until the queue is empty or the socket is full. A
 * record leaves the queue only after its send succeeded. */
static void flush(struct hub *h, uint64_t now)
{
    const struct cs370_record *r;

    while (h->store_fd >= 0 && (r = hub_queue_peek(&h->q)) != NULL) {
        if (cs370_send_record(h->store_fd, r) == 0) {
            hub_queue_pop(&h->q);
            continue;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; /* poll for POLLOUT */
        store_lost(h, strerror(errno), now);
    }
}

static void store_try_connect(struct hub *h, uint64_t now)
{
    int fd = cs370_connect(h->store_path);

    if (fd < 0) {
        int e = errno;
        schedule_connect(h, now);
        if (e == ENOENT || e == ECONNREFUSED || e == EAGAIN)
            fprintf(stderr, "hub store unavailable: %s, retry in %" PRIu64 " ms\n",
                    strerror(e), (uint64_t)((h->next_connect_ns - now) / NS_PER_MS));
        return;
    }
    if (set_nonblock(fd) != 0) {
        close_fd(&fd, "store");
        schedule_connect(h, now);
        return;
    }
    h->store_fd = fd;
    cs370_backoff_reset(&h->backoff);
    fprintf(stderr, "hub store connected, queued=%zu\n", h->q.len);
    log_drops(h, now, 1);
    flush(h, now);
}

/* The store never sends anything; readable means it closed or errored. */
static void handle_store(struct hub *h, short revents, uint64_t now)
{
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        struct cs370_record junk;
        int n = cs370_recv_record(h->store_fd, &junk);

        if (n == 0) {
            store_lost(h, "closed", now);
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
            errno != EMSGSIZE) {
            store_lost(h, strerror(errno), now);
            return;
        }
        if (n == 1 || (n < 0 && errno == EMSGSIZE))
            fprintf(stderr, "hub store sent unexpected data, ignored\n");
    }
    if (h->store_fd >= 0 && (revents & POLLOUT))
        flush(h, now);
}

/* Only called for a record cs370_record_valid() rejected. */
static const char *reject_reason(const struct cs370_record *r)
{
    if (r->crc32 != crc32_of(r, offsetof(struct cs370_record, crc32)))
        return "crc";
    if (r->source >= CS370_SOURCE_COUNT)
        return "source";
    return "invalid";
}

static void handle_client(struct hub *h, int slot, uint64_t now)
{
    struct cs370_record r;
    int fd = h->clients[slot];
    int n = cs370_recv_record(fd, &r);

    if (n == 1) {
        if (!cs370_record_valid(&r)) {
            h->rejected++;
            fprintf(stderr, "hub reject src=%d reason=%s\n", fd, reject_reason(&r));
            return;
        }
        if (hub_queue_push(&h->q, &r))
            log_drops(h, now, 0);
        flush(h, now);
        return;
    }
    if (n == 0) {
        fprintf(stderr, "hub client src=%d closed\n", fd);
        close_fd(&h->clients[slot], "client");
        return;
    }
    if (errno == EMSGSIZE) {
        h->rejected++;
        fprintf(stderr, "hub reject src=%d reason=size\n", fd);
        return;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return;
    fprintf(stderr, "hub client src=%d recv: %s, closing\n", fd, strerror(errno));
    close_fd(&h->clients[slot], "client");
}

static void handle_accept(struct hub *h)
{
    int fd = cs370_accept(h->lfd);

    if (fd < 0)
        return; /* logged by cs370_accept unless EAGAIN/EINTR */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (h->clients[i] < 0) {
            h->clients[i] = fd;
            fprintf(stderr, "hub client src=%d connected\n", fd);
            return;
        }
    }
    fprintf(stderr, "hub client limit %d reached, refusing src=%d\n", MAX_CLIENTS, fd);
    close_fd(&fd, "client");
}

static void heartbeat(struct hub *h, uint64_t now)
{
    int clients = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients += h->clients[i] >= 0;
    fprintf(stderr,
            "hub alive clients=%d queued=%zu dropped=%" PRIu64 " rejected=%" PRIu64
            " store=%s\n",
            clients, h->q.len, h->q.dropped, h->rejected,
            h->store_fd >= 0 ? "up" : "down");
    h->next_heartbeat_ns = now + HEARTBEAT_NS;
}

static int poll_timeout_ms(const struct hub *h, uint64_t now)
{
    uint64_t next = h->next_heartbeat_ns;

    if (h->store_fd < 0 && h->next_connect_ns < next)
        next = h->next_connect_ns;
    if (next <= now)
        return 0;
    uint64_t ms = (next - now + NS_PER_MS - 1) / NS_PER_MS;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

int main(void)
{
    struct hub *h = &hub;
    struct pollfd pfd[3 + MAX_CLIENTS];
    int slot_at[3 + MAX_CLIENTS];
    char dir[PATH_MAX];
    struct sigaction sa;
    int status = 1;
    uint64_t now;

    h->lfd = -1;
    h->store_fd = -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
        h->clients[i] = -1;
    hub_queue_init(&h->q);
    cs370_backoff_reset(&h->backoff);

    if (cs370_run_dir(dir, sizeof(dir)) != 0 ||
        cs370_sock_path(h->hub_path, sizeof(h->hub_path), dir, CS370_HUB_SOCK) != 0 ||
        cs370_sock_path(h->store_path, sizeof(h->store_path), dir, CS370_STORE_SOCK) != 0)
        goto out;

    if (pipe2(sig_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        fprintf(stderr, "hub: pipe2: %s\n", strerror(errno));
        goto out;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "hub: sigaction: %s\n", strerror(errno));
        goto out;
    }

    h->lfd = cs370_listen(h->hub_path);
    if (h->lfd < 0 || set_nonblock(h->lfd) != 0)
        goto out;

    fprintf(stderr, "hub start listen=%s store=%s\n", h->hub_path, h->store_path);
    now = now_ns();
    heartbeat(h, now);
    store_try_connect(h, now);

    for (;;) {
        int nfds = 0, store_idx = -1, first_client;

        pfd[nfds++] = (struct pollfd){.fd = sig_pipe[0], .events = POLLIN};
        pfd[nfds++] = (struct pollfd){.fd = h->lfd, .events = POLLIN};
        if (h->store_fd >= 0) {
            store_idx = nfds;
            pfd[nfds++] = (struct pollfd){
                .fd = h->store_fd,
                .events = (short)(POLLIN | (h->q.len ? POLLOUT : 0)),
            };
        }
        first_client = nfds;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (h->clients[i] >= 0) {
                slot_at[nfds] = i;
                pfd[nfds++] = (struct pollfd){.fd = h->clients[i], .events = POLLIN};
            }
        }

        int rc = poll(pfd, (nfds_t)nfds, poll_timeout_ms(h, now_ns()));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "hub: poll: %s\n", strerror(errno));
            goto out;
        }
        now = now_ns();

        if (pfd[0].revents & POLLIN) {
            unsigned char sig = 0;
            if (read(sig_pipe[0], &sig, 1) < 0 && errno != EAGAIN)
                fprintf(stderr, "hub: read signal pipe: %s\n", strerror(errno));
            fprintf(stderr, "hub stopping on signal %d\n", sig);
            break;
        }
        if (store_idx >= 0 && pfd[store_idx].revents)
            handle_store(h, pfd[store_idx].revents, now);
        for (int k = first_client; k < nfds; k++)
            if (pfd[k].revents)
                handle_client(h, slot_at[k], now);
        if (pfd[1].revents & POLLIN)
            handle_accept(h);
        if (h->store_fd < 0 && now >= h->next_connect_ns)
            store_try_connect(h, now);
        if (now >= h->next_heartbeat_ns)
            heartbeat(h, now);
    }

    flush(h, now_ns()); /* best effort; the store may be slow or gone */
    if (h->q.len)
        fprintf(stderr, "hub exit with queued=%zu undelivered\n", h->q.len);
    status = 0;

out:
    for (int i = 0; i < MAX_CLIENTS; i++)
        close_fd(&h->clients[i], "client");
    close_fd(&h->store_fd, "store");
    if (h->lfd >= 0) {
        close_fd(&h->lfd, "listener");
        if (unlink(h->hub_path) != 0 && errno != ENOENT)
            fprintf(stderr, "hub: unlink %s: %s\n", h->hub_path, strerror(errno));
    }
    close_fd(&sig_pipe[0], "signal pipe");
    close_fd(&sig_pipe[1], "signal pipe");
    fprintf(stderr, "hub exit status=%d\n", status);
    return status;
}
