#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stddef.h>

/* Thread-safe string -> string hash table.
 * Separate chaining, with lock striping: buckets are guarded by a smaller
 * array of reader-writer locks so unrelated keys rarely contend. */
typedef struct ht ht_t;

ht_t  *ht_create(size_t nbuckets, size_t nstripes);
void   ht_destroy(ht_t *ht);

/* Insert or overwrite. Returns 0 on success, -1 on allocation failure. */
int    ht_set(ht_t *ht, const char *key, const char *val);

/* Returns a malloc'd copy of the value (caller frees), or NULL if absent. */
char  *ht_get(ht_t *ht, const char *key);

/* Returns 1 if the key was removed, 0 if it was not present. */
int    ht_del(ht_t *ht, const char *key);

size_t ht_count(ht_t *ht);

#endif
