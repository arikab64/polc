/*
 * ipcache.c — IP → EID hashtable implementation.
 *
 * Slot states:
 *   EMPTY     — never used; probe stops here on lookup
 *   OCCUPIED  — holds a real (addr, eid) pair
 *
 * (TOMBSTONE is not currently needed since we don't expose deletion,
 *  but the state byte leaves the door open for it.)
 */
#include "ipcache.h"
#include "ast.h"     /* for finalize_64 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SLOT_EMPTY    0
#define SLOT_OCCUPIED 1

#define INITIAL_CAP   16
#define LOAD_NUM       3        /* grow when size * 4 > cap * 3 */
#define LOAD_DEN       4

typedef struct {
    uint32_t addr;
    uint64_t eid;
    uint8_t  state;
} ipc_slot;

static ipc_slot *g_slots      = NULL;
static size_t    g_cap        = 0;
static size_t    g_size       = 0;
static size_t    g_collisions = 0;

/* ---- internal ---- */

static inline size_t hash_addr(uint32_t addr, size_t cap_mask) {
    return (size_t)finalize_64((uint64_t)addr) & cap_mask;
}

/* Insert without growth-check (used by both put and rehash). Returns
 * 1 = newly inserted, 0 = update same eid, -1 = collision (different eid). */
static int raw_insert(ipc_slot *slots, size_t cap, uint32_t addr, uint64_t eid,
                      size_t *collisions_out) {
    size_t mask  = cap - 1;
    size_t idx   = hash_addr(addr, mask);
    size_t extra = 0;
    for (;;) {
        ipc_slot *s = &slots[idx];
        if (s->state == SLOT_EMPTY) {
            s->addr  = addr;
            s->eid   = eid;
            s->state = SLOT_OCCUPIED;
            if (collisions_out) *collisions_out += extra;
            return 1;
        }
        if (s->addr == addr) {
            /* Same IP — must have same EID or we broke our invariant. */
            return (s->eid == eid) ? 0 : -1;
        }
        idx = (idx + 1) & mask;
        extra++;
    }
}

static void grow(void) {
    size_t    new_cap   = g_cap ? g_cap * 2 : INITIAL_CAP;
    ipc_slot *new_slots = calloc(new_cap, sizeof *new_slots);

    /* Rehash every live entry; collisions counter is not updated during
     * rehash — it's a property of the user's insertion sequence, not of
     * internal maintenance. */
    size_t dummy = 0;
    for (size_t i = 0; i < g_cap; i++) {
        if (g_slots[i].state == SLOT_OCCUPIED) {
            raw_insert(new_slots, new_cap,
                       g_slots[i].addr, g_slots[i].eid, &dummy);
        }
    }
    free(g_slots);
    g_slots = new_slots;
    g_cap   = new_cap;
}

/* ---- public ---- */

void ipcache_init(void) {
    ipcache_free();
    g_slots = calloc(INITIAL_CAP, sizeof *g_slots);
    g_cap   = INITIAL_CAP;
    g_size  = 0;
    g_collisions = 0;
}

void ipcache_free(void) {
    free(g_slots);
    g_slots = NULL;
    g_cap = g_size = g_collisions = 0;
}

int ipcache_put(uint32_t addr, uint64_t eid) {
    if (g_cap == 0) ipcache_init();
    /* Grow if size+1 would exceed load factor. */
    if ((g_size + 1) * LOAD_DEN > g_cap * LOAD_NUM) grow();
    int rc = raw_insert(g_slots, g_cap, addr, eid, &g_collisions);
    if (rc == 1) g_size++;
    return rc;
}

int ipcache_get(uint32_t addr, uint64_t *out_eid) {
    if (g_cap == 0) return 0;
    size_t mask = g_cap - 1;
    size_t idx  = hash_addr(addr, mask);
    for (;;) {
        ipc_slot *s = &g_slots[idx];
        if (s->state == SLOT_EMPTY)     return 0;  /* miss */
        if (s->addr  == addr)           {           /* hit  */
            if (out_eid) *out_eid = s->eid;
            return 1;
        }
        idx = (idx + 1) & mask;
    }
}

void ipcache_foreach(ipcache_visitor fn, void *userdata) {
    for (size_t i = 0; i < g_cap; i++) {
        if (g_slots[i].state == SLOT_OCCUPIED)
            fn(g_slots[i].addr, g_slots[i].eid, userdata);
    }
}

size_t ipcache_size(void)       { return g_size; }
size_t ipcache_capacity(void)   { return g_cap;  }
size_t ipcache_collisions(void) { return g_collisions; }
