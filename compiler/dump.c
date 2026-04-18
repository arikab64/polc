/*
 * dump.c — human-friendly dump of the compiled SQLite policy.
 *
 * Queries each table and prints rows in a terse tabular layout.
 * Gracefully handles missing tables (runtime-only DBs won't have the
 * debug ones).
 */
#include "dump.h"
#include <sqlite3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

/* Does a table exist in this database? */
static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

static void banner(FILE *fp, const char *title) {
    fprintf(fp, "========================================\n");
    fprintf(fp, " %s\n", title);
    fprintf(fp, "========================================\n");
}

/* Run a SELECT and print each column per row as "col: value", one row
 * per paragraph. Good for small tables with mixed column types. */
static int dump_pairs(FILE *fp, sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "dump: %s\n", sqlite3_errmsg(db)); return -1; }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int n = sqlite3_column_count(st);
        for (int i = 0; i < n; i++) {
            const char *col = sqlite3_column_name(st, i);
            const unsigned char *v = sqlite3_column_text(st, i);
            fprintf(fp, "  %-14s %s\n", col, v ? (const char *)v : "(null)");
        }
        fprintf(fp, "\n");
    }
    sqlite3_finalize(st);
    return 0;
}

/* Column-aligned tabular dump: headers, separator, rows, auto-sized widths. */
static int dump_cols(FILE *fp, sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "dump: %s\n", sqlite3_errmsg(db)); return -1; }

    int ncols = sqlite3_column_count(st);
    if (ncols == 0) { sqlite3_finalize(st); return 0; }

    /* Capture column names first (they're stable for the life of the stmt). */
    char **names = malloc(ncols * sizeof *names);
    int *widths = calloc(ncols, sizeof *widths);
    for (int i = 0; i < ncols; i++) {
        names[i] = strdup(sqlite3_column_name(st, i));
        widths[i] = (int)strlen(names[i]);
    }

    /* Slurp all rows into memory so we can size columns. Policy DBs are
     * small enough that this is fine. */
    int cap = 16, count = 0;
    char ***rows = malloc(cap * sizeof *rows);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == cap) { cap *= 2; rows = realloc(rows, cap * sizeof *rows); }
        char **row = malloc(ncols * sizeof *row);
        for (int i = 0; i < ncols; i++) {
            const unsigned char *v = sqlite3_column_text(st, i);
            row[i] = v ? strdup((const char *)v) : strdup("(null)");
            int w = (int)strlen(row[i]);
            if (w > widths[i]) widths[i] = w;
        }
        rows[count++] = row;
    }
    sqlite3_finalize(st);

    /* Header + separator. */
    fprintf(fp, "  ");
    for (int i = 0; i < ncols; i++)
        fprintf(fp, "%-*s%s", widths[i], names[i], i + 1 < ncols ? "  " : "\n");
    fprintf(fp, "  ");
    for (int i = 0; i < ncols; i++) {
        for (int k = 0; k < widths[i]; k++) fputc('-', fp);
        fprintf(fp, "%s", i + 1 < ncols ? "  " : "\n");
    }

    /* Data rows. */
    for (int r = 0; r < count; r++) {
        fprintf(fp, "  ");
        for (int i = 0; i < ncols; i++) {
            fprintf(fp, "%-*s%s", widths[i], rows[r][i], i + 1 < ncols ? "  " : "\n");
        }
    }
    if (count == 0) fprintf(fp, "  (no rows)\n");

    /* Cleanup. */
    for (int r = 0; r < count; r++) {
        for (int i = 0; i < ncols; i++) free(rows[r][i]);
        free(rows[r]);
    }
    for (int i = 0; i < ncols; i++) free(names[i]);
    free(rows); free(names); free(widths);
    return 0;
}

/* Convenience wrapper: banner + dump_cols if the table exists. */
static int section(FILE *fp, sqlite3 *db, const char *title, const char *table,
                   const char *sql) {
    if (!table_exists(db, table)) return 0;
    banner(fp, title);
    int r = dump_cols(fp, db, sql);
    fprintf(fp, "\n");
    return r;
}

/* ---- public entry ---- */

int dump_db(const char *db_path, const char *txt_path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "dump: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return -1;
    }

    FILE *fp = fopen(txt_path, "w");
    if (!fp) {
        fprintf(stderr, "dump: cannot create %s: %s\n", txt_path, strerror(errno));
        sqlite3_close(db);
        return -1;
    }

    /* Intro — meta is a pairs-layout instead of a table. */
    banner(fp, "META");
    dump_pairs(fp, db, "SELECT key, value FROM meta ORDER BY key;");

    /* Runtime-core tables. */
    section(fp, db, "EID",
        "eid",
        "SELECT ordinal, printf('0x%016x', hash) AS hash, bitset_hex FROM eid ORDER BY ordinal;");

    section(fp, db, "IPCACHE",
        "ipcache",
        "SELECT printf('%d.%d.%d.%d',"
        "    (ip>>24)&255,(ip>>16)&255,(ip>>8)&255,ip&255) AS ip,"
        "  printf('0x%016x', eid_hash) AS eid_hash "
        "FROM ipcache ORDER BY ip;");

    section(fp, db, "RULE",
        "rule",
        "SELECT id, action, proto_mask, resolved, src_line AS line, src_col AS col "
        "FROM rule ORDER BY id;");

    section(fp, db, "BAGVEC",
        "bagvec",
        "SELECT id, reserved FROM bagvec ORDER BY id;");

    section(fp, db, "BAGVEC_BIT  (bag contents as (bag_id, rule_id) pairs)",
        "bagvec_bit",
        "SELECT bag_id, rule_id FROM bagvec_bit ORDER BY bag_id, rule_id;");

    section(fp, db, "BAG_SRC",
        "bag_src",
        "SELECT printf('0x%016x', eid_hash) AS eid_hash, bag_id FROM bag_src "
        "ORDER BY bag_id, eid_hash;");
    section(fp, db, "BAG_DST",
        "bag_dst",
        "SELECT printf('0x%016x', eid_hash) AS eid_hash, bag_id FROM bag_dst "
        "ORDER BY bag_id, eid_hash;");
    section(fp, db, "BAG_PORT",
        "bag_port",
        "SELECT port, bag_id FROM bag_port ORDER BY port;");
    section(fp, db, "BAG_PROTO",
        "bag_proto",
        "SELECT proto, bag_id FROM bag_proto ORDER BY proto;");

    /* Debug tables — emitted only if present (i.e., --debug was used). */
    section(fp, db, "LABEL",
        "label",
        "SELECT id, key, value FROM label ORDER BY id;");
    section(fp, db, "ENTITY",
        "entity",
        "SELECT name, printf('0x%016x', eid_hash) AS eid_hash, "
        "src_line AS line, src_col AS col FROM entity ORDER BY name;");
    section(fp, db, "ENTITY_IP",
        "entity_ip",
        "SELECT entity_name, printf('%d.%d.%d.%d',"
        "    (ip>>24)&255,(ip>>16)&255,(ip>>8)&255,ip&255) AS ip "
        "FROM entity_ip ORDER BY entity_name, ip;");
    section(fp, db, "EID_LABEL",
        "eid_label",
        "SELECT printf('0x%016x', eid_hash) AS eid_hash, label_id "
        "FROM eid_label ORDER BY eid_hash, label_id;");

    section(fp, db, "VAR_LABEL",
        "var_label",
        "SELECT name, src_line AS line, src_col AS col FROM var_label ORDER BY name;");
    section(fp, db, "VAR_LABEL_MEMBER",
        "var_label_member",
        "SELECT var_name, ordinal, label_id FROM var_label_member "
        "ORDER BY var_name, ordinal;");
    section(fp, db, "VAR_PORT",
        "var_port",
        "SELECT name, src_line AS line, src_col AS col FROM var_port ORDER BY name;");
    section(fp, db, "VAR_PORT_MEMBER",
        "var_port_member",
        "SELECT var_name, ordinal, port FROM var_port_member "
        "ORDER BY var_name, ordinal;");

    section(fp, db, "RULE_PORT",
        "rule_port",
        "SELECT rule_id, port FROM rule_port ORDER BY rule_id, port;");

    section(fp, db, "RULE_DNF_TERM",
        "rule_dnf_term",
        "SELECT id, rule_id, side, term_ord, undefined FROM rule_dnf_term "
        "ORDER BY rule_id, side, term_ord;");
    section(fp, db, "RULE_DNF_TERM_LABEL",
        "rule_dnf_term_label",
        "SELECT term_id, label_id FROM rule_dnf_term_label "
        "ORDER BY term_id, label_id;");

    section(fp, db, "RULE_SRC_EID  (cached)",
        "rule_src_eid",
        "SELECT rule_id, printf('0x%016x', eid_hash) AS eid_hash "
        "FROM rule_src_eid ORDER BY rule_id, eid_hash;");
    section(fp, db, "RULE_DST_EID  (cached)",
        "rule_dst_eid",
        "SELECT rule_id, printf('0x%016x', eid_hash) AS eid_hash "
        "FROM rule_dst_eid ORDER BY rule_id, eid_hash;");

    fclose(fp);
    sqlite3_close(db);
    return 0;
}
