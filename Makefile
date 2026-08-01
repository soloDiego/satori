CC			:= gcc
CFLAGS		:= -std=c17 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS	:= -D_POSIX_C_SOURCE=200809L -Ibuild

PKG_CFLAGS	:= $(shell pkg-config --cflags wayland-client xkbcommon)
PKG_LIBS	:= $(shell pkg-config --libs wayland-client xkbcommon)
SCANNER		:= $(shell pkg-config --variable=wayland_scanner wayland-scanner)

PROTOCOLS	:= river-window-management-v1 river-xkb-bindings-v1
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

# Separate binary, single compile+link: no object collision with build/*.o.
asan: $(PROTO_C) $(PROTO_H)
	$(CC) -std=c17 -O1 -g -fsanitize=address -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic $(CPPFLAGS) $(PKG_CFLAGS) \
		$(SRC) $(PROTO_C) -o satori-asan $(PKG_LIBS)

test: satori asan
	./scripts/test-nested.sh ./satori
	./scripts/test-nested.sh ./satori-asan

.PHONY: all clean asan test
