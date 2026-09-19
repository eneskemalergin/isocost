/* Allocation, strings, containers, and small numeric helpers. Portable C17. */
#ifndef ISOCOST_UTIL_H
#define ISOCOST_UTIL_H

#include <stddef.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define ISO_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define ISO_PRINTF(a, b)
#endif

/* Allocation failures exit with status 2; callers never see NULL. */
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *pointer, size_t size);
char *xstrdup(const char *text);
char *xstrndup(const char *text, size_t length);
char *xasprintf(const char *format, ...) ISO_PRINTF(1, 2);

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Str;

void str_append(Str *s, const char *text);
void str_appendn(Str *s, const char *text, size_t length);
void str_printf(Str *s, const char *format, ...) ISO_PRINTF(2, 3);
void str_free(Str *s);
const char *str_cstr(const Str *s); /* never NULL */

typedef struct {
    void **items;
    size_t count;
    size_t capacity;
} Vec;

void vec_push(Vec *v, void *item);
void vec_free(Vec *v);
/* Stable merge sort; cmp receives pointers to the elements and ctx. */
void sort_ctx(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *, const void *), const void *ctx);
void vec_sort(Vec *v, int (*cmp)(const void *, const void *, const void *), const void *ctx);

typedef struct {
    char **keys;
    void **values;
    size_t capacity;
    size_t count;
} Map;

void *map_get(const Map *m, const char *key);
int map_has(const Map *m, const char *key);
int map_put(Map *m, const char *key, void *value); /* 1 when the key is new */
void map_free(Map *m);

const char *path_base(const char *path);
char *path_dir(const char *path);
char *path_tail(const char *path, int components);
char *path_stem(const char *path);
char *path_join(const char *dir, const char *name);
/* Collapse repeated slashes and drop a trailing slash ("a//b/" -> "a/b"). */
char *path_clean(const char *path);
int ends_with(const char *text, const char *suffix);
int starts_with(const char *text, const char *prefix);
char *slugify(const char *text);
const void *find_bytes(const void *haystack, size_t length, const char *needle);
int glob_match(const char *pattern, const char *text); /* '*' and '?' */
size_t edit_distance(const char *a, const char *b);

int cmp_cstr(const void *a, const void *b, const void *ctx);   /* elements are char* */
/* strcmp, except runs of digits compare as numbers: "5mb" < "10mb". */
int natural_compare(const char *a, const char *b);
int cmp_double(const void *a, const void *b, const void *ctx); /* elements are double */
double quantile7(const double *sorted, size_t n, double p);

/* "1.23", "12.3", "123": three significant digits, no exponent, no trailing zeros. */
void format_number(char *out, size_t size, double value);

#endif
