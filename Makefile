#
# Makefile — policy compiler (phase 1: inventory)
#
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LEX     = flex
YACC    = bison

TARGET  = polc

OBJS    = parser.tab.o lex.yy.o main.o diag.o ipcache.o resolve.o bags.o
HEADERS = $(filter-out parser.tab.h, $(wildcard *.h))

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

parser.tab.c parser.tab.h: parser.y $(HEADERS)
	$(YACC) -d -o parser.tab.c parser.y

lex.yy.c: scanner.l parser.tab.h $(HEADERS)
	$(LEX) -o lex.yy.c scanner.l

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
