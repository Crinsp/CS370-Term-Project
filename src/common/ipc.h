#ifndef CS370_IPC_H
#define CS370_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "record.h"

/*
 * Unix-domain SOCK_SEQPACKET plumbing shared by sensord, hub and store
 * (docs/DESIGN.md section 1, IPC contract). One message is exactly one
 * struct cs370_record. Every fd is close-on-exec, every send uses
 * MSG_NOSIGNAL.
 *
 * Functions return -1 with errno set on failure and log the failure to
 * stderr. The exceptions are the "peer is not there / try again" errors
 * listed on each function: those are returned without logging, because
 * the caller retries them or logs them with better context.
 */

#define CS370_HUB_SOCK   "hub.sock"
#define CS370_STORE_SOCK "store.sock"

/* Writes the runtime dir into buf: $CS370_RUN_DIR if set, otherwise
 * $XDG_RUNTIME_DIR/cs370. Creates it (mode 0700) if it does not exist; the
 * parent must exist. Fails with ENOENT if neither variable is set and
 * ENOTDIR if the path exists but is not a directory. */
int cs370_run_dir(char *buf, size_t len);

/* Writes "<dir>/<name>" into buf. Fails with ENAMETOOLONG if the result
 * does not fit buf or a sockaddr_un's sun_path. */
int cs370_sock_path(char *buf, size_t len, const char *dir, const char *name);

/* Listening socket bound to path. A stale socket file left by a previous
 * run is removed first; any other kind of file at path fails with EEXIST. */
int cs370_listen(const char *path);

/* Accepts one connection. EAGAIN, EWOULDBLOCK and EINTR are not logged. */
int cs370_accept(int lfd);

/* Connects to path. ENOENT and ECONNREFUSED (peer not up yet) and EAGAIN
 * (peer's backlog full) are not logged. */
int cs370_connect(const char *path);

/* Sends one record. EPIPE and ECONNRESET (peer gone), EAGAIN, EWOULDBLOCK
 * and EINTR are not logged. */
int cs370_send_record(int fd, const struct cs370_record *r);

/* Receives one message into r. Returns 1 for a 32-byte message, 0 for EOF
 * (peer closed; a zero-length message also reads as EOF, and no sender
 * sends one), -1 on error. A message of any other size fails with EMSGSIZE,
 * is consumed, and leaves *r undefined; it is not logged, so the caller can
 * log the reject with its own context. ECONNRESET, EAGAIN, EWOULDBLOCK and
 * EINTR are not logged either. The record is not validated here. */
int cs370_recv_record(int fd, struct cs370_record *r);

/* Reconnect delay: 500 ms doubling up to 8 s. A zeroed struct is valid and
 * starts at the minimum. */
#define CS370_BACKOFF_MIN_MS 500u
#define CS370_BACKOFF_MAX_MS 8000u

struct cs370_backoff {
    uint32_t next_ms;
};

void cs370_backoff_reset(struct cs370_backoff *b);

/* Returns the delay to wait now and doubles the next one. */
uint32_t cs370_backoff_next(struct cs370_backoff *b);

#endif
