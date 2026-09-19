# isocost

`isocost` is a Bash, GNU awk, and GNUplot analyzer for speed-versus-memory tradeoffs. It consumes benchmark summaries from tools such as [`zebrac`](https://github.com/eneskemalergin/zebrac), calculates dimensionless coordinates, evaluates explicit cost functions, and writes deterministic tables and publication-oriented figures.

The implementation deliberately has no Python, JavaScript, `jq`, or plotting framework dependency.

See the [demo report](examples/demo-report/README.md) for the canonical high-level overview and its reproducible tables. The default uses geometric curved contours; use `--cost-model linear` when additive contours are needed, and use `--plot-layout facet` only for per-case diagnostics.

## Quick start

Check the local toolchain:

```bash
bin/isocost doctor
```

Build the checked-in realistic demo report:

```bash
examples/demo-report/build.sh
```

The resulting report is in `examples/demo-report/generated/overview/`. It contains one SVG figure, analysis tables, a manifest, diagnostics when incomplete records exist, and one standalone GNUplot source file. The generated directory is ignored by Git. The demo uses `z-flate` as its explicit per-case reference and keeps linear axes so geometric contours remain curved.

Build a report directly from the demo source:

```bash
bin/isocost report \
  --input demo \
  --input-format demo-summary \
  --normalize baseline \
  --baseline z-flate \
  --formats svg \
  --output-dir examples/demo-report/generated/overview \
  --force
```

The default report is one overview panel with curved geometric cost contours, selected labels, and middle-50% spread bars plus a spread ellipse. Use `--plot-layout facet` only when you need per-case diagnostics.

> [!WARNING]
> The fixed-width demo has unknown timing units and separate RSS summary protocols. The importer preserves those limitations; it does not invent paired timing/RSS repetitions.

Build the small canonical fixture:

```bash
bin/isocost report \
  --input examples/basic-summary.tsv \
  --input-format metric-summary-tsv \
  --output-dir /tmp/isocost-basic-report \
  --normalize baseline \
  --baseline z-flate \
  --time-weight 0.6 \
  --memory-weight 0.4 \
  --formats svg \
  --force
```

## Zebrac workflow

Zebrac produces summary JSON. Preserve that JSON and import it directly:

```bash
zebrac --quiet --allow-failures \
  --min-samples 10 --max-samples 10 \
  --json run.json -- \
  "./z-flate input.fastq" \
  "./isa-l input.fastq"

bin/isocost report \
  --input run.json \
  --input-format zebrac-json \
  --case-id medium-fastq \
  --output-dir /tmp/isocost-medium-fastq-report \
  --normalize baseline \
  --baseline z-flate \
  --labels selected \
  --formats svg \
  --force
```

The adapter keeps Zebrac's mean, standard deviation, quartiles, median, outlier count, sample count, failed-sample count, units, and source JSON. Summary JSON is not expanded into fake raw observations. Quartiles and standard deviations are descriptive spread, not confidence intervals.

For one Zebrac JSON per workload and method, name the files `CASE--VARIANT.json` and import the directory in one pass:

```bash
bin/isocost report \
  --input /tmp/zebrac-results \
  --input-format zebrac-json-dir \
  --output-dir /tmp/isocost-zebrac-report \
  --normalize baseline \
  --baseline z-flate \
  --formats svg \
  --force
```

The filename supplies `case_id` and `variant_id`; each JSON must contain exactly one Zebrac result. The directory importer sorts the files and invokes one GNU awk process, so it avoids a shell loop and keeps the generated JSON files outside the report directory.

## A/A repeatability check

Use `isocost aa` when the same measured work was captured twice. The command reads the two Zebrac summaries named by `input-manifest.tsv`, calculates run 2 divided by run 1, and returns one deterministic status. It writes no files unless the caller redirects stdout.

```bash
bin/isocost aa \
  --input /path/to/aa-input \
  --format summary

bin/isocost aa \
  --input /path/to/aa-input \
  --format json > /tmp/aa-result.json
```

The shell status is `0` for `pass`, `1` for `review`, and `2` for `invalid`. Use the JSON output for automation. Build the human report in a caller-selected temporary directory:

```bash
bin/isocost aa-report \
  --input /path/to/aa-input \
  --output-dir /tmp/isocost-aa-report \
  --force
```

The report writes `report.md`, `table.tsv`, and `aa-map.png`. It keeps the evaluator as the only source of ratios, guardrails, and decisions. See the [A/A comparison guide](docs/aa.md) and the [runnable A/A example](examples/aa/README.md) for the manifest columns, JSON fields, thresholds, and report meanings.

Build a short-lived input matrix when developing the JSON reader:

```bash
bash examples/zebrac-inputs/build.sh
bash examples/zebrac-inputs/build.sh --keep /tmp/isocost-zebrac-inputs
```

The matrix contains one canonical multi-result file, 48 canonical one-result files made from the demo, a compact file with reordered keys and escaped command text, a partial failed result, and one unsupported-schema file. The first command removes every generated JSON before it exits. The second keeps them in the directory you name for inspection or parser tests.

## Commands

```text
isocost doctor
isocost import --format FORMAT --input FILE|DIR --output FILE
isocost validate --input FILE [--input-format FORMAT]
isocost report --input FILE|DIR --output-dir DIR [options]
isocost plot --analysis-dir DIR [options]
isocost aa --input DIR [options]
isocost aa-report --input DIR --output-dir DIR [options]
```

Supported input formats:

- `metric-summary-tsv`: canonical long-form summaries.
- `raw-observations-tsv`: one row per measurement; GNU awk aggregates it.
- `demo-summary`: the repository's fixed-width demo.
- `zebrac-json`: the documented Zebrac summary JSON shape.
- `zebrac-json-dir`: one-result Zebrac JSON files named `CASE--VARIANT.json`.

Useful report and plot options:

```text
--normalize best|baseline|none
--baseline VARIANT_ID
--time-weight W --memory-weight W
--cost-model linear|geometric (default: geometric)
--selection all|feasible|pareto|pareto-feasible|best
--max-time R --max-memory R --min-speedup R --min-reps N
--pareto-rel-eps R
--error-bars none|iqr|sd|sem|minmax
--error-shape bars|ellipse|both (default: both)
--labels none|selected|frontier|all|values (default: selected)
--case-points all|selected|none
--palette zig|colorblind|vivid|solarized|tol
--colors RRGGBB,RRGGBB,...
--size WIDTHxHEIGHT
--overview-stat auto|mean|median|geometric
--case-spread none|iqr|sd|sem|minmax
--plot-layout overview|facet (default: overview)
--facet-columns N
--cost-levels auto|K1,K2,...
--formats svg,png,pdf
--x-log --y-log
```

`--normalize baseline` is the recommended report mode. A point at `(1, 1)` then means the selected baseline's time and memory for that case. `--normalize best` uses independent per-case minima and can therefore refer to two different variants. `--normalize none` keeps raw axes and does not provide dimensionless cost contours.

For normalized ratios, the default geometric cost treats reciprocal time and RSS changes consistently. Use `--cost-model linear` when the arithmetic mean of the two ratios is the intended decision rule.

Weights are normalized internally:

\[
\tilde w_t = \frac{w_t}{w_t + w_m},
\qquad
\tilde w_m = \frac{w_m}{w_t + w_m}.
\]

Time and memory budgets are normalized ratios. For example, `--max-memory 1` requires no more memory than the selected reference.

## Output

A report directory contains the analysis tables and the generated figure:

```text
summary.tsv               one row per case and variant
points.tsv                chart coordinates, spread, feasibility, cost, and selection
diagnostics.tsv           incomplete, invalid, and constraint diagnostics (when present)
manifest.tsv              command, configuration, and analysis metadata
report.gnuplot            standalone GNUplot script
report.svg/png/pdf        requested visual output formats
```

`points.tsv` is the machine-readable chart input. Columns `x` and `y` are run-time and memory-use coordinates. Error bars are descriptive intervals selected by `--error-bars`; they are not confidence intervals. The `selected` column records the requested selection, and the `pareto` column records per-case nondominance. Facet panels share explicit limits. The legend lists methods through color and marker shape; a header note explains the case points, selection rings, best-tradeoff line, equal-cost curves, and spread shape. Spread, references, Pareto lines, and selection rings use separate slate and black structural styles. Overview figures draw faint per-case points in the same variant color, and labels include the number of complete cases. `--case-points selected` keeps only selected case points, while `--case-points none` removes the case layer. `--labels values` adds the aggregate speedup and memory ratio to overview labels. The dark line is the best-tradeoff path, the ring marks selected points, and normalized figures mark `(1,1)` with crosshairs.

The default `zig` palette uses ink, Zig amber, steel, and teal, with marker shapes as a second identity cue. It keeps a dark neutral base while making neighboring methods distinct. Use `--palette colorblind`, `--palette vivid`, `--palette solarized`, or `--palette tol` for other built-in sequences. `--colors` accepts an exact comma-separated sequence of six-digit RGB values and takes precedence over the named palette. `--size` changes the output canvas without changing the report tables.

The generated contours are equations in `report.gnuplot`, clipped by the plot rectangle. Linear contours satisfy \( \tilde w_t x + \tilde w_m y = \kappa \). Geometric contours satisfy \( x^{\tilde w_t} y^{\tilde w_m} = \kappa \). No contour coordinate table is written.

## Model limitations

Spread bars and ellipses are descriptive encodings. The overview ellipse uses paired complete-case coordinates and their correlation. A facet ellipse uses the source summary bounds with no rotation because Zebrac summary JSON has no paired covariance. Neither form is a confidence interval. Use error-shape ellipse for the ellipse alone, or error-shape both to retain bars and add the ellipse.

Use an explicit baseline for reproducible reports. Per-case-best normalization is convenient for quick glances, but its time and memory references can be different variants. A Pareto frontier is descriptive and does not prove statistical significance.

Unknown units are safe for within-case ratios only if the source values are commensurate. The supplied demo's timing unit is unknown, and its RSS values are sampled maxima from a separate protocol. They should not be presented as paired measurements.

Counters such as instructions and cache misses remain in canonical input, but they are not silently folded into the two-dimensional time/RSS objective. See [`docs/math.md`](docs/math.md), [`docs/input-format.md`](docs/input-format.md), and [`docs/plots.md`](docs/plots.md) for the model, schema, and figure contract.
