CC		:= gcc
CFLAGS	:= -std=c17 -O2 -g -Wall -Wextra -Wpedantic

PKG_CFLAGS	:= $(shell pkg-config --cflags wayland-client)
PKG_LIBS	:= $(shell pkg-config --libs wayland-client)
SCANNER		:= $(shell pkg-config --variable=wayland_scanner wayland-scanner)

all: satori

satori: build/river-window-management-v1-client-protocol.o build/main.o
	$(CC) -o $@ $^ $(PKG_LIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -Ibuild $(PKG_CFLAGS) -c $< -o $@

build/%.o: build/%.c | build
	$(CC) $(CFLAGS) -Ibuild $(PKG_CFLAGS) -c $< -o $@

build/main.o: build/river-window-management-v1-client-protocol.h

build/river-window-management-v1-client-protocol.h: protocol/river-window-management-v1.xml | build
	$(SCANNER) client-header < $< > $@

build/river-window-management-v1-client-protocol.c: protocol/river-window-management-v1.xml | build
	$(SCANNER) private-code < $< > $@

build:
	mkdir -p build

clean:
	rm -rf build satori

.PHONY: all clean


