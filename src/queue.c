#include "queue.h"

#include <stdlib.h>

int fdq_init(fdq_t *q, size_t cap) {
    q->buf = malloc(cap * sizeof *q->buf);
    if (!q->buf) return -1;
    q->cap = cap;
    q->head = q->tail = q->len = 0;
    q->closed = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return 0;
}

void fdq_destroy(fdq_t *q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
}

int fdq_push(fdq_t *q, int fd) {
    pthread_mutex_lock(&q->mu);
    /* while, not if: guards against spurious wakeups */
    while (q->len == q->cap && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mu);
    if (q->closed) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->buf[q->tail] = fd;
    q->tail = (q->tail + 1) % q->cap;
    q->len++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

int fdq_pop(fdq_t *q, int *fd) {
    pthread_mutex_lock(&q->mu);
    while (q->len == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mu);
    if (q->len == 0) { /* closed and drained */
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *fd = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void fdq_close(fdq_t *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}
