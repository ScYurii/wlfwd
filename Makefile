# SPDX-License-Identifier: GPL-2.0-only
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PROGS = wlfwd pdr-restart

all: $(PROGS)

%: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(PROGS)
	install -Dm755 wlfwd $(DESTDIR)$(BINDIR)/wlfwd
	install -Dm755 pdr-restart $(DESTDIR)$(BINDIR)/pdr-restart

clean:
	rm -f $(PROGS)

.PHONY: all install clean
