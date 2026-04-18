/*
 * bags.h — four enforcement bags (phase 3b), with bitvec interning.
 *
 * Each bag maps a key to a bag_id, which references a uniqued bitvector
 * in the global bagvec store. Identical bitvectors across bags share one
 * id, so storage grows with DISTINCT bitvec content, not with key count.
 *
 * The four bags and their key types:
 *   g_bag_src   : EID.hash → bag_id
 *   g_bag_dst   : EID.hash → bag_id
 *   g_bag_port  : port     → bag_id
 *   g_bag_proto : proto    → bag_id     (only TCP and UDP)
 *
 * A packet is allowed iff:
 *   bagvec[g_bag_src[src_eid]] &
 *   bagvec[g_bag_dst[dst_eid]] &
 *   bagvec[g_bag_port[dport]]  &
 *   bagvec[g_bag_proto[proto]]
 * is nonzero. Compile time builds the structures; runtime (eBPF) evaluates.
 *
 * Two bag_ids are reserved:
 *   BAG_ID_ZERO = 0  — the all-zero bitvec. Returned when a key isn't in
 *                      a bag, so the simulator never branches on "missing key".
 *   BAG_ID_ALL  = 1  — the all-ones bitvec. Reserved for future wildcard
 *                      semantics; not populated from today's compile path.
 */
#ifndef BAGS_H
#define BAGS_H

#include <stdint.h>
#include <stddef.h>
#include "ast.h"   /* MAX_RULES */

/* One bitvector = MAX_RULES bits = 64 words of 64 bits each (MAX_RULES=4096). */
#define RULE_BITVEC_WORDS   (MAX_RULES / 64)

typedef struct {
    uint64_t w[RULE_BITVEC_WORDS];
} rule_bitvec;

/* Interned bag identifier. 32 bits is plenty — any realistic policy has
 * well under 4 billion distinct bitvectors. */
typedef uint32_t bag_id_t;

#define BAG_ID_ZERO            0u
#define BAG_ID_ALL             1u
#define BAG_ID_FIRST_DYNAMIC   2u

/* Public build + dump + free. */
void        build_bags(void);
void        print_bags(void);
void        free_bags(void);

/* Look up the bitvec content for a bag_id. Returns NULL if id is out of
 * range (which should never happen for ids returned by the map getters). */
const rule_bitvec *bagvec_get(bag_id_t id);

/* Four map getters. Missing keys return BAG_ID_ZERO — the simulator can
 * just AND the returned bitvec unconditionally, no miss/hit branch needed. */
bag_id_t    bag_src_id  (uint64_t eid);
bag_id_t    bag_dst_id  (uint64_t eid);
bag_id_t    bag_port_id (int      port);
bag_id_t    bag_proto_id(int      proto);   /* PROTO_TCP / PROTO_UDP */

#endif
