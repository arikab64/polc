/*
 * ast.h — shared AST types for the policy compiler (phase 1: inventory)
 *
 * The enforcement identity (EID) is the primary object. An entity is just
 * a (name, ip-list) pair that contributes membership to some identity;
 * the identity itself is defined by a combination of labels (a 512-bit
 * bitset) and is named by the hash of that bitset.
 */
#ifndef AST_H
#define AST_H

#include <stdint.h>

/* ---------- linked list of IPs (stored as uint32 in host byte order) ---- */
typedef struct ip_node {
    uint32_t         addr;          /* 10.0.0.1 -> 0x0a000001 */
    int              valid;         /* 0 if parse failed — skip in checks */
    int              line, col;     /* source position (1-based) */
    struct ip_node  *next;
} ip_node;

/* Parse "A.B.C.D" into a uint32 (host order). Returns 1 on success, 0 on
 * malformed input (any octet >255) — in that case *out is unchanged and
 * a diagnostic has been emitted at (line, col). */
int ip_parse(const char *s, int line, int col, uint32_t *out);

/* Format a uint32 as "A.B.C.D" into a caller-provided buffer. At least 16
 * bytes required; returns buf. */
char *ip_fmt(uint32_t addr, char buf[16]);

/* ---------- a single label: key => value, with a unique numeric id ------- */
typedef struct label_node {
    char              *key;
    char              *val;
    int                id;      /* unique per distinct (key,val) pair */
    int                line, col;   /* position of the KEY token in source */
    struct label_node *next;
} label_node;

/* ---------- label id space ---------------------------------------------
 * Label ids run 1..MAX_LABEL_ID. Bit 0 is reserved (sentinel: "no label"),
 * so the bitset holds MAX_LABELS = 512 bits and carries up to 511 real
 * labels. 512 bits = 8 × uint64_t words.
 * ----------------------------------------------------------------------- */
#define MAX_LABELS     512
#define MAX_LABEL_ID   (MAX_LABELS - 1)    /* = 511 */
#define LABEL_WORDS    (MAX_LABELS / 64)   /* = 8   */

typedef struct {
    uint64_t w[LABEL_WORDS];
} label_set;

static inline void lset_clear(label_set *s) {
    for (int i = 0; i < LABEL_WORDS; i++) s->w[i] = 0;
}
static inline void lset_set(label_set *s, int id) {
    s->w[id >> 6] |= (uint64_t)1 << (id & 63);
}
static inline int  lset_test(const label_set *s, int id) {
    return (s->w[id >> 6] >> (id & 63)) & 1;
}
static inline int  lset_eq(const label_set *a, const label_set *b) {
    for (int i = 0; i < LABEL_WORDS; i++)
        if (a->w[i] != b->w[i]) return 0;
    return 1;
}

/* SplitMix64 finalizer — bit-mixing function from the splitmix/xoshiro family. */
static inline uint64_t finalize_64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x =  x ^ (x >> 31);
    return x;
}

/* Hash of the 512-bit label set: XOR-fold the 8 words, then run finalize_64. */
static inline uint64_t lset_hash(const label_set *s) {
    uint64_t x = 0;
    for (int i = 0; i < LABEL_WORDS; i++) x ^= s->w[i];
    return finalize_64(x);
}

/* ---------- member entity: just a name and a list of IPs -----------------
 * No standalone entity struct — entities live inside the identity they belong to.
 * ------------------------------------------------------------------------- */
typedef struct member_node {
    char               *name;
    int                 line, col;  /* position of the NAME in source */
    ip_node            *ips;
    struct member_node *next;
} member_node;

/* ---------- enforcement identity -----------------------------------------
 * The primary object. Defined by a 512-bit label set and named by that
 * set's SplitMix64 hash. Holds the list of entities that resolve to it.
 * ------------------------------------------------------------------------- */
typedef struct eid_node {
    label_set          labels;    /* defining label combination */
    uint64_t           hash;      /* derived identity name (EID) */
    member_node       *members;   /* entities that resolve here    */
    struct eid_node   *next;
} eid_node;

/* ---------- constructors / helpers (defined in main.c) ---------- */

ip_node    *mk_ip(const char *text, int line, int col);
ip_node    *ip_append(ip_node *head, ip_node *node);

label_node *mk_label(const char *k, const char *v, int line, int col);
label_node *label_append(label_node *head, label_node *node);

/* Called once per "entity" parse: folds labels into a bitset, finds or
 * creates the matching identity, and records the entity as a member.
 * (line, col) is the position of the entity's NAME token. */
void        add_entity(const char *name, int line, int col,
                       ip_node *ips, label_node *labels);

/* ---------- selector AST ------------------------------------------------
 * Rules live in their own list, parsed from the `policy:` section.
 *
 * A rule's src/dst selectors are small trees over (key:value) leaves joined
 * by AND/OR. In phase 4 these get compiled into DNF and resolved against
 * the EID label table.
 * ------------------------------------------------------------------------- */

typedef enum {
    ACT_ALLOW,
    ACT_BLOCK,
    ACT_OVERRIDE_ALLOW,
    ACT_OVERRIDE_BLOCK,
} action_kind;

/* SEL_ALL is the ALL_EIDS sentinel from LANGUAGE.md §5.2 — a selector that
 * matches every EID. Materialized by the parser for sides that were bare
 * `ANY` or a bare subnet, so consumers never have to special-case "no
 * selector": every rule_side carries a real sel_node. */
typedef enum { SEL_LEAF, SEL_AND, SEL_OR, SEL_ALL } sel_kind;

typedef struct sel_node {
    sel_kind         kind;
    /* leaf only */
    char            *key;
    char            *val;
    /* binop only */
    struct sel_node *lhs;
    struct sel_node *rhs;
    int              line, col;
} sel_node;

#define PROTO_TCP  (1u << 0)
#define PROTO_UDP  (1u << 1)

typedef struct port_node {
    int                port;
    int                line, col;
    struct port_node  *next;
} port_node;

/* ---------- subnet AST --------------------------------------------------
 * A side's S_and (and-subnet) and S_or (or-subnet) per LANGUAGE.md §5.2
 * are each a small boolean tree over CIDR leaves, mirroring the selector
 * AST shape.
 *
 * Per LANGUAGE.md §6.3, the parser MATERIALIZES sentinels for sides that
 * have no subnet attached, so S_and and S_or are guaranteed non-NULL on
 * every parsed rule:
 *
 *   and-ANY  = 0.0.0.1 - 255.255.255.255   (intersection identity)
 *   or-ANY   = 0.0.0.0 / 32                (union identity, single addr 0)
 *
 * The `range` kind exists specifically to encode and-ANY without faking
 * it as a CIDR. Real input syntax produces only `cidr` leaves; `range`
 * is parser-injected.
 * ------------------------------------------------------------------------- */

typedef enum {
    SN_CIDR,         /* IP [ '/' NUMBER ]                — leaf */
    SN_RANGE,        /* low..high (inclusive)            — sentinel-only leaf */
    SN_AND,          /* internal: intersection           */
    SN_OR,           /* internal: union                  */
} subnet_kind;

typedef struct subnet_node {
    subnet_kind          kind;
    /* leaves */
    uint32_t             addr;       /* SN_CIDR: network address (host order)   */
    int                  prefix;     /* SN_CIDR: 0..32                          */
    uint32_t             range_lo;   /* SN_RANGE: inclusive low                 */
    uint32_t             range_hi;   /* SN_RANGE: inclusive high                */
    /* binops */
    struct subnet_node  *lhs;
    struct subnet_node  *rhs;
    /* common */
    int                  line, col;  /* source position (sentinels carry 0,0)   */
} subnet_node;

/* Subnet constructors (defined in main.c). */
subnet_node *mk_subnet_cidr (uint32_t addr, int prefix, int line, int col);
subnet_node *mk_subnet_range(uint32_t lo,   uint32_t hi, int line, int col);
subnet_node *subnet_binop   (subnet_kind kind, subnet_node *lhs, subnet_node *rhs);

/* Sentinel constructors — fresh tree per call so each rule owns its
 * subtree (uniform free walk, no aliasing). */
subnet_node *mk_subnet_and_any(void);   /* 0.0.0.1 .. 255.255.255.255 */
subnet_node *mk_subnet_or_any (void);   /* 0.0.0.0 / 32              */

/* Sentinel detectors — true iff the tree IS the corresponding sentinel
 * (and not a user-supplied subnet). */
int subnet_is_and_any(const subnet_node *s);
int subnet_is_or_any (const subnet_node *s);

/* ---------- rule_side: a parser-internal carrier ----------------------
 * Returned by the `side` production. Bundles everything `add_rule` needs
 * to populate one half of a rule. Heap-allocated so it fits in the bison
 * %union as a pointer; freed by add_rule once consumed.
 * ----------------------------------------------------------------------- */

typedef struct rule_side {
    sel_node    *sel;           /* never NULL — SEL_ALL sentinel if bare ANY  */
    subnet_node *sand;          /* never NULL — mk_subnet_and_any() if absent */
    subnet_node *sor;           /* never NULL — mk_subnet_or_any()  if absent */
} rule_side;

rule_side *mk_side_any  (void);
rule_side *mk_side_sel  (sel_node *sel);                      /* sel only           */
rule_side *mk_side_and  (sel_node *sel, subnet_node *sub);    /* sel AND subnet     */
rule_side *mk_side_or   (sel_node *sel, subnet_node *sub);    /* sel OR subnet      */
rule_side *mk_side_subn (subnet_node *sub);                   /* bare subnet        */

/* ---------- rule AST ---------------------------------------------------
 * A rule carries a full side per LANGUAGE.md §5.2 — the selector L
 * matches by EID, and the two subnet trees S_and / S_or narrow / broaden
 * by IP. The (L, S_and, S_or) triple is per side, so we have it twice
 * (src and dst).
 *
 * Bare `ANY` keyword (or a bare subnet — same rule per §5.2 row 5)
 * yields a `SEL_ALL` selector node (the ALL_EIDS sentinel). Phase 4 sees
 * a uniform `sel != NULL` and dispatches on `sel->kind`.
 *
 * Rule ids are 0-based and double as bit positions in the three-bag
 * bitvectors that phase 6 will build.
 * ----------------------------------------------------------------------- */

#define MAX_RULES       4096
#define MAX_RULE_ID     (MAX_RULES - 1)

typedef struct rule_node {
    int               id;        /* 0-based — bit position in bags */
    action_kind       action;

    /* src side */
    sel_node         *src;             /* never NULL — SEL_ALL if bare ANY */
    subnet_node      *src_sand;        /* never NULL (sentinel)            */
    subnet_node      *src_sor;         /* never NULL (sentinel)            */

    /* dst side */
    sel_node         *dst;             /* never NULL — SEL_ALL if bare ANY */
    subnet_node      *dst_sand;        /* never NULL (sentinel)            */
    subnet_node      *dst_sor;         /* never NULL (sentinel)            */

    port_node        *ports;
    unsigned          protos;          /* bitmask of PROTO_*      */

    int               line, col;       /* position of the action keyword */
    struct rule_node *next;
} rule_node;

/* ---------- variable tables --------------------------------------------
 * $var → list of labels.      @var → list of ports.
 * Defined in the `vars:` section; substituted inline wherever they appear.
 * Purely a macro layer — expansion happens at parse time.
 * ----------------------------------------------------------------------- */
void        var_label_define(const char *name, label_node *labels,
                             int line, int col);
void        var_port_define (const char *name, port_node  *ports,
                             int line, int col);

/* Returns a freshly-allocated deep copy of the stored list (so the caller
 * can append it into a larger list without corrupting the definition).
 * NULL if undefined — caller responsible for emitting the diagnostic. */
label_node *var_label_lookup(const char *name);
port_node  *var_port_lookup (const char *name);

/* selector constructors */
sel_node  *sel_leaf(const char *k, const char *v, int line, int col);
sel_node  *sel_binop(sel_kind kind, sel_node *lhs, sel_node *rhs);
sel_node  *sel_all  (void);   /* ALL_EIDS sentinel — matches every EID */

/* Build a right-folded OR-chain from a label list. A single-element list
 * becomes a plain leaf; a multi-element list becomes
 *   OR(a, OR(b, OR(c, d))). Frees (consumes) the input list. */
sel_node  *sel_from_labels(label_node *labels);

/* port/rule constructors */
port_node *mk_port(int p, int line, int col);
port_node *port_append(port_node *head, port_node *node);

/* add_rule consumes the rule_side wrappers (frees them after copying
 * fields out). The caller is responsible for nothing once add_rule
 * returns — even on the rule-table-overflow error path, all incoming
 * AST is freed. */
void       add_rule(action_kind a, int line, int col,
                    rule_side *src, rule_side *dst,
                    port_node *ports, unsigned protos);

void       print_rules(void);

/* Phase-3 intermediate: for each rule, compute which EIDs match its src
 * and dst selectors. Stored in a global side-table so the bag builder
 * and the debugger can both read it. Returns the number of rules resolved. */
int        resolve_rule_selectors(void);
void       print_rule_resolution(void);

void        print_label_table(void);
void        print_identities(void);
void        print_ipcache(void);
void        free_all(void);

/* label table: dedupes (key,val) pairs and assigns ids 1..MAX_LABEL_ID.
 * Returns the id (>=1) on success, or 0 if the table is full.
 * (line, col) is used only for the "table full" error diagnostic. */
int         label_intern(const char *key, const char *val, int line, int col);

/* Look up a label by id — returns 1 and fills key_out/val_out on hit, 0 on miss. */
int         label_lookup(int id, const char **key_out, const char **val_out);

/* Reverse lookup: returns the id (>=1) if (key, val) is interned, 0 if not. */
int         label_lookup_id(const char *key, const char *val);

/* ---------- accessors for the builder (SQLite emitter) -----------------
 * These expose the main.c-owned lists in a read-only, walk-friendly form.
 * --------------------------------------------------------------------- */

/* Interned labels — the rows of the "label" table. */
typedef struct label_entry {
    char               *key;
    char               *val;
    int                 id;
    struct label_entry *next;
} label_entry;
label_entry *label_list_head(void);

/* Walkers for the other global lists. */
eid_node    *eid_list_head (void);
rule_node   *rule_list_head(void);

/* $var definitions — source-level debugging info. */
typedef struct var_label_entry {
    char                    *name;
    label_node              *labels;      /* expanded label list */
    int                      line, col;
    struct var_label_entry  *next;
} var_label_entry;
var_label_entry *var_label_head(void);

/* @var definitions. */
typedef struct var_port_entry {
    char                    *name;
    port_node               *ports;
    int                      line, col;
    struct var_port_entry   *next;
} var_port_entry;
var_port_entry  *var_port_head(void);

/* Global error counter — any module can bump this; main checks it at end. */
extern int  g_semantic_errors;

#endif
