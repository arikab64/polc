/*
 * resolve.h — selector resolution (phase 3a)
 *
 * Takes the selector ASTs on each parsed rule and turns them into a set
 * of matching EIDs. This is the intermediate between the source DSL and
 * the three bitvector bags that follow.
 *
 * Representation: a selector compiles to DNF (disjunction of conjunctions).
 * Each term is a label_set where "bit i set" means "label id i must be
 * present in the EID's bitset". An EID matches the selector iff it
 * matches at least one term (i.e. its bitset is a superset of the term).
 */
#ifndef RESOLVE_H
#define RESOLVE_H

#include <stddef.h>
#include "ast.h"

/* A DNF term: a label_set where each set bit is a required label.
 * An EID matches a term iff (eid.labels & term) == term. */
typedef struct dnf_term {
    label_set         mask;
    int               undefined;   /* 1 if term references a label that
                                      wasn't in the label table — always
                                      false, kept for debug printing. */
    struct dnf_term  *next;
} dnf_term;

/* A full DNF: linked list of terms. An EID matches iff ANY term matches. */
typedef struct {
    dnf_term *terms;
} dnf;

/* A set of EIDs, stored as a dynamic array of pointers (not copies).
 * Order is insertion order; no dedup attempted since the builder already
 * walks each EID once. */
typedef struct {
    eid_node **items;
    size_t     n;
    size_t     cap;
} eid_set;

/* For each rule, we produce this resolved form. Held in a linked list
 * parallel to g_rules, keyed by rule->id. */
typedef struct resolved_rule {
    int                     rule_id;
    dnf                    *src_dnf;
    dnf                    *dst_dnf;
    eid_set                 src_eids;
    eid_set                 dst_eids;
    struct resolved_rule   *next;
} resolved_rule;

/* Phase entry points. */
void             resolve_all_rules(void);    /* walks g_rules, builds resolutions */
void             print_resolutions(void);    /* resolved + unresolved sections */
void             free_resolutions(void);

/* The two lists of resolved_rule — parallel to each other, not to g_rules.
 * Resolved: rules whose src AND dst selectors both match at least one EID.
 * Unresolved: rules where either side matched no EID — dead weight. */
resolved_rule   *resolutions_head(void);
resolved_rule   *unresolved_head(void);

/* Public helpers — kept exposed so a future debugger command can reuse
 * them against arbitrary selector expressions typed at a prompt. */
dnf             *sel_to_dnf(const sel_node *s);
void             dnf_free(dnf *d);
int              dnf_matches(const dnf *d, const label_set *eid_labels);
void             dnf_print(const dnf *d);     /* human-readable to stdout */

#endif
