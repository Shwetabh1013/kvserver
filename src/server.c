/*
 * kvserver - a multithreaded in-memory key-value server over TCP.
 *
 * Architecture:
 *   main thread  : accept() loop -> pushes client fds into a bounded queue
 *   worker pool  : N threads pop fds and serve each connection until it closes
 *   storage      : lock-striped hash table shared by all workers
 *
 * Protocol (one command per line, '\n' terminated):
 *   PING                -> +PONG
 *   SET <key> <value>   -> +OK              (value may contain spaces)
 *   GET <key>           -> $<len> <value>   or  $-1 if missing
 *   DEL <key>           -> :1 or :0
 *   COUNT               -> :<n>
 *   QUIT                -> +BYE, then the server closes the connection
 *   errors              -> -ERR <message>
 */

#include "hashtable.h"
#include "queue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_LINE    4096
#define MAX_KEY     256
#define QUEUE_CAP   1024
#define NBUCKETS    65536
#define NSTRIPES    64

static atomic_int g_stop = 0;
static ht_t      *g_table;
static fdq_t      g_queue;

static void on_signal(int sig) {
    (void)sig;
    atomic_store(&g_stop, 1);
}

/* send() until every byte is written or the peer goes away. */
static int send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int reply(int fd, const char *s) {
    return send_all(fd, s, strlen(s));
}

/* Executes one command line. Returns 1 to keep the connection, 0 to close. */
static int handle_line(int fd, char *line) {
    char *cmd = line;
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';

    if (strcmp(cmd, "PING") == 0) {
        return reply(fd, "+PONG\n") == 0;
    }
    if (strcmp(cmd, "QUIT") == 0) {
        reply(fd, "+BYE\n");
        return 0;
    }
    if (strcmp(cmd, "COUNT") == 0) {
        char out[32];
        snprintf(out, sizeof out, ":%zu\n", ht_count(g_table));
        return reply(fd, out) == 0;
    }
    if (strcmp(cmd, "SET") == 0) {
        char *val = arg ? strchr(arg, ' ') : NULL;
        if (!arg || !val || val == arg)
            return reply(fd, "-ERR usage: SET <key> <value>\n") == 0;
        *val++ = '\0';
        if (strlen(arg) > MAX_KEY)
            return reply(fd, "-ERR key too long\n") == 0;
        if (ht_set(g_table, arg, val) != 0)
            return reply(fd, "-ERR out of memory\n") == 0;
        return reply(fd, "+OK\n") == 0;
    }
    if (strcmp(cmd, "GET") == 0 || strcmp(cmd, "DEL") == 0) {
        if (!arg || *arg == '\0' || strchr(arg, ' '))
            return reply(fd, "-ERR expected exactly one key\n") == 0;
        if (cmd[0] == 'D')
            return reply(fd, ht_del(g_table, arg) ? ":1\n" : ":0\n") == 0;

        char *val = ht_get(g_table, arg);
        if (!val) return reply(fd, "$-1\n") == 0;
        size_t vlen = strlen(val);
        char head[32];
        int hl = snprintf(head, sizeof head, "$%zu ", vlen);
        int ok = send_all(fd, head, (size_t)hl) == 0 &&
                 send_all(fd, val, vlen) == 0 &&
                 send_all(fd, "\n", 1) == 0;
        free(val);
        return ok;
    }
    return reply(fd, "-ERR unknown command\n") == 0;
}

/* Reads newline-delimited commands, handling partial reads and
 * several commands arriving in one recv(). */
static void serve_client(int fd) {
    char buf[MAX_LINE + 1];
    size_t used = 0;

    for (;;) {
        ssize_t n = recv(fd, buf + used, MAX_LINE - used, 0);
        if (n == 0) return;                    /* peer closed */
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (atomic_load(&g_stop)) return; /* idle + shutting down */
                continue;
            }
            return;
        }
        used += (size_t)n;

        size_t start = 0;
        for (size_t i = 0; i < used; i++) {
            if (buf[i] != '\n') continue;
            buf[i] = '\0';
            if (i > start && buf[i - 1] == '\r') buf[i - 1] = '\0';
            if (!handle_line(fd, buf + start)) return;
            start = i + 1;
        }
        /* keep any incomplete trailing command for the next recv() */
        memmove(buf, buf + start, used - start);
        used -= start;
        if (used == MAX_LINE) {
            reply(fd, "-ERR line too long\n");
            return;
        }
    }
}

static void *worker(void *arg) {
    (void)arg;
    int fd;
    while (fdq_pop(&g_queue, &fd) == 0) {
        serve_client(fd);
        close(fd);
    }
    return NULL;
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 512) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-p port] [-t threads]\n", prog);
}

int main(int argc, char **argv) {
    int port = 7070, nthreads = 8;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) nthreads = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    if (port <= 0 || port > 65535 || nthreads <= 0 || nthreads > 1024) {
        usage(argv[0]);
        return 2;
    }

    /* A client disconnecting mid-reply must not kill the server. */
    signal(SIGPIPE, SIG_IGN);

    g_table = ht_create(NBUCKETS, NSTRIPES);
    if (!g_table || fdq_init(&g_queue, QUEUE_CAP) != 0) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    int lfd = listen_on(port);
    if (lfd < 0) return 1;

    /* Block SIGINT/SIGTERM in workers so only the main thread receives them. */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, &old);

    pthread_t *tids = malloc((size_t)nthreads * sizeof *tids);
    if (!tids) { fprintf(stderr, "allocation failed\n"); return 1; }
    for (int i = 0; i < nthreads; i++)
        pthread_create(&tids[i], NULL, worker, NULL);

    /* No SA_RESTART: a signal interrupts accept() with EINTR. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    printf("kvserver listening on port %d with %d worker threads\n", port, nthreads);
    fflush(stdout);

    while (!atomic_load(&g_stop)) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        /* 1s receive timeout lets idle workers notice shutdown. */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (fdq_push(&g_queue, cfd) != 0) close(cfd);
    }

    printf("\nshutting down...\n");
    close(lfd);
    fdq_close(&g_queue);
    for (int i = 0; i < nthreads; i++)
        pthread_join(tids[i], NULL);

    /* close any connections that were queued but never served */
    int fd;
    while (g_queue.len > 0 && fdq_pop(&g_queue, &fd) == 0) close(fd);

    free(tids);
    fdq_destroy(&g_queue);
    ht_destroy(g_table);
    printf("clean exit\n");
    return 0;
}
