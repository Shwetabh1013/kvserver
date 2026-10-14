#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stddef.h>

/* Bounded, blocking FIFO of file descriptors (producer-consumer).
 * The accept thread pushes client sockets; worker threads pop them. */
typedef struct {
    int            *buf;
    size_t          cap, head, tail, len;
    int             closed;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} fdq_t;

int  fdq_init(fdq_t *q, size_t cap);
void fdq_destroy(fdq_t *q);

/* Blocks while full. Returns 0 on success, -1 if the queue is closed. */
int  fdq_push(fdq_t *q, int fd);

/* Blocks while empty. Returns 0 on success, -1 once closed and drained. */
int  fdq_pop(fdq_t *q, int *fd);

/* Wakes every waiting thread; further pushes fail. */
void fdq_close(fdq_t *q);

#endif
