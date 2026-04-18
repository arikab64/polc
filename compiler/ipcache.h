/*
 * ipcache.h — IP → EID hashtable
 *
 * O(1) expected lookup. Keys are uint32_t (IPv4 host byte order), values
 * are uint64_t EIDs (the SplitMix64 hash of the entity's label bitset).
 *
 * Implementation: open-addressing, linear probing, power-of-two capacity,
 * grow at 75% load factor. Hash = finalize_64(addr) (SplitMix finalizer).
 */
#ifndef IPCACHE_H
#define IPCACHE_H

#include <stdint.h>
#include <stddef.h>

/* Create a fresh ipcache. Initial capacity is a small power of 2. */
void     ipcache_init(void);

/* Free all memory. Idempotent. */
void     ipcache_free(void);

/* Insert a mapping. Returns:
 *    1 = inserted new entry
 *    0 = entry already existed with the same EID (no-op)
 *   -1 = key collision with different EID (caller's invariant broken)
 */
int      ipcache_put(uint32_t addr, uint64_t eid);

/* Look up an IP.
 * Returns 1 on hit (writes *out_eid if non-NULL), 0 on miss. */
int      ipcache_get(uint32_t addr, uint64_t *out_eid);

/* Iteration callback: called once per live entry in arbitrary order. */
typedef void (*ipcache_visitor)(uint32_t addr, uint64_t eid, void *userdata);
void     ipcache_foreach(ipcache_visitor fn, void *userdata);

/* Stats, for the printer. */
size_t   ipcache_size(void);       /* live entries */
size_t   ipcache_capacity(void);   /* allocated slot count */
size_t   ipcache_collisions(void); /* total extra probes done on insert */

#endif
