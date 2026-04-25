/*
 * main.c — entry point, constructors, and pretty-printers
 *
 * Model: enforcement identities are the primary objects. Parsing an entity
 * produces a bitset → we find or create the identity with that bitset →
 * we attach the entity as a member of that identity.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "diag.h"
#include "ipcache.h"
#include "resolve.h"
#include "bags.h"
#include "builder.h"
#include "dump.h"

extern int   yyparse(void);
extern FILE *yyin;

/* ============================================================
 *  LABEL TABLE — intern distinct (key,val) pairs, assign ids
 * ============================================================ */

static label_entry *g_labels      = NULL;
static label_entry *g_labels_tail = NULL;   /* O(1) append */
static int          g_next_label_id = 1;

int g_semantic_errors = 0;

label_entry *label_list_head(void) { return g_labels; }

int label_intern(const char *key, const char *val, int line, int col) {
    for (label_entry *e = g_labels; e; e = e->next) {
        if (strcmp(e->key, key) == 0 && strcmp(e->val, val) == 0)
            return e->id;
    }
    if (g_next_label_id > MAX_LABEL_ID) {
        diag_error(line, col,
            "label table full — limit is %d distinct labels "
            "(refused '%s:%s')",
            MAX_LABEL_ID, key, val);
        g_semantic_errors++;
        return 0;
    }
    label_entry *e = calloc(1, sizeof *e);
    e->key = strdup(key);
    e->val = strdup(val);
    e->id  = g_next_label_id++;
    if (g_labels_tail) g_labels_tail->next = e; else g_labels = e;
    g_labels_tail = e;
    return e->id;
}

int label_lookup(int id, const char **key_out, const char **val_out) {
    for (label_entry *e = g_labels; e; e = e->next) {
        if (e->id == id) {
            if (key_out) *key_out = e->key;
            if (val_out) *val_out = e->val;
            return 1;
        }
    }
    return 0;
}

/* Lookup by (key,val) — returns the id (>=1) if interned, 0 if not.
 * This is the read-only counterpart to label_intern; used by the resolver
 * to check whether a selector references a known label. */
int label_lookup_id(const char *key, const char *val) {
    for (label_entry *e = g_labels; e; e = e->next)
        if (strcmp(e->key, key) == 0 && strcmp(e->val, val) == 0)
            return e->id;
    return 0;
}

/* ============================================================
 *  STRING LIST & LABEL LIST CONSTRUCTORS
 * ============================================================ */

/* ---------- IP helpers -----------------------------------------------
 * Parse "A.B.C.D" into a host-order uint32. We don't use inet_pton so we
 * can emit nice diag_error locations and we control what "malformed"
 * means (here: any octet > 255). The scanner's regex already guarantees
 * exactly four dot-separated runs of 1..3 digits — so this function only
 * needs to validate the 0..255 range.
 * ------------------------------------------------------------------- */
int ip_parse(const char *s, int line, int col, uint32_t *out) {
    unsigned a, b, c, d;
    char extra;
    /* %c at the end catches trailing garbage: sscanf returns 5 for "1.2.3.4x". */
    int n = sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra);
    if (n != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        diag_error(line, col, "malformed IP address '%s'", s);
        g_semantic_errors++;
        return 0;
    }
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

char *ip_fmt(uint32_t addr, char buf[16]) {
    snprintf(buf, 16, "%u.%u.%u.%u",
             (addr >> 24) & 0xff,
             (addr >> 16) & 0xff,
             (addr >>  8) & 0xff,
              addr        & 0xff);
    return buf;
}

ip_node *mk_ip(const char *text, int line, int col) {
    ip_node *n = calloc(1, sizeof *n);
    n->line  = line;
    n->col   = col;
    n->valid = ip_parse(text, line, col, &n->addr);
    return n;
}

ip_node *ip_append(ip_node *head, ip_node *node) {
    if (!head) return node;
    ip_node *t = head;
    while (t->next) t = t->next;
    t->next = node;
    return head;
}

label_node *mk_label(const char *k, const char *v, int line, int col) {
    label_node *n = calloc(1, sizeof *n);
    n->key  = strdup(k);
    n->val  = strdup(v);
    n->line = line;
    n->col  = col;
    n->id   = label_intern(k, v, line, col);
    return n;
}

label_node *label_append(label_node *head, label_node *node) {
    if (!head) return node;
    label_node *t = head;
    while (t->next) t = t->next;
    t->next = node;
    return head;
}

/* ============================================================
 *  ENFORCEMENT IDENTITIES — the primary object
 * ============================================================ */

static eid_node *g_eids      = NULL;
static eid_node *g_eids_tail = NULL;

/* Accessor for resolve.c and any future consumers that need to walk
 * the parsed EIDs without owning the storage. */
eid_node *eid_list_head(void) { return g_eids; }

/* Find the identity with this exact bitset, or NULL. */
static eid_node *eid_find(const label_set *labels) {
    for (eid_node *e = g_eids; e; e = e->next)
        if (lset_eq(&e->labels, labels)) return e;
    return NULL;
}

/* Create a new identity (appended at tail so first-seen order is preserved). */
static eid_node *eid_create(const label_set *labels) {
    eid_node *e = calloc(1, sizeof *e);
    e->labels = *labels;
    e->hash   = lset_hash(labels);
    if (g_eids_tail) g_eids_tail->next = e; else g_eids = e;
    g_eids_tail = e;
    return e;
}

/* Append a member (name, ips) to the tail of an identity's member list. */
static void eid_add_member(eid_node *e, const char *name,
                           int line, int col, ip_node *ips) {
    member_node *m = calloc(1, sizeof *m);
    m->name = strdup(name);
    m->line = line;
    m->col  = col;
    m->ips  = ips;
    if (!e->members) { e->members = m; return; }
    member_node *t = e->members;
    while (t->next) t = t->next;
    t->next = m;
}

/* ----------------------------------------------------------------
 *  add_entity — called from the grammar for each parsed entity line.
 *  Folds the label list into a bitset, checks for duplicate label
 *  keys, resolves-or-creates the identity, and attaches the entity
 *  as a member. (ent_line, ent_col) is the position of the entity's
 *  NAME token in the source.
 * ---------------------------------------------------------------- */
void add_entity(const char *name, int ent_line, int ent_col,
                ip_node *ips, label_node *labels) {
    label_set bitset;
    lset_clear(&bitset);

    /* Walk the transient label list, check for dup keys, set bits.
     * When a dup is found, point the caret at the *second* (offending)
     * label and reference the earlier one in the message. */
    for (label_node *l = labels; l; l = l->next) {
        int dup = 0;
        for (label_node *p = labels; p != l; p = p->next) {
            if (strcmp(p->key, l->key) == 0) {
                diag_error(l->line, l->col,
                    "duplicate label key '%s' in entity '%s' "
                    "(first seen as '%s:%s' at line %d:%d)",
                    l->key, name, p->key, p->val, p->line, p->col);
                g_semantic_errors++;
                dup = 1;
                break;
            }
        }
        if (!dup && l->id > 0) lset_set(&bitset, l->id);
    }

    /* Free the transient label list — the bitset is canonical now. */
    while (labels) {
        label_node *n = labels->next;
        free(labels->key); free(labels->val); free(labels);
        labels = n;
    }

    /* Check that no IP on this entity is already claimed by another entity.
     * An IP is a network-layer identifier and must be unique across the
     * whole inventory — even across different identities. Also catch
     * duplicates within this entity's own IP list. Comparisons are now
     * plain uint32_t equality, much cheaper than strcmp. */
    char ipbuf[16];
    for (ip_node *new_ip = ips; new_ip; new_ip = new_ip->next) {
        if (!new_ip->valid) continue;    /* parse error already reported */
        int already_flagged = 0;

        for (ip_node *earlier = ips; earlier != new_ip; earlier = earlier->next) {
            if (!earlier->valid) continue;
            if (earlier->addr == new_ip->addr) {
                diag_error(new_ip->line, new_ip->col,
                    "IP '%s' listed twice in entity '%s' "
                    "(first at line %d:%d)",
                    ip_fmt(new_ip->addr, ipbuf), name,
                    earlier->line, earlier->col);
                g_semantic_errors++;
                already_flagged = 1;
                break;
            }
        }
        if (already_flagged) continue;

        /* Duplicate against any previously-registered entity. Report the
         * first collision we find (canonical "earliest declaration") and
         * stop — otherwise a list already-malformed upstream would produce
         * N*M error lines. */
        int found = 0;
        for (eid_node *e2 = g_eids; e2 && !found; e2 = e2->next) {
            for (member_node *m2 = e2->members; m2 && !found; m2 = m2->next) {
                for (ip_node *old_ip = m2->ips; old_ip; old_ip = old_ip->next) {
                    if (!old_ip->valid) continue;
                    if (new_ip->addr == old_ip->addr) {
                        diag_error(new_ip->line, new_ip->col,
                            "IP '%s' already assigned to entity '%s' "
                            "(declared at line %d:%d)",
                            ip_fmt(new_ip->addr, ipbuf), m2->name,
                            m2->line, m2->col);
                        g_semantic_errors++;
                        found = 1;
                        break;
                    }
                }
            }
        }
    }

    /* Resolve-or-create the identity, attach this entity as a member. */
    eid_node *e = eid_find(&bitset);
    if (!e) e = eid_create(&bitset);
    eid_add_member(e, name, ent_line, ent_col, ips);
}

/* ============================================================
 *  PRETTY-PRINTERS (label table, identities)
 * ============================================================ */

void print_label_table(void) {
    int count = 0;
    for (label_entry *e = g_labels; e; e = e->next) count++;

    printf("========================================\n");
    printf(" LABEL TABLE   (%d distinct labels)\n", count);
    printf("========================================\n");
    printf("   id    key          value\n");
    printf("   ---   ----------   -------------\n");
    for (label_entry *e = g_labels; e; e = e->next)
        printf("   %-3d   %-10s   %s\n", e->id, e->key, e->val);
    printf("\n");
}

/* Compact hex for the bitset: strip leading zeros on the top word. */
static void print_lset_hex(const label_set *s) {
    int top = LABEL_WORDS - 1;
    while (top > 0 && s->w[top] == 0) top--;
    printf("%lx", (unsigned long)s->w[top]);
    for (int i = top - 1; i >= 0; i--)
        printf("%016lx", (unsigned long)s->w[i]);
}

void print_identities(void) {
    int n_ids = 0, n_members = 0;
    for (eid_node *e = g_eids; e; e = e->next) {
        n_ids++;
        for (member_node *m = e->members; m; m = m->next) n_members++;
    }

    printf("========================================\n");
    printf(" ENFORCEMENT IDENTITIES   (%d EIDs from %d entities)\n",
           n_ids, n_members);
    printf("========================================\n");

    int idx = 0;
    for (eid_node *e = g_eids; e; e = e->next, idx++) {
        printf("\nEID[%d] = 0x%016lx   (bitset=", idx, (unsigned long)e->hash);
        print_lset_hex(&e->labels);
        printf(")\n");

        printf("    labels : ");
        int any = 0;
        for (int id = 1; id <= MAX_LABEL_ID; id++) {
            if (!lset_test(&e->labels, id)) continue;
            const char *k = NULL, *v = NULL;
            if (label_lookup(id, &k, &v)) {
                printf("%s%s:%s", any ? ", " : "", k, v);
                any = 1;
            }
        }
        if (!any) printf("(none)");
        printf("\n");

        /* Endpoints: flat list of every IP resolving to this identity.
         * Invalid IPs (ones that failed ip_parse) are silently skipped —
         * they were already reported at parse time and we shouldn't print
         * 0.0.0.0 as if it were real. */
        int n_ips = 0;
        for (member_node *m = e->members; m; m = m->next)
            for (ip_node *s = m->ips; s; s = s->next)
                if (s->valid) n_ips++;
        printf("    endpoints (%d): ", n_ips);
        char ipbuf[16];
        int first = 1;
        for (member_node *m = e->members; m; m = m->next) {
            for (ip_node *s = m->ips; s; s = s->next) {
                if (!s->valid) continue;
                printf("%s%s", first ? "" : ", ", ip_fmt(s->addr, ipbuf));
                first = 0;
            }
        }
        printf("\n");

        printf("    members:\n");
        for (member_node *m = e->members; m; m = m->next) {
            printf("        %s  [", m->name);
            int mfirst = 1;
            for (ip_node *s = m->ips; s; s = s->next) {
                if (!s->valid) continue;
                printf("%s%s", mfirst ? "" : " ", ip_fmt(s->addr, ipbuf));
                mfirst = 0;
            }
            printf("]\n");
        }
    }
    printf("\n");
}

/* ============================================================
 *  VARIABLES — $label-var and @port-var
 * ============================================================ */

/* Simple linked-list symbol tables. Lookup is linear; realistic var
 * counts are tiny. (Types declared in ast.h so the builder can walk them.) */
static var_label_entry *g_vlabels = NULL;
static var_port_entry  *g_vports  = NULL;

var_label_entry *var_label_head(void) { return g_vlabels; }
var_port_entry  *var_port_head (void) { return g_vports;  }

/* Deep-copy helpers — variable expansion needs fresh nodes each time so
 * the original definition stays intact for subsequent references. */
static label_node *dup_labels(const label_node *src) {
    label_node *head = NULL, *tail = NULL;
    for (; src; src = src->next) {
        label_node *n = calloc(1, sizeof *n);
        n->key  = strdup(src->key);
        n->val  = strdup(src->val);
        n->id   = src->id;
        n->line = src->line;
        n->col  = src->col;
        if (!head) head = n; else tail->next = n;
        tail = n;
    }
    return head;
}

static port_node *dup_ports(const port_node *src) {
    port_node *head = NULL, *tail = NULL;
    for (; src; src = src->next) {
        port_node *n = calloc(1, sizeof *n);
        n->port = src->port;
        n->line = src->line;
        n->col  = src->col;
        if (!head) head = n; else tail->next = n;
        tail = n;
    }
    return head;
}

void var_label_define(const char *name, label_node *labels, int line, int col) {
    for (var_label_entry *e = g_vlabels; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            diag_error(line, col,
                "variable $%s already defined at line %d:%d",
                name, e->line, e->col);
            g_semantic_errors++;
            /* Free the new labels since we're rejecting the redefinition. */
            while (labels) {
                label_node *n = labels->next;
                free(labels->key); free(labels->val); free(labels);
                labels = n;
            }
            return;
        }
    }
    var_label_entry *e = calloc(1, sizeof *e);
    e->name   = strdup(name);
    e->labels = labels;
    e->line   = line;
    e->col    = col;
    e->next   = g_vlabels;
    g_vlabels = e;
}

void var_port_define(const char *name, port_node *ports, int line, int col) {
    for (var_port_entry *e = g_vports; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            diag_error(line, col,
                "variable @%s already defined at line %d:%d",
                name, e->line, e->col);
            g_semantic_errors++;
            while (ports) { port_node *n = ports->next; free(ports); ports = n; }
            return;
        }
    }
    var_port_entry *e = calloc(1, sizeof *e);
    e->name  = strdup(name);
    e->ports = ports;
    e->line  = line;
    e->col   = col;
    e->next  = g_vports;
    g_vports = e;
}

label_node *var_label_lookup(const char *name) {
    for (var_label_entry *e = g_vlabels; e; e = e->next)
        if (strcmp(e->name, name) == 0) return dup_labels(e->labels);
    return NULL;
}

port_node *var_port_lookup(const char *name) {
    for (var_port_entry *e = g_vports; e; e = e->next)
        if (strcmp(e->name, name) == 0) return dup_ports(e->ports);
    return NULL;
}

/* Build an OR-tree from a label list. Multi-element lists fold right:
 *   [a, b, c] -> OR(a, OR(b, c)).
 * Consumes the input list (frees label_node wrappers, but the strings are
 * strdup'd into the new sel_node leaves). */
sel_node *sel_from_labels(label_node *labels) {
    if (!labels) return NULL;

    /* Convert list to an array of leaves so we can fold right easily. */
    int n = 0;
    for (label_node *l = labels; l; l = l->next) n++;

    sel_node **leaves = calloc(n, sizeof *leaves);
    int i = 0;
    label_node *cur = labels;
    while (cur) {
        leaves[i++] = sel_leaf(cur->key, cur->val, cur->line, cur->col);
        label_node *next = cur->next;
        free(cur->key); free(cur->val); free(cur);
        cur = next;
    }

    /* Right-fold: start from the rightmost leaf and wrap it with ORs. */
    sel_node *acc = leaves[n - 1];
    for (int j = n - 2; j >= 0; j--)
        acc = sel_binop(SEL_OR, leaves[j], acc);

    free(leaves);
    return acc;
}

/* ============================================================
 *  SELECTOR & PORT CONSTRUCTORS
 * ============================================================ */

/* Forward decls so add_rule's overflow path can clean up. */
static void free_sel(sel_node *s);
static void free_subnet(subnet_node *s);

static rule_node *g_rules      = NULL;
static rule_node *g_rules_tail = NULL;

rule_node *rule_list_head(void) { return g_rules; }

sel_node *sel_leaf(const char *k, const char *v, int line, int col) {
    sel_node *n = calloc(1, sizeof *n);
    n->kind = SEL_LEAF;
    n->key  = strdup(k);
    n->val  = strdup(v);
    n->line = line;
    n->col  = col;
    return n;
}

sel_node *sel_binop(sel_kind kind, sel_node *lhs, sel_node *rhs) {
    sel_node *n = calloc(1, sizeof *n);
    n->kind = kind;
    n->lhs  = lhs;
    n->rhs  = rhs;
    /* binop's position is inherited from its left subexpr */
    n->line = lhs ? lhs->line : 0;
    n->col  = lhs ? lhs->col  : 0;
    return n;
}

sel_node *sel_all(void) {
    sel_node *n = calloc(1, sizeof *n);
    n->kind = SEL_ALL;
    return n;
}

port_node *mk_port(int p, int line, int col) {
    port_node *n = calloc(1, sizeof *n);
    n->port = p;
    n->line = line;
    n->col  = col;
    return n;
}

port_node *port_append(port_node *head, port_node *node) {
    if (!head) return node;
    port_node *t = head;
    while (t->next) t = t->next;
    t->next = node;
    return head;
}

/* ============================================================
 *  SUBNET AST
 * ============================================================ */

subnet_node *mk_subnet_cidr(uint32_t addr, int prefix, int line, int col) {
    subnet_node *n = calloc(1, sizeof *n);
    n->kind   = SN_CIDR;
    n->addr   = addr;
    n->prefix = prefix;
    n->line   = line;
    n->col    = col;
    return n;
}

subnet_node *mk_subnet_range(uint32_t lo, uint32_t hi, int line, int col) {
    subnet_node *n = calloc(1, sizeof *n);
    n->kind     = SN_RANGE;
    n->range_lo = lo;
    n->range_hi = hi;
    n->line     = line;
    n->col      = col;
    return n;
}

subnet_node *subnet_binop(subnet_kind kind, subnet_node *lhs, subnet_node *rhs) {
    subnet_node *n = calloc(1, sizeof *n);
    n->kind = kind;
    n->lhs  = lhs;
    n->rhs  = rhs;
    n->line = lhs ? lhs->line : 0;
    n->col  = lhs ? lhs->col  : 0;
    return n;
}

/* Sentinels — fresh tree per call so each rule owns its subtree.
 *
 * and-ANY is the intersection identity: every non-zero IPv4 address.
 * We model it as a SN_RANGE rather than a CIDR because no single CIDR
 * captures 0.0.0.1..255.255.255.255 (that range is 0/0 minus the host
 * 0.0.0.0). Encoding it directly as a range keeps the semantics exact
 * and avoids needing a "negation" node just for this one sentinel.
 *
 * or-ANY is the union identity: empty in effect (only matches 0.0.0.0,
 * which is never a real source/destination). Encoded as 0.0.0.0/32 per
 * §6.3. */
subnet_node *mk_subnet_and_any(void) {
    return mk_subnet_range(0x00000001u, 0xFFFFFFFFu, 0, 0);
}

subnet_node *mk_subnet_or_any(void) {
    return mk_subnet_cidr(0x00000000u, 32, 0, 0);
}

static void free_subnet(subnet_node *s) {
    if (!s) return;
    if (s->kind == SN_AND || s->kind == SN_OR) {
        free_subnet(s->lhs);
        free_subnet(s->rhs);
    }
    free(s);
}

/* ============================================================
 *  RULE_SIDE — parser-internal carriers
 * ============================================================
 *
 * Each constructor materialises whichever sentinels the caller didn't
 * supply, so by the time add_rule sees the side both sand and sor are
 * non-NULL. add_rule frees the rule_side wrapper after copying the
 * fields out.
 */

rule_side *mk_side_any(void) {
    rule_side *s = calloc(1, sizeof *s);
    s->sel       = sel_all();
    s->sand      = mk_subnet_and_any();
    s->sor       = mk_subnet_or_any();
    return s;
}

rule_side *mk_side_sel(sel_node *sel) {
    rule_side *s = calloc(1, sizeof *s);
    s->sel  = sel;
    s->sand = mk_subnet_and_any();
    s->sor  = mk_subnet_or_any();
    return s;
}

rule_side *mk_side_and(sel_node *sel, subnet_node *sub) {
    rule_side *s = calloc(1, sizeof *s);
    s->sel  = sel;
    s->sand = sub;                  /* user-supplied AND-subnet     */
    s->sor  = mk_subnet_or_any();   /* OR-subnet defaults to or-ANY */
    return s;
}

rule_side *mk_side_or(sel_node *sel, subnet_node *sub) {
    rule_side *s = calloc(1, sizeof *s);
    s->sel  = sel;
    s->sand = mk_subnet_and_any();  /* AND-subnet defaults to and-ANY */
    s->sor  = sub;                  /* user-supplied OR-subnet        */
    return s;
}

rule_side *mk_side_subn(subnet_node *sub) {
    /* Bare subnet form per §5.2 row 5: L=ALL_EIDS, S_and=subnet,
     * S_or=or-ANY. Same triple shape as `selector AND subnet` except
     * the selector is the ALL_EIDS sentinel. */
    rule_side *s = calloc(1, sizeof *s);
    s->sel       = sel_all();
    s->sand      = sub;
    s->sor       = mk_subnet_or_any();
    return s;
}

/* ============================================================
 *  add_rule — invoked once per parsed rule from the grammar
 * ============================================================ */

void add_rule(action_kind a, int line, int col,
              rule_side *src, rule_side *dst,
              port_node *ports, unsigned protos)
{
    static int next_id = 0;

    /* Enforce the rule cap. Going over means we'd need a wider bitvector
     * in the bags — reject rather than silently truncate. Free everything
     * we were handed (including the sides' subnet trees) so we don't leak. */
    if (next_id > MAX_RULE_ID) {
        diag_error(line, col,
            "rule table full — limit is %d rules", MAX_RULES);
        g_semantic_errors++;

        if (src) {
            free_sel   (src->sel);
            free_subnet(src->sand);
            free_subnet(src->sor);
            free(src);
        }
        if (dst) {
            free_sel   (dst->sel);
            free_subnet(dst->sand);
            free_subnet(dst->sor);
            free(dst);
        }
        while (ports) { port_node *n = ports->next; free(ports); ports = n; }
        return;
    }

    rule_node *r = calloc(1, sizeof *r);
    r->id     = next_id++;
    r->action = a;

    /* Move the side fields into the rule_node, then free the wrapper. */
    r->src           = src->sel;
    r->src_sand      = src->sand;
    r->src_sor       = src->sor;
    free(src);

    r->dst           = dst->sel;
    r->dst_sand      = dst->sand;
    r->dst_sor       = dst->sor;
    free(dst);

    r->ports  = ports;
    r->protos = protos;
    r->line   = line;
    r->col    = col;

    /* Validate ports are in the 0..65535 range. Port 0 is the numeric
     * equivalent of the ANY keyword (wildcard) — legal standalone,
     * already filtered by the parser's port_spec rules against appearing
     * in multi-element lists or ranges. Anything outside this range is
     * a genuine error. Done here so the error message can point at the
     * specific port token. */
    for (port_node *p = r->ports; p; p = p->next) {
        if (p->port < 0 || p->port > 65535) {
            diag_error(p->line, p->col,
                "port %d out of range (must be 0..65535, where 0 = ANY)",
                p->port);
            g_semantic_errors++;
        }
    }

    if (g_rules_tail) g_rules_tail->next = r; else g_rules = r;
    g_rules_tail = r;
}

/* ============================================================
 *  RULE PRETTY-PRINTING
 * ============================================================
 *
 * The printer's job is round-trippable readability: every rule should
 * print in a form that, modulo whitespace, parses back to the same AST.
 * That means showing AND/OR-subnets explicitly when they're non-sentinel,
 * and rendering bare ANY when both selector and subnets are at their
 * identity values. */

static const char *action_name(action_kind a) {
    switch (a) {
        case ACT_ALLOW:          return "ALLOW";
        case ACT_BLOCK:          return "BLOCK";
        case ACT_OVERRIDE_ALLOW: return "OVERRIDE-ALLOW";
        case ACT_OVERRIDE_BLOCK: return "OVERRIDE-BLOCK";
    }
    return "???";
}

/* Render a selector tree. To keep the output unambiguous:
 *   - OR nodes inside an AND get parens (since AND binds tighter)
 *   - AND nodes inside an OR get parens for clarity (not strictly needed)
 * Leaves never get parens. */
static void print_selector(const sel_node *s) {
    if (!s) { printf("(empty)"); return; }
    switch (s->kind) {
        case SEL_LEAF:
            printf("%s:%s", s->key, s->val);
            break;
        case SEL_AND:
            /* Left: wrap if it's an OR (looser binding). */
            if (s->lhs && s->lhs->kind == SEL_OR) { printf("("); print_selector(s->lhs); printf(")"); }
            else                                    print_selector(s->lhs);
            printf(" AND ");
            if (s->rhs && s->rhs->kind == SEL_OR) { printf("("); print_selector(s->rhs); printf(")"); }
            else                                    print_selector(s->rhs);
            break;
        case SEL_OR:
            /* Wrap AND subtrees for clarity. */
            if (s->lhs && s->lhs->kind == SEL_AND) { printf("("); print_selector(s->lhs); printf(")"); }
            else                                    print_selector(s->lhs);
            printf(" OR ");
            if (s->rhs && s->rhs->kind == SEL_AND) { printf("("); print_selector(s->rhs); printf(")"); }
            else                                    print_selector(s->rhs);
            break;
        case SEL_ALL:
            printf("ANY");
            break;
    }
}

/* Sentinel detectors — for round-trip rendering and unresolved-rule
 * triage in resolve.c. */
int subnet_is_and_any(const subnet_node *s) {
    return s && s->kind == SN_RANGE
            && s->range_lo == 0x00000001u
            && s->range_hi == 0xFFFFFFFFu;
}

int subnet_is_or_any(const subnet_node *s) {
    return s && s->kind == SN_CIDR
            && s->addr == 0u
            && s->prefix == 32;
}

static void print_subnet(const subnet_node *s) {
    if (!s) { printf("<null>"); return; }
    char buf[16];
    switch (s->kind) {
    case SN_CIDR:
        printf("%s/%d", ip_fmt(s->addr, buf), s->prefix);
        return;
    case SN_RANGE:
        if (subnet_is_and_any(s)) { printf("and-ANY"); return; }
        {
            char buf2[16];
            printf("%s..%s", ip_fmt(s->range_lo, buf), ip_fmt(s->range_hi, buf2));
        }
        return;
    case SN_AND:
        printf("(");
        print_subnet(s->lhs); printf(" AND "); print_subnet(s->rhs);
        printf(")");
        return;
    case SN_OR:
        printf("(");
        print_subnet(s->lhs); printf(" OR ");  print_subnet(s->rhs);
        printf(")");
        return;
    }
}

/* Render one side using the §5.2 mapping in reverse. */
static void print_side(const sel_node *sel,
                       const subnet_node *sand, const subnet_node *sor)
{
    int match_all = (sel->kind == SEL_ALL);
    int sand_id   = subnet_is_and_any(sand);
    int sor_id    = subnet_is_or_any (sor);

    /* row 1: ANY (ALL_EIDS selector, both subnets at identity) */
    if (match_all && sand_id && sor_id) { printf("ANY"); return; }

    /* row 5: bare subnet (ALL_EIDS selector, sand non-identity, sor at identity) */
    if (match_all && !sand_id && sor_id) { print_subnet(sand); return; }

    /* row 2: selector only */
    if (!match_all && sand_id && sor_id) { print_selector(sel); return; }

    /* row 3: selector AND subnet */
    if (!match_all && !sand_id && sor_id) {
        print_selector(sel); printf(" AND "); print_subnet(sand); return;
    }

    /* row 4: selector OR subnet */
    if (!match_all && sand_id && !sor_id) {
        print_selector(sel); printf(" OR "); print_subnet(sor); return;
    }

    /* Catch-all — combinations the grammar doesn't produce, but render
     * something sensible if they ever appear (e.g. via direct AST edits). */
    printf("[");
    print_selector(sel);
    printf(", S_and="); print_subnet(sand);
    printf(", S_or=");  print_subnet(sor);
    printf("]");
}

void print_rules(void) {
    int n = 0;
    for (rule_node *r = g_rules; r; r = r->next) n++;

    printf("========================================\n");
    printf(" POLICY RULES   (%d rules)\n", n);
    printf("========================================\n");

    for (rule_node *r = g_rules; r; r = r->next) {
        printf("\nR[%d] %s\n", r->id, action_name(r->action));

        printf("     src    : ");
        print_side(r->src, r->src_sand, r->src_sor);
        printf("\n");

        printf("     dst    : ");
        print_side(r->dst, r->dst_sand, r->dst_sor);
        printf("\n");

        printf("     ports  : ");
        /* A single port_node with value 0 is the ANY wildcard — render
         * it symbolically. Any other list prints numerically; a stray 0
         * embedded in a multi-element list would show as "0" so a bug
         * (which the parser should prevent) is visible rather than hidden. */
        if (r->ports && r->ports->port == 0 && r->ports->next == NULL) {
            printf("ANY");
        } else {
            int first = 1;
            for (port_node *p = r->ports; p; p = p->next) {
                printf("%s%d", first ? "" : ", ", p->port);
                first = 0;
            }
        }
        printf("\n");
        printf("     protos : ");
        /* Full mask == all known protos == ANY keyword in source. */
        if (r->protos == (PROTO_TCP | PROTO_UDP)) {
            printf("ANY");
        } else {
            int first = 1;
            if (r->protos & PROTO_TCP) { printf("TCP"); first = 0; }
            if (r->protos & PROTO_UDP) { printf("%sUDP", first ? "" : ", "); first = 0; }
            if (first) printf("(none)");
        }
        printf("\n");
    }
    printf("\n");
}

static void free_sel(sel_node *s) {
    if (!s) return;
    switch (s->kind) {
        case SEL_LEAF: free(s->key); free(s->val);          break;
        case SEL_AND:
        case SEL_OR:   free_sel(s->lhs); free_sel(s->rhs);  break;
        case SEL_ALL:                                       break;
    }
    free(s);
}

/* free_rules / free_ports are only invoked from main() at shutdown — gate
 * them under !POLC_TESTING so the test build doesn't trip
 * -Wunused-function. free_sel / free_subnet stay outside the gate because
 * add_rule's overflow path also calls them. */
#ifndef POLC_TESTING
static void free_ports(port_node *p) {
    while (p) { port_node *n = p->next; free(p); p = n; }
}

static void free_rules(void) {
    rule_node *r = g_rules;
    while (r) {
        rule_node *n = r->next;
        free_sel   (r->src);
        free_subnet(r->src_sand);
        free_subnet(r->src_sor);
        free_sel   (r->dst);
        free_subnet(r->dst_sand);
        free_subnet(r->dst_sor);
        free_ports (r->ports);
        free(r);
        r = n;
    }
    g_rules = g_rules_tail = NULL;
}
#endif

/* ============================================================
 *  IPCACHE — populate from parsed identities, then pretty-print
 * ============================================================ */

/* Everything from here to the end of the file is CLI-side glue:
 * build_ipcache / print_ipcache, the slurp/usage helpers, and main().
 * The unit test binary builds with -DPOLC_TESTING so it can link a
 * standalone AST library without dragging in ipcache, resolve, bags,
 * builder, dump, sqlite, or the parser/scanner objects. */
#ifndef POLC_TESTING

/* Walk every identity's member IPs and populate the IP → EID hashtable.
 * Skips invalid IPs (parse failed earlier). Reports as an internal error
 * if two valid IPs somehow map to different EIDs — that would mean the
 * per-entity IP-dedup check failed upstream. */
static void build_ipcache(void) {
    ipcache_init();
    for (eid_node *e = g_eids; e; e = e->next) {
        for (member_node *m = e->members; m; m = m->next) {
            for (ip_node *ip = m->ips; ip; ip = ip->next) {
                if (!ip->valid) continue;
                int rc = ipcache_put(ip->addr, e->hash);
                if (rc < 0) {
                    /* Defensive: should never happen given prior dedup. */
                    char ipbuf[16];
                    diag_error(ip->line, ip->col,
                        "internal: IP '%s' maps to two different EIDs",
                        ip_fmt(ip->addr, ipbuf));
                    g_semantic_errors++;
                }
            }
        }
    }
}

/* Collect entries into a caller-provided array for sorted output. */
typedef struct { uint32_t addr; uint64_t eid; } ipc_entry;

static void collect_cb(uint32_t addr, uint64_t eid, void *ud) {
    ipc_entry **cursor = (ipc_entry **)ud;
    (*cursor)->addr = addr;
    (*cursor)->eid  = eid;
    (*cursor)++;
}

static int cmp_by_addr(const void *a, const void *b) {
    uint32_t x = ((const ipc_entry *)a)->addr;
    uint32_t y = ((const ipc_entry *)b)->addr;
    return (x > y) - (x < y);
}

/* Find the EID's index in the global list (for pretty "EID[N]" labels). */
static int eid_index_of(uint64_t hash) {
    int idx = 0;
    for (eid_node *e = g_eids; e; e = e->next, idx++)
        if (e->hash == hash) return idx;
    return -1;
}

void print_ipcache(void) {
    size_t n = ipcache_size();
    printf("========================================\n");
    printf(" IPCACHE   (%zu entries, cap=%zu, %zu collisions on insert)\n",
           n, ipcache_capacity(), ipcache_collisions());
    printf("========================================\n");

    if (n == 0) { printf("\n"); return; }

    /* Sort by IP for deterministic, readable output. Hashtable iteration
     * order is effectively random from the user's perspective. */
    ipc_entry *arr = calloc(n, sizeof *arr);
    ipc_entry *cursor = arr;
    ipcache_foreach(collect_cb, &cursor);
    qsort(arr, n, sizeof *arr, cmp_by_addr);

    char ipbuf[16];
    for (size_t i = 0; i < n; i++) {
        int ei = eid_index_of(arr[i].eid);
        printf("    %-15s  =>  EID[%d]  0x%016lx\n",
               ip_fmt(arr[i].addr, ipbuf),
               ei,
               (unsigned long)arr[i].eid);
    }
    printf("\n");

    free(arr);
}

#endif /* !POLC_TESTING (CLI block 1: ipcache helpers & print_ipcache) */

/* ============================================================
 *  CLEANUP
 * ============================================================ */

static void free_ips(ip_node *n) {
    while (n) { ip_node *x = n->next; free(n); n = x; }
}

static void free_members(member_node *m) {
    while (m) {
        member_node *n = m->next;
        free(m->name);
        free_ips(m->ips);
        free(m);
        m = n;
    }
}

void free_all(void) {
    eid_node *e = g_eids;
    while (e) {
        eid_node *n = e->next;
        free_members(e->members);
        free(e);
        e = n;
    }
    g_eids = g_eids_tail = NULL;

    label_entry *le = g_labels;
    while (le) {
        label_entry *n = le->next;
        free(le->key); free(le->val); free(le);
        le = n;
    }
    g_labels = g_labels_tail = NULL;
    g_next_label_id = 1;

    /* Free variable tables. */
    var_label_entry *vl = g_vlabels;
    while (vl) {
        var_label_entry *n = vl->next;
        label_node *l = vl->labels;
        while (l) {
            label_node *ln = l->next;
            free(l->key); free(l->val); free(l);
            l = ln;
        }
        free(vl->name); free(vl);
        vl = n;
    }
    g_vlabels = NULL;

    var_port_entry *vp = g_vports;
    while (vp) {
        var_port_entry *n = vp->next;
        port_node *p = vp->ports;
        while (p) { port_node *pn = p->next; free(p); p = pn; }
        free(vp->name); free(vp);
        vp = n;
    }
    g_vports = NULL;
}

/* ============================================================
 *  MAIN — CLI driver (excluded from the unit-test build).
 * ============================================================ */
#ifndef POLC_TESTING

/* Slurp an entire FILE* into a NUL-terminated malloc'd buffer.
 * Caller frees. Returns NULL on I/O error. */
static char *slurp(FILE *f) {
    /* Grow-and-read loop — works on non-seekable streams too (stdin). */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        size_t want = cap - len - 1;
        size_t got  = fread(buf + len, 1, want, f);
        len += got;
        if (got < want) {
            if (ferror(f)) { free(buf); return NULL; }
            break;  /* EOF */
        }
        cap *= 2;
        char *nb = realloc(buf, cap);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
    }
    buf[len] = '\0';
    return buf;
}

/* gcc-style "no input files" diagnostic. */
static void emit_no_input(void) {
    fprintf(stderr, "polc: fatal error: no input files\n");
    fprintf(stderr, "compilation terminated.\n");
}

/* Derive a sibling path by swapping the extension.
 *   "out.db"          -> "out.txt"
 *   "a.b.db"          -> "a.b.txt"   (strip last extension only)
 *   "out"             -> "out.txt"   (no extension → just append)
 *   "./build/out.db"  -> "./build/out.txt"
 *   ".hidden"         -> ".hidden.txt" (leading dot is not an extension)
 *
 * Returns a malloc'd string the caller frees. */
static char *replace_ext(const char *path, const char *new_ext) {
    /* Locate the last '/' so we can confine the search to the basename. */
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    /* Last '.' inside the basename — and it must not be at position 0
     * (so ".hidden" isn't mistaken for an empty-stem dotfile extension). */
    const char *dot = strrchr(basename, '.');
    size_t stem_len = (dot && dot > basename)
        ? (size_t)(dot - path)
        : strlen(path);

    size_t n = stem_len + 1 /* dot */ + strlen(new_ext) + 1 /* NUL */;
    char *out = malloc(n);
    memcpy(out, path, stem_len);
    out[stem_len] = '.';
    strcpy(out + stem_len + 1, new_ext);
    return out;
}

static void usage(FILE *out) {
    fprintf(out,
        "Usage: polc -i <input.gc> [-o <out.db>] [options]\n"
        "  -i FILE      input policy file (required)\n"
        "  -o FILE      output SQLite database (default: out.db)\n"
        "  --debug      include symbolic/debug tables (labels, DNF, entity\n"
        "               names, variables, source positions).\n"
        "  -v, --verbose\n"
        "               print the full compilation log (label table, EIDs,\n"
        "               ipcache, rules, resolutions, bags). Default is a\n"
        "               one-line summary.\n"
        "  --dump       after compiling, write a human-readable dump of\n"
        "               every table in the output DB to a sibling .txt\n"
        "               file (same path as -o, extension swapped to .txt).\n"
        "  -h, --help   show this message.\n");
}

/* One-line compile summary — the default (non-verbose) compile log output. */
static void print_summary(int n_labels, int n_eids, int n_rules,
                          int n_resolved, int n_unresolved,
                          size_t n_bagvecs) {
    fprintf(stderr,
        "polc: compiled %d label%s, %d EID%s, %d rule%s (%d resolved, %d unresolved), "
        "%zu bagvec%s\n",
        n_labels, n_labels == 1 ? "" : "s",
        n_eids,   n_eids   == 1 ? "" : "s",
        n_rules,  n_rules  == 1 ? "" : "s",
        n_resolved, n_unresolved,
        n_bagvecs, n_bagvecs == 1 ? "" : "s");
}

/* Helper: count list lengths for the summary. */
static int count_labels(void) {
    int n = 0;
    for (label_entry *e = label_list_head(); e; e = e->next) n++;
    return n;
}
static int count_eids(void) {
    int n = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next) n++;
    return n;
}
static int count_rules(void) {
    int n = 0;
    for (rule_node *r = rule_list_head(); r; r = r->next) n++;
    return n;
}
static int count_resolved(void) {
    int n = 0;
    for (resolved_rule *r = resolutions_head(); r; r = r->next) n++;
    return n;
}
static int count_unresolved(void) {
    int n = 0;
    for (resolved_rule *r = unresolved_head(); r; r = r->next) n++;
    return n;
}

int main(int argc, char **argv) {
    const char *filename = NULL;
    const char *out_path = "out.db";
    int         debug    = 0;
    int         verbose  = 0;
    int         do_dump  = 0;

    /* Manual argv walk. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-i") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "polc: error: -i requires a filename\n");
                return 1;
            }
            filename = argv[++i];
        } else if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "polc: error: -o requires a filename\n");
                return 1;
            }
            out_path = argv[++i];
        } else if (strcmp(a, "--debug") == 0) {
            debug = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "--dump") == 0) {
            do_dump = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "polc: error: unrecognized argument '%s'\n", a);
            usage(stderr);
            return 1;
        }
    }

    if (!filename) {
        emit_no_input();
        return 1;
    }

    FILE *src = fopen(filename, "r");
    if (!src) { perror(filename); return 1; }

    char *source = slurp(src);
    fclose(src);
    if (!source) {
        fprintf(stderr, "error: could not read source\n");
        return 1;
    }

    diag_init(filename, source);
    yyin = fmemopen(source, strlen(source), "r");
    if (!yyin) { perror("fmemopen"); free(source); return 1; }

    int rc = yyparse();
    fclose(yyin);

    if (rc != 0) {
        /* yyerror already printed diagnostics; no need to pile on. */
    } else if (g_semantic_errors > 0) {
        fprintf(stderr, "compilation failed: %d semantic error%s.\n",
                g_semantic_errors, g_semantic_errors == 1 ? "" : "s");
        rc = 1;
    } else {
        /* The interior print_* calls only run under --verbose. Everything
         * else (build_ipcache, resolve_all_rules, build_bags, build_sqlite)
         * always runs — they produce the artifacts. */
        if (verbose) print_label_table();
        if (verbose) print_identities();
        build_ipcache();
        if (g_semantic_errors > 0) {
            fprintf(stderr, "compilation failed: %d semantic error%s.\n",
                    g_semantic_errors, g_semantic_errors == 1 ? "" : "s");
            rc = 1;
        } else {
            if (verbose) print_ipcache();
            if (verbose) print_rules();
            resolve_all_rules();
            if (verbose) print_resolutions();
            build_bags();
            if (verbose) print_bags();

            if (build_sqlite(out_path, filename, debug) != 0) {
                fprintf(stderr, "polc: error: failed to write %s\n", out_path);
                rc = 1;
            } else {
                /* Summary line — the default compile log output. */
                print_summary(count_labels(), count_eids(), count_rules(),
                              count_resolved(), count_unresolved(),
                              bagvec_count());
                fprintf(stderr, "polc: wrote %s%s\n",
                        out_path, debug ? " (with debug tables)" : "");

                if (do_dump) {
                    char *txt_path = replace_ext(out_path, "txt");
                    if (dump_db(out_path, txt_path) != 0) {
                        rc = 1;
                    } else {
                        fprintf(stderr, "polc: dumped to %s\n", txt_path);
                    }
                    free(txt_path);
                }
            }
        }
    }

    free_all();
    free_rules();
    free_resolutions();
    free_bags();
    free(source);
    ipcache_free();
    return rc;
}

#endif /* !POLC_TESTING (CLI block 2: slurp..main) */
