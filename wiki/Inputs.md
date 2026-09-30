# Inputs

## Terms

- A **result** is one command's Zebrac summary: sample counts and the median, mean, and quartiles of wall time and peak RSS.
- A **tool** is what a result measured, such as `zstd -3`. A **workload** is the input it ran on, such as `reads`.
- A **comparison** is a set of tools run on the same workloads. Each comparison gets its own figures.
- A comparison is identified by its **suite** (a benchmark suite, often empty), its **group** (a section such as `perf_gzip` or `compress`), and its **run** (a timestamp or run name, often empty).
- The **reference** is the tool every other tool is divided by, workload by workload.

## Reading files

isocost reads Zebrac summary JSON (`schema_version` 1). An input can be:

- one JSON file, with one or more results;
- a directory, searched recursively (`.git`, `node_modules`, and Zig cache directories are skipped, and symbolic links to directories are not followed);
- a `bench.meta.v1` JSONL sidecar. Every file it lists must exist, or the run stops with exit status 3.

JSON without both `zebrac_version` and `results` is skipped and counted, so run manifests and other JSON can sit next to results. A Zebrac file that does not parse stops the run with exit status 2 and the file name and byte offset.

From each result isocost keeps the command, sample and failure counts, and the median, mean, first and third quartiles, and unit of `wall_time` and `peak_rss`. Time units (ns, us, ms, s) are converted to nanoseconds and memory units (bytes, KiB, MiB, GiB, KB, MB) to bytes. Other Zebrac fields are skipped.

## Which tool and workload a result belongs to

A Zebrac result records the command it ran, not which workload or tool that command represents. The first matching rule names it. `isocost list` shows how many results each rule named.

1. **config**: a `[[tool]]` entry whose `match` glob matches the command. The workload is the file name without `.json`, the group is the directory name, and the family is the program name.
2. **sidecar**: a `bench.meta.v1` JSONL file beside the JSON or one directory up, with `raw_json`, `workload`, `tool`, and optionally `suite`, `section` (the group), and `command`. Rows match on the last two path components, so a results tree moved from another machine still matches. A file with several results also matches on the command.
3. **workload.tsv**: a `candidate<TAB>command` table beside the JSON, where the candidate is `TOOL__WORKLOAD__MODE...`. The group is the mode plus the build directory, and the run is the directory above that.
4. **path layout**: `TOOL/FORMAT/LEVEL/THREADS/CATEGORY.CLASS.OPERATION.json`. The tool is `TOOL -LEVEL`, the workload `CATEGORY.CLASS`, and the group `FORMAT OPERATION`.
5. **case--variant**: `WORKLOAD--TOOL.json`.
6. **case__tool**: `WORKLOAD__TOOL.json` in a `GROUP_YYYYMMDD_HHMMSS` directory, which also gives the run.
7. **command diff**: in a file with several results, each tool is named by its program and the command words that are not shared by every command in the file.
8. **argv0**: the program name.

If two results claim the same workload and tool in one comparison, that comparison is not drawn and both paths are reported.

## Comparisons, groups, and runs

Results are grouped into one comparison per (suite, group, run), and each comparison gets its own figures. The project is the directory that holds `bench/` or `results/`. By default only the newest run of each (suite, group) is drawn; older runs are listed at the end of `report.md`. Pass `--runs all` to draw every run.

Within a comparison every tool is divided by the reference tool on each workload. A workload without a usable reference result is counted but not drawn. Results with failed samples, a missing metric, a non-positive estimate, or an unknown unit are excluded and listed with the reason.

## Exit status

- `0`: the report was written.
- `2`: a usage error, an invalid config, or a file that could not be read or parsed. Nothing is written.
- `3`: the input is inconsistent. With no Zebrac results or a stale sidecar, nothing is written. With a duplicate identity or a missing reference in some comparisons, the other comparisons are drawn, and `report.md` and `summary.json` mark the affected ones as not drawn.

## Outputs

```text
report.md                       figures, tables, every workload, inputs and settings
summary.json                    schema isocost.report.v1: settings, inputs, and per comparison the tools and results
GROUP-overview.{png,svg,pdf}
GROUP-workloads.{png,svg,pdf}
points.tsv                      with --data: one row per result and one per tool aggregate
```

File names are the suite and group (the project when there are neither), with the run added when two comparisons would otherwise share a name. Output is deterministic: the same input, config, and binary give the same bytes, apart from the command line and timing in `report.md`. Each file is written to a temporary name and renamed, so a failed run never leaves a partial file.
