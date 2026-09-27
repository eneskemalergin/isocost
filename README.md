# isocost

`isocost` reads [Zebrac](https://github.com/eneskemalergin/zebrac) benchmark results and draws how fast and how memory-hungry each tool is, relative to a reference tool. It writes figures (PNG, SVG, PDF), a Markdown report, and a JSON summary. It is one C program, about 180 KB, that needs only libc, libm, and POSIX threads. It runs on Linux.

Status: 1.0 in development. File formats and options may still change.

![Seven compressors, relative to gzip -6](docs/images/compression-overview.png)

## Build

```bash
make                 # build/isocost
make test            # 43 checks, including AddressSanitizer and UBSan
make install         # copies it to ~/.local/bin (PREFIX=... to change)
make static          # build/isocost-static, a static binary (needs zig)
```

## Use

Give it Zebrac JSON: one file, a directory tree, or a `bench.meta.v1` sidecar.

```bash
isocost report results/ -o report/   # figures, report.md, summary.json
isocost list results/                # what would be drawn and how each result was named
isocost config > isocost.toml        # every setting with its default
```

Each comparison gets two figures:

- `*-overview` places every tool by run time and peak memory relative to the reference. It shows the spread across workloads, the tools that no other tool beats on both axes, and curves of equal combined cost.
- `*-workloads` has one row per workload, so a tool that loses on one workload cannot hide behind its average.

Figures come in screen, slide, one-column (89 mm), and two-column (183 mm) sizes. PDF and SVG are vector files with the glyphs built in, so they look the same in every viewer.

## Configure

`isocost.toml` next to the results is optional. It names the tools, sets their colors, shapes, and labels, and chooses the reference, titles, sizes, and axis limits. For example:

```toml
[report]
baseline = "gzip -6"
formats = ["png", "pdf"]

[figure]
size = "paper"          # 89 mm wide at 300 dpi, 7 pt text

[[tool]]
id = "zstd -3"
match = "zstd -3 *"     # names every result whose command matches
color = "#2a78d6"
shape = "diamond"
```

## Documentation

- [docs/config.md](docs/config.md): every setting.
- [docs/inputs.md](docs/inputs.md): which files are read, how results are named and grouped, and the exit status.
- [docs/figures.md](docs/figures.md): what each mark means and the math behind it.
- [docs/summary.md](docs/summary.md): the fields of `summary.json`.
- [examples/](examples/README.md): six real benchmarks with their configs.

## Limits

isocost does not run benchmarks or sample processes. Zebrac measures; isocost reads what Zebrac wrote. Spreads show how tools vary across workloads. They are not confidence intervals, because Zebrac writes summaries, not paired samples.
