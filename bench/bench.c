/*
 * bench - load generator for kvserver.
 * Spawns C client threads; each opens one TCP connection and issues N
 * requests (alternating SET and GET), timing every round trip.
 * Reports total throughput and p50/p95/p99 latency.
 *
 * usage: bench [host] [port] [clients] [requests_per_client]
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *host;
    int port, id, nreq;
    double *lat_us;   /* per-request latency, microseconds */
    int errors;
} job_t;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Reads one '\n'-terminated reply. Returns 0 on success. */
static int read_line(int fd, char *buf, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, 1, 0);
        if (n <= 0) return -1;
        if (buf[used++] == '\n') { buf[used] = '\0'; return 0; }
    }
    return -1;
}

static void *client(void *arg) {
    job_t *j = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)j->port);
    inet_pton(AF_INET, j->host, &a.sin_addr);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        j->errors = j->nreq;
        if (fd >= 0) close(fd);
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    char req[128], resp[256];
    for (int i = 0; i < j->nreq; i++) {
        int k = i / 2;
        int len = (i % 2 == 0)
            ? snprintf(req, sizeof req, "SET c%d:k%d value-%d\n", j->id, k, k)
            : snprintf(req, sizeof req, "GET c%d:k%d\n", j->id, k);
        double t0 = now_us();
        if (send(fd, req, (size_t)len, 0) != len || read_line(fd, resp, sizeof resp) != 0) {
            j->errors += j->nreq - i;
            break;
        }
        j->lat_us[i] = now_us() - t0;
        /* verify GET returns what this client just SET */
        if (i % 2 == 1) {
            char want[64];
            int vl = snprintf(want, sizeof want, "value-%d", k);
            char expect[96];
            snprintf(expect, sizeof expect, "$%d %s\n", vl, want);
            if (strcmp(resp, expect) != 0) j->errors++;
        } else if (strcmp(resp, "+OK\n") != 0) {
            j->errors++;
        }
    }
    send(fd, "QUIT\n", 5, 0);
    close(fd);
    return NULL;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port    = argc > 2 ? atoi(argv[2]) : 7070;
    int clients = argc > 3 ? atoi(argv[3]) : 8;
    int nreq    = argc > 4 ? atoi(argv[4]) : 20000;

    pthread_t *tids = malloc((size_t)clients * sizeof *tids);
    job_t *jobs = calloc((size_t)clients, sizeof *jobs);
    double *all = malloc((size_t)clients * (size_t)nreq * sizeof *all);
    if (!tids || !jobs || !all) { fprintf(stderr, "alloc failed\n"); return 1; }

    double t0 = now_us();
    for (int c = 0; c < clients; c++) {
        jobs[c] = (job_t){ host, port, c, nreq, all + (size_t)c * nreq, 0 };
        pthread_create(&tids[c], NULL, client, &jobs[c]);
    }
    int errors = 0;
    for (int c = 0; c < clients; c++) {
        pthread_join(tids[c], NULL);
        errors += jobs[c].errors;
    }
    double secs = (now_us() - t0) / 1e6;

    size_t total = (size_t)clients * nreq;
    qsort(all, total, sizeof *all, cmp_d);
    printf("clients=%d requests=%zu errors=%d\n", clients, total, errors);
    printf("throughput: %.0f req/s\n", total / secs);
    printf("latency us: p50=%.1f p95=%.1f p99=%.1f\n",
           all[total / 2], all[(size_t)(total * 0.95)], all[(size_t)(total * 0.99)]);

    free(all); free(jobs); free(tids);
    return errors ? 1 : 0;
}
