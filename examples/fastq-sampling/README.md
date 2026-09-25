# FASTQ subsampling

The z-fastq `sample` suite: z-fastq and seven other FASTQ subsampling tools on 10 workloads (fraction and count sampling, single-end, paired, and interleaved), with plain and gzip-compressed input. Copied from the z-fastq benchmark's newest run (2026-09-16 09:57).

```bash
build/isocost report examples/fastq-sampling/results -o examples/fastq-sampling/generated
```

Identity comes from the benchmark's own `metadata_*.jsonl` sidecar, so the file names do not have to follow any convention. The config only sets the reference, titles, and the order of the two z-fastq variants.

On gzip input, z-fastq is fastest and z-fastq with native gzip uses 0.85x the memory; they are the only tools no other tool beats on both axes. On plain input, rasusa takes 0.93x the run time on the 8 workloads it can run. The workloads view shows how much bbtools depends on the workload: 3.6x to 37x slower than z-fastq on gzip input and 10x to 236x on plain input.

Not every tool can run every workload (fqkit runs 3 of 10), so the legend shows `n=` and `report.md` flags partial coverage.
