/*
 * Figures, normalization, and reductions.
 *
 *   x = time / reference time, y = RSS / reference RSS, per workload
 *   aggregate = geometric mean over the tool's normalized workloads
 *   spread = type-7 quartiles (or min and max) of the workload ratios
 *   cost = x^wt * y^wm (geometric) or wt*x + wm*y (linear), weights normalized
 *   ellipse = log-ratio space, half the log IQR per axis, rotated by Pearson r
 *
 * These match the reviewed awk reference: 61 of 61 real z-fastq points agreed
 * within a relative difference of 4.5e-6.
 */
#include "model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

double cost_of(const Config *c, double x, double y) {
    double total = c->time_weight + c->memory_weight;
    double wt = total > 0 ? c->time_weight / total : 0.5;
    double wm = total > 0 ? c->memory_weight / total : 0.5;
    if (c->cost_model == COST_LINEAR) return wt * x + wm * y;
    return pow(x, wt) * pow(y, wm);
}

static void diag(Figure *f, char *message) { vec_push(&f->diagnostics, message); }

static int dominates(double ax, double ay, double bx, double by) { return ax <= bx && ay <= by && (ax < bx || ay < by); }

static long index_of(char *const *ids, size_t count, const char *id) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(ids[i], id) == 0) return (long)i;
    return -1;
}

/* How strongly a tool id looks like the project that produced the results. */
static int project_match(const Figure *f, const char *id) {
    size_t n = strlen(id), pn = strlen(f->project);
    if (pn && strcmp(f->project, id) == 0) return 2;
    if (pn && n > pn && strncmp(f->project, id, pn) == 0 && id[pn] == '-') return 1;
    for (size_t i = 0; i < f->records.count && i < 64; i++) {
        const char *path = ((Record *)f->records.items[i])->path;
        for (const char *p = strstr(path, id); p; p = strstr(p + 1, id))
            if ((p == path || p[-1] == '/') && (p[n] == '/' || p[n] == '\0')) return 2;
    }
    return 0;
}

static void choose_baseline(Figure *f, const char *requested, const Record ***cell) {
    f->baseline = -1;
    if (requested && *requested) {
        for (size_t v = 0; v < f->variant_count; v++)
            if (strcmp(f->variants[v].id, requested) == 0) f->baseline = (long)v;
        if (f->baseline < 0) {
            long hit = -1;
            int hits = 0;
            for (size_t v = 0; v < f->variant_count; v++)
                if (strcmp(f->variants[v].family, requested) == 0) {
                    hit = (long)v;
                    hits++;
                }
            if (hits == 1) f->baseline = hit;
        }
        if (f->baseline < 0) {
            Str names = {0};
            for (size_t v = 0; v < f->variant_count; v++) str_printf(&names, "%s%s", v ? ", " : "", f->variants[v].id);
            diag(f, xasprintf("reference '%s' is not in this figure; tools: %s", requested, str_cstr(&names)));
            str_free(&names);
            f->fatal = 1;
        }
        return;
    }
    size_t best_n = 0;
    int best_named = -1;
    for (size_t v = 0; v < f->variant_count; v++) {
        size_t n = 0;
        for (size_t c = 0; c < f->case_count; c++)
            if (cell[c][v] && cell[c][v]->status == REC_OK) n++;
        int named = project_match(f, f->variants[v].id);
        if (n > best_n || (n == best_n && named > best_named)) {
            best_n = n;
            best_named = named;
            f->baseline = (long)v;
        }
    }
    f->baseline_auto = 1;
}

static void compute_ellipse(Variant *var, const double *lx, const double *ly, size_t n) {
    double mx = 0, my = 0, cxx = 0, cyy = 0, cxy = 0, r, ax, ay;
    var->has_ellipse = 0;
    /* Five workloads minimum: with fewer, quartiles and r are too unstable to draw a shape. */
    if (n < 5) return;
    double *sx = xcalloc(n, sizeof(double)), *sy = xcalloc(n, sizeof(double));
    for (size_t i = 0; i < n; i++) {
        mx += lx[i];
        my += ly[i];
        sx[i] = lx[i];
        sy[i] = ly[i];
    }
    mx /= (double)n;
    my /= (double)n;
    for (size_t i = 0; i < n; i++) {
        cxx += (lx[i] - mx) * (lx[i] - mx);
        cyy += (ly[i] - my) * (ly[i] - my);
        cxy += (lx[i] - mx) * (ly[i] - my);
    }
    sort_ctx(sx, n, sizeof(double), cmp_double, NULL);
    sort_ctx(sy, n, sizeof(double), cmp_double, NULL);
    ax = (quantile7(sx, n, 0.75) - quantile7(sx, n, 0.25)) / 2;
    ay = (quantile7(sy, n, 0.75) - quantile7(sy, n, 0.25)) / 2;
    r = (cxx > 0 && cyy > 0) ? cxy / sqrt(cxx * cyy) : 0;
    double a = ax * ax, d = ay * ay, b = r * ax * ay;
    double tr = (a + d) / 2, disc = sqrt(((a - d) / 2) * ((a - d) / 2) + b * b);
    double l1 = tr + disc, l2 = tr - disc;
    var->e_cx = mx;
    var->e_cy = my;
    var->e_a = sqrt(l1 > 0 ? l1 : 0);
    var->e_b = sqrt(l2 > 0 ? l2 : 0);
    var->e_theta = 0.5 * atan2(2 * b, a - d);
    var->has_ellipse = var->e_a > 0;
    free(sx);
    free(sy);
}

static int cmp_workload(const void *a, const void *b, const void *ctx) {
    const Workload *x = a, *y = b;
    (void)ctx;
    if (x->order != y->order) {
        if (!x->order) return 1;
        if (!y->order) return -1;
        return x->order < y->order ? -1 : 1;
    }
    return natural_compare(x->id, y->id);
}

static void analyse(Figure *f, const Config *config) {
    size_t c, v, i;
    Vec case_ids = {0}, variant_ids = {0};
    Map seen_case = {0}, seen_variant = {0};
    const GroupSpec *group = config_group(config, f->group);

    for (i = 0; i < f->records.count; i++) {
        Record *r = f->records.items[i];
        if (!map_has(&seen_case, r->case_id)) {
            map_put(&seen_case, r->case_id, NULL);
            vec_push(&case_ids, r->case_id);
        }
        if (!map_has(&seen_variant, r->variant)) {
            map_put(&seen_variant, r->variant, NULL);
            vec_push(&variant_ids, r->variant);
        }
    }
    map_free(&seen_case);
    map_free(&seen_variant);
    vec_sort(&case_ids, cmp_cstr, NULL);
    vec_sort(&variant_ids, cmp_cstr, NULL);

    f->case_count = case_ids.count;
    f->cases = xcalloc(f->case_count + 1, sizeof(Workload));
    for (c = 0; c < f->case_count; c++) {
        const WorkloadSpec *spec = config_workload(config, case_ids.items[c]);
        f->cases[c].id = xstrdup(case_ids.items[c]);
        f->cases[c].label = xstrdup(spec && spec->label ? spec->label : f->cases[c].id);
        f->cases[c].order = spec ? spec->order : 0;
    }
    sort_ctx(f->cases, f->case_count, sizeof(Workload), cmp_workload, NULL);
    char **case_keys = xcalloc(f->case_count + 1, sizeof(char *));
    for (c = 0; c < f->case_count; c++) case_keys[c] = f->cases[c].id;

    f->variant_count = variant_ids.count;
    f->variants = xcalloc(f->variant_count + 1, sizeof(Variant));
    char **variant_keys = xcalloc(f->variant_count + 1, sizeof(char *));
    for (v = 0; v < f->variant_count; v++) {
        f->variants[v].id = xstrdup(variant_ids.items[v]);
        variant_keys[v] = f->variants[v].id;
    }
    vec_free(&case_ids);
    vec_free(&variant_ids);

    const Record ***cell = xcalloc(f->case_count + 1, sizeof(Record **));
    for (c = 0; c < f->case_count; c++) cell[c] = xcalloc(f->variant_count + 1, sizeof(Record *));
    f->cells = xcalloc(f->case_count * f->variant_count + 1, 1);

    for (i = 0; i < f->records.count; i++) {
        Record *r = f->records.items[i];
        c = (size_t)index_of(case_keys, f->case_count, r->case_id);
        v = (size_t)index_of(variant_keys, f->variant_count, r->variant);
        if (!f->variants[v].family) f->variants[v].family = xstrdup(r->family);
        if (cell[c][v]) {
            diag(f, xasprintf("duplicate result for workload '%s', tool '%s': %s and %s", r->case_id, r->variant, cell[c][v]->path, r->path));
            f->fatal = 1;
            continue;
        }
        cell[c][v] = r;
        f->cells[c * f->variant_count + v] = r->status == REC_OK ? CELL_OK : r->status == REC_FAILED ? CELL_FAILED : CELL_INCOMPLETE;
        if (r->status == REC_FAILED) {
            f->variants[v].failed_cases++;
            f->variants[v].failed_samples += r->failed;
        } else if (r->status != REC_OK) {
            f->variants[v].incomplete_cases++;
            diag(f, xasprintf("%s / %s excluded: %s (%s)", r->case_id, r->variant, r->reason, r->path));
        }
    }
    free(case_keys);
    free(variant_keys);

    for (v = 0; v < f->variant_count; v++) {
        Variant *var = &f->variants[v];
        const ToolSpec *spec = config_tool(config, var->id, var->family);
        var->label = xstrdup(spec && spec->label ? spec->label : var->id);
        var->order = spec ? spec->order : 0;
        var->hidden = spec ? spec->hide : 0;
        var->label_position = spec ? spec->label_position : LABEL_AUTO;
        var->line = spec ? spec->line : LINE_SOLID;
    }

    choose_baseline(f, group && group->baseline ? group->baseline : config->baseline, cell);
    if (f->baseline < 0) goto cleanup;

    unsigned char *use_case = xcalloc(f->case_count + 1, 1);
    for (c = 0; c < f->case_count; c++) {
        use_case[c] = 1;
        if (config->common_cases)
            for (v = 0; v < f->variant_count; v++)
                if (!f->variants[v].hidden && f->cells[c * f->variant_count + v] != CELL_OK) use_case[c] = 0;
    }

    f->points = xcalloc(f->case_count * f->variant_count + 1, sizeof(CasePoint));
    for (c = 0; c < f->case_count; c++) {
        const Record *ref = cell[c][f->baseline];
        int has_ref = ref && ref->status == REC_OK;
        if (has_ref && use_case[c]) f->referenced_cases++;
        for (v = 0; v < f->variant_count; v++) {
            const Record *r = cell[c][v];
            if (!r || r->status != REC_OK) continue;
            if (!has_ref) {
                f->cells[c * f->variant_count + v] = CELL_NO_REF;
                continue;
            }
            if (!use_case[c]) continue;
            CasePoint *p = &f->points[f->point_count++];
            p->variant = v;
            p->kase = c;
            p->record = r;
            p->reference = ref;
            p->x = r->time.estimate / ref->time.estimate;
            p->y = r->memory.estimate / ref->memory.estimate;
            p->cost = cost_of(config, p->x, p->y);
        }
    }
    free(use_case);
    if (!f->referenced_cases) {
        diag(f, xasprintf("no workload has a usable result for the reference '%s'", f->variants[f->baseline].id));
        f->fatal = 1;
        goto cleanup;
    }

    for (i = 0; i < f->point_count; i++) {
        CasePoint *p = &f->points[i];
        if (f->variants[p->variant].hidden) continue;
        p->pareto = 1;
        for (size_t j = 0; j < f->point_count && p->pareto; j++) {
            const CasePoint *q = &f->points[j];
            if (j != i && q->kase == p->kase && !f->variants[q->variant].hidden && dominates(q->x, q->y, p->x, p->y)) p->pareto = 0;
        }
        if (p->pareto) f->variants[p->variant].case_pareto++;
    }

    for (v = 0; v < f->variant_count; v++) {
        Variant *var = &f->variants[v];
        double *xs = xcalloc(f->point_count + 1, sizeof(double)), *ys = xcalloc(f->point_count + 1, sizeof(double));
        double *lx = xcalloc(f->point_count + 1, sizeof(double)), *ly = xcalloc(f->point_count + 1, sizeof(double));
        double slx = 0, sly = 0;
        size_t n = 0;
        for (i = 0; i < f->point_count; i++) {
            const CasePoint *p = &f->points[i];
            if (p->variant != v) continue;
            xs[n] = p->x;
            ys[n] = p->y;
            lx[n] = log(p->x);
            ly[n] = log(p->y);
            slx += lx[n];
            sly += ly[n];
            n++;
        }
        var->n = n;
        if (n) {
            var->gx = exp(slx / (double)n);
            var->gy = exp(sly / (double)n);
            sort_ctx(xs, n, sizeof(double), cmp_double, NULL);
            sort_ctx(ys, n, sizeof(double), cmp_double, NULL);
            if (config->spread == SPREAD_MINMAX) {
                var->xlo = xs[0];
                var->xhi = xs[n - 1];
                var->ylo = ys[0];
                var->yhi = ys[n - 1];
            } else {
                var->xlo = quantile7(xs, n, 0.25);
                var->xhi = quantile7(xs, n, 0.75);
                var->ylo = quantile7(ys, n, 0.25);
                var->yhi = quantile7(ys, n, 0.75);
            }
            var->cost = cost_of(config, var->gx, var->gy);
            compute_ellipse(var, lx, ly, n);
            if (!var->hidden && n > f->max_n) f->max_n = n;
        }
        free(xs);
        free(ys);
        free(lx);
        free(ly);
    }

    long best = -1;
    for (v = 0; v < f->variant_count; v++) {
        Variant *a = &f->variants[v];
        if (!a->n || a->hidden) continue;
        a->pareto = 1;
        for (size_t w = 0; w < f->variant_count && a->pareto; w++) {
            const Variant *b = &f->variants[w];
            if (w != v && b->n && !b->hidden && dominates(b->gx, b->gy, a->gx, a->gy)) a->pareto = 0;
        }
        if (a->n == f->max_n && (best < 0 || a->cost < f->variants[best].cost)) best = (long)v;
    }
    if (best >= 0) f->variants[best].selected = 1;
    for (v = 0; v < f->variant_count; v++) {
        const Variant *a = &f->variants[v];
        if (a->n && !a->hidden && a->n * 2 < f->referenced_cases)
            diag(f, xasprintf("%s was measured on %zu of %zu workloads; its aggregate is not comparable with tools measured on all of them",
                              a->label, a->n, f->referenced_cases));
    }

cleanup:
    for (c = 0; c < f->case_count; c++) free(cell[c]);
    free(cell);
}

/* Replace placeholders, then drop separator tokens left dangling by empty values. */
char *expand_template(const char *text, const Figure *f) {
    Str out = {0};
    char count[32];
    snprintf(count, sizeof(count), "%zu", f->referenced_cases);
    const char *reference = f->baseline >= 0 ? f->variants[f->baseline].label : "";
    while (*text) {
        const char *value = NULL;
        size_t skip = 0;
        if (*text == '{') {
            static const char *const names[] = {"{project}", "{suite}", "{group}", "{run}", "{reference}", "{workloads}"};
            const char *values[] = {f->project, f->suite, f->group, f->run, reference, count};
            for (int k = 0; k < 6; k++)
                if (starts_with(text, names[k])) {
                    value = values[k];
                    skip = strlen(names[k]);
                }
        }
        if (value) {
            str_append(&out, value);
            text += skip;
        } else {
            str_appendn(&out, text, 1);
            text++;
        }
    }
    /* Tokenize on spaces; remove separator tokens at the ends or next to another separator. */
    Vec tokens = {0};
    char *copy = xstrdup(str_cstr(&out));
    str_free(&out);
    for (char *p = copy; *p;) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        vec_push(&tokens, p);
        while (*p && *p != ' ') p++;
    }
    int *separator = xcalloc(tokens.count + 1, sizeof(int));
    for (size_t i = 0; i < tokens.count; i++) {
        const char *t = tokens.items[i];
        separator[i] = strspn(t, "/|:-") == strlen(t) || strcmp(t, "\xc2\xb7") == 0;
    }
    int previous_separator = 1;
    size_t last_word = 0;
    int any_word = 0;
    for (size_t i = 0; i < tokens.count; i++)
        if (!separator[i]) {
            last_word = i;
            any_word = 1;
        }
    for (size_t i = 0; i < tokens.count && any_word; i++) {
        if (separator[i] && (previous_separator || i > last_word)) continue;
        if (out.length) str_append(&out, " ");
        str_append(&out, tokens.items[i]);
        previous_separator = separator[i];
    }
    free(separator);
    vec_free(&tokens);
    free(copy);
    char *result = xstrdup(str_cstr(&out));
    str_free(&out);
    return result;
}

static int cmp_figure(const void *a, const void *b, const void *ctx) {
    const Figure *x = *(Figure *const *)a, *y = *(Figure *const *)b;
    int c = strcmp(x->suite, y->suite);
    (void)ctx;
    if (!c) c = strcmp(x->group, y->group);
    if (!c) c = strcmp(x->run, y->run);
    return c;
}

int build_figures(Ingest *ingest, const Config *config, Vec *figures) {
    Map by_key = {0}, slugs = {0};
    int ok = 1;
    for (size_t i = 0; i < ingest->parsed.records.count; i++) {
        Record *r = ingest->parsed.records.items[i];
        const WorkloadSpec *w = config_workload(config, r->case_id);
        if (w && w->hide) continue;
        char *key = xasprintf("%s\x1f%s\x1f%s", r->suite, r->group, r->run);
        Figure *f = map_get(&by_key, key);
        if (!f) {
            f = xcalloc(1, sizeof(*f));
            f->project = xstrdup(r->project);
            f->suite = xstrdup(r->suite);
            f->group = xstrdup(r->group);
            f->run = xstrdup(r->run);
            f->baseline = -1;
            map_put(&by_key, key, f);
            vec_push(figures, f);
        }
        vec_push(&f->records, r);
        free(key);
    }
    map_free(&by_key);
    vec_sort(figures, cmp_figure, NULL);
    for (size_t i = 0; i < figures->count; i++) {
        Figure *f = figures->items[i];
        const GroupSpec *group = config_group(config, f->group);
        analyse(f, config);
        f->title = expand_template(group && group->title ? group->title : config->title, f);
        if (!*f->title) {
            free(f->title);
            f->title = xstrdup("Zebrac results");
        }
        const char *subtitle = group && group->subtitle ? group->subtitle : config->subtitle;
        f->subtitle = strcmp(subtitle, "auto") == 0 ? NULL : expand_template(subtitle, f);
        /* File names: suite and group; the project only when neither exists. */
        char *base = *f->suite || *f->group ? xasprintf("%s-%s", f->suite, f->group) : xstrdup(*f->project ? f->project : "results");
        char *slug = slugify(base);
        if (!map_put(&slugs, slug, NULL)) {
            char *with_run = xasprintf("%s-%s", slug, f->run);
            free(slug);
            slug = slugify(with_run);
            free(with_run);
            map_put(&slugs, slug, NULL);
        }
        f->slug = slug;
        free(base);
        if (f->fatal) ok = 0;
    }
    map_free(&slugs);
    return ok;
}

/*
 * Style follows the tool, never its rank, and is shared by every figure.
 * Tools that never appear in the same figure may share an automatic slot
 * (greedy graph coloring in name order), so each figure stays within the
 * palette whenever possible. [[tool]] color, shape, and fill override the slot.
 */
/* The style key is the family, unless two tools in one figure share it (rg and rg -j1). */
static const char *style_key(const Figure *f, size_t v) {
    for (size_t w = 0; w < f->variant_count; w++)
        if (w != v && strcmp(f->variants[w].family, f->variants[v].family) == 0) return f->variants[v].id;
    return f->variants[v].family;
}

void resolve_styles(Vec *figures, const Config *config) {
    Map index = {0};
    Vec families = {0};
    for (size_t i = 0; i < figures->count; i++) {
        Figure *f = figures->items[i];
        for (size_t v = 0; v < f->variant_count; v++) {
            const char *key = style_key(f, v);
            if (!map_has(&index, key)) {
                map_put(&index, key, NULL);
                vec_push(&families, xstrdup(key));
            }
        }
    }
    vec_sort(&families, cmp_cstr, NULL);
    size_t n = families.count;
    for (size_t i = 0; i < n; i++) map_put(&index, families.items[i], (void *)(i + 1));
    unsigned char *together = xcalloc(n * n + 1, 1);
    for (size_t i = 0; i < figures->count; i++) {
        Figure *f = figures->items[i];
        for (size_t v = 0; v < f->variant_count; v++)
            for (size_t w = 0; w < f->variant_count; w++) {
                size_t a = (size_t)map_get(&index, style_key(f, v)) - 1, b = (size_t)map_get(&index, style_key(f, w)) - 1;
                if (a != b) together[a * n + b] = 1;
            }
    }
    int *slot = xcalloc(n + 1, sizeof(int));
    for (size_t i = 0; i < n; i++)
        for (int candidate = 0;; candidate++) {
            int clash = 0;
            for (size_t j = 0; j < i && !clash; j++) clash = together[i * n + j] && slot[j] == candidate;
            if (!clash) {
                slot[i] = candidate;
                break;
            }
        }
    int colors = config->palette_count > 0 ? config->palette_count : 1;
    for (size_t i = 0; i < figures->count; i++) {
        Figure *f = figures->items[i];
        for (size_t v = 0; v < f->variant_count; v++) {
            Variant *var = &f->variants[v];
            int s = slot[(size_t)map_get(&index, style_key(f, v)) - 1];
            const ToolSpec *spec = config_tool(config, var->id, var->family);
            var->color = spec && spec->has_color ? spec->color : config->palette[s % colors];
            var->shape = spec && spec->shape != SHAPE_AUTO ? spec->shape : s % SHAPE_COUNT;
            var->hollow = spec && spec->fill != FILL_AUTO ? spec->fill == FILL_HOLLOW : (s / (colors < SHAPE_COUNT ? colors : SHAPE_COUNT)) % 2;
        }
    }
    free(slot);
    free(together);
    map_free(&index);
    for (size_t i = 0; i < families.count; i++) free(families.items[i]);
    vec_free(&families);
}

void figure_free(Figure *f) {
    if (!f) return;
    free(f->project);
    free(f->suite);
    free(f->group);
    free(f->run);
    free(f->title);
    free(f->subtitle);
    free(f->slug);
    vec_free(&f->records);
    for (size_t c = 0; c < f->case_count; c++) {
        free(f->cases[c].id);
        free(f->cases[c].label);
    }
    free(f->cases);
    for (size_t v = 0; v < f->variant_count; v++) {
        free(f->variants[v].id);
        free(f->variants[v].family);
        free(f->variants[v].label);
    }
    free(f->variants);
    free(f->points);
    free(f->cells);
    for (size_t i = 0; i < f->diagnostics.count; i++) free(f->diagnostics.items[i]);
    vec_free(&f->diagnostics);
    for (size_t i = 0; i < f->files.count; i++) free(f->files.items[i]);
    vec_free(&f->files);
    free(f);
}
