CC       ?= gcc
CSTD     ?= c11
PREFIX   ?= /usr/local
BINDIR    = $(PREFIX)/bin
PKGS     := libdrm
DEFAULT_CRTC ?= 68

CPPFLAGS += -D_GNU_SOURCE -DDEFAULT_CRTC=$(DEFAULT_CRTC)
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -lm

SRC      := gamma.c
BIN      := gamma

all: $(BIN)
$(BIN): $(SRC)
	$(CC) -std=$(CSTD) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
clean:
	rm -f $(BIN) *.o
install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
.PHONY: all clean install uninstall

