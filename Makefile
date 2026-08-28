PREFIX		?= $(HOME)/.local
BINDIR		?= $(PREFIX)/bin

CC			:= gcc
CFLAGS		:= -std=c17 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS	:= -D_POSIX_C_SOURCE=200809L -Ibuild

PKG_CFLAGS	:= $(shell pkg-config --cflags wayland-client xkbcommon scfg)
PKG_LIBS	:= $(shell pkg-config --libs wayland-client xkbcommon scfg)
SCANNER		:= $(shell pkg-config --variable=wayland_scanner wayland-scanner)

PROTOCOLS	:= river-window-management-v1 river-xkb-bindings-v1 river-layer-shell-v1
PROTO_H		:= $(PROTOCOLS:%=build/%-client-protocol.h)
PROTO_C		:= $(PROTOCOLS:%=build/%-client-protocol.c)
PROTO_O		:= $(PROTOCOLS:%=build/%-client-protocol.o)

SRC			:= $(wildcard src/*.c)
OBJ			:= $(SRC:src/%.c=build/%.o)

all: satori

satori: $(OBJ) $(PROTO_O)
	$(CC) -o $@ $^ $(PKG_LIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -c $< -o $@

build/%.o: build/%.c | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -c $< -o $@

# Our sources include the generated headers; make that explicit so -j can't race.
$(OBJ): $(PROTO_H) src/satori.h

build/%-client-protocol.h: protocol/%.xml | build
	$(SCANNER) client-header < $< > $@

build/%-client-protocol.c: protocol/%.xml | build
	$(SCANNER) private-code < $< > $@

build:
	mkdir -p build

clean:
	rm -rf build satori satori-asan

# -D creates BINDIR if it is missing. Override with PREFIX= or BINDIR=.
install: satori
	install -D -m 755 satori $(BINDIR)/satori

uninstall:
	rm -f $(BINDIR)/satori

# Separate binary, single compile+link: no object collision with build/*.o.
asan: $(PROTO_C) $(PROTO_H)
	$(CC) -std=c17 -O1 -g -fsanitize=address -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic $(CPPFLAGS) $(PKG_CFLAGS) \
		$(SRC) $(PROTO_C) -o satori-asan $(PKG_LIBS)

# Test-only protocol: injects key presses into the headless compositor, which
# has no keyboard of its own. Explicit rules, so the protocol/%.xml pattern
# above does not try to claim it.
VK_XML	:= tests/virtual-keyboard-unstable-v1.xml
VK_H	:= build/virtual-keyboard-unstable-v1-client-protocol.h
VK_C	:= build/virtual-keyboard-unstable-v1-client-protocol.c

$(VK_H): $(VK_XML) | build
	$(SCANNER) client-header < $< > $@

$(VK_C): $(VK_XML) | build
	$(SCANNER) private-code < $< > $@

# memfd_create needs _GNU_SOURCE, which _POSIX_C_SOURCE alone hides.
build/keypress: tests/keypress.c $(VK_C) $(VK_H) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -D_GNU_SOURCE $(PKG_CFLAGS) -o $@ \
		tests/keypress.c $(VK_C) $(PKG_LIBS)

# Includes src/input.c (hence the explicit prerequisite), so link everything
# except build/input.o. window.o pulls in output.o for the output lookup, and
# both of those plus input.c pull in layer.o. config.o is the parser under test;
# it calls back into the included input.c for action_from_name.
TEST_OBJ := build/window.o build/output.o build/layer.o build/config.o

build/test-actions: tests/test_actions.c src/input.c src/satori.h example/config $(TEST_OBJ) $(PROTO_O) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -o $@ \
		tests/test_actions.c $(TEST_OBJ) $(PROTO_O) $(PKG_LIBS)

test: satori asan build/test-actions build/keypress
	./build/test-actions
	./scripts/test-nested.sh ./satori
	./scripts/test-nested.sh ./satori-asan
	./scripts/test-exit.sh ./satori-asan

.PHONY: all clean asan test install uninstall
