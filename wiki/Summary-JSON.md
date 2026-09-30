# summary.json

Every report directory has a `summary.json` with the same numbers as the figures and `report.md`, for scripts and agents. `isocost list INPUT... --json` prints the same document without writing any files.

The schema name is `isocost.report.v1`. Within v1, fields are only added, never renamed or removed, and their meaning does not change. A change that breaks any of that gets a new schema name.

## Example

Trimmed from `examples/compression`:

```json
{
  "schema": "isocost.report.v1",
  "isocost_version": "0.1.0",
  "config": "examples/compression/isocost.toml",
  "exit_status": 0,
  "settings": {"statistic": "median", "cost": "geometric", "time_weight": 0.5, "memory_weight": 0.5, "cases": "all", "runs": "latest", "data": false},
  "inputs": {"files": 6, "results": 42, "excluded": 0, "not_zebrac": 0, "identity_rules": {"config": 42}, "skipped_runs": []},
  "figures": [
    {
      "id": "compress",
      "title": "Compression time and memory, relative to gzip -6",
      "project": "compression", "suite": "", "group": "compress", "run": "",
      "status": "drawn",
      "reference": "gzip -6", "reference_automatic": false,
      "workloads": 3,
      "files": ["compress-overview.png", "compress-overview.svg", "compress-overview.pdf", "compress-workloads.png", "compress-workloads.svg", "compress-workloads.pdf"],
      "diagnostics": [],
      "tools": [
        {
          "id": "bzip2 -9", "label": "bzip2 -9", "hidden": false, "reference": false, "workloads": 3,
          "run_time_ratio": 3.008722392, "speedup": 0.3323669883, "memory_ratio": 4.22027771,
          "run_time_spread": [2.491711568, 3.626021344], "memory_spread": [4.067865996, 4.347204753],
          "cost": 3.563375373, "not_dominated": false, "selected": false,
          "wins_per_workload": 0, "failed_workloads": 0, "incomplete_workloads": 0
        }
      ],
      "results": [
        {
          "workload": "source", "tool": "bzip2 -9",
          "run_time_ratio": 2.797938066, "memory_ratio": 4.150105708,
          "run_time_ns": 1070560339, "peak_rss_bytes": 8040448,
          "not_dominated": false, "source": "examples/compression/results/compress/source.json"
        }
      ]
    }
  ]
}
```

## Top level

- `schema`: always `"isocost.report.v1"` for this layout.
- `isocost_version`: the program version that wrote the file.
- `config`: the `isocost.toml` path that was used, or `null` for built-in defaults.
- `exit_status`: the process exit status: `0`, `2`, or `3` (see [Inputs](Inputs#exit-status)).
- `settings`: `statistic`, `cost`, `cases`, `runs`, and `data` as configured. `time_weight` and `memory_weight` are normalized to sum to 1.
- `inputs.files`: Zebrac files read. `inputs.results`: results in this report. `inputs.excluded`: results from older runs or filtered groups. `inputs.not_zebrac`: JSON files skipped because they are not Zebrac results.
- `inputs.identity_rules`: how many results each naming rule named, keyed by rule (`config`, `sidecar`, `workload.tsv`, `path layout`, `case--variant`, `case__tool`, `command diff`, `argv0`).
- `inputs.skipped_runs`: older runs that were not drawn, one sentence each.
- `figures`: one entry per comparison, in suite, group, and run order.

## Each comparison (figures[])

- `id`: the file name prefix, such as `compress` for `compress-overview.png`.
- `title`, `project`, `suite`, `group`, `run`: identity and title.
- `status`: `"drawn"`, or `"not_drawn"` when the input was inconsistent. For a comparison that was not drawn, `diagnostics` says why and `tools` and `results` are empty.
- `reference`: the reference tool id. `reference_automatic`: `true` when isocost chose it.
- `workloads`: workloads that have a usable reference result.
- `files`: the figure files written, relative to the report directory.
- `diagnostics`: notes such as excluded results and partial coverage, one sentence each.
- `tools`: one entry per tool, in id order.
- `results`: one entry per tool and workload that was drawn.

## Each tool (tools[])

- `id`, `label`: the identity and the display name.
- `hidden`: `true` when `[[tool]] hide` removed it from figures and tables.
- `reference`: `true` for the reference tool.
- `workloads`: workloads this tool was measured on. A tool with 0 has no ratio fields.
- `run_time_ratio`, `memory_ratio`: geometric means of the per-workload ratios to the reference. Lower is better.
- `speedup`: `1 / run_time_ratio`.
- `run_time_spread`, `memory_spread`: the quartiles (or minimum and maximum with `spread = "minmax"`) of the per-workload ratios.
- `cost`: the configured cost of the aggregate point.
- `not_dominated`: `true` when no other tool is both faster and smaller on the aggregates.
- `selected`: `true` for the lowest-cost tool among those measured on every workload.
- `wins_per_workload`: workloads where no other tool is both faster and smaller.
- `failed_workloads`, `incomplete_workloads`: results excluded for failed samples, or for a missing metric or unit.

## Each result (results[])

- `workload`, `tool`: which result this is.
- `run_time_ratio`, `memory_ratio`: this result divided by the reference's result on the same workload.
- `run_time_ns`, `peak_rss_bytes`: the chosen Zebrac estimate in base units.
- `not_dominated`: `true` when no other tool is both faster and smaller on this workload.
- `source`: the Zebrac JSON file the result came from.

Numbers are written with 10 significant digits. A value that cannot be computed is `null`.
