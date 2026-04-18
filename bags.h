/*
 * bags.h — the four enforcement bags (phase 3b).
 *
 * A "bag" maps a key to a 4096-bit bitvector where bit i means "rule i
 * is relevant to this key." The four bags:
 *
 *   src_bag   — EID   → bitvec of rules whose src selector matches
 *   dst_bag   — EID   → bitvec of rules whose dst selector matches
 *   port_bag  — port  → bitvec of rules that list this port
 *   proto_bag — proto → bitvec of rules that list this proto (just TCP/UDP)
 *
 * A packet is allowed iff, at runtime:
 *   src_bag[src_eid] & dst_bag[dst_eid] & port_bag[dport] & proto_bag[proto]
 * is nonzero. (Compile-time we just build; runtime is eBPF's job.)
 */
#ifndef BAGS_H
#define BAGS_H

#include <stdint.h>
#include <stddef.h>
#include "ast.h"   /* MAX_RULES */

/* One bitvector = 4096 bits = 64 words of 64 bits each. */
#define RULE_BITVEC_WORDS   (MAX_RULES / 64)

typedef struct {
    uint64_t w[RULE_BITVEC_WORDS];
} rule_bitvec;

/* Public build + dump + free. */
void    build_bags(void);
void    print_bags(void);
void    free_bags(void);

/* Accessors for the phase 4 enforcer (runtime simulation). */
int     src_bag_get (uint64_t eid,  rule_bitvec *out);
int     dst_bag_get (uint64_t eid,  rule_bitvec *out);
int     port_bag_get(int      port, rule_bitvec *out);

#endif
