# isocost: needs a C17 compiler, libc, libm, and POSIX threads.
#
#   make                 build/isocost
#   make test            run tests/run.sh
#   make sanitize        build/isocost-sanitize with AddressSanitizer and UBSan (clang preferred)
#   make static          build/isocost-static, a static binary for this machine's CPU,
#                        built with GCC and musl in an Alpine container (podman or docker)
#   make examples        render every example into examples/*/generated/
#   make docs-images     render the figures used in README.md and wiki/
#   make font            regenerate src/font_data.h (needs Python 3; Pillow adds kerning)
#   make install         copy build/isocost to $(PREFIX)/bin (PREFIX defaults to ~/.local)
#   make clean

CC ?= cc
WARNINGS = -std=c17 -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wformat=2 -Wno-format-nonliteral -Wstrict-prototypes -Wmissing-prototypes
SOURCES = $(wildcard src/*.c)
HEADERS = $(wildcard src/*.h)
OBJDIR ?= build/obj
BIN ?= build/isocost
OBJECTS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(SOURCES))

# The drawing, compression, and glyph code is built for speed (-O3) and the
# rest for size (-Os). Measured 2026-09-23 with GCC 16 on 527 real files, one
# thread: 180 KB and 402 ms, against 252 KB and 455 ms for an all -O2 build.
# `make CC=clang` is about 8% faster again.
HOT = $(OBJDIR)/raster.o $(OBJDIR)/deflate.o $(OBJDIR)/font.o
SPEED_FLAGS ?= -O3
SIZE_FLAGS ?= -Os
# C has no exceptions and isocost never cancels threads, so unwind tables
# (16 KB) are left out.
SECTIONS = -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables
# Figures must be byte-identical on every compiler and CPU. Clang fuses a*b+c
# into one FMA instruction on AArch64 (and on x86-64 with -march=haswell),
# which rounds once instead of twice and moves pixels.
FLOAT = -ffp-contract=off
LDFLAGS ?= -s -Wl,--gc-sections

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -lm -pthread -o $@

$(HOT): $(OBJDIR)/%.o: src/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(WARNINGS) $(FLOAT) $(SPEED_FLAGS) $(SECTIONS) -c $< -o $@

$(OBJDIR)/%.o: src/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(WARNINGS) $(FLOAT) $(SIZE_FLAGS) $(SECTIONS) -c $< -o $@

$(OBJDIR):
	mkdir -p $@

.PHONY: test sanitize static examples docs-images font install clean

test: build/isocost build/isocost-sanitize
	tests/run.sh

SANITIZE_CC := $(shell command -v clang >/dev/null 2>&1 && echo clang || echo $(CC))
build/isocost-sanitize: $(SOURCES) $(HEADERS) | $(OBJDIR)
	$(SANITIZE_CC) $(WARNINGS) $(FLOAT) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -lm -pthread -o $@

sanitize: build/isocost-sanitize

# `make static` builds build/isocost-static the way releases are built: GCC
# and musl in a pinned Alpine image, for the CPU of the machine running it.
# The image digest covers every architecture Alpine publishes.
CONTAINER ?= $(shell command -v podman > /dev/null 2>&1 && echo podman || echo docker)
ALPINE ?= docker.io/library/alpine:3.24.2@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6
static:
	$(CONTAINER) run --rm -v "$(CURDIR):/src:Z" -w /src $(ALPINE) sh -euc '\
		apk add --no-cache build-base > /dev/null; \
		make BIN=build/isocost-static OBJDIR=build/obj-static LDFLAGS="-static -s -Wl,--gc-sections"'

examples: build/isocost
	examples/render.sh

docs-images: build/isocost
	mkdir -p assets
	build/isocost report examples/compression/results -o build/docs-images --group compress --format png --force -q
	cp build/docs-images/compress-overview.png assets/compression-overview.png
	cp build/docs-images/compress-workloads.png assets/compression-workloads.png

font:
	python3 tools/gen_font.py src/font_data.h

PREFIX ?= $(HOME)/.local
install: build/isocost
	mkdir -p $(PREFIX)/bin
	cp build/isocost $(PREFIX)/bin/isocost

clean:
	rm -rf build
