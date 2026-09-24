/* report.md, summary.json (isocost.report.v1), and points.tsv. */
#ifndef ISOCOST_REPORT_H
#define ISOCOST_REPORT_H

#include "model.h"
#include "plot.h"

typedef struct {
    const char *out_dir;
    const char *command_line;
    const Config *config;
    const Ingest *ingest;
    const Vec *figures;          /* Figure* */
    const ViewInfo *overview;    /* one per figure */
    double read_ms, analysis_ms, render_ms;
    int exit_status;
} ReportContext;

int write_report_md(const ReportContext *r);
int write_summary_json(const ReportContext *r);
int write_points_tsv(const ReportContext *r);
/* The same summary as JSON on stdout, for `isocost list --json`. */
void print_summary_json(const ReportContext *r, FILE *out);

#endif
