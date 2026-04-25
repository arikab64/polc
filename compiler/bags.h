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
 * Runtime evaluation contract (LANGUAGE.md §6.2 — `ANY_EID`):
 *   src_eid = ipcache_get(src_ip)        // ANY_EID if missing
 *   dst_eid = ipcache_get(dst_ip)        // ANY_EID if missing
 *   src_vec = bagvec[bag_src[src_eid]] | bagvec[bag_src[ANY_EID]]
 *   dst_vec = bagvec[bag_dst[dst_eid]] | bagvec[bag_dst[ANY_EID]]
 *   verdict = src_vec & dst_vec & bagvec[bag_port[dport]]
 *                               & bagvec[bag_proto[proto]]
 *
 * The OR with `bag_*[ANY_EID]` is what implements the `ALL_EIDS` selector
 * — a side written as bare `ANY` (or a bare subnet, once subnet support
 * lands) contributes its rule bit ONLY to the ANY_EID slot, never to the
 * concrete-EID slots. Concrete EIDs hold only the bits from rules whose
 * selector named them via labels. The runtime OR fuses the two streams.
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

/* ANY_EID — the sentinel EID hash used as a key in g_bag_src / g_bag_dst
 * to hold the "matches every EID" bits.
 *
 * Per LANGUAGE.md §6.2: "If an IP is not in the inventory, eid(addr)
 * returns ANY_EID." The runtime ipcache fallback returns this value;
 * the bag entry at this key carries the bits of every rule whose side
 * is the ALL_EIDS selector (`SEL_ALL` — bare `ANY`).
 *
 * Value 0 is chosen as the sentinel. It collides with the hash of an
 * empty (no-labels) EID — but that case is benign: an empty-labels EID
 * matches no non-trivial selector AND matches every SEL_ALL selector,
 * which is exactly the bitset stored at ANY_EID. So the collision yields
 * the correct semantics even if it ever occurs in practice.
 *
 * (The grammar requires `=> [ label+ ]`, so an entity with zero labels
 * is unreachable through normal source. Keeping the sentinel-collision
 * argument anyway because it costs nothing.) */
#define ANY_EID                ((uint64_t)0)

/* Public build + dump + free. */
void        build_bags(void);
void        print_bags(void);
void        free_bags(void);

/* Look up the bitvec content for a bag_id. Returns NULL if id is out of
 * range (which should never happen for ids returned by the map getters). */
const rule_bitvec *bagvec_get(bag_id_t id);

/* Total count of stored bitvecs (>=2 — the two reserved are always present). */
size_t      bagvec_count(void);

/* Four map getters. Missing keys return BAG_ID_ZERO — the simulator can
 * just AND the returned bitvec unconditionally, no miss/hit branch needed.
 *
 * Note: `bag_src_id(ANY_EID)` / `bag_dst_id(ANY_EID)` returns the bag_id
 * for the ALL_EIDS slot if any rule contributed to it, else BAG_ID_ZERO.
 * The runtime is responsible for OR'ing this into the per-EID lookup. */
bag_id_t    bag_src_id  (uint64_t eid);
bag_id_t    bag_dst_id  (uint64_t eid);
bag_id_t    bag_port_id (int      port);
bag_id_t    bag_proto_id(int      proto);   /* PROTO_TCP / PROTO_UDP */

/* Walkers for the four bagmaps. fn is called once per (key, bag_id)
 * pair in the map. Only non-empty mappings are visited (missing keys
 * aren't stored; they implicitly map to BAG_ID_ZERO).
 *
 * For g_bag_src / g_bag_dst, the ANY_EID slot — if populated — is
 * enumerated alongside the concrete-EID slots. Consumers that render
 * the bags should special-case key == ANY_EID and label it accordingly
 * (e.g. "ALL_EIDS" or "ANY"). */
typedef void (*bag_src_visitor  )(uint64_t eid,  bag_id_t id, void *ud);
typedef void (*bag_dst_visitor  )(uint64_t eid,  bag_id_t id, void *ud);
typedef void (*bag_port_visitor )(int      port, bag_id_t id, void *ud);
typedef void (*bag_proto_visitor)(int      proto,bag_id_t id, void *ud);

void bag_src_foreach  (bag_src_visitor   fn, void *ud);
void bag_dst_foreach  (bag_dst_visitor   fn, void *ud);
void bag_port_foreach (bag_port_visitor  fn, void *ud);
void bag_proto_foreach(bag_proto_visitor fn, void *ud);

#endif
