#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ipc.h"
#include "record.h"

/* Some cases below fail on purpose, so "ipc: ..." lines on stderr are expected. */

static int failures;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                               \
        }                                                             \
    } while (0)

static char base[] = "/tmp/cs370-test-ipc-XXXXXX";

static void join(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", base, name);
}

static int cloexec(int fd)
{
    int f = fcntl(fd, F_GETFD);
    return f >= 0 && (f & FD_CLOEXEC);
}

static struct cs370_record sample(uint32_t seq)
{
    struct cs370_record r;
    memset(&r, 0, sizeof(r));
    r.mono_ns = 123456789ull + seq;
    r.seq = seq;
    r.wall_s = 1761600000u;
    r.rh = 450;
    r.temp_c = 181;
    r.source = CS370_SOURCE_DHT;
    r.flags = CS370_FLAG_FAKE;
    cs370_record_seal(&r);
    return r;
}

static void test_run_dir(void)
{
    char want[PATH_MAX], got[PATH_MAX], file[PATH_MAX];
    struct stat st;

    /* $CS370_RUN_DIR wins, is created 0700, and a second call is fine. */
    join(want, sizeof(want), "run");
    setenv("CS370_RUN_DIR", want, 1);
    CHECK(cs370_run_dir(got, sizeof(got)) == 0);
    CHECK(strcmp(got, want) == 0);
    CHECK(stat(want, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
    CHECK(cs370_run_dir(got, sizeof(got)) == 0);

    /* An existing dir that group/other can reach is refused. */
    CHECK(chmod(want, 0755) == 0);
    errno = 0;
    CHECK(cs370_run_dir(got, sizeof(got)) == -1 && errno == EACCES);
    CHECK(chmod(want, 0700) == 0);
    CHECK(cs370_run_dir(got, sizeof(got)) == 0);

    /* Falls back to $XDG_RUNTIME_DIR/cs370. */
    unsetenv("CS370_RUN_DIR");
    setenv("XDG_RUNTIME_DIR", base, 1);
    join(want, sizeof(want), "cs370");
    CHECK(cs370_run_dir(got, sizeof(got)) == 0);
    CHECK(strcmp(got, want) == 0);
    CHECK(stat(want, &st) == 0 && (st.st_mode & 0777) == 0700);

    /* Neither set. */
    unsetenv("XDG_RUNTIME_DIR");
    errno = 0;
    CHECK(cs370_run_dir(got, sizeof(got)) == -1 && errno == ENOENT);

    /* Path exists but is a regular file. */
    join(file, sizeof(file), "plainfile");
    FILE *f = fopen(file, "w");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    setenv("CS370_RUN_DIR", file, 1);
    errno = 0;
    CHECK(cs370_run_dir(got, sizeof(got)) == -1 && errno == ENOTDIR);

    /* Result does not fit the caller's buffer. */
    char tiny[4];
    errno = 0;
    CHECK(cs370_run_dir(tiny, sizeof(tiny)) == -1 && errno == ENAMETOOLONG);

    unlink(file);
    unsetenv("CS370_RUN_DIR");
}

static void test_sock_path(void)
{
    char path[PATH_MAX], longdir[200];

    CHECK(cs370_sock_path(path, sizeof(path), "/run/x", CS370_HUB_SOCK) == 0);
    CHECK(strcmp(path, "/run/x/hub.sock") == 0);

    /* Fits sun_path but not the caller's buffer. */
    char tiny[8];
    errno = 0;
    CHECK(cs370_sock_path(tiny, sizeof(tiny), "/run/x", CS370_HUB_SOCK) == -1 &&
          errno == ENAMETOOLONG);

    /* Fits PATH_MAX but not sun_path (108 bytes on Linux). */
    memset(longdir, 'a', sizeof(longdir) - 1);
    longdir[sizeof(longdir) - 1] = '\0';
    errno = 0;
    CHECK(cs370_sock_path(path, sizeof(path), longdir, "s") == -1 && errno == ENAMETOOLONG);
    errno = 0;
    CHECK(cs370_listen(longdir) == -1 && errno == ENAMETOOLONG);
    errno = 0;
    CHECK(cs370_connect(longdir) == -1 && errno == ENAMETOOLONG);
}

static void test_round_trip(void)
{
    char path[PATH_MAX];
    struct cs370_record in = sample(7), out;
    uint8_t big[CS370_RECORD_SIZE + 1], small[16];

    join(path, sizeof(path), "rt.sock");
    int lfd = cs370_listen(path);
    CHECK(lfd >= 0);
    int cfd = cs370_connect(path);
    CHECK(cfd >= 0);
    int sfd = cs370_accept(lfd);
    CHECK(sfd >= 0);
    if (lfd < 0 || cfd < 0 || sfd < 0)
        return;

    CHECK(cloexec(lfd) && cloexec(cfd) && cloexec(sfd));

    /* Nothing pending on a nonblocking listener: EAGAIN, a routine result. */
    int fl = fcntl(lfd, F_GETFL);
    CHECK(fl >= 0 && fcntl(lfd, F_SETFL, fl | O_NONBLOCK) == 0);
    errno = 0;
    CHECK(cs370_accept(lfd) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));

    /* A record arrives byte for byte and still validates. */
    CHECK(cs370_send_record(cfd, &in) == 0);
    memset(&out, 0xff, sizeof(out));
    CHECK(cs370_recv_record(sfd, &out) == 1);
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);
    CHECK(cs370_record_valid(&out));

    /* Oversized and undersized messages are rejected and consumed, and the
     * next good record still comes through. */
    memset(big, 0, sizeof(big));
    memset(small, 0, sizeof(small));
    CHECK(send(cfd, big, sizeof(big), MSG_NOSIGNAL) == (ssize_t)sizeof(big));
    CHECK(send(cfd, small, sizeof(small), MSG_NOSIGNAL) == (ssize_t)sizeof(small));
    CHECK(cs370_send_record(cfd, &in) == 0);
    errno = 0;
    CHECK(cs370_recv_record(sfd, &out) == -1 && errno == EMSGSIZE);
    errno = 0;
    CHECK(cs370_recv_record(sfd, &out) == -1 && errno == EMSGSIZE);
    CHECK(cs370_recv_record(sfd, &out) == 1 && memcmp(&in, &out, sizeof(in)) == 0);

    /* Client closes: the server reads EOF. */
    close(cfd);
    CHECK(cs370_recv_record(sfd, &out) == 0);
    close(sfd);

    /* Server side closes: the client's send fails with EPIPE instead of
     * SIGPIPE killing this process (that would be a missing MSG_NOSIGNAL). */
    cfd = cs370_connect(path);
    sfd = cs370_accept(lfd);
    CHECK(cfd >= 0 && sfd >= 0);
    if (cfd >= 0 && sfd >= 0) {
        close(sfd);
        errno = 0;
        CHECK(cs370_send_record(cfd, &in) == -1 && errno == EPIPE);
        close(cfd);
    }

    /* While the listener is live, a second listen refuses to take over its
     * socket file, and the first listener keeps working. */
    struct stat before, after;
    CHECK(lstat(path, &before) == 0);
    errno = 0;
    CHECK(cs370_listen(path) == -1 && errno == EADDRINUSE);
    CHECK(lstat(path, &after) == 0 && after.st_ino == before.st_ino);
    cfd = cs370_connect(path);
    CHECK(cfd >= 0);
    sfd = cs370_accept(lfd);
    CHECK(sfd >= 0);
    if (sfd >= 0)
        close(sfd);
    if (cfd >= 0)
        close(cfd);

    /* Once it is gone, a new listen replaces the stale file, and the path
     * now reaches the new listener (a stale file would refuse the connect).
     * Comparing inode numbers cannot show this: the filesystem may give the
     * new file the number the unlinked one just freed. */
    close(lfd);
    lfd = cs370_listen(path);
    CHECK(lfd >= 0);
    CHECK(lstat(path, &after) == 0 && S_ISSOCK(after.st_mode));
    if (lfd >= 0) {
        cfd = cs370_connect(path);
        CHECK(cfd >= 0);
        sfd = cs370_accept(lfd);
        CHECK(sfd >= 0);
        if (sfd >= 0)
            close(sfd);
        if (cfd >= 0)
            close(cfd);
        close(lfd);
    }
    unlink(path);
}

static void test_connect_and_listen_errors(void)
{
    char path[PATH_MAX];

    /* Nobody listening yet. */
    join(path, sizeof(path), "nobody.sock");
    errno = 0;
    CHECK(cs370_connect(path) == -1 && errno == ENOENT);

    /* Socket file exists but its listener is gone. */
    int lfd = cs370_listen(path);
    CHECK(lfd >= 0);
    if (lfd >= 0)
        close(lfd);
    errno = 0;
    CHECK(cs370_connect(path) == -1 && errno == ECONNREFUSED);
    unlink(path);

    /* listen never deletes a file that is not a socket. */
    join(path, sizeof(path), "notasocket");
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    errno = 0;
    CHECK(cs370_listen(path) == -1 && errno == EEXIST);
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    unlink(path);
}

static void test_backoff(void)
{
    static const uint32_t want[] = {500, 1000, 2000, 4000, 8000, 8000, 8000};
    struct cs370_backoff b;

    cs370_backoff_reset(&b);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        CHECK(cs370_backoff_next(&b) == want[i]);

    cs370_backoff_reset(&b);
    CHECK(cs370_backoff_next(&b) == 500);

    struct cs370_backoff z = {0}; /* zeroed struct starts at the minimum */
    CHECK(cs370_backoff_next(&z) == 500);
    CHECK(cs370_backoff_next(&z) == 1000);
}

int main(void)
{
    char path[PATH_MAX];

    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }

    test_run_dir();
    test_sock_path();
    test_round_trip();
    test_connect_and_listen_errors();
    test_backoff();

    join(path, sizeof(path), "run");
    rmdir(path);
    join(path, sizeof(path), "cs370");
    rmdir(path);
    if (rmdir(base) != 0)
        perror("rmdir test dir");

    if (failures == 0)
        puts("test_ipc: ok");
    return failures ? 1 : 0;
}
