/*
 * isocost: speed and memory figures from Zebrac summary JSON.
 *
 *   isocost report INPUT... -o DIR [options]
 *   isocost list INPUT... [--json] [options]
 *   isocost config [--check FILE]
 *
 * Exit status: 0 report written, 2 usage, config, unreadable, or malformed
 * input, 3 inconsistent input (a comparison could not be drawn).
 */
#include "model.h"
#include "platform.h"
#include "plot.h"
#include "report.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fputs("isocost " ISOCOST_VERSION ": speed and memory figures from Zebrac summary JSON\n"
          "\n"
          "usage:\n"
          "  isocost report INPUT... -o DIR [options]  write figures, report.md, summary.json\n"
          "  isocost INPUT... -o DIR [options]         same as report\n"
          "  isocost list INPUT... [--json] [options]  show how results are named and grouped; write nothing\n"
          "  isocost config                            print every setting with its default\n"
          "  isocost config --check FILE [--print]     check a config file; --print shows the values\n"
          "\n"
          "INPUT is a Zebrac JSON file, a directory (searched recursively), or a bench.meta.v1 sidecar.\n"
          "isocost.toml is read from the first input directory or its parents.\n"
          "\n"
          "options (each one overrides the isocost.toml key it names):\n"
          "  -o, --out DIR              output directory; missing parents are created\n"
          "  --force                    replace an existing report in DIR\n"
          "  --config FILE              use this file instead of the nearest isocost.toml\n"
          "  --no-config                use the built-in defaults only\n"
          "  --baseline NAME            report.baseline: reference tool id or family\n"
          "  --group GLOB               report.groups\n"
          "  --suite GLOB               report.suites\n"
          "  --runs latest|all          report.runs\n"
          "  --cases all|common         report.cases\n"
          "  --statistic median|mean    report.statistic\n"
          "  --cost geometric|linear    report.cost\n"
          "  --weights T,M              report.time_weight, report.memory_weight\n"
          "  --view LIST                report.views: overview,workloads\n"
          "  --format LIST              report.formats: png,svg,pdf\n"
          "  --data                     report.data: also write points.tsv\n"
          "  --size NAME|WxHUNIT        figure.size: screen, paper, paper-wide, slide, 120x80mm\n"
          "  --dpi N                    figure.dpi\n"
          "  --title TEXT               figure.title\n"
          "  -j, --jobs N               threads (default: CPU count, at most 8; about 6 MB each)\n"
          "  -q, --quiet                no summary line on stderr\n"
          "  --json                     with list: print summary.json to stdout\n"
          "\n"
          "exit status:\n"
          "  0  report written\n"
          "  2  usage error, invalid config, or unreadable input; nothing written\n"
          "  3  inconsistent input, explained on stderr and, when some comparisons\n"
          "     were still drawn, in report.md and summary.json\n"
          "\n"
          "examples:\n"
          "  isocost report results/ -o report/\n"
          "  isocost report results/ -o paper/ --size paper --format pdf,svg\n"
          "  isocost list results/ --json | jq \".figures[].tools[].id\"\n",
          out);
}

static int die(const char *message) {
    fprintf(stderr, "isocost: %s\n", message);
    return 2;
}

static unsigned parse_list(const char *text, const char *const *names, const char *what, int *ok) {
    unsigned flags = 0;
    char *copy = xstrdup(text), *save = copy;
    for (char *item = copy; item && *item;) {
        char *comma = strchr(item, ',');
        if (comma) *comma = '\0';
        int found = -1;
        for (int i = 0; names[i]; i++)
            if (strcmp(item, names[i]) == 0) found = i;
        if (found < 0) {
            fprintf(stderr, "isocost: unknown %s '%s'\n", what, item);
            *ok = 0;
        } else {
            flags |= 1u << found;
        }
        item = comma ? comma + 1 : NULL;
    }
    free(save);
    return flags;
}

static void set_string(char **target, const char *value) {
    free(*target);
    *target = xstrdup(value);
}

static void set_glob(Vec *v, const char *value) {
    for (size_t i = 0; i < v->count; i++) free(v->items[i]);
    v->count = 0;
    vec_push(v, xstrdup(value));
}

typedef struct {
    Vec *figures;
    Scene **scenes;             /* figure * 2 + view */
    char **paths;               /* base path without extension, same index */
    unsigned formats;
    PfCounter next;
    volatile int failed;
} RenderJob;

static void *render_worker(void *arg) {
    RenderJob *job = arg;
    size_t total = job->figures->count * 2;
    Workspace *ws = workspace_new();
    for (;;) {
        long i = pf_counter_next(&job->next);
        if (i < 0 || (size_t)i >= total) {
            workspace_free(ws);
            return NULL;
        }
        Scene *scene = job->scenes[i];
        if (!scene) continue;
        static const char *const ext[3] = {".png", ".svg", ".pdf"};
        for (int k = 0; k < 3; k++) {
            if (!(job->formats & (1u << k))) continue;
            char *path = xasprintf("%s%s", job->paths[i], ext[k]);
            int ok = k == 0 ? render_png(scene, path, ws) : k == 1 ? render_svg(scene, path) : render_pdf(scene, path);
            if (!ok) {
                fprintf(stderr, "isocost: cannot write %s: %s\n", path, pf_last_error());
                job->failed = 1;
            }
            free(path);
        }
    }
}

/* --force: delete what the previous report in `out` wrote and this one did not.
   Only names from the old summary.json are touched, and only plain figure file
   names, so files a person put in the directory stay. */
static void remove_stale_outputs(const char *out, const char *old_summary, const Vec *figures, int data) {
    const char *p = old_summary;
    while ((p = strstr(p, "\"files\": [")) != NULL) {
        p += 10;
        while (*p && *p != ']') {
            const char *q = strchr(p, '"');
            if (!q) return;
            const char *e = strchr(q + 1, '"');
            if (!e) return;
            char *name = xstrndup(q + 1, (size_t)(e - q - 1));
            p = e + 1;
            while (*p == ',' || *p == ' ' || *p == '\n') p++;
            int plain = !strchr(name, '/') && !strchr(name, '\\') && name[0] != '.' &&
                        (ends_with(name, ".png") || ends_with(name, ".svg") || ends_with(name, ".pdf"));
            for (size_t i = 0; plain && i < figures->count; i++) {
                const Vec *files = &((const Figure *)figures->items[i])->files;
                for (size_t k = 0; plain && k < files->count; k++)
                    if (strcmp(files->items[k], name) == 0) plain = 0;
            }
            if (plain) {
                char *path = path_join(out, name);
                pf_remove(path);
                free(path);
            }
            free(name);
        }
    }
    if (!data && strstr(old_summary, "\"data\": true")) {
        char *path = path_join(out, "points.tsv");
        pf_remove(path);
        free(path);
    }
}

static int cmd_config(int argc, char **argv) {
    Config c;
    config_defaults(&c);
    if (argc >= 3 && strcmp(argv[2], "--check") == 0) {
        char error[600];
        if (argc < 4) {
            config_free(&c);
            return die("config --check needs a file");
        }
        if (!config_load(&c, argv[3], error, sizeof(error))) {
            fprintf(stderr, "isocost: %s\n", error);
            config_free(&c);
            return 2;
        }
        if (argc >= 5 && strcmp(argv[4], "--print") == 0) config_print(&c, stdout);
        else printf("%s: valid (%zu tools, %zu workloads, %zu groups)\n", argv[3], c.tool_count, c.workload_count, c.group_count);
        config_free(&c);
        return 0;
    }
    if (argc > 2) {
        config_free(&c);
        return die("usage: isocost config [--check FILE [--print]]");
    }
    config_print(&c, stdout);
    config_free(&c);
    return 0;
}

int main(int argc, char **argv) {
    static const char *const views[] = {"overview", "workloads", NULL};
    static const char *const formats[] = {"png", "svg", "pdf", NULL};
    Config config;
    char **inputs = NULL;
    size_t input_count = 0;
    Ingest ingest = {0};
    Vec figures = {0};
    ViewInfo *infos = NULL;
    Str command = {0};
    int status = 0, config_ready = 0;
    const char *out = NULL, *config_path = NULL;
    int list = 0, json = 0, force = 0, quiet = 0, no_config = 0, first = 1, ok = 1;
    unsigned char *old_summary = NULL;
    long jobs = 0;
    double t0 = pf_now_ms();
    font_init();

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        puts("isocost " ISOCOST_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "config") == 0) return cmd_config(argc, argv);
    inputs = xcalloc((size_t)argc + 1, sizeof(char *));
    if (strcmp(argv[1], "report") == 0) first = 2;
    if (strcmp(argv[1], "list") == 0) {
        list = 1;
        first = 2;
    }

    /* First pass: config selection, so the file can be loaded before overrides apply. */
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--no-config") == 0) no_config = 1;
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) quiet = 1;
    }
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        static const char *const with_value[] = {"-o", "--out", "--config", "--baseline", "--group", "--suite", "--runs", "--cases", "--statistic",
                                                 "--weights", "--cost", "--view", "--format", "--size", "--dpi", "--title", "-j", "--jobs", NULL};
        int takes = 0;
        for (int k = 0; with_value[k]; k++) takes |= strcmp(a, with_value[k]) == 0;
        if (takes) {
            i++;
            continue;
        }
        if (a[0] != '-' || !a[1]) inputs[input_count++] = path_clean(argv[i]);
    }

    config_defaults(&config);
    config_ready = 1;
    if (!no_config) {
        char *path = config_path ? xstrdup(config_path) : (input_count ? config_discover(inputs[0]) : NULL);
        if (path) {
            char error[600];
            if (!config_load(&config, path, error, sizeof(error))) {
                fprintf(stderr, "isocost: %s\n", error);
                free(path);
                status = 2;
                goto done;
            }
            if (!quiet && !config_path) fprintf(stderr, "isocost: using %s\n", path);
            free(path);
        }
    }

#define VALUE() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "isocost: %s needs a value\n", a), exit(2), ""))
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) continue;
        if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) out = VALUE();
        else if (strcmp(a, "--config") == 0) (void)VALUE();
        else if (strcmp(a, "--no-config") == 0) continue;
        else if (strcmp(a, "--baseline") == 0) set_string(&config.baseline, VALUE());
        else if (strcmp(a, "--group") == 0) set_glob(&config.groups, VALUE());
        else if (strcmp(a, "--suite") == 0) set_glob(&config.suites, VALUE());
        else if (strcmp(a, "--runs") == 0) {
            const char *v = VALUE();
            if (strcmp(v, "all") == 0) config.all_runs = 1;
            else if (strcmp(v, "latest") == 0) config.all_runs = 0;
            else { status = die("--runs takes latest or all"); goto done; }
        } else if (strcmp(a, "--cases") == 0) {
            const char *v = VALUE();
            if (strcmp(v, "common") == 0) config.common_cases = 1;
            else if (strcmp(v, "all") == 0) config.common_cases = 0;
            else { status = die("--cases takes all or common"); goto done; }
        } else if (strcmp(a, "--statistic") == 0) {
            const char *v = VALUE();
            if (strcmp(v, "median") == 0) config.statistic = STAT_MEDIAN;
            else if (strcmp(v, "mean") == 0) config.statistic = STAT_MEAN;
            else { status = die("--statistic takes median or mean"); goto done; }
        } else if (strcmp(a, "--weights") == 0) {
            char *end;
            const char *v = VALUE();
            config.time_weight = strtod(v, &end);
            if (*end != ',' || !(config.time_weight >= 0)) { status = die("--weights takes T,M"); goto done; }
            config.memory_weight = strtod(end + 1, &end);
            if (*end || !(config.memory_weight >= 0) || config.time_weight + config.memory_weight <= 0)
                { status = die("--weights takes two non-negative numbers that are not both 0"); goto done; }
        } else if (strcmp(a, "--cost") == 0) {
            const char *v = VALUE();
            if (strcmp(v, "geometric") == 0) config.cost_model = COST_GEOMETRIC;
            else if (strcmp(v, "linear") == 0) config.cost_model = COST_LINEAR;
            else { status = die("--cost takes geometric or linear"); goto done; }
        } else if (strcmp(a, "--view") == 0) config.views = parse_list(VALUE(), views, "view", &ok);
        else if (strcmp(a, "--format") == 0) config.formats = parse_list(VALUE(), formats, "format", &ok);
        else if (strcmp(a, "--size") == 0) set_string(&config.size, VALUE());
        else if (strcmp(a, "--dpi") == 0) {
            char *end;
            config.dpi = strtod(VALUE(), &end);
            if (*end || config.dpi < 0 || config.dpi > 2400) { status = die("--dpi takes a number from 1 to 2400"); goto done; }
        } else if (strcmp(a, "--title") == 0) set_string(&config.title, VALUE());
        else if (strcmp(a, "--data") == 0) config.data = 1;
        else if (strcmp(a, "--force") == 0) force = 1;
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) quiet = 1;
        else if (strcmp(a, "--json") == 0) json = 1;
        else if (strcmp(a, "-j") == 0 || strcmp(a, "--jobs") == 0) {
            char *end;
            jobs = strtol(VALUE(), &end, 10);
            if (*end || jobs < 1 || jobs > 256) { status = die("--jobs takes a number from 1 to 256"); goto done; }
        } else {
            fprintf(stderr, "isocost: unknown option %s (see isocost --help)\n", a);
            status = 2;
            goto done;
        }
    }
#undef VALUE
    if (!ok || !config.views || !config.formats) { status = die("--view and --format need at least one valid value"); goto done; }
    if (!input_count) {
        usage(stderr);
        status = 2;
        goto done;
    }
    FigureSize size;
    char size_error[300];
    if (!figure_size(config.size, config.dpi, &size, size_error, sizeof(size_error))) {
        fprintf(stderr, "isocost: --size: %s\n", size_error);
        status = 2;
        goto done;
    }
    if (!list) {
        if (!out) { status = die("choose an output directory with -o DIR, or use `isocost list`"); goto done; }
        int kind = pf_kind(out);
        if (kind != PF_NONE && kind != PF_DIR) { status = die("the output path exists and is not a directory"); goto done; }
        char *existing = path_join(out, "report.md");
        int has_report = pf_kind(existing) == PF_FILE;
        free(existing);
        if (has_report && force) {
            char *path = path_join(out, "summary.json");
            size_t length;
            const char *error;
            if (!pf_read_file(path, &old_summary, &length, (size_t)64 << 20, &error)) old_summary = NULL;
            free(path);
        }
        if (has_report && !force) {
            fprintf(stderr, "isocost: %s already holds a report; pass --force to replace it\n", out);
            status = 2;
            goto done;
        }
        if (kind == PF_NONE && !pf_mkdir_all(out)) {
            fprintf(stderr, "isocost: cannot create %s: %s\n", out, pf_last_error());
            status = 2;
            goto done;
        }
    }

    int threads = jobs > 0 ? (int)jobs : (pf_cpu_count() > 8 ? 8 : pf_cpu_count());
    int ingest_ok = ingest_run(&config, inputs, input_count, threads, &ingest);
    double t1 = pf_now_ms();
    if (!ingest_ok) {
        for (size_t i = 0; i < ingest.errors.count; i++) fprintf(stderr, "isocost: %s\n", (char *)ingest.errors.items[i]);
        status = ingest.exit_code ? ingest.exit_code : 3;
        goto done;
    }
    int analysis_ok = build_figures(&ingest, &config, &figures);
    resolve_styles(&figures, &config);
    double t2 = pf_now_ms();
    status = analysis_ok ? 0 : 3;

    str_append(&command, "isocost");
    for (int i = 1; i < argc; i++) str_printf(&command, " %s", argv[i]);
    infos = xcalloc(figures.count + 1, sizeof(ViewInfo));
    ReportContext rc = {out, str_cstr(&command), &config, &ingest, &figures, infos, t1 - t0, t2 - t1, 0, status};

    if (list) {
        if (json) {
            print_summary_json(&rc, stdout);
        } else {
            printf("%zu Zebrac result file(s), %zu result(s), %zu comparison(s)\n", ingest.parsed.files_read, ingest.parsed.records.count, figures.count);
            for (size_t i = 0; i < figures.count; i++) {
                Figure *f = figures.items[i];
                printf("\n%s  [suite=%s group=%s run=%s] -> %s\n", f->title, f->suite, f->group, f->run, f->slug);
                if (f->baseline >= 0)
                    printf("  reference: %s%s, %zu of %zu workloads\n", f->variants[f->baseline].label, f->baseline_auto ? " (automatic)" : "",
                           f->referenced_cases, f->case_count);
                printf("  tools:");
                for (size_t v = 0; v < f->variant_count; v++)
                    printf(" %s(%zu)%s", f->variants[v].id, f->variants[v].n, f->variants[v].hidden ? "[hidden]" : "");
                printf("\n");
                for (size_t k = 0; k < f->diagnostics.count; k++) printf("  note: %s\n", (char *)f->diagnostics.items[k]);
            }
            printf("\nidentity rules:");
            for (size_t i = 0; i < ingest.rule_counts.capacity; i++)
                if (ingest.rule_counts.keys && ingest.rule_counts.keys[i])
                    printf(" %s=%zu", ingest.rule_counts.keys[i], *(size_t *)ingest.rule_counts.values[i]);
            printf("\n");
            for (size_t i = 0; i < ingest.skipped_runs.count; i++) printf("older run not reported: %s\n", (char *)ingest.skipped_runs.items[i]);
        }
    } else {
        canvas_init();
        RenderJob job = {&figures, xcalloc(figures.count * 2 + 1, sizeof(Scene *)), xcalloc(figures.count * 2 + 1, sizeof(char *)), config.formats, {0}, 0};
        static const char *const ext[3] = {".png", ".svg", ".pdf"};
        for (size_t i = 0; i < figures.count; i++) {
            Figure *f = figures.items[i];
            if (f->fatal) continue;
            for (int view = 0; view < 2; view++) {
                if (!(config.views & (1u << view))) continue;
                ViewInfo scratch = {0};
                const char *suffix = view ? "workloads" : "overview";
                job.scenes[i * 2 + (size_t)view] = view ? view_workloads(f, &config, &size, &scratch) : view_overview(f, &config, &size, &infos[i]);
                char *base = xasprintf("%s-%s", f->slug, suffix);
                job.paths[i * 2 + (size_t)view] = path_join(out, base);
                for (int k = 0; k < 3; k++)
                    if (config.formats & (1u << k)) vec_push(&f->files, xasprintf("%s%s", base, ext[k]));
                free(base);
            }
        }
        int render_threads = threads;
        if ((size_t)render_threads > figures.count * 2) render_threads = (int)(figures.count * 2);
        pf_run_threads(render_threads, render_worker, &job);
        double t3 = pf_now_ms();
        rc.render_ms = t3 - t2;
        if (job.failed) status = 2;
        rc.exit_status = status;
        if (!write_report_md(&rc) || !write_summary_json(&rc) || (config.data && !write_points_tsv(&rc))) {
            fprintf(stderr, "isocost: cannot write the report in %s: %s\n", out, pf_last_error());
            status = 2;
        } else if (old_summary) {
            remove_stale_outputs(out, (const char *)old_summary, &figures, config.data);
        }
        size_t drawn = 0;
        for (size_t i = 0; i < figures.count; i++) {
            Figure *f = figures.items[i];
            if (f->fatal) {
                fprintf(stderr, "isocost: %s not drawn:\n", f->title);
                for (size_t k = 0; k < f->diagnostics.count; k++) fprintf(stderr, "  %s\n", (char *)f->diagnostics.items[k]);
            } else {
                drawn++;
            }
        }
        if (!quiet)
            fprintf(stderr, "isocost: %zu files, %zu results, %zu comparison(s) drawn in %s (read %.1f ms, analysis %.1f ms, figures %.1f ms)\n",
                    ingest.parsed.files_read, ingest.parsed.records.count, drawn, out, t1 - t0, t2 - t1, t3 - t2);
        for (size_t i = 0; i < figures.count * 2; i++) {
            scene_free(job.scenes[i]);
            free(job.paths[i]);
        }
        free(job.scenes);
        free(job.paths);
    }

done:
    free(old_summary);
    for (size_t i = 0; i < figures.count; i++) figure_free(figures.items[i]);
    vec_free(&figures);
    ingest_free(&ingest);
    free(infos);
    str_free(&command);
    if (config_ready) config_free(&config);
    for (size_t i = 0; i < input_count; i++) free(inputs[i]);
    free(inputs);
    return status;
}
