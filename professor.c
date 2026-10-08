/* professor.c - Magic Academy professor client (CSE 344 HW6).
 *
 *   ./professor <server_ip> <tcp_port> <username>
 *
 * Connects over TCP, auto-sends "ENROLL PROFESSOR <username>", forwards every
 * stdin line to the server and prints every server response.  A professor is
 * meant to use only INSPECT / SCROLL / ROSTER / APPARATE; if the user types
 * BREW or CONSUME the line is still forwarded and the server answers
 * "ERR UNAUTHORIZED" (the authorization rule is enforced server side).
 * On Ctrl+C it sends APPARATE, waits for "OK APPARATE" and exits cleanly.
 */
#include "shared.h"
#include <sys/select.h>

#define ROLE      ROLE_PROFESSOR  /* "PROFESSOR" */
#define ROLE_TAG  "PROFESSOR"     /* used in the [PROFESSOR <user>] line prefix */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }   /* async-signal-safe */

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <tcp_port> <username>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *ip    = argv[1];
    int         port  = atoi(argv[2]);
    const char *uname = argv[3];
    if (port < 1024) { fprintf(stderr, "tcp_port must be >= 1024\n"); return EXIT_FAILURE; }

    /* ---- signal handling ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                         /* no SA_RESTART: select() -> EINTR */
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- connect ---- */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return EXIT_FAILURE; }
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &srv.sin_addr) != 1) {
        fprintf(stderr, "invalid server ip: %s\n", ip); close(fd); return EXIT_FAILURE;
    }
    if (connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
        perror("connect"); close(fd); return EXIT_FAILURE;
    }
    printf("[%s %s] CONNECTED server=%s:%d\n", ROLE_TAG, uname, ip, port);
    fflush(stdout);

    /* ---- automatic enrolment (no SENT line for the auto command) ---- */
    {
        char e[MAX_LINE];
        snprintf(e, sizeof e, "ENROLL %s %s", ROLE, uname);
        if (send_line(fd, e) < 0) { fprintf(stderr, "send failed\n"); close(fd); return EXIT_FAILURE; }
    }

    char   inbuf[MAX_LINE * 2];
    size_t inlen      = 0;
    int    quitting   = 0;        /* APPARATE sent, waiting for OK APPARATE */
    int    wait_ticks = 0;        /* safety bound on that wait              */

    /* ===================================================== client select loop */
    while (1) {
        if (g_stop && !quitting) {                       /* Ctrl+C -> APPARATE */
            send_line(fd, "APPARATE");
            printf("\n[%s %s] SENT APPARATE\n", ROLE_TAG, uname);
            fflush(stdout);
            quitting = 1;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int maxfd = fd;
        if (!quitting) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;
        }

        struct timeval tv = { 1, 0 };                    /* 1s tick: notice g_stop */
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (r == 0) {
            if (quitting && ++wait_ticks > 3) {          /* no reply: leave anyway */
                printf("[%s %s] DISCONNECTED reason=APPARATE\n", ROLE_TAG, uname);
                fflush(stdout); close(fd); return 0;
            }
            continue;
        }

        /* ---- server -> client ---- */
        if (FD_ISSET(fd, &rfds)) {
            char tmp[1024];
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n <= 0) {                                /* server closed */
                printf("\n[%s %s] DISCONNECTED reason=shutdown\n", ROLE_TAG, uname);
                fflush(stdout); close(fd); return 0;
            }
            for (ssize_t i = 0; i < n; i++) {
                char ch = tmp[i];
                if (ch == '\n') {
                    size_t L = inlen;
                    if (L > 0 && inbuf[L - 1] == '\r') L--;
                    inbuf[L] = '\0';
                    inlen = 0;
                    printf("[%s %s] RECEIVED %s\n", ROLE_TAG, uname, inbuf);

                    if (strcmp(inbuf, "OK APPARATE") == 0) {
                        printf("[%s %s] DISCONNECTED reason=APPARATE\n", ROLE_TAG, uname);
                        fflush(stdout); close(fd); return 0;
                    }
                    if (strcmp(inbuf, "SERVER SHUTDOWN") == 0) {
                        printf("[%s %s] DISCONNECTED reason=shutdown\n", ROLE_TAG, uname);
                        fflush(stdout); close(fd); return 0;
                    }
                    if (strcmp(inbuf, "TIMEOUT DISCONNECT") == 0) {
                        printf("[%s %s] DISCONNECTED reason=timeout\n", ROLE_TAG, uname);
                        fflush(stdout); close(fd); return 0;
                    }
                    if (!quitting) printf("> ");
                    fflush(stdout);
                } else if (inlen < sizeof inbuf - 1) {
                    inbuf[inlen++] = ch;
                } else {
                    inlen = 0;                           /* drop an over-long reply */
                }
            }
        }

        /* ---- stdin -> server ---- */
        if (!quitting && FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[MAX_LINE];
            if (!fgets(line, sizeof line, stdin)) {      /* EOF on stdin */
                send_line(fd, "APPARATE");
                printf("[%s %s] SENT APPARATE\n", ROLE_TAG, uname);
                fflush(stdout);
                quitting = 1;
            } else {
                size_t L = strlen(line);
                while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
                if (L == 0) { printf("> "); fflush(stdout); continue; }
                if (send_line(fd, line) < 0) { fprintf(stderr, "send failed\n"); break; }
                printf("[%s %s] SENT %s\n", ROLE_TAG, uname, line);
                fflush(stdout);
                if (strcmp(line, "APPARATE") == 0) quitting = 1;
            }
        }
    }

    close(fd);
    return 0;
}
