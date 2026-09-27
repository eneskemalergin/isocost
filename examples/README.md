# Examples

Six real benchmarks, each with its Zebrac results, an `isocost.toml`, and a README. Render all of them into `examples/*/generated/` (ignored by Git):

```bash
make examples                    # or: examples/render.sh
examples/render.sh --size paper  # extra options go to every run
```

Measured on this machine by each example's `bench.sh`, with tools named by `[[tool]] match` rules:

- [compression](compression/README.md): 7 compressors, compressing and decompressing three 16 MiB inputs.
- [checksums](checksums/README.md): 6 checksum programs on files of 4, 64, and 256 MiB.
- [text-search](text-search/README.md): GNU grep and ripgrep (1 thread and all threads) on four searches.

Copied from other benchmarks, with tools named by the files those benchmarks write:

- [fastq-sampling](fastq-sampling/README.md): 8 FASTQ subsampling tools on 10 workloads, plain and gzip input (z-fastq, `bench.meta.v1` sidecar).
- [fasta-stats](fasta-stats/README.md): 4 FASTA statistics tools in three comparisons (z-fasta, sidecar written on another machine).
- [crc32](crc32/README.md): 11 CRC-32 implementations in four comparisons, one workload each (crc-bench, `workload.tsv`).

[aa](aa/README.md) holds two captures of one command, kept as input for a repeatability check. `isocost` does not read it.

Every value is a real Zebrac 0.6.2 measurement. Paths in copied results were made relative; nothing else was changed.
