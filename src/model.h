/*
 * Data model: Zebrac results (Record), what was read (Ingest), and one
 * comparison per figure (Figure) with its tools (Variant) and per-workload
 * points (CasePoint).
 */
#ifndef ISOCOST_MODEL_H
#define ISOCOST_MODEL_H

#include "config.h"
#include "util.h"

#define ISOCOST_VERSION "1.0.0"

enum { REC_OK, REC_FAILED, REC_INCOMPLETE, REC_BAD_UNIT };

typedef struct {
    int has_median, has_mean;
    double median, mean;       /* canonical units: nanoseconds or bytes */
    double q1, q3;             /* NAN when absent */
    char unit[24];             /* unit as written by Zebrac */
    int present;               /* the selected statistic exists */
    int statistic;             /* STAT_MEDIAN or STAT_MEAN actually used */
    double estimate;           /* the selected value */
} Metric;

typedef struct {
    char *path;
    int index;                 /* result index inside the file */
    int results_in_file;
    char *command;
    char *argv0;
    double samples;            /* NAN when absent */
    double failed;
    Metric time;
    Metric memory;
    char *zebrac_version;
    double duration_ms, min_samples, warmup;
    int status;
    char reason[128];

    /* identity */
    char *project;
    char *suite;
    char *group;
    char *run;
    char *case_id;
    char *variant;
    char *family;              /* tool without configuration; styles and baseline aliases */
    const char *rule;
} Record;

typedef struct {
    Vec records;               /* Record* */
    size_t files_read;
    size_t files_skipped;      /* JSON that is not a Zebrac result document */
    size_t bytes_read;
} ParseSet;

typedef struct {
    char *raw_json;
    char *suite, *section, *workload, *tool, *tool_family, *command;
} SidecarRow;

/* zebrac.c */
int zebrac_parse_file(const char *path, ParseSet *set, char *error, size_t error_size);
void select_statistic(Record *r, int statistic);
int sidecar_parse_file(const char *path, Vec *out, char *error, size_t error_size);
void sidecar_row_free(SidecarRow *row);
void record_free(Record *r);

/* ingest.c */
typedef struct {
    ParseSet parsed;
    Vec excluded;              /* Record* dropped by filters or run selection */
    Vec skipped_runs;          /* char* */
    Vec warnings;              /* char* */
    Vec errors;                /* char* */
    Map rule_counts;           /* rule name -> size_t* */
    size_t sidecar_rows;
    size_t sidecar_missing;
    int exit_code;             /* 2 unreadable or malformed, 3 inconsistent */
} Ingest;

/* threads: parse files on at most this many threads (1 = the calling thread only). */
int ingest_run(const Config *config, char **inputs, size_t input_count, int threads, Ingest *ingest);
void ingest_free(Ingest *ingest);

/* analysis.c */
enum { CELL_NONE, CELL_OK, CELL_FAILED, CELL_INCOMPLETE, CELL_NO_REF, CELL_HIDDEN };

typedef struct {
    char *id;
    char *family;
    char *label;               /* display name */
    int order;
    int hidden;
    /* resolved style */
    Rgb color;
    int shape;
    int hollow;
    int line;
    int label_position;
    /* results */
    size_t n;
    size_t failed_cases, incomplete_cases;
    double failed_samples;
    double gx, gy;             /* geometric means of the ratios */
    double xlo, xhi, ylo, yhi; /* spread bounds (quartiles or range) */
    double cost;
    int pareto;
    int selected;
    size_t case_pareto;
    int has_ellipse;
    double e_cx, e_cy, e_a, e_b, e_theta; /* natural-log space */
} Variant;

typedef struct {
    size_t variant;
    size_t kase;
    const Record *record;
    const Record *reference;
    double x, y, cost;
    int pareto;
} CasePoint;

typedef struct {
    char *id;
    char *label;
    int order;
} Workload;

typedef struct {
    char *project, *suite, *group, *run;
    char *title, *subtitle;    /* resolved; subtitle NULL = automatic */
    char *slug;
    Vec records;               /* Record* */
    Workload *cases;
    size_t case_count;
    Variant *variants;
    size_t variant_count;
    long baseline;             /* variant index, -1 when absent */
    int baseline_auto;
    CasePoint *points;
    size_t point_count;
    unsigned char *cells;      /* case_count x variant_count */
    size_t referenced_cases;
    size_t max_n;
    Vec diagnostics;           /* char* */
    int fatal;
    int hidden;
    Vec files;                 /* char* figure files written */
} Figure;

int build_figures(Ingest *ingest, const Config *config, Vec *figures);
void resolve_styles(Vec *figures, const Config *config);
double cost_of(const Config *config, double x, double y);
void figure_free(Figure *f);
/* Expand {project} {suite} {group} {run} {reference} {workloads}; empty parts and their separators collapse. */
char *expand_template(const char *template_text, const Figure *f);

#endif
