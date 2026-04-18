/*
 * builder.c — emit the compile results to a SQLite database.
 *
 * Called as the final phase of the compiler. Writes every table from the
 * schema (see README) in a single transaction, binding values via
 * prepared statements for speed.
 *
 * SQLite encoding notes:
 *   - u64 hash values (EIDs, IPs) are stored as 64-bit signed INTEGER.
 *     The bit pattern is preserved; display in the CLI may show negative.
 *   - The two reserved bagvec ids (BAG_ID_ZERO=0, BAG_ID_ALL=1) are
 *     stored with zero rows in bagvec_bit. Readers interpret:
 *       id 0 = no rules match; id 1 = all rules match (wildcard).
 */
#include "builder.h"
#include "ast.h"
#include "resolve.h"
#include "bags.h"
#include "ipcache.h"
#include "diag.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Shorthand: bail out of build_sqlite on any sqlite error. */
#define DB_CHECK(rc, what) do {                                        \
    if ((rc) != SQLITE_OK && (rc) != SQLITE_DONE && (rc) != SQLITE_ROW) { \
        fprintf(stderr, "builder: %s failed: %s\n",                    \
                what, sqlite3_errmsg(db));                             \
        goto error;                                                    \
    }                                                                  \
} while (0)

/* ------------------------------------------------------------------ */
/*  schema DDL — split into runtime + debug                           */
/* ------------------------------------------------------------------ */

/* Tables present in every compiled output. A simulator or datapath
 * loader only needs these; they answer "does this packet pass?" */
static const char *const SCHEMA_RUNTIME_SQL =
    /* metadata */
    "CREATE TABLE meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");"

    /* EIDs — hash + ordinal for logging; bitset_hex is trivial at runtime. */
    "CREATE TABLE eid ("
    "  hash       INTEGER PRIMARY KEY,"
    "  ordinal    INTEGER UNIQUE NOT NULL,"
    "  bitset_hex TEXT NOT NULL"
    ");"

    /* ipcache — the datapath's first lookup. */
    "CREATE TABLE ipcache ("
    "  ip       INTEGER PRIMARY KEY,"
    "  eid_hash INTEGER NOT NULL"
    ");"
    "CREATE INDEX idx_ipcache_eid ON ipcache(eid_hash);"

    /* rule — action + proto mask per id; source/line columns are NULL
     * unless --debug populates them. */
    "CREATE TABLE rule ("
    "  id         INTEGER PRIMARY KEY,"
    "  action     TEXT    NOT NULL CHECK (action IN "
    "                 ('ALLOW','BLOCK','OVERRIDE-ALLOW','OVERRIDE-BLOCK')),"
    "  proto_mask INTEGER NOT NULL,"
    "  src_line   INTEGER,"
    "  src_col    INTEGER,"
    "  source     TEXT,"
    "  resolved   INTEGER NOT NULL CHECK (resolved IN (0,1))"
    ");"

    /* bagvec + four bag maps — the runtime enforcement data. */
    "CREATE TABLE bagvec ("
    "  id       INTEGER PRIMARY KEY,"
    "  reserved INTEGER NOT NULL CHECK (reserved IN (0,1))"
    ");"
    "CREATE TABLE bagvec_bit ("
    "  bag_id  INTEGER NOT NULL,"
    "  rule_id INTEGER NOT NULL,"
    "  PRIMARY KEY (bag_id, rule_id)"
    ");"
    "CREATE INDEX idx_bagvec_bit_rule ON bagvec_bit(rule_id);"

    "CREATE TABLE bag_src ("
    "  eid_hash INTEGER PRIMARY KEY,"
    "  bag_id   INTEGER NOT NULL"
    ");"
    "CREATE TABLE bag_dst ("
    "  eid_hash INTEGER PRIMARY KEY,"
    "  bag_id   INTEGER NOT NULL"
    ");"
    "CREATE TABLE bag_port ("
    "  port   INTEGER PRIMARY KEY,"
    "  bag_id INTEGER NOT NULL"
    ");"
    "CREATE TABLE bag_proto ("
    "  proto  TEXT PRIMARY KEY CHECK (proto IN ('TCP','UDP')),"
    "  bag_id INTEGER NOT NULL"
    ");";

/* Tables emitted only when --debug is passed. Contain human-facing
 * and symbolic information: label names, entity names, variable
 * definitions, DNF term breakdown, source positions. */
static const char *const SCHEMA_DEBUG_SQL =
    /* labels — id → (key, value) name */
    "CREATE TABLE label ("
    "  id    INTEGER PRIMARY KEY,"
    "  key   TEXT    NOT NULL,"
    "  value TEXT    NOT NULL,"
    "  UNIQUE (key, value)"
    ");"
    "CREATE INDEX idx_label_key ON label(key);"

    /* entities — human names. */
    "CREATE TABLE entity ("
    "  name     TEXT PRIMARY KEY,"
    "  eid_hash INTEGER NOT NULL,"
    "  src_line INTEGER,"
    "  src_col  INTEGER"
    ");"
    "CREATE INDEX idx_entity_eid ON entity(eid_hash);"

    "CREATE TABLE entity_ip ("
    "  entity_name TEXT    NOT NULL,"
    "  ip          INTEGER NOT NULL,"
    "  UNIQUE (ip)"
    ");"
    "CREATE INDEX idx_entity_ip_entity ON entity_ip(entity_name);"

    /* eid_label — symbolic membership. */
    "CREATE TABLE eid_label ("
    "  eid_hash INTEGER NOT NULL,"
    "  label_id INTEGER NOT NULL,"
    "  PRIMARY KEY (eid_hash, label_id)"
    ");"
    "CREATE INDEX idx_eid_label_label ON eid_label(label_id);"

    /* variables. */
    "CREATE TABLE var_label ("
    "  name     TEXT PRIMARY KEY,"
    "  src_line INTEGER,"
    "  src_col  INTEGER"
    ");"
    "CREATE TABLE var_label_member ("
    "  var_name TEXT    NOT NULL,"
    "  label_id INTEGER NOT NULL,"
    "  ordinal  INTEGER NOT NULL,"
    "  PRIMARY KEY (var_name, ordinal)"
    ");"
    "CREATE TABLE var_port ("
    "  name     TEXT PRIMARY KEY,"
    "  src_line INTEGER,"
    "  src_col  INTEGER"
    ");"
    "CREATE TABLE var_port_member ("
    "  var_name TEXT    NOT NULL,"
    "  port     INTEGER NOT NULL,"
    "  ordinal  INTEGER NOT NULL,"
    "  PRIMARY KEY (var_name, ordinal)"
    ");"

    /* rule_port — denormalized rule → port list (bag_port has it inverted). */
    "CREATE TABLE rule_port ("
    "  rule_id INTEGER NOT NULL,"
    "  port    INTEGER NOT NULL,"
    "  PRIMARY KEY (rule_id, port)"
    ");"

    /* DNF — symbolic selector form. */
    "CREATE TABLE rule_dnf_term ("
    "  id        INTEGER PRIMARY KEY,"
    "  rule_id   INTEGER NOT NULL,"
    "  side      TEXT    NOT NULL CHECK (side IN ('src','dst')),"
    "  term_ord  INTEGER NOT NULL,"
    "  undefined INTEGER NOT NULL CHECK (undefined IN (0,1)),"
    "  UNIQUE (rule_id, side, term_ord)"
    ");"
    "CREATE INDEX idx_dnf_rule_side ON rule_dnf_term(rule_id, side);"

    "CREATE TABLE rule_dnf_term_label ("
    "  term_id  INTEGER NOT NULL,"
    "  label_id INTEGER NOT NULL,"
    "  PRIMARY KEY (term_id, label_id)"
    ");"
    "CREATE INDEX idx_dnf_term_label_label ON rule_dnf_term_label(label_id);"

    /* Cached resolved EIDs per rule — the pre-computed answer to
     * "which EIDs did this rule's DNF match?" */
    "CREATE TABLE rule_src_eid ("
    "  rule_id  INTEGER NOT NULL,"
    "  eid_hash INTEGER NOT NULL,"
    "  PRIMARY KEY (rule_id, eid_hash)"
    ");"
    "CREATE INDEX idx_rule_src_eid_eid ON rule_src_eid(eid_hash);"

    "CREATE TABLE rule_dst_eid ("
    "  rule_id  INTEGER NOT NULL,"
    "  eid_hash INTEGER NOT NULL,"
    "  PRIMARY KEY (rule_id, eid_hash)"
    ");"
    "CREATE INDEX idx_rule_dst_eid_eid ON rule_dst_eid(eid_hash);";

/* ------------------------------------------------------------------ */
/*  helpers                                                           */
/* ------------------------------------------------------------------ */

/* Format a bitset as a compact hex string, big-endian word order.
 * Returns a malloc'd string; caller frees. */
static char *fmt_bitset_hex(const label_set *bs) {
    /* Strip leading zero words for compactness. */
    int top = LABEL_WORDS - 1;
    while (top >= 0 && bs->w[top] == 0) top--;
    if (top < 0) return strdup("0");

    /* 16 hex digits per word + 1 terminator; allocate worst case. */
    size_t cap = (size_t)(top + 1) * 16 + 1;
    char *s = malloc(cap);
    char *p = s;
    /* First (top) word: no leading zeros */
    p += snprintf(p, cap - (p - s), "%llx",
                  (unsigned long long)bs->w[top]);
    for (int i = top - 1; i >= 0; i--)
        p += snprintf(p, cap - (p - s), "%016llx",
                      (unsigned long long)bs->w[i]);
    return s;
}

/* Translate action_kind → SQL string. */
static const char *action_str(action_kind a) {
    switch (a) {
        case ACT_ALLOW:          return "ALLOW";
        case ACT_BLOCK:          return "BLOCK";
        case ACT_OVERRIDE_ALLOW: return "OVERRIDE-ALLOW";
        case ACT_OVERRIDE_BLOCK: return "OVERRIDE-BLOCK";
    }
    return "ALLOW";
}

/* ------------------------------------------------------------------ */
/*  per-section emitters. Each returns 0 on success, nonzero on error. */
/* ------------------------------------------------------------------ */

static int emit_meta(sqlite3 *db, const char *source_file, int debug) {
    sqlite3_stmt *ins = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO meta(key,value) VALUES(?,?);", -1, &ins, NULL),
        "prepare meta");

    /* Count compile-time stats. */
    int n_labels = 0;
    for (label_entry *e = label_list_head(); e; e = e->next) n_labels++;
    int n_entities = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next)
        for (member_node *m = e->members; m; m = m->next) n_entities++;
    int n_eids = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next) n_eids++;
    int n_resolved = 0, n_unresolved = 0;
    for (resolved_rule *r = resolutions_head(); r; r = r->next) n_resolved++;
    for (resolved_rule *r = unresolved_head();  r; r = r->next) n_unresolved++;

    char buf[64];
    time_t now = time(NULL);

    #define META_PUT(k, v) do {                                        \
        sqlite3_reset(ins);                                            \
        sqlite3_bind_text(ins, 1, (k), -1, SQLITE_STATIC);             \
        sqlite3_bind_text(ins, 2, (v), -1, SQLITE_TRANSIENT);          \
        rc = sqlite3_step(ins); DB_CHECK(rc, "meta insert");           \
    } while (0)

    META_PUT("compiler",     "polc");
    META_PUT("source_file",  source_file ? source_file : "<stdin>");
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    META_PUT("compiled_at", buf);
    snprintf(buf, sizeof buf, "%d", MAX_LABEL_ID);
    META_PUT("max_label_id", buf);
    snprintf(buf, sizeof buf, "%d", MAX_RULES);
    META_PUT("max_rules", buf);
    snprintf(buf, sizeof buf, "%d", n_labels);     META_PUT("n_labels",   buf);
    snprintf(buf, sizeof buf, "%d", n_entities);   META_PUT("n_entities", buf);
    snprintf(buf, sizeof buf, "%d", n_eids);       META_PUT("n_eids",     buf);
    snprintf(buf, sizeof buf, "%d", n_resolved);   META_PUT("n_rules_resolved",   buf);
    snprintf(buf, sizeof buf, "%d", n_unresolved); META_PUT("n_rules_unresolved", buf);
    snprintf(buf, sizeof buf, "%zu", bagvec_count()); META_PUT("n_bagvecs", buf);
    META_PUT("eid_hash_encoding",
             "int64 (bit-identical to u64; high-bit values display as negative)");
    META_PUT("bag_reserved_ids",
             "0=ZERO (all-bits-off); 1=ALL (wildcard; stored with no rows in bagvec_bit)");
    META_PUT("debug", debug ? "true" : "false");

    #undef META_PUT
    sqlite3_finalize(ins);
    return 0;
error:
    if (ins) sqlite3_finalize(ins);
    return -1;
}

static int emit_labels(sqlite3 *db) {
    sqlite3_stmt *ins = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO label(id,key,value) VALUES(?,?,?);", -1, &ins, NULL),
        "prepare label");
    for (label_entry *e = label_list_head(); e; e = e->next) {
        sqlite3_reset(ins);
        sqlite3_bind_int (ins, 1, e->id);
        sqlite3_bind_text(ins, 2, e->key, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, e->val, -1, SQLITE_STATIC);
        rc = sqlite3_step(ins); DB_CHECK(rc, "label insert");
    }
    sqlite3_finalize(ins);
    return 0;
error:
    if (ins) sqlite3_finalize(ins);
    return -1;
}

static int emit_entities(sqlite3 *db) {
    sqlite3_stmt *ent = NULL, *ip = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO entity(name,eid_hash,src_line,src_col) VALUES(?,?,?,?);",
        -1, &ent, NULL), "prepare entity");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO entity_ip(entity_name,ip) VALUES(?,?);",
        -1, &ip, NULL), "prepare entity_ip");

    for (eid_node *e = eid_list_head(); e; e = e->next) {
        for (member_node *m = e->members; m; m = m->next) {
            sqlite3_reset(ent);
            sqlite3_bind_text (ent, 1, m->name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ent, 2, (sqlite3_int64)e->hash);
            sqlite3_bind_int  (ent, 3, m->line);
            sqlite3_bind_int  (ent, 4, m->col);
            rc = sqlite3_step(ent); DB_CHECK(rc, "entity insert");

            for (ip_node *p = m->ips; p; p = p->next) {
                if (!p->valid) continue;
                sqlite3_reset(ip);
                sqlite3_bind_text (ip, 1, m->name, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ip, 2, (sqlite3_int64)(uint64_t)p->addr);
                rc = sqlite3_step(ip); DB_CHECK(rc, "entity_ip insert");
            }
        }
    }
    sqlite3_finalize(ent);
    sqlite3_finalize(ip);
    return 0;
error:
    if (ent) sqlite3_finalize(ent);
    if (ip)  sqlite3_finalize(ip);
    return -1;
}

static int emit_eids(sqlite3 *db, int debug) {
    sqlite3_stmt *eid = NULL, *elabel = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO eid(hash,ordinal,bitset_hex) VALUES(?,?,?);",
        -1, &eid, NULL), "prepare eid");
    if (debug) {
        DB_CHECK(sqlite3_prepare_v2(db,
            "INSERT INTO eid_label(eid_hash,label_id) VALUES(?,?);",
            -1, &elabel, NULL), "prepare eid_label");
    }

    int ord = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next, ord++) {
        char *hex = fmt_bitset_hex(&e->labels);
        sqlite3_reset(eid);
        sqlite3_bind_int64(eid, 1, (sqlite3_int64)e->hash);
        sqlite3_bind_int  (eid, 2, ord);
        sqlite3_bind_text (eid, 3, hex, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(eid); free(hex); DB_CHECK(rc, "eid insert");

        if (!debug) continue;
        for (int id = 1; id <= MAX_LABEL_ID; id++) {
            if (!lset_test(&e->labels, id)) continue;
            sqlite3_reset(elabel);
            sqlite3_bind_int64(elabel, 1, (sqlite3_int64)e->hash);
            sqlite3_bind_int  (elabel, 2, id);
            rc = sqlite3_step(elabel); DB_CHECK(rc, "eid_label insert");
        }
    }
    sqlite3_finalize(eid);
    if (elabel) sqlite3_finalize(elabel);
    return 0;
error:
    if (eid)    sqlite3_finalize(eid);
    if (elabel) sqlite3_finalize(elabel);
    return -1;
}

struct ipcache_ctx { sqlite3_stmt *ins; int rc; };
static void ipcache_cb(uint32_t addr, uint64_t eid, void *ud) {
    struct ipcache_ctx *c = ud;
    if (c->rc) return;              /* already errored */
    sqlite3_reset(c->ins);
    sqlite3_bind_int64(c->ins, 1, (sqlite3_int64)(uint64_t)addr);
    sqlite3_bind_int64(c->ins, 2, (sqlite3_int64)eid);
    int rc = sqlite3_step(c->ins);
    if (rc != SQLITE_DONE && rc != SQLITE_OK) c->rc = rc;
}

static int emit_ipcache(sqlite3 *db) {
    sqlite3_stmt *ins = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO ipcache(ip,eid_hash) VALUES(?,?);", -1, &ins, NULL),
        "prepare ipcache");
    struct ipcache_ctx c = { ins, 0 };
    ipcache_foreach(ipcache_cb, &c);
    if (c.rc) { rc = c.rc; DB_CHECK(rc, "ipcache insert"); }
    sqlite3_finalize(ins);
    return 0;
error:
    if (ins) sqlite3_finalize(ins);
    return -1;
}

static int emit_vars(sqlite3 *db) {
    sqlite3_stmt *vl = NULL, *vlm = NULL, *vp = NULL, *vpm = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO var_label(name,src_line,src_col) VALUES(?,?,?);",
        -1, &vl, NULL), "prepare var_label");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO var_label_member(var_name,label_id,ordinal) VALUES(?,?,?);",
        -1, &vlm, NULL), "prepare var_label_member");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO var_port(name,src_line,src_col) VALUES(?,?,?);",
        -1, &vp, NULL), "prepare var_port");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO var_port_member(var_name,port,ordinal) VALUES(?,?,?);",
        -1, &vpm, NULL), "prepare var_port_member");

    for (var_label_entry *v = var_label_head(); v; v = v->next) {
        sqlite3_reset(vl);
        sqlite3_bind_text(vl, 1, v->name, -1, SQLITE_STATIC);
        sqlite3_bind_int (vl, 2, v->line);
        sqlite3_bind_int (vl, 3, v->col);
        rc = sqlite3_step(vl); DB_CHECK(rc, "var_label insert");

        int ord = 0;
        for (label_node *l = v->labels; l; l = l->next, ord++) {
            int id = label_lookup_id(l->key, l->val);
            if (id <= 0) continue;   /* defensive */
            sqlite3_reset(vlm);
            sqlite3_bind_text(vlm, 1, v->name, -1, SQLITE_STATIC);
            sqlite3_bind_int (vlm, 2, id);
            sqlite3_bind_int (vlm, 3, ord);
            rc = sqlite3_step(vlm); DB_CHECK(rc, "var_label_member insert");
        }
    }
    for (var_port_entry *v = var_port_head(); v; v = v->next) {
        sqlite3_reset(vp);
        sqlite3_bind_text(vp, 1, v->name, -1, SQLITE_STATIC);
        sqlite3_bind_int (vp, 2, v->line);
        sqlite3_bind_int (vp, 3, v->col);
        rc = sqlite3_step(vp); DB_CHECK(rc, "var_port insert");

        int ord = 0;
        for (port_node *p = v->ports; p; p = p->next, ord++) {
            sqlite3_reset(vpm);
            sqlite3_bind_text(vpm, 1, v->name, -1, SQLITE_STATIC);
            sqlite3_bind_int (vpm, 2, p->port);
            sqlite3_bind_int (vpm, 3, ord);
            rc = sqlite3_step(vpm); DB_CHECK(rc, "var_port_member insert");
        }
    }

    sqlite3_finalize(vl);
    sqlite3_finalize(vlm);
    sqlite3_finalize(vp);
    sqlite3_finalize(vpm);
    return 0;
error:
    if (vl)  sqlite3_finalize(vl);
    if (vlm) sqlite3_finalize(vlm);
    if (vp)  sqlite3_finalize(vp);
    if (vpm) sqlite3_finalize(vpm);
    return -1;
}

/* Render the source text of a rule from line/col info. Since we didn't
 * retain the original substring, we synthesize a canonical form — the
 * debugger can always re-read the original file via line/col. */
static char *synth_rule_source(const rule_node *r) {
    /* For now, a simple placeholder. A future enhancement: slurp the
     * source file and extract the line range. Keeping it null-able. */
    (void)r;
    return NULL;
}

static int emit_rules(sqlite3 *db, int debug) {
    sqlite3_stmt *rule = NULL, *rport = NULL;
    char *resolved = NULL;
    int rc;
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO rule(id,action,proto_mask,src_line,src_col,source,resolved) "
        "VALUES(?,?,?,?,?,?,?);", -1, &rule, NULL), "prepare rule");
    if (debug) {
        DB_CHECK(sqlite3_prepare_v2(db,
            "INSERT INTO rule_port(rule_id,port) VALUES(?,?);",
            -1, &rport, NULL), "prepare rule_port");
    }

    /* Build a fast "is this rule resolved?" lookup. */
    int max_id = 0;
    for (rule_node *r = rule_list_head(); r; r = r->next)
        if (r->id > max_id) max_id = r->id;
    resolved = calloc((size_t)max_id + 1, 1);
    for (resolved_rule *rr = resolutions_head(); rr; rr = rr->next)
        if (rr->rule_id <= max_id) resolved[rr->rule_id] = 1;

    for (rule_node *r = rule_list_head(); r; r = r->next) {
        sqlite3_reset(rule);
        sqlite3_bind_int (rule, 1, r->id);
        sqlite3_bind_text(rule, 2, action_str(r->action), -1, SQLITE_STATIC);
        sqlite3_bind_int (rule, 3, r->protos);
        /* Source line/col/text are debug-only niceties. */
        if (debug) {
            sqlite3_bind_int (rule, 4, r->line);
            sqlite3_bind_int (rule, 5, r->col);
            char *src = synth_rule_source(r);
            if (src) sqlite3_bind_text(rule, 6, src, -1, SQLITE_TRANSIENT);
            else     sqlite3_bind_null(rule, 6);
            free(src);
        } else {
            sqlite3_bind_null(rule, 4);
            sqlite3_bind_null(rule, 5);
            sqlite3_bind_null(rule, 6);
        }
        sqlite3_bind_int (rule, 7, resolved[r->id] ? 1 : 0);
        rc = sqlite3_step(rule); DB_CHECK(rc, "rule insert");

        if (!debug) continue;
        for (port_node *p = r->ports; p; p = p->next) {
            sqlite3_reset(rport);
            sqlite3_bind_int(rport, 1, r->id);
            sqlite3_bind_int(rport, 2, p->port);
            rc = sqlite3_step(rport); DB_CHECK(rc, "rule_port insert");
        }
    }
    free(resolved);
    sqlite3_finalize(rule);
    if (rport) sqlite3_finalize(rport);
    return 0;
error:
    free(resolved);
    if (rule)  sqlite3_finalize(rule);
    if (rport) sqlite3_finalize(rport);
    return -1;
}

static int emit_dnf_for(sqlite3 *db, resolved_rule *list,
                        sqlite3_stmt *term_ins, sqlite3_stmt *lbl_ins,
                        int *next_term_id) {
    int rc;
    for (resolved_rule *rr = list; rr; rr = rr->next) {
        /* src side */
        int ord = 0;
        for (dnf_term *t = rr->src_dnf ? rr->src_dnf->terms : NULL;
             t; t = t->next, ord++) {
            int tid = (*next_term_id)++;
            sqlite3_reset(term_ins);
            sqlite3_bind_int (term_ins, 1, tid);
            sqlite3_bind_int (term_ins, 2, rr->rule_id);
            sqlite3_bind_text(term_ins, 3, "src", -1, SQLITE_STATIC);
            sqlite3_bind_int (term_ins, 4, ord);
            sqlite3_bind_int (term_ins, 5, t->undefined ? 1 : 0);
            rc = sqlite3_step(term_ins); DB_CHECK(rc, "dnf term insert");
            for (int id = 1; id <= MAX_LABEL_ID; id++) {
                if (!lset_test(&t->mask, id)) continue;
                sqlite3_reset(lbl_ins);
                sqlite3_bind_int(lbl_ins, 1, tid);
                sqlite3_bind_int(lbl_ins, 2, id);
                rc = sqlite3_step(lbl_ins); DB_CHECK(rc, "dnf label insert");
            }
        }
        /* dst side */
        ord = 0;
        for (dnf_term *t = rr->dst_dnf ? rr->dst_dnf->terms : NULL;
             t; t = t->next, ord++) {
            int tid = (*next_term_id)++;
            sqlite3_reset(term_ins);
            sqlite3_bind_int (term_ins, 1, tid);
            sqlite3_bind_int (term_ins, 2, rr->rule_id);
            sqlite3_bind_text(term_ins, 3, "dst", -1, SQLITE_STATIC);
            sqlite3_bind_int (term_ins, 4, ord);
            sqlite3_bind_int (term_ins, 5, t->undefined ? 1 : 0);
            rc = sqlite3_step(term_ins); DB_CHECK(rc, "dnf term insert");
            for (int id = 1; id <= MAX_LABEL_ID; id++) {
                if (!lset_test(&t->mask, id)) continue;
                sqlite3_reset(lbl_ins);
                sqlite3_bind_int(lbl_ins, 1, tid);
                sqlite3_bind_int(lbl_ins, 2, id);
                rc = sqlite3_step(lbl_ins); DB_CHECK(rc, "dnf label insert");
            }
        }
    }
    return 0;
error:
    return -1;
}

static int emit_dnf_and_cached_eids(sqlite3 *db) {
    sqlite3_stmt *term_ins = NULL, *lbl_ins = NULL;
    sqlite3_stmt *src_eid  = NULL, *dst_eid  = NULL;
    int rc;

    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO rule_dnf_term(id,rule_id,side,term_ord,undefined) "
        "VALUES(?,?,?,?,?);", -1, &term_ins, NULL), "prepare dnf_term");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO rule_dnf_term_label(term_id,label_id) VALUES(?,?);",
        -1, &lbl_ins, NULL), "prepare dnf_term_label");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO rule_src_eid(rule_id,eid_hash) VALUES(?,?);",
        -1, &src_eid, NULL), "prepare rule_src_eid");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO rule_dst_eid(rule_id,eid_hash) VALUES(?,?);",
        -1, &dst_eid, NULL), "prepare rule_dst_eid");

    int next_term_id = 1;
    /* DNF for BOTH resolved and unresolved rules — debugger wants both. */
    if (emit_dnf_for(db, resolutions_head(), term_ins, lbl_ins, &next_term_id)) goto error;
    if (emit_dnf_for(db, unresolved_head(),  term_ins, lbl_ins, &next_term_id)) goto error;

    /* Cached resolved EIDs — only for resolved rules. */
    for (resolved_rule *rr = resolutions_head(); rr; rr = rr->next) {
        for (size_t i = 0; i < rr->src_eids.n; i++) {
            sqlite3_reset(src_eid);
            sqlite3_bind_int  (src_eid, 1, rr->rule_id);
            sqlite3_bind_int64(src_eid, 2, (sqlite3_int64)rr->src_eids.items[i]->hash);
            rc = sqlite3_step(src_eid); DB_CHECK(rc, "rule_src_eid insert");
        }
        for (size_t i = 0; i < rr->dst_eids.n; i++) {
            sqlite3_reset(dst_eid);
            sqlite3_bind_int  (dst_eid, 1, rr->rule_id);
            sqlite3_bind_int64(dst_eid, 2, (sqlite3_int64)rr->dst_eids.items[i]->hash);
            rc = sqlite3_step(dst_eid); DB_CHECK(rc, "rule_dst_eid insert");
        }
    }
    sqlite3_finalize(term_ins);
    sqlite3_finalize(lbl_ins);
    sqlite3_finalize(src_eid);
    sqlite3_finalize(dst_eid);
    return 0;
error:
    if (term_ins) sqlite3_finalize(term_ins);
    if (lbl_ins)  sqlite3_finalize(lbl_ins);
    if (src_eid)  sqlite3_finalize(src_eid);
    if (dst_eid)  sqlite3_finalize(dst_eid);
    return -1;
}

/* ---- bags ---- */

struct bagmap_ctx { sqlite3_stmt *ins; int rc; int is_proto; };

static void bagmap_u64_cb(uint64_t key, bag_id_t id, void *ud) {
    struct bagmap_ctx *c = ud;
    if (c->rc) return;
    sqlite3_reset(c->ins);
    sqlite3_bind_int64(c->ins, 1, (sqlite3_int64)key);
    sqlite3_bind_int  (c->ins, 2, (int)id);
    int rc = sqlite3_step(c->ins);
    if (rc != SQLITE_DONE && rc != SQLITE_OK) c->rc = rc;
}
static void bagmap_port_cb(int port, bag_id_t id, void *ud) {
    struct bagmap_ctx *c = ud;
    if (c->rc) return;
    sqlite3_reset(c->ins);
    sqlite3_bind_int(c->ins, 1, port);
    sqlite3_bind_int(c->ins, 2, (int)id);
    int rc = sqlite3_step(c->ins);
    if (rc != SQLITE_DONE && rc != SQLITE_OK) c->rc = rc;
}
static void bagmap_proto_cb(int proto, bag_id_t id, void *ud) {
    struct bagmap_ctx *c = ud;
    if (c->rc) return;
    const char *name = (proto == PROTO_TCP) ? "TCP"
                     : (proto == PROTO_UDP) ? "UDP" : NULL;
    if (!name) return;
    sqlite3_reset(c->ins);
    sqlite3_bind_text(c->ins, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int (c->ins, 2, (int)id);
    int rc = sqlite3_step(c->ins);
    if (rc != SQLITE_DONE && rc != SQLITE_OK) c->rc = rc;
}

static int emit_bags(sqlite3 *db) {
    sqlite3_stmt *bv = NULL, *bvb = NULL;
    sqlite3_stmt *bs = NULL, *bd = NULL, *bp = NULL, *bt = NULL;
    int rc;

    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO bagvec(id,reserved) VALUES(?,?);", -1, &bv, NULL),
        "prepare bagvec");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO bagvec_bit(bag_id,rule_id) VALUES(?,?);", -1, &bvb, NULL),
        "prepare bagvec_bit");

    size_t n = bagvec_count();
    for (size_t i = 0; i < n; i++) {
        int reserved = (i == BAG_ID_ZERO || i == BAG_ID_ALL);
        sqlite3_reset(bv);
        sqlite3_bind_int(bv, 1, (int)i);
        sqlite3_bind_int(bv, 2, reserved);
        rc = sqlite3_step(bv); DB_CHECK(rc, "bagvec insert");

        /* Reserved ids get no bits. BAG_ID_ZERO is trivially empty;
         * BAG_ID_ALL is the wildcard — semantics encoded by meta. */
        if (reserved) continue;

        const rule_bitvec *v = bagvec_get((bag_id_t)i);
        if (!v) continue;
        for (int bit = 0; bit < MAX_RULES; bit++) {
            if (!(v->w[bit >> 6] & ((uint64_t)1 << (bit & 63)))) continue;
            sqlite3_reset(bvb);
            sqlite3_bind_int(bvb, 1, (int)i);
            sqlite3_bind_int(bvb, 2, bit);
            rc = sqlite3_step(bvb); DB_CHECK(rc, "bagvec_bit insert");
        }
    }

    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO bag_src(eid_hash,bag_id) VALUES(?,?);", -1, &bs, NULL),
        "prepare bag_src");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO bag_dst(eid_hash,bag_id) VALUES(?,?);", -1, &bd, NULL),
        "prepare bag_dst");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO bag_port(port,bag_id) VALUES(?,?);", -1, &bp, NULL),
        "prepare bag_port");
    DB_CHECK(sqlite3_prepare_v2(db,
        "INSERT INTO bag_proto(proto,bag_id) VALUES(?,?);", -1, &bt, NULL),
        "prepare bag_proto");

    struct bagmap_ctx cs = { bs, 0, 0 }; bag_src_foreach  (bagmap_u64_cb,  &cs);
    struct bagmap_ctx cd = { bd, 0, 0 }; bag_dst_foreach  (bagmap_u64_cb,  &cd);
    struct bagmap_ctx cp = { bp, 0, 0 }; bag_port_foreach (bagmap_port_cb, &cp);
    struct bagmap_ctx ct = { bt, 0, 1 }; bag_proto_foreach(bagmap_proto_cb,&ct);
    if (cs.rc) { rc = cs.rc; DB_CHECK(rc, "bag_src insert");   }
    if (cd.rc) { rc = cd.rc; DB_CHECK(rc, "bag_dst insert");   }
    if (cp.rc) { rc = cp.rc; DB_CHECK(rc, "bag_port insert");  }
    if (ct.rc) { rc = ct.rc; DB_CHECK(rc, "bag_proto insert"); }

    sqlite3_finalize(bv); sqlite3_finalize(bvb);
    sqlite3_finalize(bs); sqlite3_finalize(bd);
    sqlite3_finalize(bp); sqlite3_finalize(bt);
    return 0;
error:
    if (bv)  sqlite3_finalize(bv);
    if (bvb) sqlite3_finalize(bvb);
    if (bs)  sqlite3_finalize(bs);
    if (bd)  sqlite3_finalize(bd);
    if (bp)  sqlite3_finalize(bp);
    if (bt)  sqlite3_finalize(bt);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  public entry point                                                */
/* ------------------------------------------------------------------ */

int build_sqlite(const char *path, const char *source_file, int debug) {
    sqlite3 *db = NULL;
    int rc;

    /* Remove any existing DB — we rebuild from scratch every run. */
    (void)remove(path);

    rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "builder: cannot open %s: %s\n",
                path, sqlite3_errmsg(db));
        goto error;
    }

    /* Faster writes: WAL off (default rollback journal is fine), and
     * defer fsync for this run. Acceptable — we rebuild on every compile. */
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL);

    char *err = NULL;
    rc = sqlite3_exec(db, SCHEMA_RUNTIME_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "builder: runtime schema create failed: %s\n", err);
        sqlite3_free(err);
        goto error;
    }
    if (debug) {
        rc = sqlite3_exec(db, SCHEMA_DEBUG_SQL, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "builder: debug schema create failed: %s\n", err);
            sqlite3_free(err);
            goto error;
        }
    }

    rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    DB_CHECK(rc, "BEGIN");

    /* Runtime tables — always emitted. */
    if (emit_meta(db, source_file, debug))  goto error;
    if (emit_eids(db, debug))               goto error;   /* eid always; eid_label only if debug */
    if (emit_ipcache(db))                   goto error;
    if (emit_rules(db, debug))              goto error;   /* rule always; rule_port + positions only if debug */
    if (emit_bags(db))                      goto error;

    /* Debug-only tables. */
    if (debug) {
        if (emit_labels(db))                    goto error;
        if (emit_entities(db))                  goto error;
        if (emit_vars(db))                      goto error;
        if (emit_dnf_and_cached_eids(db))       goto error;
    }

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    DB_CHECK(rc, "COMMIT");

    sqlite3_close(db);
    return 0;

error:
    if (db) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
    }
    return -1;
}
