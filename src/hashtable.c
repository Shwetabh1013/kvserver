#include "hashtable.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry {
    char *key;
    char *val;
    struct entry *next;
} entry_t;

struct ht {
    entry_t         **buckets;
    size_t            nbuckets;
    pthread_rwlock_t *locks;
    size_t            nstripes;
    atomic_size_t     count;
};

/* djb2 string hash */
static uint64_t hash_str(const char *s) {
    uint64_t h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + c;
    return h;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

ht_t *ht_create(size_t nbuckets, size_t nstripes) {
    if (nbuckets == 0 || nstripes == 0) return NULL;
    ht_t *ht = calloc(1, sizeof *ht);
    if (!ht) return NULL;
    ht->buckets = calloc(nbuckets, sizeof *ht->buckets);
    ht->locks = malloc(nstripes * sizeof *ht->locks);
    if (!ht->buckets || !ht->locks) {
        free(ht->buckets);
        free(ht->locks);
        free(ht);
        return NULL;
    }
    ht->nbuckets = nbuckets;
    ht->nstripes = nstripes;
    for (size_t i = 0; i < nstripes; i++)
        pthread_rwlock_init(&ht->locks[i], NULL);
    atomic_init(&ht->count, 0);
    return ht;
}

void ht_destroy(ht_t *ht) {
    if (!ht) return;
    for (size_t i = 0; i < ht->nbuckets; i++) {
        entry_t *e = ht->buckets[i];
        while (e) {
            entry_t *next = e->next;
            free(e->key);
            free(e->val);
            free(e);
            e = next;
        }
    }
    for (size_t i = 0; i < ht->nstripes; i++)
        pthread_rwlock_destroy(&ht->locks[i]);
    free(ht->locks);
    free(ht->buckets);
    free(ht);
}

static size_t bucket_of(ht_t *ht, const char *key) {
    return (size_t)(hash_str(key) % ht->nbuckets);
}

static pthread_rwlock_t *lock_of(ht_t *ht, size_t bucket) {
    return &ht->locks[bucket % ht->nstripes];
}

int ht_set(ht_t *ht, const char *key, const char *val) {
    size_t b = bucket_of(ht, key);
    pthread_rwlock_t *lk = lock_of(ht, b);

    /* Allocate outside the lock to keep the critical section short. */
    char *nval = xstrdup(val);
    if (!nval) return -1;

    pthread_rwlock_wrlock(lk);
    for (entry_t *e = ht->buckets[b]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            char *old = e->val;
            e->val = nval;
            pthread_rwlock_unlock(lk);
            free(old);
            return 0;
        }
    }
    entry_t *e = malloc(sizeof *e);
    char *nkey = xstrdup(key);
    if (!e || !nkey) {
        pthread_rwlock_unlock(lk);
        free(e);
        free(nkey);
        free(nval);
        return -1;
    }
    e->key = nkey;
    e->val = nval;
    e->next = ht->buckets[b];
    ht->buckets[b] = e;
    atomic_fetch_add(&ht->count, 1);
    pthread_rwlock_unlock(lk);
    return 0;
}

char *ht_get(ht_t *ht, const char *key) {
    size_t b = bucket_of(ht, key);
    pthread_rwlock_t *lk = lock_of(ht, b);
    char *out = NULL;

    pthread_rwlock_rdlock(lk);
    for (entry_t *e = ht->buckets[b]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            out = xstrdup(e->val);
            break;
        }
    }
    pthread_rwlock_unlock(lk);
    return out;
}

int ht_del(ht_t *ht, const char *key) {
    size_t b = bucket_of(ht, key);
    pthread_rwlock_t *lk = lock_of(ht, b);

    pthread_rwlock_wrlock(lk);
    entry_t **pp = &ht->buckets[b];
    while (*pp) {
        entry_t *e = *pp;
        if (strcmp(e->key, key) == 0) {
            *pp = e->next;
            atomic_fetch_sub(&ht->count, 1);
            pthread_rwlock_unlock(lk);
            free(e->key);
            free(e->val);
            free(e);
            return 1;
        }
        pp = &e->next;
    }
    pthread_rwlock_unlock(lk);
    return 0;
}

size_t ht_count(ht_t *ht) {
    return atomic_load(&ht->count);
}
