# Examples

The repository has six real benchmarks under [examples/](https://github.com/eneskemalergin/isocost/tree/main/examples), each with its Zebrac results, an `isocost.toml`, and a README. Every value is a real Zebrac 0.6.2 measurement. Render all of them from a checkout:

```bash
make examples                    # writes examples/*/generated/ (ignored by Git)
examples/render.sh --size paper  # extra options go to every run
```

Pick the example closest to how your results are laid out, and copy its config.

| Example | What it compares | How results are named |
| --- | --- | --- |
| [compression](https://github.com/eneskemalergin/isocost/tree/main/examples/compression) | 7 compressors, compressing and decompressing three 16 MiB inputs | `[[tool]] match` rules on the command; `[[group]]` sets a reference per direction |
| [checksums](https://github.com/eneskemalergin/isocost/tree/main/examples/checksums) | 6 coreutils checksum programs on 4, 64, and 256 MiB files | `[[tool]] match`; `[[workload]] order` sorts the file sizes |
| [text-search](https://github.com/eneskemalergin/isocost/tree/main/examples/text-search) | GNU grep and ripgrep (one thread and all threads) on four searches | first-match rules: `rg * -j1 *` before `rg *` |
| [fastq-sampling](https://github.com/eneskemalergin/isocost/tree/main/examples/fastq-sampling) | 8 FASTQ subsampling tools on 10 workloads, plain and gzip input | the benchmark's `bench.meta.v1` sidecar |
| [fasta-stats](https://github.com/eneskemalergin/isocost/tree/main/examples/fasta-stats) | 4 FASTA statistics tools in three comparisons | a sidecar written on another machine, matched on the last two path components |
| [crc32](https://github.com/eneskemalergin/isocost/tree/main/examples/crc32) | 11 CRC-32 implementations in four comparisons, one workload each | crc-bench's `workload.tsv` |

The first three were measured with the `bench.sh` script in their directory, which needs Zebrac and the compared tools; it builds its inputs under `data/`, which Git ignores. The last three were copied from other benchmarks, with paths made relative and nothing else changed.

## Compression

![Compression overview](https://raw.githubusercontent.com/eneskemalergin/isocost/main/assets/compression-overview.png)

Relative to `gzip -6`, `pigz -6` (all cores) compresses in 0.054x the time and is the only tool besides gzip that no other tool beats on both axes; `gzip -6` uses the least memory. For decompression, zstd is fastest (0.53x the time of `gzip -d`), and gzip, pigz, and zstd are all unbeaten on both axes.
