# isocost

isocost reads [Zebrac](https://github.com/eneskemalergin/zebrac) benchmark results and draws how fast and how memory-hungry each tool is, relative to a reference tool. It writes figures (PNG, SVG, PDF), a Markdown report, and a JSON summary. It is one static Linux binary of about 250 KB.

![Seven compressors, relative to gzip -6](https://raw.githubusercontent.com/eneskemalergin/isocost/main/assets/compression-overview.png)

The name comes from economics: an isocost line joins combinations of two inputs that cost the same. The dashed curves in the overview join combinations of run time and peak memory with the same combined cost.

## Start here

- [Getting started](Getting-Started): install a release, measure with Zebrac, and write a first report.
- [GitHub Action](GitHub-Action): write a report in another repository's workflow.
- [Examples](Examples): six real benchmarks with their configs.

## Reference

- [Inputs](Inputs): which files are read, how results are named and grouped, and the exit status.
- [Configuration](Configuration): every setting in `isocost.toml` and every command-line option.
- [Reading the figures](Figures): what each mark means and the math behind it.
- [summary.json](Summary-JSON): the fields of the machine-readable summary.

## Limits

isocost does not run benchmarks or sample processes. Zebrac measures; isocost reads what Zebrac wrote. Spreads show how tools vary across workloads. They are not confidence intervals, because Zebrac writes summaries, not paired samples.

isocost is at version 0.1. Options, config keys, and `summary.json` fields may still change before 1.0; the [changelog](https://github.com/eneskemalergin/isocost/blob/main/CHANGELOG.md) lists every change.
