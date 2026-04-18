/*
 * diag.c — error reporter. Given (line, col), locate the line in the
 * retained source text, print "file:line:col: error: <msg>", then
 * echo the source line and a caret.
 */
#include "diag.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *g_filename = "<stdin>";
static const char *g_source   = NULL;

void diag_init(const char *filename, const char *source_text) {
    if (filename) g_filename = filename;
    g_source = source_text;
}

/* Return a pointer to the start of the Nth line (1-based), or NULL if we
 * don't have the source or the line doesn't exist. */
static const char *find_line_start(int line) {
    if (!g_source || line <= 0) return NULL;
    const char *p = g_source;
    int curr = 1;
    while (curr < line && *p) {
        if (*p == '\n') curr++;
        p++;
    }
    return (curr == line) ? p : NULL;
}

/* Internal shared emitter — parameterized by the label ("error"/"warning"). */
static void emit(const char *label, int line, int col,
                 const char *fmt, va_list ap) {
    if (col > 0)       fprintf(stderr, "%s:%d:%d: %s: ", g_filename, line, col, label);
    else if (line > 0) fprintf(stderr, "%s:%d: %s: ",     g_filename, line, label);
    else               fprintf(stderr, "%s: %s: ",        g_filename, label);

    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    /* Source echo + caret (skip if we have no position or no source). */
    const char *start = find_line_start(line);
    if (!start) return;

    const char *end = start;
    while (*end && *end != '\n') end++;

    fprintf(stderr, "    %.*s\n", (int)(end - start), start);

    if (col > 0) {
        fputs("    ", stderr);
        for (int i = 1; i < col; i++) {
            char c = start[i - 1];
            fputc(c == '\t' ? '\t' : ' ', stderr);
        }
        fputs("^\n", stderr);
    }
}

void diag_error(int line, int col, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    emit("error", line, col, fmt, ap);
    va_end(ap);
}

void diag_warning(int line, int col, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    emit("warning", line, col, fmt, ap);
    va_end(ap);
}
