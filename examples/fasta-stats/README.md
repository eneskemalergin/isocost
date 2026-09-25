# FASTA statistics

The z-fasta `stats` suite: z-fasta with two index formats (FAI, ZFI) against noodles and rust-bio, as three comparisons: full files, a growing sequence count, and a growing file size. Copied from the z-fasta benchmark's newest run (2026-08-18 07:34).

```bash
build/isocost report examples/fasta-stats/results -o examples/fasta-stats/generated
```

The sidecar was written on another machine, so its paths point elsewhere; isocost matches rows on the last two path components and still names every result.

The ZFI index is faster and smaller than FAI on full files (0.91x time, 0.85x memory) and as the sequence count grows. As file size grows, FAI is best on both axes.
