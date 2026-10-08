/* shared.h - Common protocol definitions and helpers for Magic Academy (HW6).
 *
 * Included first by hogwarts.c, wizard.c and professor.c.  It also pulls in the
 * POSIX headers everybody needs and provides the partial-write-safe send
 * helpers.  Only `static inline` functions live here so that a translation
 * unit that does not use one of them does not trigger -Wunused-function.
 */
#ifndef SHARED_H
#define SHARED_H

#define _POSIX_C_SOURCE 200809L   /* localtime_r, getopt, sigaction, inet_ntop ... */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---------------------------------------------------------------- limits */
#define MAX_LINE        512   /* maximum length of one protocol line (incl. '\n') */
#define MAX_USERNAME    32
#define MAX_TYPE        16
#define MAX_ING_NAME    17    /* 16 visible chars + NUL terminator                */
#define MAX_INGREDIENTS 256

/* ---------------------------------------------------------------- roles  */
#define ROLE_WIZARD     "WIZARD"
#define ROLE_PROFESSOR  "PROFESSOR"

/* send_all() - write the whole buffer, transparently coping with partial
 * writes and EINTR.  Returns 0 on success, -1 if the connection is broken. */
static inline int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;          /* retry interrupted write */
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* send_line() - append a single '\n' to `msg` (which must not already contain
 * one) and send it reliably.  The line is clipped to MAX_LINE bytes. */
static inline int send_line(int fd, const char *msg)
{
    char tmp[MAX_LINE + 2];
    size_t len = strlen(msg);
    if (len > MAX_LINE) len = MAX_LINE;
    memcpy(tmp, msg, len);
    tmp[len++] = '\n';
    return send_all(fd, tmp, len);
}

#endif /* SHARED_H */
