#
# Makefile — policy compiler (phase 1: inventory)
#
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LEX     = flex
YACC    = bison

TARGET  = polc

OBJS    = parser.tab.o lex.yy.o main.o diag.o ipcache.o resolve.o bags.o builder.o dump.o

# sqlite3 library + headers.  Prefer pkg-config if available (handles
# macOS/Homebrew, Nix, and non-default prefixes). Falls back to plain -lsqlite3.
SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   := $(shell pkg-config --libs   sqlite3 2>/dev/null)
ifeq ($(strip $(SQLITE_LIBS)),)
SQLITE_LIBS := -lsqlite3
endif
CFLAGS += $(SQLITE_CFLAGS)
LDLIBS  = $(SQLITE_LIBS)

# The generated headers we produce are excluded from HEADERS so bison and
# flex rules don't end up with self-dependencies (parser.tab.h -> parser.tab.h).
HEADERS = $(filter-out parser.tab.h, $(wildcard *.h))

.PHONY: all clean test test-clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# bison produces parser.tab.c AND parser.tab.h in one step
parser.tab.c parser.tab.h: parser.y $(HEADERS)
	$(YACC) -d -o parser.tab.c parser.y

# flex needs parser.tab.h (for token ids)
lex.yy.c: scanner.l parser.tab.h $(HEADERS)
	$(LEX) -o lex.yy.c scanner.l

# One pattern rule for every .c -> .o.  All object files depend on every
# header — coarse but safe, and rebuild cost is negligible at this scale.
# (If header churn becomes a concern, switch to gcc -MMD -MP.)
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------
#  Unit tests — fork-per-test runner under test/. No external deps
#  (the runner is hand-rolled in test/test.{c,h}).
#
#  Test binary links a stripped-down build of the AST module: main.c
#  is recompiled with -DPOLC_TESTING so the CLI block (build_ipcache,
#  print_ipcache, slurp..main) is excluded — that keeps ipcache,
#  resolve, bags, builder, dump, sqlite, and the parser/scanner objects
#  out of the test link.
# ---------------------------------------------------------------------
TEST_DIR  = test
TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_OBJS = $(TEST_SRCS:.c=.o)
TEST_HDR  = $(TEST_DIR)/test.h
TEST_MAIN_O = $(TEST_DIR)/main.test.o

# main.c rebuilt for the test binary (CLI block disabled).
$(TEST_MAIN_O): main.c $(HEADERS)
	$(CC) $(CFLAGS) -DPOLC_TESTING -c $< -o $@

# Per-suite objects. Depend on every project header plus test.h so a
# header edit triggers a rebuild — coarse but cheap at this scale.
$(TEST_DIR)/%.o: $(TEST_DIR)/%.c $(HEADERS) $(TEST_HDR)
	$(CC) $(CFLAGS) -I. -c $< -o $@

# test_script.c drives the parser end-to-end via yyparse, so the test
# binary picks up parser.tab.o + lex.yy.o here. Still no sqlite — the
# CLI block in main.c is excluded by -DPOLC_TESTING.
$(TEST_DIR)/run-tests: $(TEST_OBJS) $(TEST_MAIN_O) diag.o parser.tab.o lex.yy.o
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_DIR)/run-tests
	./$(TEST_DIR)/run-tests

test-clean:
	rm -f $(TEST_OBJS) $(TEST_MAIN_O) $(TEST_DIR)/run-tests

# Install Vim syntax highlighting for *.gc files.
# Writes to ~/.vim/ (classic) and ~/.config/nvim/ (Neovim, if that's what you use).
vim-install:
	@mkdir -p $$HOME/.vim/syntax $$HOME/.vim/ftdetect $$HOME/.vim/ftplugin
	cp vim/syntax/gc.vim    $$HOME/.vim/syntax/
	cp vim/ftdetect/gc.vim  $$HOME/.vim/ftdetect/
	cp vim/ftplugin/gc.vim  $$HOME/.vim/ftplugin/
	@echo "installed gc.vim into $$HOME/.vim/"
	@echo "open a .gc file and run  :set filetype?  to verify"


# Install VSCode syntax highlighting for *.gc files.
vscode-install:
	@mkdir -p $$HOME/.vscode/extensions/gc-syntax
	@mkdir -p $$HOME/.vscode/extensions/gc-syntax/syntaxes
	cp vscode/gc-syntax/package.json                 $$HOME/.vscode/extensions/gc-syntax/
	cp vscode/gc-syntax/language-configuration.json  $$HOME/.vscode/extensions/gc-syntax/
	cp vscode/gc-syntax/syntaxes/gc.tmLanguage.json  $$HOME/.vscode/extensions/gc-syntax/syntaxes/
	@echo "installed gc-syntax into $$HOME/.vscode/extensions/"
	@echo "restart VSCode, open a .gc file, and check the language mode in the status bar"

clean: test-clean
	rm -f $(TARGET) $(OBJS) parser.tab.c parser.tab.h lex.yy.c
