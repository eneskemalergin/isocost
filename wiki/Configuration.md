# Configuration

`isocost.toml` is optional. Every key has a default, and `isocost config` prints all of them with a short comment each. Command-line options override the file, and the file overrides the defaults.

isocost looks for `isocost.toml` in the first input directory and then in its parents. It stops at the directory that holds `.git`, or after 4 levels. It prints the file it used on stderr and records it in `report.md` and `summary.json`. Use `--config FILE` to choose a file, or `--no-config` to use only the defaults.

Check a file without drawing:

```bash
isocost config --check isocost.toml           # "valid", or the first error
isocost config --check isocost.toml --print   # every value after the file is applied
```

An error names the file, line, and column, and suggests the closest valid key or value:

```text
isocost: isocost.toml:2:9: unknown key 'titel' in [figure]; did you mean 'title'?
```

The file is TOML 1.0 without multi-line strings, dates, or dotted keys in assignments. Write `[overview]` and then `x_scale = "log"`, not `overview.x_scale = "log"`.

## [report]

```toml
[report]
title = "Speed and memory report"  # first heading of report.md
baseline = ""                      # reference tool id or family; "" chooses one
statistic = "median"               # median or mean
cost = "geometric"                 # geometric or linear
time_weight = 1
memory_weight = 1
cases = "all"                      # all or common
runs = "latest"                    # latest or all
groups = ["*"]                     # globs
suites = ["*"]                     # globs
views = ["overview", "workloads"]
formats = ["png"]                  # png, svg, pdf
data = false                       # also write points.tsv
```

- With `baseline = ""`, isocost picks the tool measured on the most workloads and prefers the project's own tool. `report.md` says when the choice was automatic.
- `statistic` picks the Zebrac estimate for each result. When a median is missing, the mean is used and marked.
- `cost = "geometric"` draws curves of equal $x^{w_t} y^{w_m}$; `linear` draws lines of equal $w_t x + w_m y$. The weights are divided by their sum, so `1` and `1` mean half and half. They cannot both be 0.
- `cases = "common"` keeps only the workloads that every shown tool ran, so every aggregate covers the same workloads.
- `runs = "latest"` draws the newest run of each group. `all` draws every run.
- A comparison is drawn only when its group matches one of `groups` and its suite matches one of `suites`.

## [figure]

```toml
[figure]
size = "screen"       # screen, paper, paper-wide, slide, or WxH with px, mm, or in
dpi = 0               # 0 = the size's own value
font_scale = 1
header = "auto"       # auto, show, or hide
title = "{project} {suite} {group}"
subtitle = "auto"     # auto, "" for none, or a template
legend = "top"        # top, bottom, right, or none
note = "auto"         # auto, "" for none, or your own text
```

The size presets:

| Preset | Canvas | DPI | Base text |
| --- | --- | ---: | --- |
| `screen` | 1600 x 1000 px | 96 | 10.5 pt |
| `paper` | 89 x 72 mm | 300 | 7 pt |
| `paper-wide` | 183 x 100 mm | 300 | 7 pt |
| `slide` | 13.33 x 7.5 in | 144 | 14 pt |

- `paper` fits one journal column and `paper-wide` two.
- A custom size in `mm` or `in` uses 300 dpi and 7 pt text. A custom size in `px` uses 96 dpi and 10.5 pt text. For a `px` size, `dpi` scales text and marks but keeps the pixel count.
- Every stroke, marker, and gap is a multiple of the base text size, so a figure keeps its proportions at any size.
- On print sizes (`paper`, `paper-wide`, and custom `mm` or `in` sizes), `header = "auto"` and `note = "auto"` leave the title, subtitle, and note out, because journals put them in the caption. Markers are drawn smaller than on screen.
- The workloads view keeps the preset width and grows taller with the number of workloads. A custom size is kept exactly.
- `title` and `subtitle` can use `{project}`, `{suite}`, `{group}`, `{run}`, `{reference}`, and `{workloads}`. A separator such as `/` or `-` next to an empty value is dropped.
- `subtitle = "auto"` names the reference, the number of workloads, and the run.

## [overview]

```toml
[overview]
x_label = "Run time relative to {reference}"
y_label = "Peak memory relative to {reference}"
x_scale = "auto"       # auto, log, or linear
y_scale = "auto"
x_min = 0              # 0 = automatic, for all four limits
x_max = 0
y_min = 0
y_max = 0
x_ticks = []           # [] = automatic
y_ticks = []
contours = true
contour_levels = []    # [] = automatic
case_points = true
spread = "iqr"         # iqr, minmax, or none
ellipse = true
pareto = true
highlight = true
reference_lines = true
labels = "auto"        # auto or none
```

- `auto` scales switch to log when the data spans more than 6x.
- Limits, ticks, and contour levels are ratios to the reference, such as `0.5` or `2`.
- `case_points` draws one faint mark per workload. `spread` draws bars over the middle 50% of workloads (`iqr`) or over all of them (`minmax`).
- `ellipse` adds an outline around each tool measured on 5 or more workloads.
- `pareto` draws a line through the tools that no other tool beats on both axes. `highlight` tints the region that is faster and smaller than the reference.
- `labels = "auto"` names each tool next to its mark where the name fits without covering another mark. A name that does not fit is left to the legend.

## [workloads]

```toml
[workloads]
metrics = ["time", "memory"]  # one panel per metric
sort = "name"                 # name, time, memory, or config
scale = "log"                 # log or linear
labels = true
```

- `sort = "name"` compares numbers inside names as numbers, so `5mb` comes before `10mb`. `time` and `memory` sort by the reference's own value, smallest first. `config` uses `[[workload]] order`.
- A workload name wider than 30% of the figure is shortened with `...`. The SVG keeps the full name as a tooltip. `[[workload]] label` sets a shorter name.

## [theme]

```toml
[theme]
background = "#fcfcfb"     # background and text halos
ink = "#0b0b0b"            # titles, labels, the Pareto line, the selection ring
ink_secondary = "#52514e"  # axis titles and ticks
muted = "#898781"          # notes and equal-cost labels
grid = "#e1e0d9"
reference = "#a9a79f"      # lines through 1x
contour = "#b9b7ae"        # equal-cost curves
region = "#f1f0ea"         # faster-and-smaller tint
palette = ["#2a78d6", "#eb6834", "#1baf7a", "#763a64", "#b191ea", "#ce35a8", "#876114", "#5922bf"]
```

Tools without a `[[tool]] color` take palette colors in turn. The default eight colors stay distinguishable for every pair, including under the common color-vision deficiencies. `palette` takes 1 to 16 colors. When a report has more tools than colors, the shapes repeat as hollow outlines. Every tool also has a legend entry and a table row, so a reader never has to match colors alone.

## [[tool]]

Each entry styles one tool. With `match`, it also names results.

```toml
[[tool]]
id = "zstd -3"           # required and unique
match = "zstd -3 *"      # glob on the command
label = "zstd level 3"   # shown in figures and tables; default is id
color = "#2a78d6"
shape = "diamond"        # circle, square, triangle, diamond, triangle-down, plus, star, cross
fill = "solid"           # solid or hollow
line = "dashed"          # solid, dashed, or dotted, for this tool's spread marks
order = 1                # legend and table position; 0 = after the ordered tools
hide = false
label_position = "auto"  # auto, left, right, above, below, or none
```

- isocost tests `match` against the command with the program path reduced to the program name: `/usr/bin/zstd -3 -c f` becomes `zstd -3 -c f`. `*` matches any text and `?` one character.
- The first matching entry wins, so put specific patterns before general ones.
- A matched result's workload is its file name without `.json`. Its group is the name of the directory the file is in.
- The program name is the tool's family, so `gzip -6` and `gzip -d` keep one color across figures. When two tools in one figure share a program, as `rg` and `rg -j1` do, each gets its own color and shape.
- An entry without `match` only styles a tool that another rule named. isocost compares `id` with the tool's id first and then with its family.
- `hide = true` removes the tool from figures and tables. A hidden tool can still be the reference.

## [[workload]]

```toml
[[workload]]
id = "reads"             # required
label = "FASTQ reads"
order = 3                # used by sort = "config" and by tables
hide = false             # true drops this workload everywhere
```

## [[group]]

Settings for the comparisons whose group matches `match`. The first matching entry wins.

```toml
[[group]]
match = "decompress"     # required glob
title = "Decompression"
subtitle = ""
baseline = "gzip -d"
hide = false             # true skips these comparisons
```

## Command-line options

Each of these options overrides one key. `isocost --help` lists them all.

- `--baseline NAME`: `report.baseline`
- `--group GLOB`, `--suite GLOB`: `report.groups`, `report.suites` (one pattern)
- `--runs latest|all`, `--cases all|common`: `report.runs`, `report.cases`
- `--statistic median|mean`, `--cost geometric|linear`: `report.statistic`, `report.cost`
- `--weights T,M`: `report.time_weight`, `report.memory_weight`
- `--view LIST`, `--format LIST`, `--data`: `report.views`, `report.formats`, `report.data`
- `--size NAME|WxHUNIT`, `--dpi N`, `--title TEXT`: `figure.size`, `figure.dpi`, `figure.title`

The remaining options have no config key:

- `-o DIR`: the output directory, required for `report`. Missing parent directories are created.
- `--force`: replace a report already in `DIR`. Figures that the previous `summary.json` lists but the new report does not write are deleted, and so is an old `points.tsv` when the new report writes none. Other files in `DIR` are left alone.
- `-j N`: threads for reading and drawing. The default is the CPU count, at most 8, each using about 6 MB at screen size.
- `-q`: no summary line on stderr.
- `--json`: with `list`, print `summary.json` to stdout.
