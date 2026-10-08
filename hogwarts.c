/* hogwarts.c - Magic Academy server (CSE 344 HW6).
 *
 * Single-threaded TCP server.  Every socket - the listening socket and all
 * connected clients - is multiplexed through one select() loop.  There is no
 * thread, no fork-per-client and no sleep(): the idle timeout is enforced by
 * recomputing the select() timeout before every call.
 *
 *   ./hogwarts -p <tcp_port> -s <ingredients.txt> -l <logfile>
 *              -n <max_clients> -t <timeout>
 */
#include "shared.h"

#include <sys/select.h>
#include <stdarg.h>

/* ============================================================ ingredients */

typedef struct {
    char name[MAX_ING_NAME];
    long qty;                 /* global stock, never negative */
} ingredient_t;

static ingredient_t g_ing[MAX_INGREDIENTS];
static int          g_ning = 0;

/* ========================================================== per-client state */

typedef struct {
    int    fd;                          /* -1 == free slot                      */
    char   username[MAX_USERNAME];
    char   type[MAX_TYPE];              /* "WIZARD" / "PROFESSOR" / ""           */
    int    enrolled;                    /* 1 once a valid ENROLL was processed   */
    char   line_buf[MAX_LINE];          /* partial-read accumulator              */
    size_t buf_len;                     /* bytes currently held in line_buf      */
    int    overlong;                    /* 1 while discarding an over-long line  */
    time_t last_active;                 /* timestamp of last byte received       */
    long   spellbook[MAX_INGREDIENTS];  /* WIZARD only: per-ingredient holdings  */
} client_t;

static client_t *g_clients     = NULL;
static int       g_max_clients = 0;

/* ============================================================ globals */

static volatile sig_atomic_t g_stop      = 0;
static FILE                 *g_logf      = NULL;
static int                   g_listen_fd = -1;

static void on_sigint(int sig) { (void)sig; g_stop = 1; }   /* async-signal-safe */

/* ============================================================ logging */

/* Writes one timestamped line to both stdout and the -l logfile.
 * Format:  [YYYY-MM-DD HH:MM:SS] KEYWORD field=value ...                       */
static void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;

    fprintf(stdout, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);

    if (g_logf) {
        fprintf(g_logf, "[%s] ", ts);
        va_start(ap, fmt);
        vfprintf(g_logf, fmt, ap);
        va_end(ap);
        fputc('\n', g_logf);
        fflush(g_logf);
    }
}

/* Bounded "append printf" used to build SPELLBOOK / SCROLL / ROSTER replies. */
static void str_append(char *dst, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *len += (size_t)n;
    if (*len >= cap) *len = cap - 1;            /* clamp on truncation */
}

/* ============================================================ ingredients */

static int ing_find(const char *name)
{
    for (int i = 0; i < g_ning; i++)
        if (strcmp(g_ing[i].name, name) == 0) return i;
    return -1;
}

/* uppercase letters / digits / underscore, 1..16 chars */
static int valid_ing_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static void load_ingredients(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen ingredients"); exit(EXIT_FAILURE); }

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char name[64];
        long q;
        if (sscanf(line, "%63s %ld", name, &q) != 2) continue;   /* bad format */
        if (!valid_ing_name(name))                  continue;
        if (q <= 0)                                 continue;
        if (g_ning >= MAX_INGREDIENTS)              break;
        if (ing_find(name) >= 0)                    continue;    /* duplicate   */
        strncpy(g_ing[g_ning].name, name, MAX_ING_NAME - 1);
        g_ing[g_ning].name[MAX_ING_NAME - 1] = '\0';
        g_ing[g_ning].qty = q;
        g_ning++;
    }
    fclose(f);
}

/* ============================================================ client slots */

static client_t *slot_free(void)
{
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].fd < 0) return &g_clients[i];
    return NULL;
}

static int slot_count(void)
{
    int c = 0;
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].fd >= 0) c++;
    return c;
}

static int username_taken(const char *u)
{
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].fd >= 0 && g_clients[i].enrolled &&
            strcmp(g_clients[i].username, u) == 0) return 1;
    return 0;
}

static void client_reset(client_t *c)
{
    c->fd          = -1;
    c->username[0] = '\0';
    c->type[0]     = '\0';
    c->enrolled    = 0;
    c->buf_len     = 0;
    c->overlong    = 0;
    c->last_active = 0;
    memset(c->spellbook, 0, sizeof c->spellbook);
}

static void client_close(client_t *c)        /* close fd, free the slot */
{
    if (c->fd < 0) return;
    close(c->fd);
    client_reset(c);
}

static const char *uname_or_dash(const client_t *c)
{
    return c->enrolled ? c->username : "-";
}

/* ============================================================ command parsing */

/* Splits `s` in place on runs of spaces/tabs.  Returns argc. */
static int split_ws(char *s, char *argv[], int maxv)
{
    int argc = 0;
    char *p = s;
    while (*p && argc < maxv) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static int parse_pos_long(const char *s, long *out)
{
    if (!s || !*s) return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v <= 0) return 0;
    *out = v;
    return 1;
}

/* Processes one complete protocol line for client `c`.
 * Returns 1 if the client must be disconnected (APPARATE), 0 otherwise.       */
static int handle_line(client_t *c, char *line)
{
    char *argv[8];
    int argc = split_ws(line, argv, 8);
    if (argc == 0) return 0;                                  /* blank line */

    const char *cmd = argv[0];

    /* ----------------------------------------- not enrolled yet */
    if (!c->enrolled) {
        if (strcmp(cmd, "ENROLL") == 0) {
            if (argc < 3) { send_line(c->fd, "ERR UNKNOWN ENROLL"); return 0; }
            const char *role  = argv[1];
            const char *uname = argv[2];
            if (strcmp(role, ROLE_WIZARD) != 0 && strcmp(role, ROLE_PROFESSOR) != 0) {
                send_line(c->fd, "ERR UNKNOWN ENROLL");
                return 0;
            }
            if (strlen(uname) >= MAX_USERNAME || username_taken(uname)) {
                send_line(c->fd, "ERR ENROLL name_taken");
                return 0;
            }
            strncpy(c->username, uname, MAX_USERNAME - 1);
            c->username[MAX_USERNAME - 1] = '\0';
            strncpy(c->type, role, MAX_TYPE - 1);
            c->type[MAX_TYPE - 1] = '\0';
            c->enrolled = 1;

            char resp[MAX_LINE];
            snprintf(resp, sizeof resp, "OK ENROLL %s", c->username);
            send_line(c->fd, resp);
            logmsg("ENROLL username=%s type=%s fd=%d", c->username, c->type, c->fd);
            return 0;
        }
        if (strcmp(cmd, "APPARATE") == 0) { send_line(c->fd, "OK APPARATE"); return 1; }
        send_line(c->fd, "ERR NOT_ENROLLED");
        return 0;
    }

    /* ----------------------------------------- enrolled */
    int is_wiz = (strcmp(c->type, ROLE_WIZARD) == 0);

    if (strcmp(cmd, "APPARATE") == 0) { send_line(c->fd, "OK APPARATE"); return 1; }

    if (strcmp(cmd, "ENROLL") == 0) { send_line(c->fd, "ERR UNKNOWN ENROLL"); return 0; }

    /* ---- BREW / CONSUME : wizard only -------------------------------- */
    if (strcmp(cmd, "BREW") == 0 || strcmp(cmd, "CONSUME") == 0) {
        if (!is_wiz) { send_line(c->fd, "ERR UNAUTHORIZED"); return 0; }

        long qty;
        if (argc < 3 || !valid_ing_name(argv[1]) || !parse_pos_long(argv[2], &qty)) {
            char e[MAX_LINE];
            snprintf(e, sizeof e, "ERR UNKNOWN %s", cmd);
            send_line(c->fd, e);
            return 0;
        }
        int idx = ing_find(argv[1]);
        if (idx < 0) { send_line(c->fd, "ERR UNKNOWN_INGREDIENT"); return 0; }

        long oldq = g_ing[idx].qty;

        if (cmd[0] == 'B') {                                   /* BREW */
            g_ing[idx].qty   += qty;
            c->spellbook[idx] += qty;
            char resp[MAX_LINE];
            snprintf(resp, sizeof resp, "OK BREW %s %ld %ld",
                     g_ing[idx].name, qty, g_ing[idx].qty);
            send_line(c->fd, resp);
            logmsg("BREW wizard=%s ingredient=%s qty=%ld old_qty=%ld new_qty=%ld",
                   c->username, g_ing[idx].name, qty, oldq, g_ing[idx].qty);
        } else {                                               /* CONSUME */
            if (c->spellbook[idx] < qty || g_ing[idx].qty < qty) {
                send_line(c->fd, "ERR INSUFFICIENT_INGREDIENTS");
                return 0;
            }
            g_ing[idx].qty   -= qty;
            if (g_ing[idx].qty < 0) g_ing[idx].qty = 0;        /* never negative */
            c->spellbook[idx] -= qty;
            char resp[MAX_LINE];
            snprintf(resp, sizeof resp, "OK CONSUME %s %ld %ld",
                     g_ing[idx].name, qty, g_ing[idx].qty);
            send_line(c->fd, resp);
            logmsg("CONSUME wizard=%s ingredient=%s qty=%ld old_qty=%ld new_qty=%ld",
                   c->username, g_ing[idx].name, qty, oldq, g_ing[idx].qty);
        }
        return 0;
    }

    /* ---- SPELLBOOK : wizard only ------------------------------------- */
    if (strcmp(cmd, "SPELLBOOK") == 0) {
        if (!is_wiz) { send_line(c->fd, "ERR UNAUTHORIZED"); return 0; }
        char buf[4096];
        size_t len = 0;
        int any = 0;
        str_append(buf, sizeof buf, &len, "OK SPELLBOOK ");
        for (int i = 0; i < g_ning; i++) {
            if (c->spellbook[i] <= 0) continue;
            str_append(buf, sizeof buf, &len, "%s%s:%ld",
                       any ? "," : "", g_ing[i].name, c->spellbook[i]);
            any = 1;
        }
        if (!any) strcpy(buf, "OK SPELLBOOK EMPTY");
        send_line(c->fd, buf);
        return 0;
    }

    /* ---- INSPECT ----------------------------------------------------- */
    if (strcmp(cmd, "INSPECT") == 0) {
        if (argc < 2 || !valid_ing_name(argv[1])) {
            send_line(c->fd, "ERR UNKNOWN INSPECT"); return 0;
        }
        int idx = ing_find(argv[1]);
        if (idx < 0) { send_line(c->fd, "ERR UNKNOWN_INGREDIENT"); return 0; }
        char resp[MAX_LINE];
        snprintf(resp, sizeof resp, "OK INSPECT %s %ld", g_ing[idx].name, g_ing[idx].qty);
        send_line(c->fd, resp);
        logmsg("INSPECT %s=%s ingredient=%s qty=%ld",
               is_wiz ? "wizard" : "professor", c->username, g_ing[idx].name, g_ing[idx].qty);
        return 0;
    }

    /* ---- SCROLL ------------------------------------------------------ */
    if (strcmp(cmd, "SCROLL") == 0) {
        char buf[4096];
        size_t len = 0;
        str_append(buf, sizeof buf, &len, "OK SCROLL ");
        for (int i = 0; i < g_ning; i++)
            str_append(buf, sizeof buf, &len, "%s%s:%ld",
                       i ? "," : "", g_ing[i].name, g_ing[i].qty);
        send_line(c->fd, buf);
        logmsg("SCROLL %s=%s ingredients=%d",
               is_wiz ? "wizard" : "professor", c->username, g_ning);
        return 0;
    }

    /* ---- ROSTER ------------------------------------------------------ */
    if (strcmp(cmd, "ROSTER") == 0) {
        char buf[4096];
        size_t len = 0;
        int n = 0;
        str_append(buf, sizeof buf, &len, "OK ROSTER ");
        for (int i = 0; i < g_max_clients; i++) {
            if (g_clients[i].fd >= 0 && g_clients[i].enrolled) {
                str_append(buf, sizeof buf, &len, "%s%s", n ? "," : "", g_clients[i].username);
                n++;
            }
        }
        send_line(c->fd, buf);
        logmsg("ROSTER %s=%s clients=%d", is_wiz ? "wizard" : "professor", c->username, n);
        return 0;
    }

    /* ---- unknown ----------------------------------------------------- */
    {
        char e[MAX_LINE];
        snprintf(e, sizeof e, "ERR UNKNOWN %s", cmd);
        send_line(c->fd, e);
    }
    return 0;
}

/* ============================================================ I/O on a client */

static void client_on_readable(client_t *c)
{
    char tmp[1024];
    ssize_t n = read(c->fd, tmp, sizeof tmp);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        logmsg("CLIENT_DISCONNECTED username=%s reason=hangup", uname_or_dash(c));
        client_close(c);
        return;
    }
    if (n == 0) {                                       /* peer closed */
        logmsg("CLIENT_DISCONNECTED username=%s reason=hangup", uname_or_dash(c));
        client_close(c);
        return;
    }

    c->last_active = time(NULL);                         /* activity -> reset idle */

    for (ssize_t i = 0; i < n; i++) {
        char ch = tmp[i];

        if (c->overlong) {                              /* still draining a long line */
            if (ch == '\n') { c->overlong = 0; c->buf_len = 0; }
            continue;
        }

        if (ch == '\n') {                               /* a complete line is ready */
            size_t L = c->buf_len;
            if (L > 0 && c->line_buf[L - 1] == '\r') L--;   /* tolerate CRLF */
            char line[MAX_LINE];
            memcpy(line, c->line_buf, L);
            line[L] = '\0';
            c->buf_len = 0;
            if (handle_line(c, line)) {                 /* APPARATE */
                logmsg("CLIENT_DISCONNECTED username=%s reason=APPARATE", uname_or_dash(c));
                client_close(c);
                return;
            }
            continue;
        }

        if (c->buf_len >= sizeof(c->line_buf) - 1) {    /* line would exceed 512 */
            send_line(c->fd, "ERR TOOLONG");
            c->overlong = 1;
            c->buf_len  = 0;
            continue;
        }
        c->line_buf[c->buf_len++] = ch;
    }
}

/* ============================================================ accept */

static void do_accept(void)
{
    struct sockaddr_in cli;
    socklen_t clen = sizeof cli;
    int fd = accept(g_listen_fd, (struct sockaddr *)&cli, &clen);
    if (fd < 0) return;                                 /* transient: ignore */

    char ip[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);

    if (slot_count() >= g_max_clients) {                /* full - reject politely */
        send_line(fd, "ERR HOGWARTS_FULL");
        logmsg("REJECTED fd=%d ip=%s reason=HOGWARTS_FULL", fd, ip);
        close(fd);
        return;
    }

    client_t *c = slot_free();                          /* guaranteed non-NULL */
    client_reset(c);
    c->fd          = fd;
    c->last_active = time(NULL);
    logmsg("CLIENT_CONNECTED fd=%d ip=%s", fd, ip);
}

/* ============================================================ usage / main */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -p <tcp_port> -s <ingredients.txt> -l <logfile> "
        "-n <max_clients> -t <timeout>\n", prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int port = -1, max_clients = -1, timeout = -1;
    const char *ingpath = NULL, *logpath = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:s:l:n:t:")) != -1) {
        switch (opt) {
            case 'p': port        = atoi(optarg); break;
            case 's': ingpath     = optarg;       break;
            case 'l': logpath     = optarg;       break;
            case 'n': max_clients = atoi(optarg); break;
            case 't': timeout     = atoi(optarg); break;
            default : usage(argv[0]);
        }
    }
    if (port < 1024 || !ingpath || !logpath || max_clients < 1 || timeout < 1)
        usage(argv[0]);
    if (access(ingpath, R_OK) != 0) { perror("ingredients file"); return EXIT_FAILURE; }

    g_logf = fopen(logpath, "a");
    if (!g_logf) { perror("logfile"); return EXIT_FAILURE; }

    load_ingredients(ingpath);

    g_max_clients = max_clients;
    g_clients = calloc((size_t)max_clients, sizeof(client_t));
    if (!g_clients) { perror("calloc"); return EXIT_FAILURE; }
    for (int i = 0; i < max_clients; i++) client_reset(&g_clients[i]);

    /* ---- signals : SIGINT sets a flag; select() returns EINTR (no SA_RESTART) */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);                           /* writing to a dead peer */

    /* ---- listening socket ---- */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); return EXIT_FAILURE; }
    int yes = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family      = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port        = htons((uint16_t)port);
    if (bind(g_listen_fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
        perror("bind"); return EXIT_FAILURE;
    }
    if (listen(g_listen_fd, 16) < 0) { perror("listen"); return EXIT_FAILURE; }

    logmsg("SERVER_STARTED port=%d max_clients=%d timeout=%d ingredients=%d",
           port, max_clients, timeout, g_ning);
    printf("Hogwarts is ready. Port: %d | Max Clients: %d | Timeout: %ds\n",
           port, max_clients, timeout);
    fflush(stdout);

    /* ===================================================== main select loop */
    while (!g_stop) {
        time_t now = time(NULL);

        /* (1) idle sweep -- BEFORE every select(), no sleep, no thread */
        for (int i = 0; i < g_max_clients; i++) {
            client_t *c = &g_clients[i];
            if (c->fd < 0) continue;
            long elapsed = (long)(now - c->last_active);
            if (elapsed >= timeout) {
                send_line(c->fd, "TIMEOUT DISCONNECT");
                logmsg("TIMEOUT username=%s fd=%d elapsed=%lds",
                       uname_or_dash(c), c->fd, elapsed);
                logmsg("CLIENT_DISCONNECTED username=%s reason=timeout", uname_or_dash(c));
                client_close(c);
            }
        }

        /* (2) build the fd_set and the smallest "time left until a timeout" */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_listen_fd, &rfds);
        int  maxfd   = g_listen_fd;
        int  active  = 0;
        long min_rem = (long)timeout;
        now = time(NULL);
        for (int i = 0; i < g_max_clients; i++) {
            client_t *c = &g_clients[i];
            if (c->fd < 0) continue;
            FD_SET(c->fd, &rfds);
            if (c->fd > maxfd) maxfd = c->fd;
            active++;
            long rem = (long)timeout - (long)(now - c->last_active);
            if (rem < 0) rem = 0;
            if (rem < min_rem) min_rem = rem;
        }

        struct timeval tv = { 0, 0 }, *ptv = NULL;
        if (active > 0) { tv.tv_sec = min_rem; tv.tv_usec = 0; ptv = &tv; }

        /* (3) wait for I/O or for the nearest idle deadline */
        int r = select(maxfd + 1, &rfds, NULL, NULL, ptv);
        if (r < 0) {
            if (errno == EINTR) continue;               /* signal: re-check g_stop */
            perror("select");
            break;
        }
        if (r == 0) continue;                           /* timeout: loop -> sweep */

        /* (4) new connection? */
        if (FD_ISSET(g_listen_fd, &rfds)) do_accept();

        /* (5) data from connected clients? */
        for (int i = 0; i < g_max_clients; i++) {
            client_t *c = &g_clients[i];
            if (c->fd < 0) continue;
            if (FD_ISSET(c->fd, &rfds)) client_on_readable(c);
        }
    }

    /* ===================================================== graceful shutdown */
    for (int i = 0; i < g_max_clients; i++) {
        client_t *c = &g_clients[i];
        if (c->fd < 0) continue;
        send_line(c->fd, "SERVER SHUTDOWN");
        close(c->fd);
        client_reset(c);
    }
    logmsg("SHUTDOWN signal=SIGINT");
    logmsg("CLEANUP_DONE clients=0");

    if (g_listen_fd >= 0) close(g_listen_fd);
    free(g_clients);
    if (g_logf) fclose(g_logf);
    return 0;
}
