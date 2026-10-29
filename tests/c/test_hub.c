/*
 * Integration test: runs the hub binary as a child process and plays both
 * of its peers, a sensord client on hub.sock and the store on store.sock.
 * The hub's stderr goes to hub.log in each case's run dir and is printed if
 * that case fails.
 */
#include <errno.h>
#include <fcntl.h>
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

/* SIGTERM, then the hub's exit status, or -1 if it did not exit cleanly
 * within 3 s (it is SIGKILLed so no child outlives the test). */
static int stop_hub(pid_t pid)
{
    int st;

    if (pid <= 0)
        return -1;
    kill(pid, SIGTERM);
    for (int i = 0; i < 300; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
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

static int log_has(const char *needle)
{
    static char buf[1 << 16];
    FILE *f = fopen(log_path, "r");
    size_t n;

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
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
    pid_t pid;

    use_run_dir("late");
    pid = start_hub();
    fds[0] = connect_hub();
    CHECK(pid > 0 && fds[0] >= 0);
    if (pid <= 0 || fds[0] < 0)
        goto out;

    send_range(fds[0], 0, 10);
    sleep_ms(200);
    fds[1] = cs370_listen(store_sock);
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
    pid_t pid;

    use_run_dir("restart");
    fds[1] = cs370_listen(store_sock);
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
    sleep_ms(100);

    send_range(fds[0], 1, 6);
    sleep_ms(100);
    fds[1] = cs370_listen(store_sock);
    fds[2] = accept_timeout(fds[1], 5000);
    CHECK(fds[2] >= 0);
    if (fds[2] >= 0)
        expect_range(fds[2], 1, 6);
    CHECK(log_has("hub store lost"));
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
    finish_run_dir(before);
}

/* 300 records with the store down: the queue keeps the newest 256 (seq
 * 44..299), drops and logs the oldest 44, and nothing else arrives. */
static void test_overflow(void)
{
    int before = failures;
    int fds[3] = {-1, -1, -1}; /* client, store listener, store conn */
    struct cs370_record r;
    pid_t pid;

    use_run_dir("overflow");
    pid = start_hub();
    fds[0] = connect_hub();
    CHECK(pid > 0 && fds[0] >= 0);
    if (pid <= 0 || fds[0] < 0)
        goto out;

    send_range(fds[0], 0, 300);
    sleep_ms(500); /* let the hub read all 300 before the store appears */
    fds[1] = cs370_listen(store_sock);
    fds[2] = accept_timeout(fds[1], 10000);
    CHECK(fds[2] >= 0);
    if (fds[2] >= 0) {
        expect_range(fds[2], 44, 300);
        CHECK(recv_timeout(fds[2], &r, 300) == TIMEOUT);
    }
    CHECK(log_has("hub store drop n=44"));
    CHECK(stop_hub(pid) == 0);
    pid = -1;

out:
    if (pid > 0)
        stop_hub(pid);
    close_all(fds, 3);
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
    test_overflow();

    if (rmdir(base) != 0)
        perror("rmdir test dir");

    if (failures == 0)
        puts("test_hub: ok");
    return failures ? 1 : 0;
}
