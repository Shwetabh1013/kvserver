/* Unit tests for the hash table, including a concurrent stress test. */
#include "../src/hashtable.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 8
#define PER_THREAD 20000

static ht_t *shared;

static void test_basic(void) {
    ht_t *ht = ht_create(16, 4);
    assert(ht && ht_count(ht) == 0);
    assert(ht_get(ht, "missing") == NULL);

    assert(ht_set(ht, "a", "1") == 0);
    char *v = ht_get(ht, "a");
    assert(v && strcmp(v, "1") == 0);
    free(v);

    assert(ht_set(ht, "a", "2") == 0);       /* overwrite */
    assert(ht_count(ht) == 1);
    v = ht_get(ht, "a");
    assert(strcmp(v, "2") == 0);
    free(v);

    assert(ht_del(ht, "a") == 1);
    assert(ht_del(ht, "a") == 0);
    assert(ht_count(ht) == 0);
    ht_destroy(ht);
    puts("PASS basic set/get/overwrite/delete");
}

static void test_collisions(void) {
    ht_t *ht = ht_create(1, 1);              /* force every key into one chain */
    char k[32], val[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof k, "key%d", i);
        snprintf(val, sizeof val, "v%d", i);
        assert(ht_set(ht, k, val) == 0);
    }
    assert(ht_count(ht) == 1000);
    for (int i = 0; i < 1000; i += 2) {
        snprintf(k, sizeof k, "key%d", i);
        assert(ht_del(ht, k) == 1);
    }
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof k, "key%d", i);
        char *g = ht_get(ht, k);
        assert((i % 2 == 0) == (g == NULL));
        free(g);
    }
    assert(ht_count(ht) == 500);
    ht_destroy(ht);
    puts("PASS 1000 keys in a single collision chain");
}

static void *hammer(void *arg) {
    long id = (long)arg;
    char k[48], val[48];
    for (int i = 0; i < PER_THREAD; i++) {
        snprintf(k, sizeof k, "t%ld:%d", id, i);
        snprintf(val, sizeof val, "%d", i);
        assert(ht_set(shared, k, val) == 0);
        char *g = ht_get(shared, k);
        assert(g && strcmp(g, val) == 0);
        free(g);
        if (i % 3 == 0) assert(ht_del(shared, k) == 1);
    }
    return NULL;
}

static void test_concurrent(void) {
    shared = ht_create(4096, 64);
    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, hammer, (void *)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    size_t deleted_per = (PER_THREAD + 2) / 3;
    assert(ht_count(shared) == (size_t)THREADS * (PER_THREAD - deleted_per));
    ht_destroy(shared);
    printf("PASS %d threads x %d ops, final count consistent\n", THREADS, PER_THREAD);
}

int main(void) {
    test_basic();
    test_collisions();
    test_concurrent();
    puts("all hashtable tests passed");
    return 0;
}
