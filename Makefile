CC ?= cc
CFLAGS = -pedantic -Wall -Wextra -Wno-unused-parameter -Os -lX11 # -std=c99

PREFIX ?= /usr

SRC = wmd.c wmc.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean install

all: wmd wmc

wmd: wmd.o
	$(CC) $(CFLAGS) $< -o $@

wmc: wmc.o
	$(CC) $(CFLAGS) $< -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f wmd wmc $(OBJ)

install: all
	install -Dm 755 wmd $(PREFIX)/bin/wmd
	install -Dm 755 wmc $(PREFIX)/bin/wmc

uninstall:
	rm -f $(PREFIX)/bin/wmd $(PREFIX)/bin/wmc
