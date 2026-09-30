# Reading the figures

Every comparison has two figures. Both put the reference tool at 1x and use the same color and shape for a tool across every figure in a report.

## Overview

![Overview of seven compressors](https://raw.githubusercontent.com/eneskemalergin/isocost/main/assets/compression-overview.png)

- **Large marks**: each tool's geometric mean over the workloads it was measured on. The legend shows `n=` when tools were measured on different numbers of workloads. An aggregate over fewer workloads is not directly comparable, and `report.md` says so.
- **Faint marks**: single workloads.
- **Bars**: the middle 50% of workloads (or the full range with `spread = "minmax"`).
- **Outline**: for tools with 5 or more workloads, an ellipse from the same spread, rotated by how run time and memory move together across workloads.
- **Dark line**: connects the tools no other tool beats on both axes.
- **Ring**: the lowest-cost tool among those measured on every workload.
- **Dashed curves**: equal cost, labeled `C=`.
- **Tinted corner**: faster and smaller than the reference.
- **Axes**: log when the data spans more than 6x, so equal factors take equal space.

Bars and outlines describe how a tool varies across workloads. They are not confidence intervals: Zebrac writes summaries per result, not paired raw samples, so a sampling interval for a ratio cannot be recovered.

## Workloads

![Workloads view of seven compressors](https://raw.githubusercontent.com/eneskemalergin/isocost/main/assets/compression-workloads.png)

One row per workload and one panel per metric. Each mark is one tool's value divided by the reference's value on that workload, and the vertical line at 1x is the reference. Use it to see whether a good average hides a workload where a tool loses.

## The math

For workload $c$, tool $v$, and reference $r$, with $t$ the chosen Zebrac estimate of wall time and $m$ of peak RSS:

$$x_{cv} = \frac{t_{cv}}{t_{cr}}, \qquad y_{cv} = \frac{m_{cv}}{m_{cr}}$$

The speedup is $1/x$. A tool's aggregate over its $n_v$ workloads is the geometric mean, which treats a twofold slowdown and a twofold speedup as equal and opposite:

$$\bar{x}_v = \exp\left(\frac{1}{n_v} \sum_c \log x_{cv}\right)$$

Spread bars use type-7 quartiles of the workload ratios. The outline uses half the interquartile range of $\log x$ and $\log y$ as scales $s_x$, $s_y$ and the Pearson correlation $\rho$ of the log ratios:

$$S = \begin{pmatrix} s_x^2 & \rho s_x s_y \\ \rho s_x s_y & s_y^2 \end{pmatrix}$$

Its axes are the square roots of the eigenvalues of $S$, rotated along the first eigenvector, drawn in log space and mapped to the axes.

Weights $a_t$ and $a_m$ are normalized to $w_t = a_t / (a_t + a_m)$ and $w_m = 1 - w_t$. The geometric cost (default) and linear cost are:

$$C_{\mathrm{geo}} = x^{w_t} y^{w_m}, \qquad C_{\mathrm{lin}} = w_t x + w_m y$$

A tool $u$ dominates $v$ when $x_u \le x_v$ and $y_u \le y_v$ with at least one strict inequality. The dark line connects the aggregates that no other aggregate dominates. The "Wins per workload" column in `report.md` counts workloads where a tool is not dominated.

Automatic equal-cost levels come from the cost at the four corners of the plot, rounded to readable values (1, 1.5, 2, 3, 4, and their powers). Automatic reference choice picks the tool measured on the most workloads, preferring the project's own tool; `report.md` marks the choice as automatic.

## What the figures can and cannot claim

They can show that a tool's chosen estimate is lower or higher than the reference, that a tool is or is not beaten on both axes, and how a tool varies across the supplied workloads. They cannot show statistical significance, a sampling confidence region, a causal explanation, or where an unmeasured configuration would land.

## Publishing

- Use `size = "paper"` (one column) or `"paper-wide"` (two columns) with `formats = ["pdf"]` for journals. Print sizes leave the title and note out; `report.md` has a caption for the manuscript.
- PDF and SVG contain only vector paths, with glyph outlines embedded. They need no fonts and look the same in every viewer.
- PNG files carry their DPI, so they import at their physical size.
- Keep `summary.json` or `points.tsv` beside a published figure so every plotted number can be traced to its Zebrac file.
