#
# Makefile — policy compiler (phase 1: inventory)
#
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LEX     = flex
YACC    = bison

TARGET  = polc

OBJS    = parser.tab.o lex.yy.o main.o diag.o ipcache.o resolve.o

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# bison produces parser.tab.c AND parser.tab.h in one step
parser.tab.c parser.tab.h: parser.y ast.h diag.h
	$(YACC) -d -o parser.tab.c parser.y

# flex needs parser.tab.h (for token ids)
lex.yy.c: scanner.l parser.tab.h ast.h diag.h
	$(LEX) -o lex.yy.c scanner.l

# explicit deps so headers drive rebuilds
parser.tab.o: parser.tab.c ast.h diag.h
	$(CC) $(CFLAGS) -c parser.tab.c

lex.yy.o: lex.yy.c parser.tab.h ast.h diag.h
	$(CC) $(CFLAGS) -c lex.yy.c

main.o: main.c ast.h diag.h ipcache.h resolve.h
	$(CC) $(CFLAGS) -c main.c

diag.o: diag.c diag.h
	$(CC) $(CFLAGS) -c diag.c

ipcache.o: ipcache.c ipcache.h ast.h
	$(CC) $(CFLAGS) -c ipcache.c

resolve.o: resolve.c resolve.h ast.h diag.h
	$(CC) $(CFLAGS) -c resolve.c

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
