#include "report.h"
#include "platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared helpers ------------------------------------------------------- */

static void ratio_ascii(char *out, size_t size, double v) {
    char n[32];
    format_number(n, sizeof(n), v);
    snprintf(out, size, "%sx", n);
}

static const char *human_time(char *out, size_t size, double ns) {
    char n[32];
    const char *unit = "ns";
    double v = ns;
    if (ns >= 1e9) { v = ns / 1e9; unit = "s"; }
    else if (ns >= 1e6) { v = ns / 1e6; unit = "ms"; }
    else if (ns >= 1e3) { v = ns / 1e3; unit = "us"; }
    format_number(n, sizeof(n), v);
    snprintf(out, size, "%s %s", n, unit);
    return out;
}

static const char *human_bytes(char *out, size_t size, double bytes) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    char n[32];
    int u = 0;
    while (bytes >= 1024 && u < 4) {
        bytes /= 1024;
        u++;
    }
    format_number(n, sizeof(n), bytes);
    snprintf(out, size, "%s %s", n, units[u]);
    return out;
}

static void md(Str *s, const char *text) {
    for (; *text; text++) {
        if (strchr("|*_`[]<\\", *text)) str_appendn(s, "\\", 1);
        if (*text == '\n' || *text == '\t') str_append(s, " ");
        else str_appendn(s, text, 1);
    }
}

static const char *cost_name(const Config *c) { return c->cost_model == COST_LINEAR ? "linear" : "geometric"; }

static void weights(const Config *c, double *wt, double *wm) {
    double total = c->time_weight + c->memory_weight;
    *wt = c->time_weight / total;
    *wm = c->memory_weight / total;
}

static int cmp_by_cost(const void *a, const void *b, const void *ctx) {
    const Figure *f = ctx;
    const Variant *x = &f->variants[*(const size_t *)a], *y = &f->variants[*(const size_t *)b];
    if (!x->n != !y->n) return x->n ? -1 : 1;
    if ((x->n == f->max_n) != (y->n == f->max_n)) return x->n == f->max_n ? -1 : 1;
    if (x->cost != y->cost) return x->cost < y->cost ? -1 : 1;
    return strcmp(x->id, y->id);
}

/* ---- report.md ---------------------------------------------------------------- */

static void summary_sentence(Str *s, const Figure *f) {
    const Variant *ref = &f->variants[f->baseline];
    long fastest = -1, smallest = -1, selected = -1;
    char a[48], b[48];
    for (size_t v = 0; v < f->variant_count; v++) {
        const Variant *var = &f->variants[v];
        if (!var->n || var->hidden) continue;
        if (fastest < 0 || var->gx < f->variants[fastest].gx) fastest = (long)v;
        if (smallest < 0 || var->gy < f->variants[smallest].gy) smallest = (long)v;
        if (var->selected) selected = (long)v;
    }
    if (fastest < 0) return;
    const Variant *fv = &f->variants[fastest], *sv = &f->variants[smallest];
    ratio_ascii(a, sizeof(a), fv->gx);
    ratio_ascii(b, sizeof(b), sv->gy);
    str_append(s, "Compared with **");
    md(s, ref->label);
    str_printf(s, "** over %zu workload%s, **", f->referenced_cases, f->referenced_cases == 1 ? "" : "s");
    md(s, fv->label);
    str_printf(s, "** is fastest (%s the run time", a);
    if (fv->n < f->referenced_cases) str_printf(s, ", measured on %zu of %zu workloads", fv->n, f->referenced_cases);
    str_append(s, ") and **");
    md(s, sv->label);
    str_printf(s, "** uses the least memory (%s", b);
    if (sv->n < f->referenced_cases) str_printf(s, ", %zu of %zu workloads", sv->n, f->referenced_cases);
    str_append(s, ").");
    if (selected >= 0) {
        str_append(s, " Lowest combined cost among tools measured on every workload: **");
        md(s, f->variants[selected].label);
        str_append(s, "**.");
    }
    str_append(s, " Not beaten on both axes: ");
    int first = 1;
    for (size_t v = 0; v < f->variant_count; v++)
        if (f->variants[v].n && f->variants[v].pareto && !f->variants[v].hidden) {
            if (!first) str_append(s, ", ");
            md(s, f->variants[v].label);
            first = 0;
        }
    str_append(s, ".\n\n");
}

static void figure_section(Str *s, const ReportContext *r, const Figure *f, const ViewInfo *vi) {
    const Config *c = r->config;
    char a[48], b[48], d[48], e[48];
    double wt, wm;
    weights(c, &wt, &wm);
    str_append(s, "## ");
    md(s, f->title);
    str_append(s, "\n\n");
    if (f->fatal) {
        str_append(s, "**Not drawn.** The input for this comparison is inconsistent:\n\n");
        for (size_t i = 0; i < f->diagnostics.count; i++) {
            str_append(s, "- ");
            md(s, f->diagnostics.items[i]);
            str_append(s, "\n");
        }
        str_append(s, "\n");
        return;
    }
    const Variant *ref = &f->variants[f->baseline];
    summary_sentence(s, f);
    for (size_t i = 0; i < f->files.count; i++) {
        const char *file = f->files.items[i];
        if (ends_with(file, ".png") || (!(c->formats & FORMAT_PNG) && ends_with(file, ".svg"))) {
            str_append(s, "![");
            md(s, f->title);
            str_printf(s, "](%s)\n\n", file);
        }
    }
    str_append(s, "*Each tool is divided by ");
    md(s, ref->label);
    str_printf(s, "%s on every workload (Zebrac %s wall time and peak RSS). The large marks are geometric means over the workloads a tool was measured on. "
                  "Bars%s show the %s of workloads, a descriptive spread and not a confidence interval. Cost is %s with time weight %g and memory weight %g. "
                  "Axes: %s run time, %s memory.*\n\n",
               f->baseline_auto ? " (chosen automatically; set `baseline` to choose)" : "", c->statistic == STAT_MEAN ? "mean" : "median",
               c->ellipse ? " and outlines" : "", c->spread == SPREAD_MINMAX ? "full range" : "middle 50%", cost_name(c), wt, wm,
               vi->x_log ? "log" : "linear", vi->y_log ? "log" : "linear");

    str_append(s, "| Tool | Workloads | Run time | Speedup | Memory | Run time spread | Memory spread | Cost | Not dominated | Wins per workload |\n");
    str_append(s, "| --- | ---: | ---: | ---: | ---: | --- | --- | ---: | :---: | ---: |\n");
    Str unmeasured = {0};
    size_t *order = xcalloc(f->variant_count + 1, sizeof(size_t));
    for (size_t v = 0; v < f->variant_count; v++) order[v] = v;
    sort_ctx(order, f->variant_count, sizeof(size_t), cmp_by_cost, f);
    for (size_t k = 0; k < f->variant_count; k++) {
        const Variant *v = &f->variants[order[k]];
        if (v->hidden) continue;
        str_append(s, "| ");
        if (v->selected) str_append(s, "**");
        md(s, v->label);
        if (v->selected) str_append(s, "**");
        if ((long)order[k] == f->baseline) str_append(s, " (reference)");
        if (!v->n) {
            str_printf(s, " | 0 of %zu | | | | | | | | |\n", f->referenced_cases);
            str_append(&unmeasured, "- ");
            md(&unmeasured, v->label);
            str_append(&unmeasured, v->failed_cases ? " had failed samples on every workload\n" : " shares no workload with the reference\n");
            continue;
        }
        ratio_ascii(a, sizeof(a), v->gx);
        ratio_ascii(b, sizeof(b), 1 / v->gx);
        ratio_ascii(d, sizeof(d), v->gy);
        str_printf(s, " | %zu of %zu | %s | %s | %s", v->n, f->referenced_cases, a, b, d);
        ratio_ascii(a, sizeof(a), v->xlo);
        ratio_ascii(b, sizeof(b), v->xhi);
        ratio_ascii(d, sizeof(d), v->ylo);
        ratio_ascii(e, sizeof(e), v->yhi);
        str_printf(s, " | %s to %s | %s to %s | %.3g | %s | %zu |\n", a, b, d, e, v->cost, v->pareto ? "yes" : "", v->case_pareto);
    }
    free(order);
    str_append(s, "\nRun time and memory are ratios to the reference; lower is better. Speedup is the reciprocal of the run-time ratio. "
                  "Wins per workload counts workloads where no other tool is both faster and smaller. "
                  "The tool in bold has the lowest cost among those measured on every workload.\n\n");

    str_printf(s, "<details><summary>Every workload (%zu workloads, %zu tools)</summary>\n\n", f->case_count, f->variant_count);
    str_append(s, "| Workload | Reference time | Reference memory |");
    for (size_t v = 0; v < f->variant_count; v++)
        if (!f->variants[v].hidden) {
            str_append(s, " ");
            md(s, f->variants[v].label);
            str_append(s, " |");
        }
    str_append(s, "\n| --- | ---: | ---: |");
    for (size_t v = 0; v < f->variant_count; v++)
        if (!f->variants[v].hidden) str_append(s, " :---: |");
    str_append(s, "\n");
    for (size_t k = 0; k < f->case_count; k++) {
        const CasePoint *refp = NULL;
        for (size_t i = 0; i < f->point_count; i++)
            if (f->points[i].kase == k) {
                refp = &f->points[i];
                break;
            }
        str_append(s, "| ");
        md(s, f->cases[k].label);
        if (refp) str_printf(s, " | %s | %s |", human_time(a, sizeof(a), refp->reference->time.estimate), human_bytes(b, sizeof(b), refp->reference->memory.estimate));
        else str_append(s, " | no reference | |");
        for (size_t v = 0; v < f->variant_count; v++) {
            if (f->variants[v].hidden) continue;
            unsigned char cell = f->cells[k * f->variant_count + v];
            const CasePoint *cp = NULL;
            for (size_t i = 0; i < f->point_count; i++)
                if (f->points[i].kase == k && f->points[i].variant == v) {
                    cp = &f->points[i];
                    break;
                }
            if (cp) {
                ratio_ascii(a, sizeof(a), cp->x);
                ratio_ascii(b, sizeof(b), cp->y);
                str_printf(s, " %s / %s%s |", a, b, cp->pareto ? " (best)" : "");
            } else {
                str_printf(s, " %s |", cell == CELL_FAILED ? "failed" : cell == CELL_INCOMPLETE ? "incomplete" : cell == CELL_NO_REF ? "no reference" : cell == CELL_OK ? "excluded" : "not run");
            }
        }
        str_append(s, "\n");
    }
    str_append(s, "\nEach cell is run time / memory relative to the reference. (best) marks a result that no other tool beats on both axes on that workload.\n\n</details>\n\n");
    if (f->diagnostics.count || unmeasured.length) {
        str_append(s, "Notes:\n\n");
        str_append(s, str_cstr(&unmeasured));
        for (size_t i = 0; i < f->diagnostics.count; i++) {
            str_append(s, "- ");
            md(s, f->diagnostics.items[i]);
            str_append(s, "\n");
        }
        str_append(s, "\n");
    }
    str_free(&unmeasured);
    if (vi->labels_dropped)
        str_printf(s, "%zu direct label(s) were left off the overview because they would overlap other marks; the legend and this table name every tool.\n\n", vi->labels_dropped);
}

int write_report_md(const ReportContext *r) {
    Str s = {0};
    const Config *c = r->config;
    double wt, wm;
    weights(c, &wt, &wm);
    str_append(&s, "# ");
    md(&s, c->report_title);
    str_printf(&s, "\n\nRead %zu Zebrac result file(s); %zu result(s) are in %zu comparison(s)", r->ingest->parsed.files_read,
               r->ingest->parsed.records.count, r->figures->count);
    if (r->ingest->excluded.count) str_printf(&s, " (%zu more from older runs or filtered groups are listed at the end)", r->ingest->excluded.count);
    str_append(&s, ". Every value comes from Zebrac summary JSON; nothing was measured again.\n\n");
    for (size_t i = 0; i < r->figures->count; i++) figure_section(&s, r, r->figures->items[i], &r->overview[i]);

    str_append(&s, "## Inputs and settings\n\n");
    str_printf(&s, "- Command: `%s`\n", r->command_line);
    str_printf(&s, "- Configuration: %s\n", c->path ? c->path : "built-in defaults");
    str_printf(&s, "- Source statistic: %s. Cost: %s, time weight %g, memory weight %g. Workloads: %s. Runs: %s.\n",
               c->statistic == STAT_MEAN ? "mean" : "median", cost_name(c), wt, wm, c->common_cases ? "only those every tool ran" : "all",
               c->all_runs ? "all" : "newest per group");
    {
        Map seen = {0};
        Str list = {0};
        for (size_t i = 0; i < r->ingest->parsed.records.count; i++) {
            const Record *rec = r->ingest->parsed.records.items[i];
            if (map_put(&seen, rec->zebrac_version, NULL)) str_printf(&list, "%s%s", list.length ? ", " : "", rec->zebrac_version);
        }
        str_printf(&s, "- Zebrac version(s): %s\n", list.length ? list.data : "none");
        map_free(&seen);
        str_free(&list);
        for (size_t i = 0; i < r->ingest->parsed.records.count; i++) {
            const Record *rec = r->ingest->parsed.records.items[i];
            char *key = xasprintf("%.0f ms per command, at least %.0f samples, %.0f warmup runs", rec->duration_ms, rec->min_samples, rec->warmup);
            if (map_put(&seen, key, NULL)) str_printf(&list, "%s%s", list.length ? "; " : "", key);
            free(key);
        }
        str_printf(&s, "- Zebrac settings: %s\n", list.length ? list.data : "unknown");
        map_free(&seen);
        str_free(&list);
    }
    str_append(&s, "- Identity rules used:");
    for (size_t i = 0; i < r->ingest->rule_counts.capacity; i++)
        if (r->ingest->rule_counts.keys && r->ingest->rule_counts.keys[i])
            str_printf(&s, " %s (%zu)", r->ingest->rule_counts.keys[i], *(size_t *)r->ingest->rule_counts.values[i]);
    str_append(&s, "\n");
    if (r->ingest->parsed.files_skipped) str_printf(&s, "- JSON files that are not Zebrac results: %zu\n", r->ingest->parsed.files_skipped);
    if (r->ingest->skipped_runs.count) {
        str_append(&s, "- Older runs not reported (set `runs = \"all\"` to include them):\n");
        for (size_t i = 0; i < r->ingest->skipped_runs.count; i++) {
            str_append(&s, "  - ");
            md(&s, r->ingest->skipped_runs.items[i]);
            str_append(&s, "\n");
        }
    }
    for (size_t i = 0; i < r->ingest->warnings.count; i++) {
        str_append(&s, "- Warning: ");
        md(&s, r->ingest->warnings.items[i]);
        str_append(&s, "\n");
    }
    str_printf(&s, "- Made by isocost %s in %.0f ms (read %.1f ms, analysis %.1f ms, figures %.1f ms).\n", ISOCOST_VERSION,
               r->read_ms + r->analysis_ms + r->render_ms, r->read_ms, r->analysis_ms, r->render_ms);

    char *path = path_join(r->out_dir, "report.md");
    int ok = pf_write_file_atomic(path, s.data, s.length);
    free(path);
    str_free(&s);
    return ok;
}

/* ---- summary.json ------------------------------------------------------------ */

static void json_string(Str *s, const char *text) {
    str_append(s, "\"");
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == '"' || ch == '\\') str_printf(s, "\\%c", ch);
        else if (ch == '\n') str_append(s, "\\n");
        else if (ch == '\t') str_append(s, "\\t");
        else if (ch < 0x20) str_printf(s, "\\u%04x", ch);
        else str_appendn(s, text, 1);
    }
    str_append(s, "\"");
}

static void json_number(Str *s, double v) {
    if (!isfinite(v)) str_append(s, "null");
    else str_printf(s, "%.10g", v);
}

static void json_summary(const ReportContext *r, Str *s) {
    const Config *c = r->config;
    double wt, wm;
    weights(c, &wt, &wm);
    str_append(s, "{\n  \"schema\": \"isocost.report.v1\",\n  \"isocost_version\": \"" ISOCOST_VERSION "\",\n  \"config\": ");
    if (c->path) json_string(s, c->path);
    else str_append(s, "null");
    str_printf(s, ",\n  \"exit_status\": %d,\n  \"settings\": {\"statistic\": \"%s\", \"cost\": \"%s\", \"time_weight\": ", r->exit_status,
               c->statistic == STAT_MEAN ? "mean" : "median", cost_name(c));
    json_number(s, wt);
    str_append(s, ", \"memory_weight\": ");
    json_number(s, wm);
    str_printf(s, ", \"cases\": \"%s\", \"runs\": \"%s\", \"data\": %s},\n", c->common_cases ? "common" : "all", c->all_runs ? "all" : "latest",
               c->data ? "true" : "false");
    str_printf(s, "  \"inputs\": {\"files\": %zu, \"results\": %zu, \"excluded\": %zu, \"not_zebrac\": %zu, \"identity_rules\": {",
               r->ingest->parsed.files_read, r->ingest->parsed.records.count, r->ingest->excluded.count, r->ingest->parsed.files_skipped);
    int first = 1;
    for (size_t i = 0; i < r->ingest->rule_counts.capacity; i++)
        if (r->ingest->rule_counts.keys && r->ingest->rule_counts.keys[i]) {
            if (!first) str_append(s, ", ");
            json_string(s, r->ingest->rule_counts.keys[i]);
            str_printf(s, ": %zu", *(size_t *)r->ingest->rule_counts.values[i]);
            first = 0;
        }
    str_append(s, "}, \"skipped_runs\": [");
    for (size_t i = 0; i < r->ingest->skipped_runs.count; i++) {
        if (i) str_append(s, ", ");
        json_string(s, r->ingest->skipped_runs.items[i]);
    }
    str_append(s, "]},\n  \"figures\": [");
    for (size_t fi = 0; fi < r->figures->count; fi++) {
        const Figure *f = r->figures->items[fi];
        str_append(s, fi ? ",\n    {" : "\n    {");
        str_append(s, "\"id\": ");
        json_string(s, f->slug);
        str_append(s, ", \"title\": ");
        json_string(s, f->title);
        str_append(s, ", \"project\": ");
        json_string(s, f->project);
        str_append(s, ", \"suite\": ");
        json_string(s, f->suite);
        str_append(s, ", \"group\": ");
        json_string(s, f->group);
        str_append(s, ", \"run\": ");
        json_string(s, f->run);
        str_printf(s, ", \"status\": \"%s\", \"reference\": ", f->fatal ? "not_drawn" : "drawn");
        if (f->baseline >= 0) json_string(s, f->variants[f->baseline].id);
        else str_append(s, "null");
        str_printf(s, ", \"reference_automatic\": %s, \"workloads\": %zu, \"files\": [", f->baseline_auto ? "true" : "false", f->referenced_cases);
        for (size_t i = 0; i < f->files.count; i++) {
            if (i) str_append(s, ", ");
            json_string(s, f->files.items[i]);
        }
        str_append(s, "], \"diagnostics\": [");
        for (size_t i = 0; i < f->diagnostics.count; i++) {
            if (i) str_append(s, ", ");
            json_string(s, f->diagnostics.items[i]);
        }
        str_append(s, "],\n     \"tools\": [");
        int first_tool = 1;
        for (size_t v = 0; v < f->variant_count && !f->fatal; v++) {
            const Variant *var = &f->variants[v];
            str_append(s, first_tool ? "\n       {" : ",\n       {");
            first_tool = 0;
            str_append(s, "\"id\": ");
            json_string(s, var->id);
            str_append(s, ", \"label\": ");
            json_string(s, var->label);
            str_printf(s, ", \"hidden\": %s, \"reference\": %s, \"workloads\": %zu", var->hidden ? "true" : "false",
                       (long)v == f->baseline ? "true" : "false", var->n);
            if (var->n) {
                str_append(s, ", \"run_time_ratio\": ");
                json_number(s, var->gx);
                str_append(s, ", \"speedup\": ");
                json_number(s, 1 / var->gx);
                str_append(s, ", \"memory_ratio\": ");
                json_number(s, var->gy);
                str_append(s, ", \"run_time_spread\": [");
                json_number(s, var->xlo);
                str_append(s, ", ");
                json_number(s, var->xhi);
                str_append(s, "], \"memory_spread\": [");
                json_number(s, var->ylo);
                str_append(s, ", ");
                json_number(s, var->yhi);
                str_append(s, "], \"cost\": ");
                json_number(s, var->cost);
            }
            str_printf(s, ", \"not_dominated\": %s, \"selected\": %s, \"wins_per_workload\": %zu, \"failed_workloads\": %zu, \"incomplete_workloads\": %zu}",
                       var->pareto ? "true" : "false", var->selected ? "true" : "false", var->case_pareto, var->failed_cases, var->incomplete_cases);
        }
        str_append(s, "],\n     \"results\": [");
        for (size_t i = 0; i < f->point_count && !f->fatal; i++) {
            const CasePoint *p = &f->points[i];
            str_append(s, i ? ",\n       {" : "\n       {");
            str_append(s, "\"workload\": ");
            json_string(s, f->cases[p->kase].id);
            str_append(s, ", \"tool\": ");
            json_string(s, f->variants[p->variant].id);
            str_append(s, ", \"run_time_ratio\": ");
            json_number(s, p->x);
            str_append(s, ", \"memory_ratio\": ");
            json_number(s, p->y);
            str_append(s, ", \"run_time_ns\": ");
            json_number(s, p->record->time.estimate);
            str_append(s, ", \"peak_rss_bytes\": ");
            json_number(s, p->record->memory.estimate);
            str_printf(s, ", \"not_dominated\": %s, \"source\": ", p->pareto ? "true" : "false");
            json_string(s, p->record->path);
            str_append(s, "}");
        }
        str_append(s, "]}");
    }
    str_append(s, "\n  ]\n}\n");
}

int write_summary_json(const ReportContext *r) {
    Str s = {0};
    json_summary(r, &s);
    char *path = path_join(r->out_dir, "summary.json");
    int ok = pf_write_file_atomic(path, s.data, s.length);
    free(path);
    str_free(&s);
    return ok;
}

void print_summary_json(const ReportContext *r, FILE *out) {
    Str s = {0};
    json_summary(r, &s);
    fputs(str_cstr(&s), out);
    str_free(&s);
}

/* ---- points.tsv --------------------------------------------------------------- */

int write_points_tsv(const ReportContext *r) {
    Str s = {0};
    str_append(&s, "figure\tworkload\ttool\treference\trun_time_ratio\tmemory_ratio\trun_time_ns\tpeak_rss_bytes\treference_run_time_ns\treference_peak_rss_bytes\tcost\tnot_dominated\tsource\n");
    for (size_t fi = 0; fi < r->figures->count; fi++) {
        const Figure *f = r->figures->items[fi];
        if (f->fatal) continue;
        const char *ref = f->variants[f->baseline].id;
        for (size_t k = 0; k < f->point_count; k++) {
            const CasePoint *p = &f->points[k];
            str_printf(&s, "%s\t%s\t%s\t%s\t%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%d\t%s\n", f->slug, f->cases[p->kase].id,
                       f->variants[p->variant].id, ref, p->x, p->y, p->record->time.estimate, p->record->memory.estimate,
                       p->reference->time.estimate, p->reference->memory.estimate, p->cost, p->pareto, p->record->path);
        }
        for (size_t v = 0; v < f->variant_count; v++) {
            const Variant *var = &f->variants[v];
            if (!var->n) continue;
            str_printf(&s, "%s\t*\t%s\t%s\t%.10g\t%.10g\t\t\t\t\t%.10g\t%d\taggregate of %zu workloads\n", f->slug, var->id, ref, var->gx, var->gy,
                       var->cost, var->pareto, var->n);
        }
    }
    char *path = path_join(r->out_dir, "points.tsv");
    int ok = pf_write_file_atomic(path, s.data, s.length);
    free(path);
    str_free(&s);
    return ok;
}
