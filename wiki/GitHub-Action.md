# GitHub Action

The repository is also a GitHub Action. It downloads a static isocost release, writes a report from Zebrac results, adds the report text to the job summary, and uploads the report directory as a workflow artifact. It runs on Linux runners, x86-64 or ARM64.

```yaml
jobs:
  bench:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v7
      # ... install Zebrac and the tools, then write results/ with zebrac --json
      - uses: eneskemalergin/isocost@v0.1.0
        with:
          results: results/
          args: --format png,svg --size screen
```

Pin a release tag, or a full commit SHA for the strictest supply-chain policy. When the action is used at a release tag, it installs that same release; at any other ref it installs the newest release unless `version` is set.

## Inputs

| Input | Default | Meaning |
| --- | --- | --- |
| `results` | required | Zebrac JSON files, directories, or `bench.meta.v1` sidecars, separated by spaces or newlines |
| `output` | `isocost-report` | report directory; replaced if it already holds a report |
| `args` | empty | extra `isocost report` options, such as `--baseline "gzip -6" --format pdf` |
| `version` | the action's tag, else latest | isocost release to install, such as `0.1.0` |
| `summary` | `true` | add `report.md`, without its images, to the job summary |
| `upload` | `true` | upload the report directory as a workflow artifact |
| `artifact-name` | `isocost-report` | name of that artifact |

## Outputs

| Output | Meaning |
| --- | --- |
| `report-dir` | the report directory |
| `version` | the isocost version that ran |
| `exit-status` | isocost's exit status: `0` report written, `3` some comparisons not drawn (see [Inputs](Inputs#exit-status)) |

The step fails when isocost exits with status 2 or 3. The report is still summarized and uploaded when some comparisons could not be drawn. Add `continue-on-error: true` to keep the job green in that case.
