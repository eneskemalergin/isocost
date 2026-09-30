# Getting started

## Install

Each [release](https://github.com/eneskemalergin/isocost/releases) has a static Linux binary for x86-64 and AArch64. It needs no libraries.

```bash
version=0.1.0
arch=$(uname -m)                      # x86_64 or aarch64
base=https://github.com/eneskemalergin/isocost/releases/download/v$version
curl -fsSLO "$base/isocost-$version-$arch-linux.tar.gz"
curl -fsSLO "$base/SHA256SUMS"
sha256sum --check --ignore-missing SHA256SUMS
tar -xzf "isocost-$version-$arch-linux.tar.gz"
install -m 755 "isocost-$version-$arch-linux/isocost" ~/.local/bin/isocost
isocost --version
```

Or build from source with a C17 compiler:

```bash
git clone https://github.com/eneskemalergin/isocost
cd isocost
make                 # build/isocost
make test            # 43 checks
make install         # copies it to ~/.local/bin (PREFIX=... to change)
```

`make static` builds a static binary with `zig cc` and musl, the way releases are built.

## Measure with Zebrac

isocost reads the JSON that [Zebrac](https://github.com/eneskemalergin/zebrac) writes. Run every tool on one input in a single Zebrac call, so they share machine conditions, and write one JSON file per input:

```bash
mkdir -p results/compress
for input in source.tar binary.bin; do
    zebrac --quiet --duration 2000 --warmup 2 --json "results/compress/${input%.*}.json" -- \
        "gzip -6 -c $input" "zstd -3 -c $input" "xz -6 -c $input"
done
```

The file name becomes the workload name and the directory name becomes the group. [Inputs](Inputs) lists every layout isocost understands.

## Write a report

```bash
isocost list results/                 # what would be drawn, and how each result was named
isocost report results/ -o report/    # figures, report.md, summary.json
```

`report/report.md` has the figures, a table per comparison, and every workload. Read [Reading the figures](Figures) for what each mark means.

## Name the tools

Without a config, a tool is named by its program and the command words that differ between commands. To choose names, colors, and the reference, write `isocost.toml` next to `results/`:

```bash
isocost config > isocost.toml         # every setting with its default and a comment
```

```toml
[report]
baseline = "gzip -6"

[[tool]]
id = "zstd -3"
match = "zstd -3 *"     # names every result whose command matches
color = "#2a78d6"
```

`isocost config --check isocost.toml` reports the first error with its line and column. [Configuration](Configuration) lists every key.

## For papers and slides

```bash
isocost report results/ -o figures/ --size paper --format pdf        # 89 mm, one column
isocost report results/ -o figures/ --size paper-wide --format pdf   # 183 mm, two columns
isocost report results/ -o slides/ --size slide --format png
```

PDF and SVG files contain the glyph outlines, so they look the same in every viewer.
