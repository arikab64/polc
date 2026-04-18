/*
 * builder.h — SQLite binary emission (phase 4).
 *
 * Walks all in-memory compile state (labels, entities, EIDs, ipcache,
 * variables, rules, DNF, resolved rules, bagvec store, four bag maps)
 * and writes it to a SQLite database consumable by external simulator,
 * visibility, and debugger tools.
 *
 * See builder.c for the schema and emission order.
 */
#ifndef BUILDER_H
#define BUILDER_H

/* Emit the compiled state to a SQLite database at `path`.
 * `source_file` is recorded in meta (NULL → "<stdin>").
 * `debug` = 1 includes the debug tables (labels, entity names, DNF
 *             terms, variable definitions, resolved-EID cache,
 *             rule_port, eid_label). These are human-facing and
 *             symbolic — only needed for simulators that want
 *             explanations, or for interactive debugging.
 * `debug` = 0 emits only the runtime tables a datapath loader needs.
 * Returns 0 on success, nonzero on failure (diagnostic already printed). */
int build_sqlite(const char *path, const char *source_file, int debug);

#endif
