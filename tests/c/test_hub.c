/*
 * Integration test: runs the hub binary as a child process and plays both
 * of its peers, a sensord client on hub.sock and the store on store.sock.
 * The hub's stderr goes to hub.log in each case's run dir and is printed if
 * that case fails. Instead of sleeping and hoping, tests send SIGUSR1 and
 * wait for the heartbeat line to report the state they need.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "record.h"

#ifndef HUB_BIN
#error "build with -DHUB_BIN='\"path/to/hub\"' (the Makefile does)"
#endif

static int failures;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                               \
        }                                                             \
    } while (0)

#define TIMEOUT -2

static char base[] = "/tmp/cs370-test-hub-XXXXXX";
static char run_dir[256]; /* short /tmp path, so the joins below always fit */
static char hub_sock[PATH_MAX], store_sock[PATH_MAX], log_path[PATH_MAX];

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static void use_run_dir(const char *name)
{
    snprintf(run_dir, sizeof(run_dir), "%s/%s", base, name);
    if (mkdir(run_dir, 0700) != 0)
        perror("mkdir run dir");
    setenv("CS370_RUN_DIR", run_dir, 1);
    snprintf(hub_sock, sizeof(hub_sock), "%s/%s", run_dir, CS370_HUB_SOCK);
    snprintf(store_sock, sizeof(store_sock), "%s/%s", run_dir, CS370_STORE_SOCK);
    snprintf(log_path, sizeof(log_path), "%s/hub.log", run_dir);
}

static void finish_run_dir(int failures_before)
{
    if (failures != failures_before) {
        FILE *f = fopen(log_path, "r");
        char line[512];
        fprintf(stderr, "---- %s ----\n", log_path);
        while (f && fgets(line, sizeof(line), f))
            fputs(line, stderr);
        if (f)
            fclose(f);
        fprintf(stderr, "----\n");
    }
    unlink(log_path);
    unlink(store_sock);
    unlink(hub_sock);
    if (rmdir(run_dir) != 0)
        perror("rmdir run dir");
}

static pid_t start_hub(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDERR_FILENO) < 0)
            _exit(127);
        execl(HUB_BIN, "hub", (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        perror("fork");
    return pid;
}

/* Exit status once the hub exits on its own within ms, else -1 (and it is
 * SIGKILLed so no child outlives the test). */
static int wait_exit(pid_t pid, int ms)
{
    int st;

    for (int i = 0; i < ms / 10; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
}

/* SIGTERM, then the hub's exit status (-1 if it did not exit within 3 s). */
static int stop_hub(pid_t pid)
{
    if (pid <= 0)
        return -1;
    kill(pid, SIGTERM);
    return wait_exit(pid, 3000);
}

/* The hub needs a moment to create hub.sock. */
static int connect_hub(void)
{
    for (int i = 0; i < 300; i++) {
        int fd = cs370_connect(hub_sock);
        if (fd >= 0)
            return fd;
        sleep_ms(10);
    }
    return -1;
}

static int accept_timeout(int lfd, int ms)
{
    struct pollfd p = {.fd = lfd, .events = POLLIN};
    return poll(&p, 1, ms) == 1 ? cs370_accept(lfd) : -1;
}

/* 1 record, 0 EOF, -1 error, TIMEOUT. */
static int recv_timeout(int fd, struct cs370_record *r, int ms)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int rc = poll(&p, 1, ms);
    if (rc == 0)
        return TIMEOUT;
    if (rc < 0)
        return -1;
    return cs370_recv_record(fd, r);
}

static const char *read_log(void)
{
    static char buf[1 << 16];
    FILE *f = fopen(log_path, "r");
    size_t n = 0;

    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static int log_count(const char *needle)
{
    const char *p = read_log();
    int n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static int log_has(const char *needle)
{
    return log_count(needle) > 0;
}

/* Waits up to ms for needle to appear in the log. */
static int wait_log(const char *needle, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (log_has(needle))
            return 1;
        sleep_ms(10);
    }
    return 0;
}

struct hub_status {
    int clients;
    size_t queued;
    uint64_t dropped;
};

/* Asks the hub for a heartbeat (SIGUSR1) and parses the new line. */
static int hub_status(pid_t pid, struct hub_status *s)
{
    int before = log_count("hub alive ");

    kill(pid, SIGUSR1);
    for (int i = 0; i < 100 && log_count("hub alive ") <= before; i++)
        sleep_ms(10);
    if (log_count("hub alive ") <= before)
        return -1;

    const char *p = read_log(), *last = NULL;
    while ((p = strstr(p, "hub alive ")) != NULL)
        last = p++;
    return sscanf(last, "hub alive clients=%d queued=%zu dropped=%" SCNu64,
                  &s->clients, &s->queued, &s->dropped) == 3 ? 0 : -1;
}

/* Waits up to 5 s for min <= queued <= max with exactly `dropped` drops. */
static int wait_queued(pid_t pid, size_t min, size_t max, uint64_t dropped)
{
    struct hub_status s;

    for (int i = 0; i < 100; i++) {
        if (hub_status(pid, &s) == 0 && s.queued >= min && s.queued <= max &&
            s.dropped == dropped)
            return 1;
        sleep_ms(50);
    }
    return 0;
}

static struct cs370_record rec(uint32_t seq)
{
    struct cs370_record r;
    memset(&r, 0, sizeof(r));
    r.mono_ns = 1000000000ull + seq;
    r.seq = seq;
    r.wall_s = 1761600000u;
    r.lux = (float)seq;
    r.source = CS370_SOURCE_LIGHT;
    r.flags = CS370_FLAG_FAKE;
    cs370_record_seal(&r);
    return r;
}

static void send_range(int fd, uint32_t from, uint32_t to)
{
    for (uint32_t s = from; s < to; s++) {
        struct cs370_record r = rec(s);
        CHECK(cs370_send_record(fd, &r) == 0);
    }
}

/* Expects exactly records from..to-1, in order, each byte-identical. */
static void expect_range(int fd, uint32_t from, uint32_t to)
{
    for (uint32_t s = from; s < to; s++) {
        struct cs370_record got, want = rec(s);
        int rc = recv_timeout(fd, &got, 3000);
        CHECK(rc == 1);
        if (rc != 1)
            return;
        CHECK(memcmp(&got, &want, sizeof(got)) == 0);
        if (memcmp(&got, &want, sizeof(got)) != 0) {
            fprintf(stderr, "  expected seq %u, got seq %u\n", s, got.seq);
            return;
        }
    }
}

static void close_all(int *fds, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* How many unread records one SEQPACKET connection holds on this machine
 * (278 on the Pi; it depends on kernel buffer sizes). */
static int socket_capacity(void)
{
    char path[PATH_MAX];
    int lfd, cfd = -1, sfd = -1, n = -1;

    snprintf(path, sizeof(path), "%s/cap.sock", run_dir);
    lfd = cs370_listen(path);
    if (lfd < 0)
        return -1;
    cfd = cs370_connect(path);
    sfd = cfd >= 0 ? cs370_accept(lfd) : -1;
    if (sfd >= 0 && fcntl(cfd, F_SETFL, O_NONBLOCK) == 0) {
        struct cs370_record r = rec(0);
        n = 0;
        while (cs370_send_record(cfd, &r) == 0)
            n++;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            n = -1;
    }
    if (sfd >= 0)
        close(sfd);
    if (cfd >= 0)
        close(cfd);
    close(lfd);
    unlink(path);
    return n;
}

/* Store up from the start: records are forwarded unchanged, invalid ones are
 * rejected without dropping the client, two clients both get through, and
 * SIGTERM exits 0, unlinks hub.sock and closes the store connection. */
static void test_forward(void)
{
    int before = failures;
    int fds[4] = {-1, -1, -1, -1}; /* store listener, store conn, client 1, client 2 */
    struct cs370_record r;
    pid_t pid = -1;

    use_run_dir("forward");
    fds[0] = cs370_listen(store_sock);
    CHECK(fds[0] >= 0);
    if (fds[0] < 0)
        goto out;
    pid = start_hub();
    fds[2] = connect_hub();
    fds[1] = accept_timeout(fds[0], 3000);
    CHECK(pid > 0 && fds[1] >= 0 && fds[2] >= 0);
    if (pid <= 0 || fds[1] < 0 || fds[2] < 0)
        goto out;

    send_range(fds[2], 0, 5);
    expect_range(fds[1], 0, 5);

    /* Bad CRC, unknown source, wrong size: none reach the store, and the
     * same client's next record still does. */
    r = rec(100);
    r.crc32 ^= 1;
    CHECK(cs370_send_record(fds[2], &r) == 0);
    r = rec(101);
    r.source = CS370_SOURCE_COUNT;
    cs370_record_seal(&r);
    CHECK(cs370_send_record(fds[2], &r) == 0);
    CHECK(send(fds[2], "short", 5, MSG_NOSIGNAL) == 5);
    send_range(fds[2], 5, 6);
    expect_range(fds[1], 5, 6);
    CHECK(log_has("reason=crc"));
    CHECK(log_has("reason=source"));
    CHECK(log_has("reason=size"));

    fds[3] = connect_hub();
    CHECK(fds[3] >= 0);
    if (fds[3] >= 0) {
        send_range(fds[3], 6, 7);
        expect_range(fds[1], 6, 7);
    }

    CHECK(stop_hub(pid) == 0);
    pid = -1;
    CHECK(access(hub_sock, F_OK) != 0);
    CHECK(recv_timeout(fds[1], &r, 1000) == 0);
    CHECK(log_has("hub alive"));

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 4);
    finish_run_dir(before);
}

/* Store not up yet: records wait in the queue and arrive in order once the
 * store starts and the hub's backoff retry connects. */
static void test_store_late(void)
{
    int before = failures;
    int fds[3] = {-1, -1, -1}; /* client, store listener, store conn */
    pid_t pid = -1;

    use_run_dir("late");
    pid = start_hub();
    fds[0] = connect_hub();
    CHECK(pid > 0 && fds[0] >= 0);
    if (pid <= 0 || fds[0] < 0)
        goto out;

    send_range(fds[0], 0, 10);
    CHECK(wait_queued(pid, 10, 10, 0));
    fds[1] = cs370_listen(store_sock);
    CHECK(fds[1] >= 0);
    if (fds[1] < 0)
        goto out;
    fds[2] = accept_timeout(fds[1], 5000); /* retries at 0.5 s, 1.5 s, ... */
    CHECK(fds[2] >= 0);
    if (fds[2] >= 0)
        expect_range(fds[2], 0, 10);
    CHECK(log_has("hub store unavailable"));
    CHECK(log_has("hub store connected, queued="));
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
    finish_run_dir(before);
}

/* Store dies mid-run: the hub notices, keeps what the client sends meanwhile,
 * reconnects when the store comes back, and delivers it all in order. */
static void test_store_restart(void)
{
    int before = failures;
    int fds[3] = {-1, -1, -1}; /* client, store listener, store conn */
    pid_t pid = -1;

    use_run_dir("restart");
    fds[1] = cs370_listen(store_sock);
    CHECK(fds[1] >= 0);
    if (fds[1] < 0)
        goto out;
    pid = start_hub();
    fds[0] = connect_hub();
    fds[2] = accept_timeout(fds[1], 3000);
    CHECK(pid > 0 && fds[0] >= 0 && fds[2] >= 0);
    if (pid <= 0 || fds[0] < 0 || fds[2] < 0)
        goto out;

    send_range(fds[0], 0, 1);
    expect_range(fds[2], 0, 1);

    close(fds[2]);
    close(fds[1]);
    fds[1] = fds[2] = -1;
    unlink(store_sock);
    CHECK(wait_log("hub store lost", 3000));

    send_range(fds[0], 1, 6);
    CHECK(wait_queued(pid, 5, 5, 0));
    fds[1] = cs370_listen(store_sock);
    CHECK(fds[1] >= 0);
    if (fds[1] < 0)
        goto out;
    fds[2] = accept_timeout(fds[1], 5000);
    CHECK(fds[2] >= 0);
    if (fds[2] >= 0)
        expect_range(fds[2], 1, 6);
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
    finish_run_dir(before);
}

/* A store that accepts and dies at once is retried with growing delays,
 * and the backoff resets only once a record is actually delivered. */
static void test_store_crash_loop(void)
{
    int before = failures;
    int fds[3] = {-1, -1, -1}; /* client, store listener, store conn */
    pid_t pid = -1;

    use_run_dir("crashloop");
    fds[1] = cs370_listen(store_sock);
    CHECK(fds[1] >= 0);
    if (fds[1] < 0)
        goto out;
    pid = start_hub();
    CHECK(pid > 0);
    if (pid <= 0)
        goto out;

    for (int i = 0; i < 3; i++) {
        int s = accept_timeout(fds[1], 5000);
        CHECK(s >= 0);
        if (s < 0)
            goto out;
        close(s);
    }
    CHECK(wait_log("queued=0, retry in 2000 ms", 3000));
    CHECK(log_has("queued=0, retry in 500 ms"));
    CHECK(log_has("queued=0, retry in 1000 ms"));

    /* A healthy connection that delivers a record resets the backoff. */
    fds[0] = connect_hub();
    fds[2] = accept_timeout(fds[1], 5000);
    CHECK(fds[0] >= 0 && fds[2] >= 0);
    if (fds[0] < 0 || fds[2] < 0)
        goto out;
    send_range(fds[0], 0, 1);
    expect_range(fds[2], 0, 1);
    close(fds[2]);
    fds[2] = -1;
    /* Without the reset this retry would be 4000 ms. */
    for (int i = 0; i < 300 && log_count("queued=0, retry in 500 ms") < 2; i++)
        sleep_ms(10);
    CHECK(log_count("queued=0, retry in 500 ms") == 2);
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
    finish_run_dir(before);
}

/* Store connected but not reading: once its socket is full, the hub keeps
 * records in its queue (the EAGAIN/POLLOUT path) and delivers every one in
 * order when the store catches up. */
static void test_store_full(void)
{
    int before = failures;
    int fds[3] = {-1, -1, -1}; /* client, store listener, store conn */
    pid_t pid = -1;
    int cap;

    use_run_dir("full");
    cap = socket_capacity();
    CHECK(cap > 0);
    if (cap <= 0)
        goto out;
    uint32_t n = (uint32_t)cap + 100; /* overfills the socket, fits the 256-record queue */

    fds[1] = cs370_listen(store_sock);
    CHECK(fds[1] >= 0);
    if (fds[1] < 0)
        goto out;
    pid = start_hub();
    fds[0] = connect_hub();
    fds[2] = accept_timeout(fds[1], 3000);
    CHECK(pid > 0 && fds[0] >= 0 && fds[2] >= 0);
    if (pid <= 0 || fds[0] < 0 || fds[2] < 0)
        goto out;

    send_range(fds[0], 0, n);
    CHECK(wait_queued(pid, 1, 256, 0)); /* queue in use while the store is up */
    expect_range(fds[2], 0, n);
    CHECK(wait_queued(pid, 0, 0, 0));
    CHECK(!log_has("hub store lost"));
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
    finish_run_dir(before);
}

/* 300 records with the store down: the queue keeps the newest 256 (seq
 * 44..299), drops the oldest 44, and nothing else arrives. The drop line is
 * rate-limited: one at the first drop, one with the total on reconnect. */
static void test_overflow(void)
{
    int before = failures;
    int fds[3] = {-1, -1, -1}; /* client, store listener, store conn */
    struct cs370_record r;
    pid_t pid = -1;

    use_run_dir("overflow");
    pid = start_hub();
    fds[0] = connect_hub();
    CHECK(pid > 0 && fds[0] >= 0);
    if (pid <= 0 || fds[0] < 0)
        goto out;

    send_range(fds[0], 0, 300);
    CHECK(wait_queued(pid, 256, 256, 44));
    fds[1] = cs370_listen(store_sock);
    CHECK(fds[1] >= 0);
    if (fds[1] < 0)
        goto out;
    fds[2] = accept_timeout(fds[1], 10000);
    CHECK(fds[2] >= 0);
    if (fds[2] >= 0) {
        expect_range(fds[2], 44, 300);
        CHECK(recv_timeout(fds[2], &r, 300) == TIMEOUT);
    }
    CHECK(log_has("hub store drop n=1\n"));
    CHECK(log_has("hub store drop n=44\n"));
    CHECK(log_count("hub store drop n=") == 2);
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
    finish_run_dir(before);
}

/* A 9th client is accepted and closed with a log line; the first 8 keep
 * working. Clients send one at a time, so the expected order is fixed. */
static void test_client_limit(void)
{
    int before = failures;
    int fds[11]; /* store listener, store conn, 9 clients */
    struct cs370_record r;
    pid_t pid = -1;

    for (int i = 0; i < 11; i++)
        fds[i] = -1;
    use_run_dir("limit");
    fds[0] = cs370_listen(store_sock);
    CHECK(fds[0] >= 0);
    if (fds[0] < 0)
        goto out;
    pid = start_hub();
    fds[2] = connect_hub();
    fds[1] = accept_timeout(fds[0], 3000);
    CHECK(pid > 0 && fds[1] >= 0 && fds[2] >= 0);
    if (pid <= 0 || fds[1] < 0 || fds[2] < 0)
        goto out;

    for (uint32_t i = 0; i < 8; i++) {
        if (i > 0)
            fds[2 + i] = connect_hub();
        CHECK(fds[2 + i] >= 0);
        if (fds[2 + i] < 0)
            goto out;
        send_range(fds[2 + i], i, i + 1);
        expect_range(fds[1], i, i + 1);
    }

    fds[10] = connect_hub();
    CHECK(fds[10] >= 0);
    if (fds[10] >= 0)
        CHECK(recv_timeout(fds[10], &r, 3000) == 0); /* hub closed it */
    CHECK(log_has("hub client limit 8 reached"));

    send_range(fds[2], 8, 9);
    expect_range(fds[1], 8, 9);
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 11);
    finish_run_dir(before);
}

/* SIGTERM with records queued: still exits 0 and says what it lost. */
static void test_shutdown_queued(void)
{
    int before = failures;
    int cfd = -1;
    pid_t pid = -1;

    use_run_dir("shutdown");
    pid = start_hub();
    cfd = connect_hub();
    CHECK(pid > 0 && cfd >= 0);
    if (pid > 0 && cfd >= 0) {
        send_range(cfd, 0, 5);
        CHECK(wait_queued(pid, 5, 5, 0));
        CHECK(stop_hub(pid) == 0);
        pid = -1;
        CHECK(log_has("hub exit with queued=5 undelivered"));
    }
    if (pid > 0)
        stop_hub(pid);
    if (cfd >= 0)
        close(cfd);
    finish_run_dir(before);
}

/* An unusable run dir (group/other accessible) is a startup failure: exit 1
 * without creating hub.sock. */
static void test_startup_failure(void)
{
    int before = failures;
    pid_t pid = -1;

    use_run_dir("badstart");
    CHECK(chmod(run_dir, 0755) == 0);
    pid = start_hub();
    CHECK(pid > 0);
    if (pid > 0)
        CHECK(wait_exit(pid, 3000) == 1);
    CHECK(access(hub_sock, F_OK) != 0);
    CHECK(log_has("run dir must be mode 0700"));
    chmod(run_dir, 0700);
    finish_run_dir(before);
}

int main(void)
{
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }

    test_forward();
    test_store_late();
    test_store_restart();
    test_store_crash_loop();
    test_store_full();
    test_overflow();
    test_client_limit();
    test_shutdown_queued();
    test_startup_failure();

    if (rmdir(base) != 0)
        perror("rmdir test dir");

    if (failures == 0)
        puts("test_hub: ok");
    return failures ? 1 : 0;
}
