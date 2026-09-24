# CRC-32

Eleven CRC-32 implementations (C, C++, Rust, Zig) from crc-bench, one-shot and streaming over 64 KiB, each in a native build (tuned for the host CPU: `-march=native` in C, `target-cpu=native` in Rust, `-mcpu=native` in Zig) and a generic build (baseline x86-64). Copied from the audit run of 2026-09-17.

```bash
build/isocost report examples/crc32/results -o examples/crc32/generated
```

Identity comes from crc-bench's `workload.tsv`: each candidate name is `TOOL__CASE__MODE...`, which gives four comparisons (mode by build). With one workload per comparison, the figures show single results and the note says so.

`zig-crc32-dispatch-v1` is lowest cost in all four comparisons. In the native builds, `c-isa-l` is slightly faster (0.09x of `zig-stdlib`) but uses more memory, so both are unbeaten on both axes.

With 11 tools in one figure, colors repeat and the repeated tools are drawn as hollow outlines, so every tool keeps a distinct color and shape pair.
