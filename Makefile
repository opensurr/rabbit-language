CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O3
LDLIBS   = -lm
PREFIX  ?= /usr/local
BIN      = bin/rabbit
SRC      = src/rabbit.c

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/rabbit
	install -m 755 scripts/rabrun $(DESTDIR)$(PREFIX)/bin/rabrun
	install -m 755 scripts/rabget $(DESTDIR)$(PREFIX)/bin/rabget
	install -d $(DESTDIR)$(PREFIX)/share/rabbit/lib
	cp lib/*.rab $(DESTDIR)$(PREFIX)/share/rabbit/lib/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/rabbit $(DESTDIR)$(PREFIX)/bin/rabrun $(DESTDIR)$(PREFIX)/bin/rabget
	rm -rf $(DESTDIR)$(PREFIX)/share/rabbit

test: $(BIN)
	RABBIT=../$(BIN) RABBIT_LIB=../lib sh tests/run.sh

vsix:
	rm -rf build rabbit.vsix
	mkdir -p build/extension
	cp -r vscode-rabbit/. build/extension/
	cp "vsix/[Content_Types].xml" vsix/extension.vsixmanifest build/
	cd build && zip -qr ../rabbit.vsix .
	rm -rf build

clean:
	rm -rf bin build

.PHONY: all install uninstall test vsix clean
