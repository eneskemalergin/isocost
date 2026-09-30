# Changelog

Changes by version. Publication dates are added when a version is released.

## [Unreleased]

## [0.1.0]

First public version of isocost, a C17 program that needs only libc, libm, and POSIX threads.

### Added

- `isocost report` reads Zebrac summary JSON (single files, directory trees, or `bench.meta.v1` sidecars) and writes figures, `report.md`, and `summary.json`. `isocost list` shows how results are named and grouped without writing anything. `isocost config` prints and checks `isocost.toml`.
- An overview figure per comparison: every tool placed by run time and peak memory relative to a reference, with per-workload marks, descriptive spread bars and ellipses, the tools no other tool beats on both axes, and curves of equal geometric or linear cost.
- A workloads figure per comparison, with one row per workload, so a tool that loses on one workload cannot hide behind its average.
- PNG, SVG, and PDF output with DejaVu Sans glyphs built in, at screen, slide, one-column (89 mm), two-column (183 mm), and custom sizes. Output is byte-identical across runs.
- `isocost.toml` configuration for tool names, colors, shapes, labels, references, titles, sizes, and axis limits. Unknown keys and wrong types are errors that name the line and suggest the closest key.
- Six example benchmarks with real Zebrac 0.6.2 results: compression, checksums, text search, FASTQ subsampling, FASTA statistics, and CRC-32.
- 43 tests, including golden values, failure exit codes, PDF, SVG, and PNG validation, and AddressSanitizer, LeakSanitizer, and UBSan runs.
- Static Linux release archives for x86-64 and AArch64, built with `zig cc` and musl, with SHA-256 checksums.
- A GitHub Action (`uses: eneskemalergin/isocost@v0.1.0`) that downloads a release and writes a report from Zebrac results in another repository's workflow.
- GitHub Wiki pages synchronized from `wiki/`, and CI that runs the tests with GCC and Clang, checks shell scripts and workflows, and checks that the README figures match the code.
