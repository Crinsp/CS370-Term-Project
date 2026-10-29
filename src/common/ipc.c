#define _GNU_SOURCE /* accept4 */
#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Logs "ipc: <what> <arg>: <strerror>" and returns -1 with errno preserved. */
static int fail(const char *what, const char *arg)
{
    int e = errno;
    fprintf(stderr, "ipc: %s %s: %s\n", what, arg ? arg : "", strerror(e));
    errno = e;
    return -1;
}

/* Closes fd on an error path without losing the errno being reported. */
static void close_keep_errno(int fd)
{
    int e = errno;
    if (fd >= 0 && close(fd) != 0)
        fail("close", NULL);
    errno = e;
}

static int fill_addr(struct sockaddr_un *a, const char *path)
{
    size_t n = strlen(path);

    if (n >= sizeof(a->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, n + 1);
    return 0;
}

int cs370_run_dir(char *buf, size_t len)
{
    const char *env = getenv("CS370_RUN_DIR");
    struct stat st;
    int n;

    if (env && *env) {
        n = snprintf(buf, len, "%s", env);
    } else {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (!xdg || !*xdg) {
            errno = ENOENT;
            return fail("run dir", "(neither CS370_RUN_DIR nor XDG_RUNTIME_DIR is set)");
        }
        n = snprintf(buf, len, "%s/cs370", xdg);
    }
    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        return fail("run dir", env && *env ? env : "$XDG_RUNTIME_DIR/cs370");
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
        return fail("mkdir", buf);
    if (stat(buf, &st) != 0)
        return fail("stat", buf);
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return fail("run dir", buf);
    }
    return 0;
}

int cs370_sock_path(char *buf, size_t len, const char *dir, const char *name)
{
    struct sockaddr_un a;
    int n = snprintf(buf, len, "%s/%s", dir, name);

    if (n < 0 || (size_t)n >= len || (size_t)n >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG;
        return fail("socket path", name);
    }
    return 0;
}

int cs370_listen(const char *path)
{
    struct sockaddr_un addr;
    struct stat st;
    int fd = -1;

    if (fill_addr(&addr, path) != 0)
        return fail("listen path", path);

    /* Remove a socket file left by a previous run (the supervisor runs one
     * instance), but never delete anything that is not a socket. */
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            return fail("listen: not a socket", path);
        }
        if (unlink(path) != 0)
            return fail("unlink", path);
    } else if (errno != ENOENT) {
        return fail("lstat", path);
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail("socket", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fail("bind", path);
        goto err;
    }
    if (listen(fd, 8) != 0) {
        fail("listen", path);
        goto err;
    }
    return fd;

err:
    close_keep_errno(fd);
    return -1;
}

int cs370_accept(int lfd)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

    if (fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        fail("accept", NULL);
    return fd;
}

int cs370_connect(const char *path)
{
    struct sockaddr_un addr;
    int fd = -1;

    if (fill_addr(&addr, path) != 0)
        return fail("connect path", path);

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail("socket", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN)
            fail("connect", path);
        goto err;
    }
    return fd;

err:
    close_keep_errno(fd);
    return -1;
}

int cs370_send_record(int fd, const struct cs370_record *r)
{
    ssize_t n = send(fd, r, sizeof(*r), MSG_NOSIGNAL);

    if (n < 0) {
        if (errno != EPIPE && errno != ECONNRESET && errno != EAGAIN &&
            errno != EWOULDBLOCK && errno != EINTR)
            fail("send", NULL);
        return -1;
    }
    if ((size_t)n != sizeof(*r)) {
        errno = EMSGSIZE; /* SEQPACKET sends are atomic; this should not happen */
        return fail("send: short", NULL);
    }
    return 0;
}

int cs370_recv_record(int fd, struct cs370_record *r)
{
    /* MSG_TRUNC makes recv report the message's real length, so an
     * oversized message is detected instead of silently cut to 32 bytes. */
    ssize_t n = recv(fd, r, sizeof(*r), MSG_TRUNC);

    if (n < 0) {
        if (errno != ECONNRESET && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR)
            fail("recv", NULL);
        return -1;
    }
    if (n == 0)
        return 0;
    if ((size_t)n != sizeof(*r)) {
        errno = EMSGSIZE;
        return -1;
    }
    return 1;
}

void cs370_backoff_reset(struct cs370_backoff *b)
{
    b->next_ms = CS370_BACKOFF_MIN_MS;
}

uint32_t cs370_backoff_next(struct cs370_backoff *b)
{
    uint32_t d = b->next_ms < CS370_BACKOFF_MIN_MS ? CS370_BACKOFF_MIN_MS : b->next_ms;

    if (d > CS370_BACKOFF_MAX_MS)
        d = CS370_BACKOFF_MAX_MS;
    b->next_ms = d >= CS370_BACKOFF_MAX_MS / 2 ? CS370_BACKOFF_MAX_MS : d * 2;
    return d;
}
