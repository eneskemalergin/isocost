/*
 * Input discovery and identity.
 *
 * A Zebrac result records the command it ran, not which workload or tool it
 * represents. The first matching rule names it:
 *
 *   config        [[tool]] match glob on the command (argv0 reduced to its file name)
 *   sidecar       bench.meta.v1 JSONL beside or one level above the JSON; matched on the
 *                 last two path components, plus the command for multi-result files
 *   workload.tsv  command -> TOOL__CASE__MODE... (crc-bench)
 *   path layout   TOOL/FORMAT/LEVEL/THREADS/CATEGORY.CLASS.OPERATION.json (z-flate)
 *   case--variant CASE--VARIANT.json
 *   case__tool    CASE__TOOL.json, directory NAME_YYYYMMDD_HHMMSS
 *   command diff  several results in one file, named by the command tokens that differ
 *   argv0         one result: the executable name
 */
#include "model.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    Vec json_paths;
    Vec sidecar_paths;
    Vec workload_paths;
    Map seen_files;
    Map scanned_dirs;
} Discovery;

static void add_unique(Map *seen, Vec *list, const char *path) {
    if (map_has(seen, path)) return;
    map_put(seen, path, NULL);
    vec_push(list, xstrdup(path));
}

static void classify(Discovery *d, const char *path) {
    const char *base = path_base(path);
    if (ends_with(base, ".json")) add_unique(&d->seen_files, &d->json_paths, path);
    else if (ends_with(base, ".jsonl")) add_unique(&d->seen_files, &d->sidecar_paths, path);
    else if (strcmp(base, "workload.tsv") == 0) add_unique(&d->seen_files, &d->workload_paths, path);
}

typedef struct {
    Discovery *d;
    const char *dir;
    int depth;
} WalkCtx;

static void walk(Discovery *d, const char *dir, int depth);

static void walk_entry(void *ctx, const char *name) {
    WalkCtx *w = ctx;
    if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 || strcmp(name, ".zig-cache") == 0 ||
        strcmp(name, "zig-cache") == 0)
        return;
    char *child = path_join(w->dir, name);
    int kind = pf_kind_nofollow(child);
    if (kind == PF_DIR) walk(w->d, child, w->depth + 1);
    else if (kind == PF_FILE) classify(w->d, child);
    free(child);
}

static void walk(Discovery *d, const char *dir, int depth) {
    WalkCtx ctx = {d, dir, depth};
    if (depth > 32) return;
    pf_list_dir(dir, walk_entry, &ctx);
}

static void sidecar_entry(void *ctx, const char *name) {
    WalkCtx *w = ctx;
    if (ends_with(name, ".jsonl") || strcmp(name, "workload.tsv") == 0) {
        char *child = path_join(w->dir, name);
        if (pf_kind(child) == PF_FILE) classify(w->d, child);
        free(child);
    }
}

/* Sidecars may sit beside the JSON or one level above a section directory. */
static void scan_for_sidecars(Discovery *d, const char *dir) {
    WalkCtx ctx = {d, dir, 0};
    if (map_has(&d->scanned_dirs, dir)) return;
    map_put(&d->scanned_dirs, dir, NULL);
    pf_list_dir(dir, sidecar_entry, &ctx);
}

static char *short_token(const char *token);

static void ingest_warn(Ingest *ingest, char *message) { vec_push(&ingest->warnings, message); }

static void ingest_error(Ingest *ingest, int code, char *message) {
    vec_push(&ingest->errors, message);
    if (code > ingest->exit_code) ingest->exit_code = code;
}

/* "perf_plain_20260916_092052" -> "perf_plain" and "20260916_092052". */
static int split_timestamp(const char *name, char **prefix, char **stamp) {
    size_t n = strlen(name);
    if (n < 17) return 0;
    const char *t = name + n - 16;
    if (t[0] != '_' || t[9] != '_') return 0;
    for (int i = 1; i < 16; i++)
        if (i != 9 && (t[i] < '0' || t[i] > '9')) return 0;
    *prefix = xstrndup(name, (size_t)(t - name));
    *stamp = xstrdup(t + 1);
    return 1;
}

/* Name of the directory `levels` above the file (1 = parent). */
static char *component_above(const char *path, int levels) {
    char *dir = path_dir(path), *next;
    for (int i = 1; i < levels; i++) {
        next = path_dir(dir);
        free(dir);
        dir = next;
    }
    next = xstrdup(path_base(dir));
    free(dir);
    return next;
}

/* The component before "/<marker>/", or "". */
static char *component_before(const char *path, const char *marker) {
    const char *p = strstr(path, marker), *start;
    if (!p) return xstrdup("");
    start = p;
    while (start > path && start[-1] != '/') start--;
    return xstrndup(start, (size_t)(p - start));
}

/* Project: the directory holding bench/ or results/. */
static char *infer_project(const char *path) {
    char *p = component_before(path, "/bench/");
    if (!*p) {
        free(p);
        p = component_before(path, "/results/");
    }
    return p;
}

/* "/usr/bin/zstd -3 -c f" -> "zstd -3 -c f" */
static char *normalized_command(const Record *r) {
    const char *cmd = r->command;
    const char *space = strchr(cmd, ' ');
    size_t first = space ? (size_t)(space - cmd) : strlen(cmd);
    char *exe = xstrndup(cmd, first);
    char *out = xasprintf("%s%s", path_base(exe), space ? space : "");
    free(exe);
    return out;
}

static void set_identity(Record *r, const char *rule, char *suite, char *group, char *run, char *kase, char *variant, char *family) {
    r->rule = rule;
    r->project = infer_project(r->path);
    if (suite && strcmp(suite, r->project) == 0) {
        free(suite);
        suite = NULL;
    }
    r->suite = suite ? suite : xstrdup("");
    r->group = group ? group : xstrdup("");
    r->run = run ? run : xstrdup("");
    r->case_id = kase;
    r->variant = variant;
    r->family = family ? family : xstrdup(variant);
}

static int rule_config(Record *r, const Config *config) {
    char *command = normalized_command(r);
    const ToolSpec *hit = NULL;
    for (size_t i = 0; i < config->tool_count && !hit; i++)
        if (config->tools[i].match && glob_match(config->tools[i].match, command)) hit = &config->tools[i];
    free(command);
    if (!hit) return 0;
    const char *space = strchr(r->command, ' ');
    char *exe = space ? xstrndup(r->command, (size_t)(space - r->command)) : xstrdup(r->command);
    /* The executable is the family, so "gzip -6" and "gzip -d" share one color across figures. */
    set_identity(r, "config", xstrdup(""), component_above(r->path, 1), xstrdup(""), path_stem(r->path), xstrdup(hit->id), short_token(exe));
    free(exe);
    return 1;
}

static int rule_sidecar(Record *r, const Map *sidecars) {
    char *tail = path_tail(r->path, 2), *key;
    SidecarRow *row;
    char *prefix, *stamp, *dirname;
    key = r->results_in_file > 1 ? xasprintf("%s\n%s", tail, r->command) : xstrdup(tail);
    row = map_get(sidecars, key);
    free(key);
    free(tail);
    if (!row) return 0;
    dirname = component_above(r->path, 1);
    if (!split_timestamp(dirname, &prefix, &stamp)) {
        prefix = xstrdup(dirname);
        stamp = xstrdup("");
    }
    free(dirname);
    set_identity(r, "sidecar", xstrdup(row->suite ? row->suite : ""), xstrdup(row->section ? row->section : prefix), stamp,
                 xstrdup(row->workload), xstrdup(row->tool), NULL);
    free(prefix);
    return 1;
}

static int rule_workload(Record *r, const Map *workloads) {
    char *dir = path_dir(r->path);
    char *key = xasprintf("%s\t%s", dir, r->command);
    const char *candidate = map_get(workloads, key);
    char *parts[16];
    int count = 0;
    free(key);
    free(dir);
    if (!candidate) return 0;
    char *copy = xstrdup(candidate), *cursor = copy, *sep;
    while (count < 16) {
        sep = strstr(cursor, "__");
        parts[count++] = cursor;
        if (!sep) break;
        *sep = '\0';
        cursor = sep + 2;
    }
    if (count < 2) {
        free(copy);
        return 0;
    }
    Str group = {0};
    for (int i = 2; i < count; i++) {
        if (strcmp(parts[i], "c0") == 0) continue; /* chunk size 0 means one-shot */
        if (group.length) str_append(&group, " ");
        str_append(&group, parts[i]);
    }
    char *build = component_above(r->path, 1);
    if (group.length) str_printf(&group, " (%s)", build);
    else str_append(&group, build);
    set_identity(r, "workload.tsv", xstrdup(""), xstrdup(str_cstr(&group)), component_above(r->path, 2), xstrdup(parts[1]),
                 xstrdup(parts[0]), NULL);
    str_free(&group);
    free(build);
    free(copy);
    return 1;
}

static int rule_layout(Record *r) {
    char *stem = path_stem(r->path);
    char *first = strchr(stem, '.'), *last = strrchr(stem, '.');
    if (!first || first == last || strchr(first + 1, '.') != last || r->results_in_file != 1) {
        free(stem);
        return 0;
    }
    char *threads = component_above(r->path, 1), *level = component_above(r->path, 2);
    char *format = component_above(r->path, 3), *tool = component_above(r->path, 4);
    char *kase = xstrndup(stem, (size_t)(last - stem));
    char *op = xstrdup(last + 1);
    char *variant = strcmp(level, "-") == 0 ? xstrdup(tool) : xasprintf("%s -%s", tool, level);
    char *group = strcmp(threads, "ST") == 0 ? xasprintf("%s %s", format, op) : xasprintf("%s %s %s", format, op, threads);
    set_identity(r, "path layout", xstrdup(""), group, xstrdup(""), kase, variant, xstrdup(tool));
    free(threads);
    free(level);
    free(format);
    free(tool);
    free(op);
    free(stem);
    return 1;
}

static int rule_separator(Record *r, const char *separator, const char *rule) {
    char *stem, *at = NULL, *dirname, *prefix, *stamp;
    if (r->results_in_file != 1) return 0;
    stem = path_stem(r->path);
    for (char *p = strstr(stem, separator); p; p = strstr(p + 1, separator)) at = p;
    if (!at || at == stem || !at[strlen(separator)]) {
        free(stem);
        return 0;
    }
    dirname = component_above(r->path, 1);
    if (!split_timestamp(dirname, &prefix, &stamp)) {
        int dashes = strcmp(separator, "--") == 0;
        prefix = dashes ? xstrdup("") : xstrdup(dirname);
        stamp = dashes ? xstrdup(dirname) : xstrdup("");
    }
    free(dirname);
    set_identity(r, rule, component_before(r->path, "/results/"), prefix, stamp, xstrndup(stem, (size_t)(at - stem)),
                 xstrdup(at + strlen(separator)), NULL);
    free(stem);
    return 1;
}

static char *short_token(const char *token) {
    const char *base = path_base(token);
    return xstrdup(*base ? base : token);
}

/* Several results in one file: name each by the command tokens that differ. */
static void rule_command_diff(Record **records, size_t count) {
    char ***tokens = xcalloc(count, sizeof(char **));
    size_t *token_count = xcalloc(count, sizeof(size_t));
    for (size_t i = 0; i < count; i++) {
        const char *p = records[i]->command;
        tokens[i] = xcalloc(strlen(p) / 2 + 2, sizeof(char *));
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (p > start) tokens[i][token_count[i]++] = xstrndup(start, (size_t)(p - start));
        }
    }
    for (size_t i = 0; i < count; i++) {
        Str name = {0};
        for (size_t t = 0; t < token_count[i] && name.length <= 40; t++) {
            /* Keep a token unless every command in the file contains it somewhere. */
            int everywhere = 1;
            for (size_t j = 0; j < count && everywhere; j++) {
                int found = 0;
                for (size_t u = 0; u < token_count[j] && !found; u++) found = strcmp(tokens[i][t], tokens[j][u]) == 0;
                everywhere = found;
            }
            if (!everywhere || t == 0) {
                char *s = short_token(tokens[i][t]);
                if (name.length) str_append(&name, " ");
                str_append(&name, s);
                free(s);
            }
        }
        if (!name.length) str_printf(&name, "result-%d", records[i]->index + 1);
        char *argv0 = short_token(records[i]->argv0 ? records[i]->argv0 : (token_count[i] ? tokens[i][0] : "command"));
        set_identity(records[i], "command diff", component_before(records[i]->path, "/results/"), component_above(records[i]->path, 1),
                     xstrdup(""), path_stem(records[i]->path), xstrdup(str_cstr(&name)), argv0);
        str_free(&name);
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t t = 0; t < token_count[i]; t++) free(tokens[i][t]);
        free(tokens[i]);
    }
    free(tokens);
    free(token_count);
}

static void rule_argv0(Record *r) {
    const char *space = strchr(r->command, ' ');
    char *first = space ? xstrndup(r->command, (size_t)(space - r->command)) : xstrdup(r->command);
    char *name = short_token(r->argv0 ? r->argv0 : (*first ? first : "command"));
    set_identity(r, "argv0", component_before(r->path, "/results/"), xstrdup(""), component_above(r->path, 1), path_stem(r->path), name, NULL);
    free(first);
}

static void load_workload_tsv(const char *path, Map *workloads, Ingest *ingest) {
    unsigned char *data;
    size_t length, start = 0;
    const char *why = NULL;
    int line = 0;
    char *dir = path_dir(path);
    if (!pf_read_file(path, &data, &length, 64u << 20, &why)) {
        ingest_warn(ingest, xasprintf("cannot read %s: %s", path, why));
        free(dir);
        return;
    }
    while (start < length) {
        size_t end = start;
        while (end < length && data[end] != '\n') end++;
        char *text = xstrndup((char *)data + start, end - start);
        char *tab = strchr(text, '\t');
        if (line++ > 0 && tab) {
            *tab = '\0';
            char *command = tab + 1;
            size_t n = strlen(command);
            while (n && command[n - 1] == '\r') command[--n] = '\0';
            char *key = xasprintf("%s\t%s", dir, command);
            char *old = map_get(workloads, key);
            free(old);
            map_put(workloads, key, xstrdup(text));
            free(key);
        }
        free(text);
        start = end + 1;
    }
    free(data);
    free(dir);
}

/* Files are parsed on several threads, each into its own slot, then merged in path order. */
typedef struct {
    char **paths;
    size_t count;
    ParseSet *sets;
    char **errors;
    PfCounter next;
} ParseJob;

static void *parse_worker(void *arg) {
    ParseJob *job = arg;
    char error[512];
    for (;;) {
        long i = pf_counter_next(&job->next);
        if (i < 0 || (size_t)i >= job->count) return NULL;
        if (!zebrac_parse_file(job->paths[i], &job->sets[i], error, sizeof(error))) job->errors[i] = xstrdup(error);
    }
}

static int cmp_record(const void *a, const void *b, const void *ctx) {
    const Record *x = *(Record *const *)a, *y = *(Record *const *)b;
    int c = strcmp(x->path, y->path);
    (void)ctx;
    return c ? c : (x->index > y->index) - (x->index < y->index);
}

static void count_rule(Ingest *ingest, const char *rule) {
    size_t *n = map_get(&ingest->rule_counts, rule);
    if (!n) {
        n = xcalloc(1, sizeof(*n));
        map_put(&ingest->rule_counts, rule, n);
    }
    (*n)++;
}

static int any_glob(const Vec *globs, const char *text) {
    if (!globs->count) return 1;
    for (size_t i = 0; i < globs->count; i++)
        if (glob_match(globs->items[i], text)) return 1;
    return 0;
}

static void read_sidecar_input(Discovery *d, Ingest *ingest, const char *input) {
    Vec listed = {0};
    char error[512];
    char *dir = path_dir(input);
    int n = sidecar_parse_file(input, &listed, error, sizeof(error));
    add_unique(&d->seen_files, &d->sidecar_paths, input);
    if (n < 0) ingest_error(ingest, 2, xstrdup(error));
    else if (n == 0) ingest_error(ingest, 3, xasprintf("%s lists no bench.meta.v1 rows", input));
    for (size_t k = 0; k < listed.count; k++) {
        SidecarRow *row = listed.items[k];
        char *tail = path_tail(row->raw_json, 2);
        char *local = path_join(dir, tail);
        if (pf_kind(row->raw_json) == PF_FILE) classify(d, row->raw_json);
        else if (pf_kind(local) == PF_FILE) classify(d, local);
        else if (++ingest->sidecar_missing <= 5)
            ingest_error(ingest, 3, xasprintf("sidecar row %s / %s points to a missing file: %s", row->workload, row->tool, row->raw_json));
        free(tail);
        free(local);
        sidecar_row_free(row);
    }
    if (ingest->sidecar_missing > 5) ingest_error(ingest, 3, xasprintf("%zu sidecar rows point to missing files in total", ingest->sidecar_missing));
    vec_free(&listed);
    free(dir);
}

int ingest_run(const Config *config, char **inputs, size_t input_count, int max_threads, Ingest *ingest) {
    Discovery d = {0};
    Map sidecars = {0}, workloads = {0};
    Vec rows = {0};
    char error[512];

    for (size_t i = 0; i < input_count; i++) {
        int kind = pf_kind(inputs[i]);
        if (kind == PF_NONE) ingest_error(ingest, 2, xasprintf("input not found: %s", inputs[i]));
        else if (kind == PF_DIR) walk(&d, inputs[i], 0);
        else if (ends_with(inputs[i], ".jsonl")) read_sidecar_input(&d, ingest, inputs[i]);
        else classify(&d, inputs[i]);
    }

    size_t json_count = d.json_paths.count;
    for (size_t i = 0; i < json_count; i++) {
        char *dir = path_dir(d.json_paths.items[i]);
        char *parent = path_dir(dir);
        scan_for_sidecars(&d, dir);
        scan_for_sidecars(&d, parent);
        free(dir);
        free(parent);
    }
    for (size_t i = 0; i < d.sidecar_paths.count; i++)
        if (sidecar_parse_file(d.sidecar_paths.items[i], &rows, error, sizeof(error)) < 0)
            ingest_warn(ingest, xasprintf("ignored sidecar: %s", error));
    for (size_t i = 0; i < rows.count; i++) {
        SidecarRow *row = rows.items[i];
        char *tail = path_tail(row->raw_json, 2);
        map_put(&sidecars, tail, row);
        if (row->command) {
            char *key = xasprintf("%s\n%s", tail, row->command);
            map_put(&sidecars, key, row);
            free(key);
        }
        free(tail);
        ingest->sidecar_rows++;
    }
    for (size_t i = 0; i < d.workload_paths.count; i++) load_workload_tsv(d.workload_paths.items[i], &workloads, ingest);

    vec_sort(&d.json_paths, cmp_cstr, NULL);
    {
        ParseJob job = {(char **)d.json_paths.items, d.json_paths.count, xcalloc(d.json_paths.count + 1, sizeof(ParseSet)),
                        xcalloc(d.json_paths.count + 1, sizeof(char *)), {0}};
        int threads = max_threads > 0 ? max_threads : 1;
        if ((size_t)threads > job.count / 16 + 1) threads = (int)(job.count / 16 + 1); /* small inputs stay on one thread */
        pf_run_threads(threads, parse_worker, &job);
        for (size_t i = 0; i < job.count; i++) {
            ParseSet *one = &job.sets[i];
            for (size_t k = 0; k < one->records.count; k++) vec_push(&ingest->parsed.records, one->records.items[k]);
            vec_free(&one->records);
            ingest->parsed.files_read += one->files_read;
            ingest->parsed.files_skipped += one->files_skipped;
            ingest->parsed.bytes_read += one->bytes_read;
            if (job.errors[i]) ingest_error(ingest, 2, job.errors[i]);
        }
        free(job.sets);
        free(job.errors);
    }
    vec_sort(&ingest->parsed.records, cmp_record, NULL);

    Vec *records = &ingest->parsed.records;
    for (size_t i = 0; i < records->count; i++) {
        Record *r = records->items[i];
        select_statistic(r, config->statistic);
        if (r->variant) continue;
        if (rule_config(r, config) || rule_sidecar(r, &sidecars) || rule_workload(r, &workloads) || rule_layout(r) ||
            rule_separator(r, "--", "case--variant") || rule_separator(r, "__", "case__tool")) {
            count_rule(ingest, r->rule);
            continue;
        }
        if (r->results_in_file > 1) {
            size_t j = i;
            while (j < records->count && strcmp(((Record *)records->items[j])->path, r->path) == 0) j++;
            rule_command_diff((Record **)records->items + i, j - i);
            for (size_t k = i; k < j; k++) {
                select_statistic(records->items[k], config->statistic);
                count_rule(ingest, "command diff");
            }
            i = j - 1;
            continue;
        }
        rule_argv0(r);
        count_rule(ingest, r->rule);
    }

    /* Filters, hidden groups, and newest-run selection per (suite, group). */
    Map newest = {0};
    for (size_t i = 0; i < records->count; i++) {
        Record *r = records->items[i];
        char *key = xasprintf("%s\x1f%s", r->suite, r->group);
        char *best = map_get(&newest, key);
        if (!best || strcmp(r->run, best) > 0) map_put(&newest, key, r->run);
        free(key);
    }
    Map skipped = {0};
    size_t kept = 0;
    for (size_t i = 0; i < records->count; i++) {
        Record *r = records->items[i];
        const GroupSpec *spec = config_group(config, r->group);
        int keep = any_glob(&config->groups, r->group) && any_glob(&config->suites, r->suite) && !(spec && spec->hide);
        if (keep && !config->all_runs) {
            char *key = xasprintf("%s\x1f%s", r->suite, r->group);
            const char *best = map_get(&newest, key);
            if (best && strcmp(best, r->run) != 0) {
                char *label = xasprintf("%s%s%s run %s (newer run %s is reported)", r->suite, *r->suite ? " / " : "",
                                        *r->group ? r->group : "(ungrouped)", r->run, best);
                if (map_put(&skipped, label, NULL)) vec_push(&ingest->skipped_runs, xstrdup(label));
                free(label);
                keep = 0;
            }
            free(key);
        }
        if (keep) records->items[kept++] = r;
        else vec_push(&ingest->excluded, r);
    }
    records->count = kept;
    map_free(&newest);
    map_free(&skipped);

    if (!records->count && !ingest->errors.count)
        ingest_error(ingest, 3, xasprintf("no Zebrac results found in %zu input path(s) (%zu JSON files were not Zebrac results)",
                                          input_count, ingest->parsed.files_skipped));

    for (size_t i = 0; i < rows.count; i++) sidecar_row_free(rows.items[i]);
    vec_free(&rows);
    for (size_t i = 0; i < workloads.capacity; i++)
        if (workloads.keys && workloads.keys[i]) free(workloads.values[i]);
    map_free(&workloads);
    map_free(&sidecars);
    for (size_t i = 0; i < d.json_paths.count; i++) free(d.json_paths.items[i]);
    for (size_t i = 0; i < d.sidecar_paths.count; i++) free(d.sidecar_paths.items[i]);
    for (size_t i = 0; i < d.workload_paths.count; i++) free(d.workload_paths.items[i]);
    vec_free(&d.json_paths);
    vec_free(&d.sidecar_paths);
    vec_free(&d.workload_paths);
    map_free(&d.seen_files);
    map_free(&d.scanned_dirs);
    return ingest->errors.count ? 0 : 1;
}

void ingest_free(Ingest *ingest) {
    for (size_t i = 0; i < ingest->parsed.records.count; i++) record_free(ingest->parsed.records.items[i]);
    for (size_t i = 0; i < ingest->excluded.count; i++) record_free(ingest->excluded.items[i]);
    vec_free(&ingest->parsed.records);
    vec_free(&ingest->excluded);
    for (size_t i = 0; i < ingest->skipped_runs.count; i++) free(ingest->skipped_runs.items[i]);
    for (size_t i = 0; i < ingest->warnings.count; i++) free(ingest->warnings.items[i]);
    for (size_t i = 0; i < ingest->errors.count; i++) free(ingest->errors.items[i]);
    vec_free(&ingest->skipped_runs);
    vec_free(&ingest->warnings);
    vec_free(&ingest->errors);
    for (size_t i = 0; i < ingest->rule_counts.capacity; i++)
        if (ingest->rule_counts.keys && ingest->rule_counts.keys[i]) free(ingest->rule_counts.values[i]);
    map_free(&ingest->rule_counts);
}
