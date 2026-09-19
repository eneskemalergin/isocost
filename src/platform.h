/*
 * Operating-system calls used by isocost. Everything else is portable C17.
 * src/platform_posix.c implements this for Linux (and other POSIX systems).
 */
#ifndef ISOCOST_PLATFORM_H
#define ISOCOST_PLATFORM_H

#include <stddef.h>

enum { PF_NONE, PF_FILE, PF_DIR, PF_OTHER };

int pf_kind(const char *path);                 /* follows symlinks */
int pf_kind_nofollow(const char *path);        /* PF_DIR only for a real directory */

/* Calls visit(ctx, name) for each entry except "." and "..". Returns 0 when the directory cannot be read. */
int pf_list_dir(const char *path, void (*visit)(void *ctx, const char *name), void *ctx);

int pf_mkdir(const char *path);                /* 1 on success or when it already exists */
int pf_mkdir_all(const char *path);            /* like mkdir -p */
/* Read up to limit bytes; data is NUL-terminated. Returns 0 and sets *error on failure. */
int pf_read_file(const char *path, unsigned char **data, size_t *length, size_t limit, const char **error);
/* Write a sibling temporary file and rename it over path. */
int pf_write_file_atomic(const char *path, const void *data, size_t length);
/* Delete a file. 1 on success or when it does not exist. */
int pf_remove(const char *path);
const char *pf_last_error(void);

double pf_now_ms(void);
int pf_cpu_count(void);

/* Run worker(arg) on count threads and wait. Falls back to the calling thread. */
void pf_run_threads(int count, void *(*worker)(void *), void *arg);

/* Atomic counter shared by render workers. */
typedef struct { volatile long value; } PfCounter;
long pf_counter_next(PfCounter *counter);

#endif
