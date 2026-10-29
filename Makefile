# SPDX-License-Identifier: Apache-2.0
# clixon-vppcfg: Clixon (YANG/CLI/NETCONF) di atas vppcfg.
PREFIX  ?= /usr/local
LIBDIR  := $(PREFIX)/lib/clixon-vppcfg
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -fPIC -I$(PREFIX)/include
LDFLAGS += -L$(PREFIX)/lib

all: build/backend_vppcfg.so build/cli_vppcfg.so

build/backend_vppcfg.so: src/backend_vppcfg.c
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS) -lclixon_backend -lclixon -lcligen

build/cli_vppcfg.so: src/cli_vppcfg.c
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS) -lclixon_cli -lclixon -lcligen

install: all
	install -D -m 644 build/backend_vppcfg.so $(DESTDIR)$(LIBDIR)/backend/backend_vppcfg.so
	install -D -m 644 build/cli_vppcfg.so $(DESTDIR)$(LIBDIR)/cli/cli_vppcfg.so
	install -d $(DESTDIR)$(LIBDIR)/clispec
	rm -f $(DESTDIR)$(LIBDIR)/clispec/*.cli
	install -m 644 clispec/*.cli $(DESTDIR)$(LIBDIR)/clispec/
	install -d $(DESTDIR)$(LIBDIR)/clispec-vrp
	rm -f $(DESTDIR)$(LIBDIR)/clispec-vrp/*.cli
	install -m 644 clispec-vrp/*.cli $(DESTDIR)$(LIBDIR)/clispec-vrp/
	install -d $(DESTDIR)$(PREFIX)/share/clixon-vppcfg/yang
	install -m 644 yang/*.yang $(DESTDIR)$(PREFIX)/share/clixon-vppcfg/yang/
	install -D -m 755 helper/clixon-vppcfg $(DESTDIR)$(PREFIX)/libexec/clixon-vppcfg/clixon-vppcfg
	install -D -m 755 helper/nettool $(DESTDIR)$(PREFIX)/libexec/clixon-vppcfg/nettool
	visudo -cqf etc/sudoers-clixon-vppcfg
	install -D -m 440 etc/sudoers-clixon-vppcfg $(DESTDIR)/etc/sudoers.d/clixon-vppcfg
	install -D -m 755 bin/rtr $(DESTDIR)$(PREFIX)/bin/rtr
	install -D -m 755 bin/vrp $(DESTDIR)$(PREFIX)/bin/vrp
	install -D -m 644 etc/vppcfg.xml $(DESTDIR)/etc/clixon/vppcfg.xml
	install -D -m 644 systemd/clixon-vppcfg.service $(DESTDIR)/etc/systemd/system/clixon-vppcfg.service

clean:
	rm -rf build

.PHONY: all install clean
