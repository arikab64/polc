/*
 * dump.h — write a human-readable dump of a compiled policy database.
 *
 * Reads the SQLite file at `db_path` and writes every present table as
 * a labeled section to a text file at `txt_path`. Works on both runtime
 * and --debug databases; tables not present in the DB are skipped.
 */
#ifndef DUMP_H
#define DUMP_H

int dump_db(const char *db_path, const char *txt_path);

#endif
