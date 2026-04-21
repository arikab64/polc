/*
 * resolve.c — compile selectors to DNF, match against EID bitsets.
 */
#include "resolve.h"
#include "diag.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* We need access to the symbol tables and global EID list from main.c.
 * Declared extern here; defined in main.c. */
extern int g_semantic_errors;

/* These two are internal to main.c normally — expose just enough for us. */
extern int       label_lookup_id(const char *key, const char *val);  /* 0 if missing */
extern eid_node *eid_list_head(void);

/* ---------- tiny helpers -------------------------------------------- */

static dnf_term *mk_term(void) {
    return calloc(1, sizeof(dnf_term));
}

static dnf *mk_dnf(void) {
    return calloc(1, sizeof(dnf));
}

static void dnf_append_term(dnf *d, dnf_term *t) {
    if (!d->terms) { d->terms = t; return; }
    dnf_term *tail = d->terms;
    while (tail->next) tail = tail->next;
    tail->next = t;
}

/* Concatenate b's terms onto a. b is consumed (freed). */
static void dnf_concat(dnf *a, dnf *b) {
    if (!b) return;
    if (!b->terms) { free(b); return; }
    if (!a->terms) { a->terms = b->terms; b->terms = NULL; free(b); return; }
    dnf_term *tail = a->terms;
    while (tail->next) tail = tail->next;
    tail->next = b->terms;
    b->terms = NULL;
    free(b);
}

/* Union two label_sets (bitwise OR). */
static void lset_union(label_set *dst, const label_set *a, const label_set *b) {
    for (int i = 0; i < LABEL_WORDS; i++) dst->w[i] = a->w[i] | b->w[i];
}

/* Subset test: is (needle & haystack) == needle?  I.e. does haystack
 * contain all bits of needle? */
static int lset_subset(const label_set *needle, const label_set *haystack) {
    for (int i = 0; i < LABEL_WORDS; i++)
        if ((needle->w[i] & haystack->w[i]) != needle->w[i]) return 0;
    return 1;
}

/* ---------- DNF compilation ---------------------------------------- */

/* Leaf: one term with one bit set (or "undefined" if the label isn't in
 * the label table — term mask stays zero and can never match anything
 * nonempty, but we flag it so the debug output can call it out). */
static dnf *dnf_leaf(const char *key, const char *val) {
    dnf      *d = mk_dnf();
    dnf_term *t = mk_term();
    int       id = label_lookup_id(key, val);
    if (id > 0) {
        lset_set(&t->mask, id);
    } else {
        t->undefined = 1;
    }
    d->terms = t;
    return d;
}

/* OR: concatenate the term lists. */
static dnf *dnf_or(dnf *a, dnf *b) {
    dnf_concat(a, b);
    return a;
}

/* AND: cross-product. For every (term_a, term_b) pair, produce a new
 * term with mask = term_a.mask | term_b.mask (union of requirements). */
static dnf *dnf_and(dnf *a, dnf *b) {
    dnf *out = mk_dnf();
    for (dnf_term *ta = a->terms; ta; ta = ta->next) {
        for (dnf_term *tb = b->terms; tb; tb = tb->next) {
            dnf_term *nt = mk_term();
            lset_union(&nt->mask, &ta->mask, &tb->mask);
            nt->undefined = ta->undefined || tb->undefined;
            dnf_append_term(out, nt);
        }
    }
    dnf_free(a);
    dnf_free(b);
    return out;
}

dnf *sel_to_dnf(const sel_node *s) {
    if (!s) return mk_dnf();   /* empty DNF matches nothing */
    switch (s->kind) {
        case SEL_LEAF: return dnf_leaf(s->key, s->val);
        case SEL_OR:   return dnf_or (sel_to_dnf(s->lhs), sel_to_dnf(s->rhs));
        case SEL_AND:  return dnf_and(sel_to_dnf(s->lhs), sel_to_dnf(s->rhs));
    }
    return mk_dnf();
}

void dnf_free(dnf *d) {
    if (!d) return;
    dnf_term *t = d->terms;
    while (t) { dnf_term *n = t->next; free(t); t = n; }
    free(d);
}

/* ---------- matching ------------------------------------------------ */

int dnf_matches(const dnf *d, const label_set *eid_labels) {
    for (dnf_term *t = d->terms; t; t = t->next) {
        if (t->undefined) continue;   /* unsatisfiable; skip */
        if (lset_subset(&t->mask, eid_labels)) return 1;
    }
    return 0;
}

/* ---------- human-readable printing -------------------------------- */

static void print_term(const dnf_term *t) {
    int first = 1;
    int any = 0;
    for (int id = 1; id <= MAX_LABEL_ID; id++) {
        if (!lset_test(&t->mask, id)) continue;
        const char *k = NULL, *v = NULL;
        if (label_lookup(id, &k, &v)) {
            printf("%s%s:%s", first ? "" : " AND ", k, v);
            first = 0;
            any = 1;
        }
    }
    if (!any) printf("<empty>");
    if (t->undefined) printf("  [undefined label → always false]");
}

void dnf_print(const dnf *d) {
    if (!d || !d->terms) { printf("<no terms>"); return; }
    int first = 1;
    for (dnf_term *t = d->terms; t; t = t->next) {
        if (!first) printf("  OR  ");
        /* Parenthesize a term if it has more than one conjunct for
         * readability at the OR boundary. */
        int n = 0;
        for (int id = 1; id <= MAX_LABEL_ID; id++)
            if (lset_test(&t->mask, id)) n++;
        if (n > 1) printf("(");
        print_term(t);
        if (n > 1) printf(")");
        first = 0;
    }
}

/* ---------- EID set (dynamic array) -------------------------------- */

static void eid_set_push(eid_set *s, eid_node *e) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->items = realloc(s->items, s->cap * sizeof *s->items);
    }
    s->items[s->n++] = e;
}

static void eid_set_free(eid_set *s) {
    free(s->items);
    s->items = NULL;
    s->n = s->cap = 0;
}

/* ---------- per-rule resolution ------------------------------------ */

static resolved_rule *g_resolved        = NULL;
static resolved_rule *g_resolved_tail   = NULL;
static resolved_rule *g_unresolved      = NULL;
static resolved_rule *g_unresolved_tail = NULL;

static resolved_rule *resolve_one(const rule_node *r) {
    resolved_rule *rr = calloc(1, sizeof *rr);
    rr->rule_id = r->id;
    rr->src_dnf = sel_to_dnf(r->src);
    rr->dst_dnf = sel_to_dnf(r->dst);

    /* Walk every EID once for src, once for dst. O(|EIDs|). */
    for (eid_node *e = eid_list_head(); e; e = e->next) {
        if (dnf_matches(rr->src_dnf, &e->labels)) eid_set_push(&rr->src_eids, e);
        if (dnf_matches(rr->dst_dnf, &e->labels)) eid_set_push(&rr->dst_eids, e);
    }
    return rr;
}

extern rule_node *rule_list_head(void);   /* defined in main.c */

void resolve_all_rules(void) {
    for (rule_node *r = rule_list_head(); r; r = r->next) {
        resolved_rule *rr = resolve_one(r);

        /* Triage: a rule where either side matches zero EIDs is dead in
         * the current inventory — can never fire. Park it in the
         * unresolved bucket and emit a warning. Keep the compilation
         * successful; this isn't an error, just a heads-up. */
        if (rr->src_eids.n == 0 || rr->dst_eids.n == 0) {
            const char *which =
                (rr->src_eids.n == 0 && rr->dst_eids.n == 0) ? "src and dst" :
                (rr->src_eids.n == 0)                         ? "src"         :
                                                                "dst";
            diag_warning(r->line, r->col,
                "rule R[%d] has no matching entities — %s selector"
                " matches no EIDs in the current inventory",
                r->id, which);

            if (g_unresolved_tail) g_unresolved_tail->next = rr;
            else                   g_unresolved = rr;
            g_unresolved_tail = rr;
        } else {
            if (g_resolved_tail) g_resolved_tail->next = rr;
            else                 g_resolved = rr;
            g_resolved_tail = rr;
        }
    }
}

resolved_rule *resolutions_head(void)   { return g_resolved;   }
resolved_rule *unresolved_head(void)    { return g_unresolved; }

void free_resolutions(void) {
    for (resolved_rule *r = g_resolved; r; ) {
        resolved_rule *n = r->next;
        dnf_free(r->src_dnf);
        dnf_free(r->dst_dnf);
        eid_set_free(&r->src_eids);
        eid_set_free(&r->dst_eids);
        free(r);
        r = n;
    }
    g_resolved = g_resolved_tail = NULL;

    for (resolved_rule *r = g_unresolved; r; ) {
        resolved_rule *n = r->next;
        dnf_free(r->src_dnf);
        dnf_free(r->dst_dnf);
        eid_set_free(&r->src_eids);
        eid_set_free(&r->dst_eids);
        free(r);
        r = n;
    }
    g_unresolved = g_unresolved_tail = NULL;
}

/* ---------- output section ----------------------------------------- */

/* Find the ordinal of an eid_node in the global list — for "EID[N]" labels. */
static int eid_index_of(const eid_node *target) {
    int i = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next, i++)
        if (e == target) return i;
    return -1;
}

/* Action name stringification — duplicated here rather than plumbed
 * through another header. Small enough to copy. */
static const char *action_name(action_kind a) {
    switch (a) {
        case ACT_ALLOW:          return "ALLOW";
        case ACT_BLOCK:          return "BLOCK";
        case ACT_OVERRIDE_ALLOW: return "OVERRIDE-ALLOW";
        case ACT_OVERRIDE_BLOCK: return "OVERRIDE-BLOCK";
    }
    return "???";
}

/* Look up a rule_node by its id. O(n) but n is small. */
static const rule_node *rule_by_id(int id) {
    for (rule_node *r = rule_list_head(); r; r = r->next)
        if (r->id == id) return r;
    return NULL;
}

/* Shared renderer for a single resolved_rule block. */
static void print_one_resolved(const resolved_rule *rr) {
    const rule_node *rule = rule_by_id(rr->rule_id);
    if (!rule) { printf("R[%d] <missing rule_node>\n", rr->rule_id); return; }

    printf("\nR[%d] %s\n", rule->id, action_name(rule->action));

    printf("     src dnf  : ");
    dnf_print(rr->src_dnf);
    printf("\n");
    printf("     src eids : ");
    if (rr->src_eids.n == 0) { printf("<none>"); }
    else for (size_t i = 0; i < rr->src_eids.n; i++) {
        printf("%sEID[%d]", i ? ", " : "", eid_index_of(rr->src_eids.items[i]));
    }
    printf("\n");

    printf("     dst dnf  : ");
    dnf_print(rr->dst_dnf);
    printf("\n");
    printf("     dst eids : ");
    if (rr->dst_eids.n == 0) { printf("<none>"); }
    else for (size_t i = 0; i < rr->dst_eids.n; i++) {
        printf("%sEID[%d]", i ? ", " : "", eid_index_of(rr->dst_eids.items[i]));
    }
    printf("\n");

    printf("     ports    : ");
    /* Mirror print_rules() in main.c: a single port_node with value 0
     * is the ANY wildcard; otherwise render the list numerically. */
    if (rule->ports && rule->ports->port == 0 && rule->ports->next == NULL) {
        printf("ANY");
    } else {
        int first = 1;
        for (port_node *p = rule->ports; p; p = p->next) {
            printf("%s%d", first ? "" : ", ", p->port);
            first = 0;
        }
    }
    printf("\n");

    printf("     protos   : ");
    /* Full mask == all known protos == ANY. */
    if (rule->protos == (PROTO_TCP | PROTO_UDP)) {
        printf("ANY");
    } else {
        int first = 1;
        if (rule->protos & PROTO_TCP) { printf("TCP"); first = 0; }
        if (rule->protos & PROTO_UDP) { printf("%sUDP", first ? "" : ", "); first = 0; }
        if (first) printf("(none)");
    }
    printf("\n");
}

void print_resolutions(void) {
    /* Counts. */
    int n_res = 0, n_unres = 0;
    for (resolved_rule *r = g_resolved;   r; r = r->next) n_res++;
    for (resolved_rule *r = g_unresolved; r; r = r->next) n_unres++;

    /* --- resolved --- */
    printf("========================================\n");
    printf(" RESOLVED RULES   (%d rules → EIDs)\n", n_res);
    printf("========================================\n");
    if (n_res == 0) {
        printf("    (none)\n");
    } else {
        for (resolved_rule *r = g_resolved; r; r = r->next) print_one_resolved(r);
    }
    printf("\n");

    /* --- unresolved --- */
    printf("========================================\n");
    printf(" UNRESOLVED RULES   (%d rules skipped)\n", n_unres);
    printf("========================================\n");
    if (n_unres == 0) {
        printf("    (none)\n");
    } else {
        printf("Rules below match no entities in the current inventory and\n");
        printf("will not contribute to the bags.\n");
        for (resolved_rule *r = g_unresolved; r; r = r->next) print_one_resolved(r);
    }
    printf("\n");
}
