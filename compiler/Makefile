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

.PHONY: all clean test

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

test: $(TARGET)
	./$(TARGET) policy.gc

# Install Vim syntax highlighting for *.gc files.
# Writes to ~/.vim/ (classic) and ~/.config/nvim/ (Neovim, if that's what you use).
vim-install:
	@mkdir -p $$HOME/.vim/syntax $$HOME/.vim/ftdetect $$HOME/.vim/ftplugin
	cp vim/syntax/gc.vim    $$HOME/.vim/syntax/
	cp vim/ftdetect/gc.vim  $$HOME/.vim/ftdetect/
	cp vim/ftplugin/gc.vim  $$HOME/.vim/ftplugin/
	@echo "installed gc.vim into $$HOME/.vim/"
	@echo "open a .gc file and run  :set filetype?  to verify"

clean:
	rm -f $(TARGET) $(OBJS) parser.tab.c parser.tab.h lex.yy.c
