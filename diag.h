/*
 * diag.h — error reporting with line + column + caret (gcc-style).
 */
#ifndef DIAG_H
#define DIAG_H

/* Call once at startup with the source filename and its full text.
 * The text pointer is retained (not copied) — caller must keep it alive
 * for the duration of parsing. Pass filename=NULL for "<stdin>". */
void diag_init(const char *filename, const char *source_text);

/* Emit a gcc-style error at (line, col). Line and col are 1-based.
 * col<=0 means "column unknown" and suppresses the caret. */
void diag_error(int line, int col, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Same shape, but prints "warning:" instead of "error:" and does NOT
 * increment the semantic error counter — purely advisory. */
void diag_warning(int line, int col, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
