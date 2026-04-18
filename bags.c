/*
 * bags.c — interned bag storage.
 *
 * Build is two-pass:
 *   1. build_bags()  — populate four "scratch" bags (key → rule_bitvec).
 *   2. intern_bags() — walk each scratch bag, intern each bitvec into the
 *      global bagvec store, write the returned bag_id into the "final"
 *      four maps (key → bag_id_t).  Free the scratch tables.
 *
 * The final four maps plus one shared bagvec store are what everything
 * downstream (printer, accessors, SQLite emitter) consumes.
 */
#include "bags.h"
#include "ast.h"
#include "resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  bitvec primitives
 * ============================================================ */

static inline void rvec_clear(rule_bitvec *v) {
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) v->w[i] = 0;
}
static inline void rvec_set(rule_bitvec *v, int bit) {
    v->w[bit >> 6] |= (uint64_t)1 << (bit & 63);
}
static inline int rvec_test(const rule_bitvec *v, int bit) {
    return (v->w[bit >> 6] >> (bit & 63)) & 1;
}
static inline int rvec_is_zero(const rule_bitvec *v) {
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) if (v->w[i]) return 0;
    return 1;
}
static inline int rvec_is_all_ones(const rule_bitvec *v) {
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) if (v->w[i] != UINT64_MAX) return 0;
    return 1;
}
static inline int rvec_eq(const rule_bitvec *a, const rule_bitvec *b) {
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) if (a->w[i] != b->w[i]) return 0;
    return 1;
}

/* Content hash: SplitMix64-finalize of the XOR-fold of all words. Used
 * only for dedup lookup; the bag_id is a sequential ordinal, not this. */
static uint64_t rvec_hash(const rule_bitvec *v) {
    uint64_t x = 0;
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) x ^= v->w[i];
    return finalize_64(x);
}

/* ============================================================
 *  bagvec store — dynamic array of rule_bitvec indexed by bag_id_t,
 *  plus a (content hash → bag_id) hashtable for dedup.
 * ============================================================ */

typedef struct {
    rule_bitvec  *vecs;          /* dynamic array; vecs[id] is the content */
    size_t        n;             /* count (== next unassigned id) */
    size_t        cap;
} bagvec_store;

/* Dedup index: content hash → bag_id. Open addressing. Slot state 0=empty. */
typedef struct {
    uint64_t  key;       /* content hash */
    bag_id_t  id;
    uint8_t   state;
} bagvec_idx_slot;

typedef struct {
    bagvec_idx_slot *slots;
    size_t           cap;
    size_t           size;
} bagvec_index;

static bagvec_store g_vecs;
static bagvec_index g_vec_idx;

/* Append a bitvec to the store and return its new id. Does not dedup —
 * dedup is the caller's job (so we can handle the reserved ids specially). */
static bag_id_t bagvec_store_append(const rule_bitvec *v) {
    if (g_vecs.n == g_vecs.cap) {
        g_vecs.cap = g_vecs.cap ? g_vecs.cap * 2 : 16;
        g_vecs.vecs = realloc(g_vecs.vecs, g_vecs.cap * sizeof(rule_bitvec));
    }
    g_vecs.vecs[g_vecs.n] = *v;
    return (bag_id_t)(g_vecs.n++);
}

/* Index: find or insert. On find, verifies content matches (collision check).
 * Returns 1 if an existing id matched, 0 if the slot was empty (caller
 * should insert content and set state). */
static int bagvec_idx_lookup(uint64_t hash, bag_id_t *out) {
    if (g_vec_idx.cap == 0) return 0;
    size_t mask = g_vec_idx.cap - 1;
    size_t idx  = (size_t)hash & mask;
    for (;;) {
        bagvec_idx_slot *s = &g_vec_idx.slots[idx];
        if (!s->state) return 0;
        if (s->key == hash) { *out = s->id; return 1; }
        idx = (idx + 1) & mask;
    }
}

static void bagvec_idx_insert(uint64_t hash, bag_id_t id) {
    /* Grow at 75% load. */
    if (g_vec_idx.cap == 0 || (g_vec_idx.size + 1) * 4 > g_vec_idx.cap * 3) {
        size_t new_cap = g_vec_idx.cap ? g_vec_idx.cap * 2 : 16;
        bagvec_idx_slot *ns = calloc(new_cap, sizeof *ns);
        size_t mask = new_cap - 1;
        for (size_t i = 0; i < g_vec_idx.cap; i++) {
            if (!g_vec_idx.slots[i].state) continue;
            size_t j = (size_t)g_vec_idx.slots[i].key & mask;
            while (ns[j].state) j = (j + 1) & mask;
            ns[j] = g_vec_idx.slots[i];
        }
        free(g_vec_idx.slots);
        g_vec_idx.slots = ns;
        g_vec_idx.cap   = new_cap;
    }
    size_t mask = g_vec_idx.cap - 1;
    size_t idx  = (size_t)hash & mask;
    while (g_vec_idx.slots[idx].state) idx = (idx + 1) & mask;
    g_vec_idx.slots[idx].key   = hash;
    g_vec_idx.slots[idx].id    = id;
    g_vec_idx.slots[idx].state = 1;
    g_vec_idx.size++;
}

/* Intern: returns a bag_id for the given bitvec content. Collapses to
 * the reserved ids for all-zero and all-ones. Otherwise dedups via the
 * content-hash index. */
static bag_id_t bagvec_intern(const rule_bitvec *v) {
    if (rvec_is_zero(v))     return BAG_ID_ZERO;
    if (rvec_is_all_ones(v)) return BAG_ID_ALL;

    uint64_t h = rvec_hash(v);
    bag_id_t existing;
    if (bagvec_idx_lookup(h, &existing)) {
        /* Verify true equality (guard against the astronomically rare
         * 64-bit hash collision). */
        if (rvec_eq(&g_vecs.vecs[existing], v)) return existing;
        fprintf(stderr, "bags: hash collision on bag_id %u — aborting\n", existing);
        exit(2);
    }
    bag_id_t id = bagvec_store_append(v);
    bagvec_idx_insert(h, id);
    return id;
}

/* Initialize the bagvec store with the two reserved entries. */
static void bagvec_reset(void) {
    free(g_vecs.vecs);   g_vecs.vecs = NULL;   g_vecs.n = g_vecs.cap = 0;
    free(g_vec_idx.slots); g_vec_idx.slots = NULL;
    g_vec_idx.cap = g_vec_idx.size = 0;

    rule_bitvec zero, ones;
    rvec_clear(&zero);
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) ones.w[i] = UINT64_MAX;

    bag_id_t z = bagvec_store_append(&zero);   (void)z;   /* becomes id 0 */
    bag_id_t a = bagvec_store_append(&ones);   (void)a;   /* becomes id 1 */
    /* We deliberately do NOT register the reserved ids in the dedup
     * index — bagvec_intern() short-circuits zero/all-ones before looking
     * at the index, so registering them would just waste a slot. */
}

const rule_bitvec *bagvec_get(bag_id_t id) {
    if (id >= g_vecs.n) return NULL;
    return &g_vecs.vecs[id];
}

/* ============================================================
 *  bagmap — key:uint64 → bag_id. Open addressing, linear probing.
 *  State byte distinguishes empty from occupied so key 0 is legitimate.
 * ============================================================ */

typedef struct {
    uint64_t key;
    bag_id_t id;
    uint8_t  state;
} bagmap_slot;

typedef struct {
    bagmap_slot *slots;
    size_t       cap;
    size_t       size;
} bagmap;

static inline size_t bagmap_mix(uint64_t k, size_t mask) {
    return (size_t)finalize_64(k) & mask;
}

/* Returns 1 and fills *out if the key is present. Returns 0 otherwise. */
static int bagmap_find(const bagmap *b, uint64_t key, bag_id_t *out) {
    if (b->cap == 0) return 0;
    size_t mask = b->cap - 1;
    size_t idx  = bagmap_mix(key, mask);
    for (;;) {
        const bagmap_slot *s = &b->slots[idx];
        if (!s->state)    return 0;
        if (s->key == key) { *out = s->id; return 1; }
        idx = (idx + 1) & mask;
    }
}

static void bagmap_set(bagmap *b, uint64_t key, bag_id_t id) {
    /* Grow at 75% load. */
    if (b->cap == 0 || (b->size + 1) * 4 > b->cap * 3) {
        size_t new_cap = b->cap ? b->cap * 2 : 16;
        bagmap_slot *ns = calloc(new_cap, sizeof *ns);
        size_t mask = new_cap - 1;
        for (size_t i = 0; i < b->cap; i++) {
            if (!b->slots[i].state) continue;
            size_t j = bagmap_mix(b->slots[i].key, mask);
            while (ns[j].state) j = (j + 1) & mask;
            ns[j] = b->slots[i];
        }
        free(b->slots);
        b->slots = ns;
        b->cap   = new_cap;
    }
    size_t mask = b->cap - 1;
    size_t idx  = bagmap_mix(key, mask);
    for (;;) {
        bagmap_slot *s = &b->slots[idx];
        if (!s->state) {
            s->key   = key;
            s->id    = id;
            s->state = 1;
            b->size++;
            return;
        }
        if (s->key == key) { s->id = id; return; }    /* overwrite */
        idx = (idx + 1) & mask;
    }
}

static void bagmap_free(bagmap *b) {
    free(b->slots);
    b->slots = NULL;
    b->cap = b->size = 0;
}

/* ============================================================
 *  scratch bag (pass-1 storage): key → rule_bitvec (by value in slot).
 *  Same shape as bagmap but with a full bitvec per slot; discarded
 *  after interning.
 * ============================================================ */

typedef struct {
    uint64_t     key;
    rule_bitvec  vec;
    uint8_t      state;
} scratch_slot;

typedef struct {
    scratch_slot *slots;
    size_t        cap;
    size_t        size;
} scratch_bag;

static rule_bitvec *scratch_get_or_create(scratch_bag *b, uint64_t key) {
    if (b->cap == 0 || (b->size + 1) * 4 > b->cap * 3) {
        size_t new_cap = b->cap ? b->cap * 2 : 16;
        scratch_slot *ns = calloc(new_cap, sizeof *ns);
        size_t mask = new_cap - 1;
        for (size_t i = 0; i < b->cap; i++) {
            if (!b->slots[i].state) continue;
            size_t j = bagmap_mix(b->slots[i].key, mask);
            while (ns[j].state) j = (j + 1) & mask;
            ns[j] = b->slots[i];
        }
        free(b->slots);
        b->slots = ns;
        b->cap   = new_cap;
    }
    size_t mask = b->cap - 1;
    size_t idx  = bagmap_mix(key, mask);
    for (;;) {
        scratch_slot *s = &b->slots[idx];
        if (!s->state) {
            s->key = key;
            rvec_clear(&s->vec);
            s->state = 1;
            b->size++;
            return &s->vec;
        }
        if (s->key == key) return &s->vec;
        idx = (idx + 1) & mask;
    }
}

static void scratch_free(scratch_bag *b) {
    free(b->slots);
    b->slots = NULL;
    b->cap = b->size = 0;
}

/* ============================================================
 *  The four final bags (public per user request: g_bag_*)
 *  and their scratch predecessors.
 * ============================================================ */

static bagmap g_bag_src;
static bagmap g_bag_dst;
static bagmap g_bag_port;
static bagmap g_bag_proto;

/* Track the highest resolved rule id — used for print width. */
static int g_max_rule_id = -1;

/* ============================================================
 *  build (pass 1): populate scratch bags
 * ============================================================ */

extern rule_node *rule_list_head(void);   /* from main.c */

static const rule_node *rule_by_id(int id) {
    for (rule_node *r = rule_list_head(); r; r = r->next)
        if (r->id == id) return r;
    return NULL;
}

/* ============================================================
 *  intern (pass 2): walk each scratch bag, produce the final bagmap.
 * ============================================================ */

static void intern_scratch_into(const scratch_bag *src, bagmap *dst) {
    for (size_t i = 0; i < src->cap; i++) {
        if (!src->slots[i].state) continue;
        bag_id_t id = bagvec_intern(&src->slots[i].vec);
        /* Only record non-zero mappings — missing key ⇒ BAG_ID_ZERO via
         * the getter fallback, so storing zeros would be redundant. */
        if (id == BAG_ID_ZERO) continue;
        bagmap_set(dst, src->slots[i].key, id);
    }
}

void build_bags(void) {
    /* Clean slate. */
    bagmap_free(&g_bag_src);
    bagmap_free(&g_bag_dst);
    bagmap_free(&g_bag_port);
    bagmap_free(&g_bag_proto);
    bagvec_reset();
    g_max_rule_id = -1;

    /* --- pass 1: build scratch bags --- */
    scratch_bag s_src = {0}, s_dst = {0}, s_port = {0}, s_proto = {0};

    for (resolved_rule *rr = resolutions_head(); rr; rr = rr->next) {
        const rule_node *rule = rule_by_id(rr->rule_id);
        if (!rule) continue;
        if (rule->id > g_max_rule_id) g_max_rule_id = rule->id;

        for (size_t i = 0; i < rr->src_eids.n; i++) {
            rule_bitvec *v = scratch_get_or_create(&s_src, rr->src_eids.items[i]->hash);
            rvec_set(v, rule->id);
        }
        for (size_t i = 0; i < rr->dst_eids.n; i++) {
            rule_bitvec *v = scratch_get_or_create(&s_dst, rr->dst_eids.items[i]->hash);
            rvec_set(v, rule->id);
        }
        for (port_node *p = rule->ports; p; p = p->next) {
            rule_bitvec *v = scratch_get_or_create(&s_port, (uint64_t)p->port);
            rvec_set(v, rule->id);
        }
        if (rule->protos & PROTO_TCP) {
            rule_bitvec *v = scratch_get_or_create(&s_proto, PROTO_TCP);
            rvec_set(v, rule->id);
        }
        if (rule->protos & PROTO_UDP) {
            rule_bitvec *v = scratch_get_or_create(&s_proto, PROTO_UDP);
            rvec_set(v, rule->id);
        }
    }

    /* --- pass 2: intern into bagvec store, populate final maps --- */
    intern_scratch_into(&s_src,   &g_bag_src);
    intern_scratch_into(&s_dst,   &g_bag_dst);
    intern_scratch_into(&s_port,  &g_bag_port);
    intern_scratch_into(&s_proto, &g_bag_proto);

    scratch_free(&s_src);
    scratch_free(&s_dst);
    scratch_free(&s_port);
    scratch_free(&s_proto);
}

/* ============================================================
 *  getters
 * ============================================================ */

bag_id_t bag_src_id(uint64_t eid)  {
    bag_id_t id;
    return bagmap_find(&g_bag_src, eid, &id) ? id : BAG_ID_ZERO;
}
bag_id_t bag_dst_id(uint64_t eid)  {
    bag_id_t id;
    return bagmap_find(&g_bag_dst, eid, &id) ? id : BAG_ID_ZERO;
}
bag_id_t bag_port_id(int port) {
    bag_id_t id;
    return bagmap_find(&g_bag_port, (uint64_t)port, &id) ? id : BAG_ID_ZERO;
}
bag_id_t bag_proto_id(int proto) {
    bag_id_t id;
    return bagmap_find(&g_bag_proto, (uint64_t)proto, &id) ? id : BAG_ID_ZERO;
}

size_t bagvec_count(void) { return g_vecs.n; }

/* Generic walker — called by the four type-specific wrappers. */
static void bagmap_foreach(const bagmap *b,
                           void (*cb)(uint64_t, bag_id_t, void *), void *ud) {
    if (!b->slots) return;
    for (size_t i = 0; i < b->cap; i++)
        if (b->slots[i].state)
            cb(b->slots[i].key, b->slots[i].id, ud);
}

/* Tiny shims to let the typed visitors take more specific first args
 * without casting fn pointers (which would be undefined behavior). */
struct bag_src_shim   { bag_src_visitor   fn; void *ud; };
struct bag_dst_shim   { bag_dst_visitor   fn; void *ud; };
struct bag_port_shim  { bag_port_visitor  fn; void *ud; };
struct bag_proto_shim { bag_proto_visitor fn; void *ud; };

static void src_cb  (uint64_t k, bag_id_t id, void *s)
    { struct bag_src_shim   *sh = s; sh->fn(k,         id, sh->ud); }
static void dst_cb  (uint64_t k, bag_id_t id, void *s)
    { struct bag_dst_shim   *sh = s; sh->fn(k,         id, sh->ud); }
static void port_cb (uint64_t k, bag_id_t id, void *s)
    { struct bag_port_shim  *sh = s; sh->fn((int)k,    id, sh->ud); }
static void proto_cb(uint64_t k, bag_id_t id, void *s)
    { struct bag_proto_shim *sh = s; sh->fn((int)k,    id, sh->ud); }

void bag_src_foreach  (bag_src_visitor   fn, void *ud)
    { struct bag_src_shim   sh = { fn, ud }; bagmap_foreach(&g_bag_src,   src_cb,   &sh); }
void bag_dst_foreach  (bag_dst_visitor   fn, void *ud)
    { struct bag_dst_shim   sh = { fn, ud }; bagmap_foreach(&g_bag_dst,   dst_cb,   &sh); }
void bag_port_foreach (bag_port_visitor  fn, void *ud)
    { struct bag_port_shim  sh = { fn, ud }; bagmap_foreach(&g_bag_port,  port_cb,  &sh); }
void bag_proto_foreach(bag_proto_visitor fn, void *ud)
    { struct bag_proto_shim sh = { fn, ud }; bagmap_foreach(&g_bag_proto, proto_cb, &sh); }

/* ============================================================
 *  print
 * ============================================================ */

/* Render a bitvec LSB-first, 0..max_bit wide, into buf. */
static char *fmt_bits(const rule_bitvec *v, int max_bit, char *buf) {
    int i = 0;
    for (int b = 0; b <= max_bit; b++) buf[i++] = rvec_test(v, b) ? '1' : '0';
    buf[i] = '\0';
    return buf;
}

/* Header row(s) showing rule-id columns. */
static void print_bit_header(int indent, int max_bit) {
    char tens[MAX_RULES + 1];
    char ones[MAX_RULES + 1];
    for (int b = 0; b <= max_bit; b++) {
        tens[b] = (b >= 10) ? ('0' + (b / 10) % 10) : ' ';
        ones[b] = '0' + (b % 10);
    }
    tens[max_bit + 1] = ones[max_bit + 1] = '\0';
    if (max_bit >= 10) printf("%*srule:  %s\n", indent, "", tens);
    printf("%*srule:  %s\n", indent, "", ones);
}

extern eid_node *eid_list_head(void);

static int eid_ord_by_hash(uint64_t hash) {
    int i = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next, i++)
        if (e->hash == hash) return i;
    return -1;
}

/* Dump a bagmap as "KEY -> BAG[id]" rows. Key formatter is pluggable.
 * When `sort_as_eid` is set, we sort keys by their EID ordinal so output
 * reads EID[0], EID[1], EID[2], ...; otherwise we sort by raw key. */
static void print_map(const char *name, const bagmap *b,
                      void (*fmt_key)(uint64_t, char *, size_t),
                      int sort_as_eid) {
    printf("\n%-11s (%zu entries):\n", name, b->size);
    typedef struct { uint64_t key; bag_id_t id; int sort_k; } kv;
    kv *arr = calloc(b->size, sizeof *arr);
    size_t n = 0;
    for (size_t i = 0; i < b->cap; i++)
        if (b->slots[i].state) {
            arr[n].key    = b->slots[i].key;
            arr[n].id     = b->slots[i].id;
            arr[n].sort_k = sort_as_eid ? eid_ord_by_hash(b->slots[i].key)
                                        : (int)b->slots[i].key;
            n++;
        }
    /* Bubble-sort — small n. */
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (arr[i].sort_k > arr[j].sort_k)
                { kv t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
    char kbuf[32];
    for (size_t i = 0; i < n; i++) {
        fmt_key(arr[i].key, kbuf, sizeof kbuf);
        printf("  %-10s -> BAG[%u]\n", kbuf, arr[i].id);
    }
    free(arr);
}

static void fmt_eid_key(uint64_t h, char *buf, size_t n) {
    int ord = eid_ord_by_hash(h);
    if (ord >= 0) snprintf(buf, n, "EID[%d]", ord);
    else          snprintf(buf, n, "0x%llx", (unsigned long long)h);
}
static void fmt_port_key(uint64_t p, char *buf, size_t n) {
    snprintf(buf, n, "%u", (unsigned)p);
}
static void fmt_proto_key(uint64_t p, char *buf, size_t n) {
    const char *name =
        ((int)p == PROTO_TCP) ? "TCP" :
        ((int)p == PROTO_UDP) ? "UDP" : "???";
    snprintf(buf, n, "%s", name);
}

void print_bags(void) {
    printf("========================================\n");
    printf(" BAGVEC   (%zu unique vectors, 2 reserved)\n", g_vecs.n);
    printf("========================================\n");

    if (g_max_rule_id < 0) {
        printf("  (no resolved rules — bags are empty)\n\n");
        return;
    }

    char bits[MAX_RULES + 1];
    /* Print the reserved two first, then the dynamic ones. */
    print_bit_header(12, g_max_rule_id);
    printf("  BAG[0]    %s   (reserved: zero)\n",
           fmt_bits(&g_vecs.vecs[BAG_ID_ZERO], g_max_rule_id, bits));
    printf("  BAG[1]    %s   (reserved: all)\n",
           fmt_bits(&g_vecs.vecs[BAG_ID_ALL],  g_max_rule_id, bits));
    for (size_t i = BAG_ID_FIRST_DYNAMIC; i < g_vecs.n; i++) {
        printf("  BAG[%zu]    %s\n",
               i, fmt_bits(&g_vecs.vecs[i], g_max_rule_id, bits));
    }

    printf("\n========================================\n");
    printf(" BAGS\n");
    printf("========================================\n");
    print_map("g_bag_src",   &g_bag_src,   fmt_eid_key,   1);
    print_map("g_bag_dst",   &g_bag_dst,   fmt_eid_key,   1);
    print_map("g_bag_port",  &g_bag_port,  fmt_port_key,  0);
    print_map("g_bag_proto", &g_bag_proto, fmt_proto_key, 0);
    printf("\n");
}

/* ============================================================
 *  free
 * ============================================================ */

void free_bags(void) {
    bagmap_free(&g_bag_src);
    bagmap_free(&g_bag_dst);
    bagmap_free(&g_bag_port);
    bagmap_free(&g_bag_proto);
    free(g_vecs.vecs);
    g_vecs.vecs = NULL;
    g_vecs.n = g_vecs.cap = 0;
    free(g_vec_idx.slots);
    g_vec_idx.slots = NULL;
    g_vec_idx.cap = g_vec_idx.size = 0;
    g_max_rule_id = -1;
}
