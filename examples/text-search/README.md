# Text search

GNU grep and ripgrep searching a copy of `/usr/include` (57 MiB of C headers): a common word, a case-insensitive word, a regular expression, and a rare word.

```bash
build/isocost report examples/text-search/results -o examples/text-search/generated
```

Relative to GNU grep, ripgrep with all threads takes 0.19x the time; GNU grep uses the least memory. With one thread, ripgrep is faster than GNU grep on three of the four searches (0.73x to 0.84x) and slightly slower on the regular expression (1.04x).

The config shows first-match rules: `rg * -j1 *` comes before `rg *`, so the one-thread runs get their own tool.

## Measurement

- Machine: AMD Ryzen 9 3950X (32 threads), Linux 7.2.5, 2026-09-23.
- Tools: GNU grep 3.12 (`/usr/bin/grep`), ripgrep 15.2.0.
- Zebrac 0.6.2: 2000 ms per command, at least 10 samples, 2 warmup runs.

Rerun with `examples/text-search/bench.sh`. A pattern with no match cannot be benchmarked: both tools exit 1 when nothing matches, which Zebrac records as a failed run.
