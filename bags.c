/*
 * bags.c — build the four enforcement bags from resolved rules.
 *
 * Keyed bags (src, dst, port) are small open-addressing hashtables that
 * map a key to a rule_bitvec. Linear probing, 75% load factor, grow by
 * doubling, SplitMix64 of the key for the hash. Same shape as ipcache.c,
 * just with a bigger value type (512-byte bitvec per entry).
 *
 * The proto bag is just two named bitvecs — there are only two protos.
 */
#include "bags.h"
#include "ast.h"
#include "resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bitvec primitives ---- */

static inline void rvec_clear(rule_bitvec *v) {
    for (int i = 0; i < RULE_BITVEC_WORDS; i++) v->w[i] = 0;
}
static inline void rvec_set(rule_bitvec *v, int bit) {
    v->w[bit >> 6] |= (uint64_t)1 << (bit & 63);
}
static inline int rvec_test(const rule_bitvec *v, int bit) {
    return (v->w[bit >> 6] >> (bit & 63)) & 1;
}

/* ---- generic uint64-keyed bag (used for src_bag and dst_bag) ----
 * Small open-addressing hashtable. Each slot carries an "occupied"
 * byte so the zero key 0 is a legitimate value.
 */
typedef struct {
    uint64_t     key;
    rule_bitvec  vec;
    uint8_t      state;   /* 0 = empty, 1 = occupied */
} bag64_slot;

typedef struct {
    bag64_slot *slots;
    size_t      cap;
    size_t      size;
} bag64;

#define BAG_INITIAL_CAP 16

static inline size_t mix_key(uint64_t k, size_t mask) {
    return (size_t)finalize_64(k) & mask;
}

/* Find-or-create semantics: return a pointer to the slot for `key`,
 * inserting it with an empty bitvec if it wasn't there. Grows the
 * table if needed. */
static rule_bitvec *bag64_vec(bag64 *b, uint64_t key) {
    /* Grow at 75% load. Check *before* inserting so we don't overflow. */
    if (b->cap == 0 || (b->size + 1) * 4 > b->cap * 3) {
        size_t new_cap = b->cap ? b->cap * 2 : BAG_INITIAL_CAP;
        bag64_slot *ns = calloc(new_cap, sizeof *ns);
        /* Rehash. */
        size_t mask = new_cap - 1;
        for (size_t i = 0; i < b->cap; i++) {
            if (!b->slots[i].state) continue;
            size_t idx = mix_key(b->slots[i].key, mask);
            while (ns[idx].state) idx = (idx + 1) & mask;
            ns[idx] = b->slots[i];
        }
        free(b->slots);
        b->slots = ns;
        b->cap   = new_cap;
    }

    size_t mask = b->cap - 1;
    size_t idx  = mix_key(key, mask);
    for (;;) {
        bag64_slot *s = &b->slots[idx];
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

/* Lookup only — returns NULL on miss without inserting. */
static const rule_bitvec *bag64_find(const bag64 *b, uint64_t key) {
    if (b->cap == 0) return NULL;
    size_t mask = b->cap - 1;
    size_t idx  = mix_key(key, mask);
    for (;;) {
        const bag64_slot *s = &b->slots[idx];
        if (!s->state)    return NULL;
        if (s->key == key) return &s->vec;
        idx = (idx + 1) & mask;
    }
}

/* Return pointers to all occupied slots in insertion-order-independent order.
 * Caller provides the array; n is set to count. Used by printers. */
typedef struct { uint64_t key; const rule_bitvec *vec; } bag64_entry;
static size_t bag64_collect(const bag64 *b, bag64_entry *out) {
    size_t n = 0;
    for (size_t i = 0; i < b->cap; i++) {
        if (b->slots[i].state) {
            out[n].key = b->slots[i].key;
            out[n].vec = &b->slots[i].vec;
            n++;
        }
    }
    return n;
}

static void bag64_free(bag64 *b) {
    free(b->slots);
    b->slots = NULL;
    b->cap = b->size = 0;
}

/* ---- the four bags ---- */

static bag64       g_src_bag;
static bag64       g_dst_bag;
static bag64       g_port_bag;   /* same shape; keys are just small */
static rule_bitvec g_proto_tcp;
static rule_bitvec g_proto_udp;

/* Track the highest rule id we've actually set, so the printer knows
 * how many bit columns to show. */
static int g_max_rule_id = -1;

/* ---- build ---- */

extern rule_node *rule_list_head(void);  /* from main.c */

/* Look up a rule_node by id. The resolved_rule carries only an id, so
 * we cross-reference back to the parsed rule_node for ports/protos. */
static const rule_node *rule_by_id(int id) {
    for (rule_node *r = rule_list_head(); r; r = r->next)
        if (r->id == id) return r;
    return NULL;
}

void build_bags(void) {
    /* Clean slate in case build_bags is called twice (e.g. a future REPL). */
    bag64_free(&g_src_bag);
    bag64_free(&g_dst_bag);
    bag64_free(&g_port_bag);
    rvec_clear(&g_proto_tcp);
    rvec_clear(&g_proto_udp);
    g_max_rule_id = -1;

    for (resolved_rule *rr = resolutions_head(); rr; rr = rr->next) {
        const rule_node *rule = rule_by_id(rr->rule_id);
        if (!rule) continue;           /* defensive; shouldn't happen */
        if (rule->id > g_max_rule_id) g_max_rule_id = rule->id;

        /* src_bag: one bit set per (src_eid, rule) */
        for (size_t i = 0; i < rr->src_eids.n; i++) {
            rule_bitvec *v = bag64_vec(&g_src_bag, rr->src_eids.items[i]->hash);
            rvec_set(v, rule->id);
        }
        /* dst_bag */
        for (size_t i = 0; i < rr->dst_eids.n; i++) {
            rule_bitvec *v = bag64_vec(&g_dst_bag, rr->dst_eids.items[i]->hash);
            rvec_set(v, rule->id);
        }
        /* port_bag */
        for (port_node *p = rule->ports; p; p = p->next) {
            rule_bitvec *v = bag64_vec(&g_port_bag, (uint64_t)p->port);
            rvec_set(v, rule->id);
        }
        /* proto_bag */
        if (rule->protos & PROTO_TCP) rvec_set(&g_proto_tcp, rule->id);
        if (rule->protos & PROTO_UDP) rvec_set(&g_proto_udp, rule->id);
    }
}

/* ---- print ---- */

/* Render bits 0..max_bit LSB-first (rule 0 on the left) as a string of
 * '1'/'0'. Returns the provided buffer. Caller owns enough space
 * (max_bit + 2 bytes). */
static char *fmt_bits(const rule_bitvec *v, int max_bit, char *buf) {
    int i = 0;
    for (int b = 0; b <= max_bit; b++) buf[i++] = rvec_test(v, b) ? '1' : '0';
    buf[i] = '\0';
    return buf;
}

/* Find the EID's ordinal in g_eids so we can print "EID[N]" labels. */
extern eid_node *eid_list_head(void);
static int eid_index_of_hash(uint64_t hash) {
    int i = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next, i++)
        if (e->hash == hash) return i;
    return -1;
}

/* Print a header row like "          rule:  0123456" so column alignment
 * shows which bit is which rule. Only prints rule-id digits mod 10 (so
 * rule 12 shows "2"); for clarity we also print a second row with the
 * tens digit when max_bit >= 10. */
static void print_bit_header(int indent, int max_bit) {
    char tens[MAX_RULES + 1];
    char ones[MAX_RULES + 1];
    for (int b = 0; b <= max_bit; b++) {
        tens[b] = (b >= 10) ? ('0' + (b / 10) % 10) : ' ';
        ones[b] = '0' + (b % 10);
    }
    tens[max_bit + 1] = ones[max_bit + 1] = '\0';

    if (max_bit >= 10)
        printf("%*srule:  %s\n", indent, "", tens);
    printf("%*srule:  %s\n", indent, "", ones);
}

void print_bags(void) {
    printf("========================================\n");
    printf(" BAGS\n");
    printf("========================================\n");

    if (g_max_rule_id < 0) {
        printf("  (no resolved rules — bags are empty)\n\n");
        return;
    }

    char bits[MAX_RULES + 1];

    /* --- src_bag --- */
    printf("\nsrc_bag  (%zu entries):\n", g_src_bag.size);
    {
        bag64_entry *arr = calloc(g_src_bag.size, sizeof *arr);
        size_t n = bag64_collect(&g_src_bag, arr);
        /* Print rule-id header. */
        print_bit_header(16, g_max_rule_id);
        /* Sort by EID ordinal for predictable output. */
        for (int ord = 0; ord < (int)n + 10; ord++) {
            /* linear scan — small n */
            for (size_t i = 0; i < n; i++) {
                if (eid_index_of_hash(arr[i].key) != ord) continue;
                printf("  EID[%d]   %s\n",
                       ord, fmt_bits(arr[i].vec, g_max_rule_id, bits));
            }
        }
        free(arr);
    }

    /* --- dst_bag --- */
    printf("\ndst_bag  (%zu entries):\n", g_dst_bag.size);
    {
        bag64_entry *arr = calloc(g_dst_bag.size, sizeof *arr);
        size_t n = bag64_collect(&g_dst_bag, arr);
        print_bit_header(16, g_max_rule_id);
        for (int ord = 0; ord < (int)n + 10; ord++) {
            for (size_t i = 0; i < n; i++) {
                if (eid_index_of_hash(arr[i].key) != ord) continue;
                printf("  EID[%d]   %s\n",
                       ord, fmt_bits(arr[i].vec, g_max_rule_id, bits));
            }
        }
        free(arr);
    }

    /* --- port_bag --- */
    printf("\nport_bag (%zu entries):\n", g_port_bag.size);
    {
        bag64_entry *arr = calloc(g_port_bag.size, sizeof *arr);
        size_t n = bag64_collect(&g_port_bag, arr);
        print_bit_header(16, g_max_rule_id);
        /* Sort by port ascending for readability. Bubble-sort — n is tiny. */
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++)
                if (arr[i].key > arr[j].key) {
                    bag64_entry t = arr[i]; arr[i] = arr[j]; arr[j] = t;
                }
        for (size_t i = 0; i < n; i++) {
            printf("  %-8lu %s\n",
                   (unsigned long)arr[i].key,
                   fmt_bits(arr[i].vec, g_max_rule_id, bits));
        }
        free(arr);
    }

    /* --- proto_bag --- */
    printf("\nproto_bag:\n");
    print_bit_header(16, g_max_rule_id);
    printf("  TCP      %s\n", fmt_bits(&g_proto_tcp, g_max_rule_id, bits));
    printf("  UDP      %s\n", fmt_bits(&g_proto_udp, g_max_rule_id, bits));

    printf("\n");
}

/* ---- accessors ---- */

int src_bag_get(uint64_t eid, rule_bitvec *out) {
    const rule_bitvec *v = bag64_find(&g_src_bag, eid);
    if (!v) return 0;
    *out = *v;
    return 1;
}
int dst_bag_get(uint64_t eid, rule_bitvec *out) {
    const rule_bitvec *v = bag64_find(&g_dst_bag, eid);
    if (!v) return 0;
    *out = *v;
    return 1;
}
int port_bag_get(int port, rule_bitvec *out) {
    const rule_bitvec *v = bag64_find(&g_port_bag, (uint64_t)port);
    if (!v) return 0;
    *out = *v;
    return 1;
}

/* ---- free ---- */

void free_bags(void) {
    bag64_free(&g_src_bag);
    bag64_free(&g_dst_bag);
    bag64_free(&g_port_bag);
    rvec_clear(&g_proto_tcp);
    rvec_clear(&g_proto_udp);
    g_max_rule_id = -1;
}
