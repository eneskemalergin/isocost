# isocost: needs a C17 compiler, libc, libm, and POSIX threads.
#
#   make                 build/isocost
#   make test            run tests/run.sh
#   make sanitize        build/isocost-sanitize with AddressSanitizer and UBSan (clang preferred)
#   make static          build/isocost-static, a static Linux binary via `zig cc` (musl);
#                        TARGET=aarch64-linux-musl builds for 64-bit ARM
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
LDFLAGS ?= -s -Wl,--gc-sections

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -lm -pthread -o $@

$(HOT): $(OBJDIR)/%.o: src/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(WARNINGS) $(SPEED_FLAGS) $(SECTIONS) -c $< -o $@

$(OBJDIR)/%.o: src/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(WARNINGS) $(SIZE_FLAGS) $(SECTIONS) -c $< -o $@

$(OBJDIR):
	mkdir -p $@

.PHONY: test sanitize static examples docs-images font install clean

test: build/isocost build/isocost-sanitize
	tests/run.sh

SANITIZE_CC := $(shell command -v clang >/dev/null 2>&1 && echo clang || echo $(CC))
build/isocost-sanitize: $(SOURCES) $(HEADERS) | $(OBJDIR)
	$(SANITIZE_CC) $(WARNINGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -lm -pthread -o $@

sanitize: build/isocost-sanitize

# `make static` builds build/isocost-static for x86_64 Linux. Another zig
# target, such as TARGET=aarch64-linux-musl, writes build/isocost-TARGET.
TARGET ?= x86_64-linux-musl
STATIC_BIN = build/isocost-$(if $(filter x86_64-linux-musl,$(TARGET)),static,$(TARGET))
static:
	$(MAKE) BIN=$(STATIC_BIN) OBJDIR=build/obj-$(TARGET) CC="zig cc -target $(TARGET)" LDFLAGS="-static -s -Wl,--gc-sections"

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
