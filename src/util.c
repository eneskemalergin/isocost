#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(void) {
    fputs("isocost: out of memory\n", stderr);
    exit(2);
}

void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p;
    if (size && count > SIZE_MAX / size) out_of_memory();
    p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *pointer, size_t size) {
    void *p = realloc(pointer, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

char *xstrdup(const char *text) {
    if (!text) text = "";
    return xstrndup(text, strlen(text));
}

char *xstrndup(const char *text, size_t length) {
    char *copy = xmalloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

char *xasprintf(const char *format, ...) {
    va_list args, copy;
    int length;
    char *out;
    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return xstrdup("");
    }
    out = xmalloc((size_t)length + 1);
    vsnprintf(out, (size_t)length + 1, format, args);
    va_end(args);
    return out;
}

static void str_reserve(Str *s, size_t extra) {
    size_t need = s->length + extra + 1, cap;
    if (need <= s->capacity) return;
    cap = s->capacity ? s->capacity : 256;
    while (cap < need) cap *= 2;
    s->data = xrealloc(s->data, cap);
    s->capacity = cap;
}

void str_appendn(Str *s, const char *text, size_t length) {
    str_reserve(s, length);
    memcpy(s->data + s->length, text, length);
    s->length += length;
    s->data[s->length] = '\0';
}

void str_append(Str *s, const char *text) { str_appendn(s, text, strlen(text)); }

void str_printf(Str *s, const char *format, ...) {
    va_list args, copy;
    int length;
    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length > 0) {
        str_reserve(s, (size_t)length);
        vsnprintf(s->data + s->length, (size_t)length + 1, format, args);
        s->length += (size_t)length;
    }
    va_end(args);
}

void str_free(Str *s) {
    free(s->data);
    s->data = NULL;
    s->length = s->capacity = 0;
}

const char *str_cstr(const Str *s) { return s->data ? s->data : ""; }

void vec_push(Vec *v, void *item) {
    if (v->count == v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 16;
        v->items = xrealloc(v->items, v->capacity * sizeof(void *));
    }
    v->items[v->count++] = item;
}

void vec_free(Vec *v) {
    free(v->items);
    v->items = NULL;
    v->count = v->capacity = 0;
}

static void merge_pass(char *src, char *dst, size_t count, size_t size, size_t width,
                       int (*cmp)(const void *, const void *, const void *), const void *ctx) {
    for (size_t lo = 0; lo < count; lo += 2 * width) {
        size_t mid = lo + width < count ? lo + width : count;
        size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
        size_t i = lo, j = mid, k = lo;
        while (i < mid && j < hi) {
            if (cmp(src + j * size, src + i * size, ctx) < 0) memcpy(dst + k++ * size, src + j++ * size, size);
            else memcpy(dst + k++ * size, src + i++ * size, size);
        }
        while (i < mid) memcpy(dst + k++ * size, src + i++ * size, size);
        while (j < hi) memcpy(dst + k++ * size, src + j++ * size, size);
    }
}

void sort_ctx(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *, const void *), const void *ctx) {
    char *a = base, *b, *src, *dst;
    if (count < 2) return;
    b = xmalloc(count * size);
    src = a;
    dst = b;
    for (size_t width = 1; width < count; width *= 2) {
        char *t;
        merge_pass(src, dst, count, size, width, cmp, ctx);
        t = src;
        src = dst;
        dst = t;
    }
    if (src != a) memcpy(a, src, count * size);
    free(b);
}

void vec_sort(Vec *v, int (*cmp)(const void *, const void *, const void *), const void *ctx) {
    if (v->count > 1) sort_ctx(v->items, v->count, sizeof(void *), cmp, ctx);
}

static uint64_t hash_text(const char *text) {
    uint64_t h = 1469598103934665603ULL;
    for (; *text; text++) {
        h ^= (unsigned char)*text;
        h *= 1099511628211ULL;
    }
    return h;
}

static void map_grow(Map *m) {
    size_t old = m->capacity;
    char **keys = m->keys;
    void **values = m->values;
    m->capacity = old ? old * 2 : 64;
    m->keys = xcalloc(m->capacity, sizeof(char *));
    m->values = xcalloc(m->capacity, sizeof(void *));
    m->count = 0;
    for (size_t i = 0; i < old; i++) {
        if (!keys[i]) continue;
        size_t j = hash_text(keys[i]) & (m->capacity - 1);
        while (m->keys[j]) j = (j + 1) & (m->capacity - 1);
        m->keys[j] = keys[i];
        m->values[j] = values[i];
        m->count++;
    }
    free(keys);
    free(values);
}

static long map_slot(const Map *m, const char *key) {
    size_t j;
    if (!m->capacity) return -1;
    j = hash_text(key) & (m->capacity - 1);
    while (m->keys[j]) {
        if (strcmp(m->keys[j], key) == 0) return (long)j;
        j = (j + 1) & (m->capacity - 1);
    }
    return -1;
}

void *map_get(const Map *m, const char *key) {
    long j = map_slot(m, key);
    return j < 0 ? NULL : m->values[j];
}

int map_has(const Map *m, const char *key) { return map_slot(m, key) >= 0; }

int map_put(Map *m, const char *key, void *value) {
    size_t j;
    if ((m->count + 1) * 2 > m->capacity) map_grow(m);
    j = hash_text(key) & (m->capacity - 1);
    while (m->keys[j]) {
        if (strcmp(m->keys[j], key) == 0) {
            m->values[j] = value;
            return 0;
        }
        j = (j + 1) & (m->capacity - 1);
    }
    m->keys[j] = xstrdup(key);
    m->values[j] = value;
    m->count++;
    return 1;
}

void map_free(Map *m) {
    for (size_t i = 0; i < m->capacity; i++) free(m->keys[i]);
    free(m->keys);
    free(m->values);
    memset(m, 0, sizeof(*m));
}

const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *path_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

char *path_tail(const char *path, int components) {
    const char *p = path + strlen(path);
    int seen = 0;
    while (p > path) {
        p--;
        if (*p == '/' && ++seen == components) return xstrdup(p + 1);
    }
    return xstrdup(path);
}

char *path_stem(const char *path) {
    const char *base = path_base(path);
    const char *dot = strrchr(base, '.');
    return dot && dot != base ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
}

char *path_join(const char *dir, const char *name) {
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '/') return xasprintf("%s%s", dir, name);
    return xasprintf("%s/%s", dir, name);
}

char *path_clean(const char *path) {
    Str s = {0};
    for (const char *p = path; *p; p++) {
        if (*p == '/' && s.length && s.data[s.length - 1] == '/') continue;
        str_appendn(&s, p, 1);
    }
    while (s.length > 1 && s.data[s.length - 1] == '/') s.data[--s.length] = '\0';
    char *out = xstrdup(str_cstr(&s));
    str_free(&s);
    return out;
}

int ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && memcmp(text + a - b, suffix, b) == 0;
}

int starts_with(const char *text, const char *prefix) { return strncmp(text, prefix, strlen(prefix)) == 0; }

char *slugify(const char *text) {
    Str s = {0};
    int dash = 0;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (isalnum(c) || c == '.' || c == '_') {
            char lower = (char)tolower(c);
            str_appendn(&s, &lower, 1);
            dash = 0;
        } else if (!dash && s.length) {
            str_append(&s, "-");
            dash = 1;
        }
    }
    while (s.length && s.data[s.length - 1] == '-') s.data[--s.length] = '\0';
    if (!s.length) str_append(&s, "figure");
    return s.data;
}

const void *find_bytes(const void *haystack, size_t length, const char *needle) {
    size_t n = strlen(needle);
    const unsigned char *h = haystack;
    if (!n) return haystack;
    for (size_t i = 0; i + n <= length; i++) {
        const unsigned char *hit = memchr(h + i, (unsigned char)needle[0], length - n - i + 1);
        if (!hit) return NULL;
        i = (size_t)(hit - h);
        if (memcmp(hit, needle, n) == 0) return hit;
    }
    return NULL;
}

/* Iterative glob with backtracking to the last '*'. */
int glob_match(const char *pattern, const char *text) {
    const char *star = NULL, *resume = NULL;
    while (*text) {
        if (*pattern == '*') {
            star = pattern++;
            resume = text;
        } else if (*pattern == '?' || *pattern == *text) {
            pattern++;
            text++;
        } else if (star) {
            pattern = star + 1;
            text = ++resume;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}

size_t edit_distance(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    size_t *row = xcalloc(m + 1, sizeof(size_t));
    for (size_t j = 0; j <= m; j++) row[j] = j;
    for (size_t i = 1; i <= n; i++) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= m; j++) {
            size_t up = row[j];
            size_t best = diag + (a[i - 1] != b[j - 1]);
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    size_t d = row[m];
    free(row);
    return d;
}

int natural_compare(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            size_t na = 0, nb = 0;
            while (isdigit((unsigned char)a[na])) na++;
            while (isdigit((unsigned char)b[nb])) nb++;
            if (na != nb) return na < nb ? -1 : 1;
            int c = strncmp(a, b, na);
            if (c) return c;
            a += na;
            b += nb;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int cmp_cstr(const void *a, const void *b, const void *ctx) {
    (void)ctx;
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int cmp_double(const void *a, const void *b, const void *ctx) {
    double x = *(const double *)a, y = *(const double *)b;
    (void)ctx;
    return (x > y) - (x < y);
}

/* Hyndman-Fan type 7, the same definition the original awk reference used. */
double quantile7(const double *sorted, size_t n, double p) {
    double position, fraction;
    size_t lower, upper;
    if (n == 0) return 0.0;
    position = 1.0 + (double)(n - 1) * p;
    lower = (size_t)position;
    fraction = position - (double)lower;
    upper = fraction > 0 ? lower + 1 : lower;
    if (upper > n) upper = n;
    return sorted[lower - 1] + (sorted[upper - 1] - sorted[lower - 1]) * fraction;
}

void format_number(char *out, size_t size, double v) {
    double a = fabs(v);
    int decimals;
    if (!isfinite(v)) {
        snprintf(out, size, "n/a");
        return;
    }
    if (a >= 100 || a == 0) decimals = 0;
    else if (a >= 10) decimals = 1;
    else if (a >= 1) decimals = 2;
    else decimals = (int)ceil(-log10(a)) + 1;
    if (decimals > 6) decimals = 6;
    snprintf(out, size, "%.*f", decimals, v);
    if (strchr(out, '.')) {
        size_t n = strlen(out);
        while (n && out[n - 1] == '0') out[--n] = '\0';
        if (n && out[n - 1] == '.') out[--n] = '\0';
    }
}
