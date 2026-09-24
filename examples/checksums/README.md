# Checksums

Six checksum programs from GNU coreutils on random files of 4, 64, and 256 MiB.

```bash
build/isocost report examples/checksums/results -o examples/checksums/generated
```

Relative to `sha256sum`, `cksum` (CRC) takes 0.36x the time and `b2sum` (BLAKE2b) uses 0.6x the memory; those two are the only tools no other tool beats on both axes. Memory differs by less than 2x across all tools, so the memory axis stays linear while the time axis is logarithmic.

The config orders the file sizes with `[[workload]] order` and `[workloads] sort = "config"`.

## Measurement

- Machine: AMD Ryzen 9 3950X, Linux 7.2.5, 2026-09-23.
- Tools: GNU coreutils 9.10.
- Zebrac 0.6.2: 2000 ms per command, at least 10 samples, 2 warmup runs.

Rerun with `examples/checksums/bench.sh`.
