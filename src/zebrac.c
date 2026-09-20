/*
 * Zebrac summary JSON and bench.meta.v1 JSONL reader.
 *
 * Strict JSON: key order and whitespace are free, unknown keys are skipped
 * without building a tree, and malformed input is rejected with the byte
 * offset. Only the fields a report needs are kept.
 */
#include "model.h"
#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 128
#define MAX_INPUT_BYTES (256ULL * 1024ULL * 1024ULL)
#define MAX_STRING_BYTES (1024ULL * 1024ULL)
#define MAX_RESULTS 100000

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    char error[192];
} Parser;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

static void set_error(Parser *parser, const char *message) {
    if (parser->error[0] == '\0') {
        (void)snprintf(parser->error, sizeof(parser->error), "at byte %zu: %s",
                       parser->position, message);
    }
}

static void set_expected_error(Parser *parser, unsigned char expected) {
    if (parser->error[0] == '\0') {
        (void)snprintf(parser->error, sizeof(parser->error),
                       "at byte %zu: expected '%c'", parser->position, expected);
    }
}

static int buffer_reserve(Buffer *buffer, size_t extra) {
    size_t required;
    size_t capacity;
    char *data;

    if (extra > MAX_STRING_BYTES - buffer->length - 1U) {
        return 0;
    }
    required = buffer->length + extra + 1U;
    if (required <= buffer->capacity) {
        return 1;
    }
    capacity = buffer->capacity == 0U ? 64U : buffer->capacity;
    while (capacity < required) {
        if (capacity > MAX_STRING_BYTES / 2U) {
            capacity = MAX_STRING_BYTES;
            break;
        }
        capacity *= 2U;
    }
    data = (char *)realloc(buffer->data, capacity);
    if (data == NULL) {
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append_bytes(Buffer *buffer, const unsigned char *bytes, size_t count) {
    if (!buffer_reserve(buffer, count)) return 0;
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int buffer_append_byte(Buffer *buffer, unsigned char value) {
    if (!buffer_reserve(buffer, 1U)) {
        return 0;
    }
    buffer->data[buffer->length++] = (char)value;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int buffer_append_utf8(Buffer *buffer, uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        return buffer_append_byte(buffer, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7ffU) {
        return buffer_reserve(buffer, 2U) &&
            buffer_append_byte(buffer, (unsigned char)(0xc0U | (codepoint >> 6U))) &&
            buffer_append_byte(buffer, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    }
    if (codepoint <= 0xffffU) {
        return buffer_reserve(buffer, 3U) &&
            buffer_append_byte(buffer, (unsigned char)(0xe0U | (codepoint >> 12U))) &&
            buffer_append_byte(buffer, (unsigned char)(0x80U | ((codepoint >> 6U) & 0x3fU))) &&
            buffer_append_byte(buffer, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    }
    return buffer_reserve(buffer, 4U) &&
        buffer_append_byte(buffer, (unsigned char)(0xf0U | (codepoint >> 18U))) &&
        buffer_append_byte(buffer, (unsigned char)(0x80U | ((codepoint >> 12U) & 0x3fU))) &&
        buffer_append_byte(buffer, (unsigned char)(0x80U | ((codepoint >> 6U) & 0x3fU))) &&
        buffer_append_byte(buffer, (unsigned char)(0x80U | (codepoint & 0x3fU)));
}

static void buffer_free(Buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a' + 10U);
    if (value >= 'A' && value <= 'F') return (int)(value - 'A' + 10U);
    return -1;
}

static void skip_whitespace(Parser *parser) {
    while (parser->position < parser->length &&
           (parser->data[parser->position] == ' ' ||
            parser->data[parser->position] == '\t' ||
            parser->data[parser->position] == '\n' ||
            parser->data[parser->position] == '\r')) {
        parser->position++;
    }
}

static int consume(Parser *parser, unsigned char expected) {
    skip_whitespace(parser);
    if (parser->position >= parser->length ||
        parser->data[parser->position] != expected) {
        set_expected_error(parser, expected);
        return 0;
    }
    parser->position++;
    return 1;
}

static int consume_literal(Parser *parser, const char *literal) {
    size_t length = strlen(literal);

    skip_whitespace(parser);
    if (parser->position + length > parser->length ||
        memcmp(parser->data + parser->position, literal, length) != 0) {
        set_error(parser, "invalid JSON literal");
        return 0;
    }
    parser->position += length;
    return 1;
}

static int parse_string(Parser *parser, char **output) {
    Buffer buffer = {0};
    uint32_t codepoint;
    uint32_t high_surrogate;
    unsigned char value;
    int digit;
    int i;

    *output = NULL;
    skip_whitespace(parser);
    if (parser->position >= parser->length ||
        parser->data[parser->position] != '"') {
        set_error(parser, "expected JSON string");
        return 0;
    }
    parser->position++;
    while (parser->position < parser->length) {
        value = parser->data[parser->position++];
        if (value == '"') {
            if (buffer.data == NULL && !buffer_append_byte(&buffer, '\0')) {
                set_error(parser, "out of memory");
                buffer_free(&buffer);
                return 0;
            }
            if (buffer.data != NULL && buffer.length > 0U &&
                buffer.data[buffer.length - 1U] == '\0') {
                buffer.length--;
            }
            *output = buffer.data;
            return 1;
        }
        if (value < 0x20U) {
            set_error(parser, "control character in JSON string");
            buffer_free(&buffer);
            return 0;
        }
        if (value != '\\' && value != '"' && value >= 0x20U) {
            /* Copy the whole run of plain bytes at once. */
            size_t start = parser->position - 1U, end = parser->position;
            while (end < parser->length && parser->data[end] != '"' && parser->data[end] != '\\' && parser->data[end] >= 0x20U) end++;
            parser->position = end;
            if (!buffer_append_bytes(&buffer, parser->data + start, end - start)) {
                set_error(parser, "JSON string is too large");
                buffer_free(&buffer);
                return 0;
            }
            continue;
        }
        if (parser->position >= parser->length) {
            set_error(parser, "unfinished JSON escape");
            buffer_free(&buffer);
            return 0;
        }
        value = parser->data[parser->position++];
        switch (value) {
            case '"': if (!buffer_append_byte(&buffer, '"')) goto string_too_large; break;
            case '\\': if (!buffer_append_byte(&buffer, '\\')) goto string_too_large; break;
            case '/': if (!buffer_append_byte(&buffer, '/')) goto string_too_large; break;
            case 'b': if (!buffer_append_byte(&buffer, '\b')) goto string_too_large; break;
            case 'f': if (!buffer_append_byte(&buffer, '\f')) goto string_too_large; break;
            case 'n': if (!buffer_append_byte(&buffer, '\n')) goto string_too_large; break;
            case 'r': if (!buffer_append_byte(&buffer, '\r')) goto string_too_large; break;
            case 't': if (!buffer_append_byte(&buffer, '\t')) goto string_too_large; break;
            case 'u':
                codepoint = 0U;
                for (i = 0; i < 4; i++) {
                    if (parser->position >= parser->length ||
                        (digit = hex_value(parser->data[parser->position++])) < 0) {
                        set_error(parser, "invalid Unicode escape");
                        buffer_free(&buffer);
                        return 0;
                    }
                    codepoint = (codepoint << 4U) | (uint32_t)digit;
                }
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    high_surrogate = codepoint;
                    if (parser->position + 6U > parser->length ||
                        parser->data[parser->position++] != '\\' ||
                        parser->data[parser->position++] != 'u') {
                        set_error(parser, "high surrogate lacks a low surrogate");
                        buffer_free(&buffer);
                        return 0;
                    }
                    codepoint = 0U;
                    for (i = 0; i < 4; i++) {
                        if ((digit = hex_value(parser->data[parser->position++])) < 0) {
                            set_error(parser, "invalid low surrogate");
                            buffer_free(&buffer);
                            return 0;
                        }
                        codepoint = (codepoint << 4U) | (uint32_t)digit;
                    }
                    if (codepoint < 0xdc00U || codepoint > 0xdfffU) {
                        set_error(parser, "invalid low surrogate");
                        buffer_free(&buffer);
                        return 0;
                    }
                    codepoint = 0x10000U + ((high_surrogate - 0xd800U) << 10U) +
                        (codepoint - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    set_error(parser, "unpaired low surrogate");
                    buffer_free(&buffer);
                    return 0;
                }
                if (codepoint == 0U) {
                    set_error(parser, "NUL is not allowed in a normalized JSON string");
                    buffer_free(&buffer);
                    return 0;
                }
                if (!buffer_append_utf8(&buffer, codepoint)) goto string_too_large;
                break;
            default:
                set_error(parser, "unknown JSON escape");
                buffer_free(&buffer);
                return 0;
        }
    }
    set_error(parser, "unterminated JSON string");
    buffer_free(&buffer);
    return 0;

string_too_large:
    set_error(parser, "JSON string is too large");
    buffer_free(&buffer);
    return 0;
}

static int parse_number(Parser *parser, double *output) {
    size_t start;
    size_t length;
    size_t copied;
    char number[128];
    char *end;
    int exponent_digits;
    int fraction_digits;

    skip_whitespace(parser);
    start = parser->position;
    if (parser->position < parser->length && parser->data[parser->position] == '-') {
        parser->position++;
    }
    if (parser->position >= parser->length) {
        set_error(parser, "incomplete JSON number");
        return 0;
    }
    if (parser->data[parser->position] == '0') {
        parser->position++;
        if (parser->position < parser->length &&
            isdigit(parser->data[parser->position])) {
            set_error(parser, "leading zero in JSON number");
            return 0;
        }
    } else if (parser->data[parser->position] >= '1' &&
               parser->data[parser->position] <= '9') {
        while (parser->position < parser->length &&
               isdigit(parser->data[parser->position])) {
            parser->position++;
        }
    } else {
        set_error(parser, "invalid JSON number");
        return 0;
    }
    if (parser->position < parser->length && parser->data[parser->position] == '.') {
        parser->position++;
        fraction_digits = 0;
        while (parser->position < parser->length &&
               isdigit(parser->data[parser->position])) {
            parser->position++;
            fraction_digits++;
        }
        if (fraction_digits == 0) {
            set_error(parser, "JSON fraction has no digits");
            return 0;
        }
    }
    if (parser->position < parser->length &&
        (parser->data[parser->position] == 'e' ||
         parser->data[parser->position] == 'E')) {
        parser->position++;
        if (parser->position < parser->length &&
            (parser->data[parser->position] == '+' ||
             parser->data[parser->position] == '-')) {
            parser->position++;
        }
        exponent_digits = 0;
        while (parser->position < parser->length &&
               isdigit(parser->data[parser->position])) {
            parser->position++;
            exponent_digits++;
        }
        if (exponent_digits == 0) {
            set_error(parser, "JSON exponent has no digits");
            return 0;
        }
    }
    length = parser->position - start;
    if (length >= sizeof(number)) {
        set_error(parser, "JSON number is too long");
        return 0;
    }
    /* Without an exponent, 127 digits cannot overflow a double: a skipped value needs no conversion. */
    if (!output && !memchr(parser->data + start, 'e', length) && !memchr(parser->data + start, 'E', length)) return 1;
    copied = length;
    memcpy(number, parser->data + start, copied);
    number[copied] = '\0';
    errno = 0;
    end = NULL;
    double value = strtod(number, &end);
    if (output) *output = value;
    if (end == NULL || *end != '\0' || !isfinite(value)) {
        set_error(parser, "JSON number is not finite");
        return 0;
    }
    return 1;
}

static int parse_number_or_null(Parser *parser, double *value, int *present) {
    skip_whitespace(parser);
    if (parser->position + 4U <= parser->length &&
        memcmp(parser->data + parser->position, "null", 4U) == 0) {
        parser->position += 4U;
        *present = 0;
        return 1;
    }
    *present = 1;
    return parse_number(parser, value);
}

static int skip_value(Parser *parser, unsigned depth);

static int parse_array_skip(Parser *parser, unsigned depth) {
    int first = 1;

    if (depth > MAX_DEPTH || !consume(parser, '[')) return 0;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == ']') {
        parser->position++;
        return 1;
    }
    while (parser->position < parser->length) {
        if (!first && !consume(parser, ',')) return 0;
        first = 0;
        if (!skip_value(parser, depth + 1U)) return 0;
        skip_whitespace(parser);
        if (parser->position < parser->length && parser->data[parser->position] == ']') {
            parser->position++;
            return 1;
        }
    }
    set_error(parser, "unterminated JSON array");
    return 0;
}

static int parse_object_skip(Parser *parser, unsigned depth) {
    char *key = NULL;
    int first = 1;

    if (depth > MAX_DEPTH || !consume(parser, '{')) return 0;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == '}') {
        parser->position++;
        return 1;
    }
    while (parser->position < parser->length) {
        if (!first && !consume(parser, ',')) return 0;
        first = 0;
        if (!parse_string(parser, &key)) return 0;
        free(key);
        key = NULL;
        if (!consume(parser, ':') || !skip_value(parser, depth + 1U)) return 0;
        skip_whitespace(parser);
        if (parser->position < parser->length && parser->data[parser->position] == '}') {
            parser->position++;
            return 1;
        }
    }
    free(key);
    set_error(parser, "unterminated JSON object");
    return 0;
}

static int skip_value(Parser *parser, unsigned depth) {
    char *ignored = NULL;

    if (depth > MAX_DEPTH) {
        set_error(parser, "JSON nesting is too deep");
        return 0;
    }
    skip_whitespace(parser);
    if (parser->position >= parser->length) {
        set_error(parser, "missing JSON value");
        return 0;
    }
    switch (parser->data[parser->position]) {
        case '{': return parse_object_skip(parser, depth);
        case '[': return parse_array_skip(parser, depth);
        case '"':
            if (!parse_string(parser, &ignored)) return 0;
            free(ignored);
            return 1;
        case 't': return consume_literal(parser, "true");
        case 'f': return consume_literal(parser, "false");
        case 'n': return consume_literal(parser, "null");
        default: {
            return parse_number(parser, NULL);
        }
    }
}


/* ---- Zebrac documents --------------------------------------------------- */

typedef struct {
    double mean, median, q1, q3;
    int has_mean, has_median, has_q1, has_q3;
    char *unit;
} RawMetric;

typedef struct {
    char *command;
    char *argv0;
    double sample_count, failed_sample_count;
    int has_sample_count, has_failed;
    RawMetric wall_time, peak_rss;
} RawResult;

static int parse_nullable_string(Parser *parser, char **out) {
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == 'n') {
        *out = NULL;
        return consume_literal(parser, "null");
    }
    return parse_string(parser, out);
}

/* Iterate an object: calls field(key) positioned at the value; field must consume it. */
typedef int (*FieldFn)(Parser *parser, const char *key, void *ctx);

static int parse_object(Parser *parser, FieldFn field, void *ctx) {
    char *key = NULL;
    int first = 1;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == 'n') return consume_literal(parser, "null");
    if (!consume(parser, '{')) return 0;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == '}') {
        parser->position++;
        return 1;
    }
    while (parser->position < parser->length) {
        if (!first && !consume(parser, ',')) return 0;
        first = 0;
        if (!parse_string(parser, &key) || !consume(parser, ':')) {
            free(key);
            return 0;
        }
        int ok = field(parser, key, ctx);
        free(key);
        key = NULL;
        if (!ok) return 0;
        skip_whitespace(parser);
        if (parser->position < parser->length && parser->data[parser->position] == '}') {
            parser->position++;
            return 1;
        }
    }
    set_error(parser, "unterminated JSON object");
    return 0;
}

static int metric_field(Parser *parser, const char *key, void *ctx) {
    RawMetric *m = ctx;
    int present;
    if (strcmp(key, "mean") == 0) {
        if (!parse_number_or_null(parser, &m->mean, &present)) return 0;
        m->has_mean = present;
    } else if (strcmp(key, "median") == 0) {
        if (!parse_number_or_null(parser, &m->median, &present)) return 0;
        m->has_median = present;
    } else if (strcmp(key, "q1") == 0) {
        if (!parse_number_or_null(parser, &m->q1, &present)) return 0;
        m->has_q1 = present;
    } else if (strcmp(key, "q3") == 0) {
        if (!parse_number_or_null(parser, &m->q3, &present)) return 0;
        m->has_q3 = present;
    } else if (strcmp(key, "unit") == 0) {
        free(m->unit);
        m->unit = NULL;
        return parse_nullable_string(parser, &m->unit);
    } else {
        return skip_value(parser, 3U);
    }
    return 1;
}

static int parse_argv0(Parser *parser, char **argv0) {
    int first = 1;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == 'n') return consume_literal(parser, "null");
    if (!consume(parser, '[')) return 0;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == ']') {
        parser->position++;
        return 1;
    }
    while (parser->position < parser->length) {
        if (!first && !consume(parser, ',')) return 0;
        if (first) {
            char *item = NULL;
            if (!parse_string(parser, &item)) return 0;
            free(*argv0);
            *argv0 = item;
        } else if (!skip_value(parser, 3U)) {
            return 0;
        }
        first = 0;
        skip_whitespace(parser);
        if (parser->position < parser->length && parser->data[parser->position] == ']') {
            parser->position++;
            return 1;
        }
    }
    set_error(parser, "unterminated argv array");
    return 0;
}

static int result_field(Parser *parser, const char *key, void *ctx) {
    RawResult *r = ctx;
    int present;
    double number;
    if (strcmp(key, "command") == 0) {
        free(r->command);
        r->command = NULL;
        return parse_nullable_string(parser, &r->command);
    }
    if (strcmp(key, "argv") == 0) return parse_argv0(parser, &r->argv0);
    if (strcmp(key, "sample_count") == 0) {
        if (!parse_number_or_null(parser, &number, &present)) return 0;
        r->sample_count = number;
        r->has_sample_count = present;
        return 1;
    }
    if (strcmp(key, "failed_sample_count") == 0) {
        if (!parse_number_or_null(parser, &number, &present)) return 0;
        r->failed_sample_count = number;
        r->has_failed = present;
        return 1;
    }
    if (strcmp(key, "wall_time") == 0) return parse_object(parser, metric_field, &r->wall_time);
    if (strcmp(key, "peak_rss") == 0) return parse_object(parser, metric_field, &r->peak_rss);
    return skip_value(parser, 2U);
}

static void raw_result_free(RawResult *r) {
    free(r->command);
    free(r->argv0);
    free(r->wall_time.unit);
    free(r->peak_rss.unit);
    memset(r, 0, sizeof(*r));
}

typedef struct {
    double schema;
    int has_version;
    char *version;
    double duration, min_samples, warmup;
    int has_results;
    Vec results;             /* RawResult* */
} RawDocument;

static int config_field(Parser *parser, const char *key, void *ctx) {
    RawDocument *d = ctx;
    double *target = strcmp(key, "duration_ms") == 0 ? &d->duration
                   : strcmp(key, "min_samples") == 0 ? &d->min_samples
                   : strcmp(key, "warmup") == 0 ? &d->warmup : NULL;
    int present;
    double number;
    skip_whitespace(parser);
    if (!target || parser->position >= parser->length ||
        !(isdigit(parser->data[parser->position]) || parser->data[parser->position] == '-' || parser->data[parser->position] == 'n'))
        return skip_value(parser, 2U);
    if (!parse_number_or_null(parser, &number, &present)) return 0;
    if (present) *target = number;
    return 1;
}

static int document_field(Parser *parser, const char *key, void *ctx) {
    RawDocument *d = ctx;
    if (strcmp(key, "schema_version") == 0) {
        int present;
        skip_whitespace(parser);
        if (parser->position < parser->length && parser->data[parser->position] == '"') {
            d->schema = -1;
            return skip_value(parser, 1U);
        }
        if (!parse_number_or_null(parser, &d->schema, &present)) return 0;
        if (!present) d->schema = NAN;
        return 1;
    }
    if (strcmp(key, "zebrac_version") == 0) {
        free(d->version);
        d->version = NULL;
        d->has_version = 1;
        return parse_nullable_string(parser, &d->version);
    }
    if (strcmp(key, "config") == 0) return parse_object(parser, config_field, d);
    if (strcmp(key, "results") == 0) {
        int first = 1;
        d->has_results = 1;
        if (!consume(parser, '[')) return 0;
        skip_whitespace(parser);
        if (parser->position < parser->length && parser->data[parser->position] == ']') {
            parser->position++;
            return 1;
        }
        for (;;) {
            if (!first && !consume(parser, ',')) return 0;
            first = 0;
            if (d->results.count >= MAX_RESULTS) {
                set_error(parser, "too many results");
                return 0;
            }
            RawResult *r = xcalloc(1, sizeof(*r));
            vec_push(&d->results, r);
            skip_whitespace(parser);
            if (parser->position < parser->length && parser->data[parser->position] == 'n') {
                set_error(parser, "a result must be an object");
                return 0;
            }
            if (!parse_object(parser, result_field, r)) return 0;
            skip_whitespace(parser);
            if (parser->position < parser->length && parser->data[parser->position] == ']') {
                parser->position++;
                return 1;
            }
            if (parser->position >= parser->length) {
                set_error(parser, "unterminated results array");
                return 0;
            }
        }
    }
    return skip_value(parser, 1U);
}

/* Factor to nanoseconds or bytes. Returns 0 for an unknown unit. */
static int unit_factor(const char *unit, int is_time, double *factor) {
    static const struct { const char *name; double f; } times[] = {
        {"nanoseconds", 1}, {"nanosecond", 1}, {"ns", 1},
        {"microseconds", 1e3}, {"microsecond", 1e3}, {"us", 1e3}, {"\xc2\xb5s", 1e3},
        {"milliseconds", 1e6}, {"millisecond", 1e6}, {"ms", 1e6},
        {"seconds", 1e9}, {"second", 1e9}, {"s", 1e9},
    };
    static const struct { const char *name; double f; } sizes[] = {
        {"bytes", 1}, {"byte", 1}, {"b", 1},
        {"kib", 1024.0}, {"kibibytes", 1024.0}, {"mib", 1048576.0}, {"mebibytes", 1048576.0},
        {"gib", 1073741824.0}, {"gibibytes", 1073741824.0},
        {"kb", 1e3}, {"kilobytes", 1e3}, {"mb", 1e6}, {"megabytes", 1e6},
    };
    char lower[32];
    size_t i;
    if (!unit) return 0;
    for (i = 0; unit[i] && i + 1 < sizeof(lower); i++) lower[i] = (char)tolower((unsigned char)unit[i]);
    lower[i] = '\0';
    if (is_time) {
        for (i = 0; i < sizeof(times) / sizeof(times[0]); i++)
            if (strcmp(lower, times[i].name) == 0) { *factor = times[i].f; return 1; }
    } else {
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if (strcmp(lower, sizes[i].name) == 0) { *factor = sizes[i].f; return 1; }
    }
    return 0;
}

static void fill_metric(Metric *out, const RawMetric *in, int is_time, Record *record) {
    double f = 1.0;
    memset(out, 0, sizeof(*out));
    out->q1 = out->q3 = NAN;
    snprintf(out->unit, sizeof(out->unit), "%s", in->unit ? in->unit : "");
    if (!in->has_median && !in->has_mean) return;
    if (!unit_factor(in->unit, is_time, &f)) {
        if (record->status == REC_OK) {
            record->status = REC_BAD_UNIT;
            snprintf(record->reason, sizeof(record->reason), "unknown %s unit '%s'", is_time ? "wall_time" : "peak_rss",
                     in->unit ? in->unit : "(none)");
        }
        return;
    }
    out->has_median = in->has_median;
    out->has_mean = in->has_mean;
    out->median = in->median * f;
    out->mean = in->mean * f;
    if (in->has_q1) out->q1 = in->q1 * f;
    if (in->has_q3) out->q3 = in->q3 * f;
}

static void select_metric(Metric *m, int statistic) {
    int use_median = statistic == STAT_MEDIAN ? m->has_median : !m->has_mean && m->has_median;
    m->present = m->has_median || m->has_mean;
    m->statistic = use_median ? STAT_MEDIAN : STAT_MEAN;
    m->estimate = use_median ? m->median : m->mean;
}

/* Choose median or mean for both metrics and derive the record status. */
void select_statistic(Record *r, int statistic) {
    select_metric(&r->time, statistic);
    select_metric(&r->memory, statistic);
    if (r->status == REC_BAD_UNIT) return;
    r->status = REC_OK;
    r->reason[0] = '\0';
    if (r->failed > 0) {
        r->status = REC_FAILED;
        snprintf(r->reason, sizeof(r->reason), "%.0f failed samples", r->failed);
    } else if (!r->time.present || !r->memory.present) {
        r->status = REC_INCOMPLETE;
        snprintf(r->reason, sizeof(r->reason), "missing %s", !r->time.present ? "wall_time" : "peak_rss");
    } else if (!(r->time.estimate > 0) || !(r->memory.estimate > 0)) {
        r->status = REC_INCOMPLETE;
        snprintf(r->reason, sizeof(r->reason), "non-positive estimate");
    }
}

void record_free(Record *r) {
    if (!r) return;
    free(r->path);
    free(r->command);
    free(r->argv0);
    free(r->zebrac_version);
    free(r->project);
    free(r->suite);
    free(r->group);
    free(r->run);
    free(r->case_id);
    free(r->variant);
    free(r->family);
    free(r);
}

int zebrac_parse_file(const char *path, ParseSet *set, char *error, size_t error_size) {
    unsigned char *data = NULL;
    size_t length = 0;
    const char *why = NULL;
    Parser parser;
    RawDocument doc;
    int ok = 0;

    if (!pf_read_file(path, &data, &length, MAX_INPUT_BYTES, &why)) {
        snprintf(error, error_size, "%s: cannot read: %s", path, why);
        return 0;
    }
    set->bytes_read += length;
    /* Skip JSON that is not a Zebrac document (run manifests, indexes) before parsing it. */
    if (!find_bytes(data, length, "\"zebrac_version\"") || !find_bytes(data, length, "\"results\"")) {
        set->files_skipped++;
        free(data);
        return 1;
    }
    memset(&parser, 0, sizeof(parser));
    parser.data = data;
    parser.length = length;
    memset(&doc, 0, sizeof(doc));
    doc.schema = doc.duration = doc.min_samples = doc.warmup = NAN;

    skip_whitespace(&parser);
    if (parser.position < parser.length && parser.data[parser.position] != '{') {
        set_error(&parser, "expected a JSON object");
        goto parse_error;
    }
    if (!parse_object(&parser, document_field, &doc)) goto parse_error;
    skip_whitespace(&parser);
    if (parser.position != parser.length) {
        set_error(&parser, "trailing data after JSON object");
        goto parse_error;
    }
    if (!doc.has_version || !doc.has_results) {
        set->files_skipped++;
        ok = 1;
        goto done;
    }
    if (!(doc.schema == 1.0)) {
        snprintf(error, error_size, "%s: unsupported Zebrac schema_version (expected 1)", path);
        goto done;
    }
    set->files_read++;
    for (size_t i = 0; i < doc.results.count; i++) {
        RawResult *raw = doc.results.items[i];
        Record *r = xcalloc(1, sizeof(*r));
        r->path = xstrdup(path);
        r->index = (int)i;
        r->results_in_file = (int)doc.results.count;
        r->command = xstrdup(raw->command ? raw->command : "");
        r->argv0 = raw->argv0 ? xstrdup(raw->argv0) : NULL;
        r->samples = raw->has_sample_count ? raw->sample_count : NAN;
        r->failed = raw->has_failed ? raw->failed_sample_count : 0;
        r->zebrac_version = xstrdup(doc.version ? doc.version : "unknown");
        r->duration_ms = doc.duration;
        r->min_samples = doc.min_samples;
        r->warmup = doc.warmup;
        r->status = REC_OK;
        fill_metric(&r->time, &raw->wall_time, 1, r);
        fill_metric(&r->memory, &raw->peak_rss, 0, r);
        select_statistic(r, STAT_MEDIAN);
        vec_push(&set->records, r);
    }
    ok = 1;
    goto done;

parse_error:
    snprintf(error, error_size, "%s: %s", path, parser.error[0] ? parser.error : "malformed JSON");
done:
    for (size_t i = 0; i < doc.results.count; i++) {
        raw_result_free(doc.results.items[i]);
        free(doc.results.items[i]);
    }
    vec_free(&doc.results);
    free(doc.version);
    free(data);
    return ok;
}

/* ---- bench.meta.v1 JSONL sidecar ----------------------------------------- */

typedef struct {
    SidecarRow *row;
    int is_meta;
} SidecarLine;

static int sidecar_field(Parser *parser, const char *key, void *ctx) {
    SidecarLine *line = ctx;
    char **target = NULL;
    char *value = NULL;
    skip_whitespace(parser);
    if (parser->position >= parser->length || parser->data[parser->position] != '"') return skip_value(parser, 1U);
    if (!parse_string(parser, &value)) return 0;
    if (strcmp(key, "schema_version") == 0) line->is_meta = strcmp(value, "bench.meta.v1") == 0;
    else if (strcmp(key, "raw_json") == 0) target = &line->row->raw_json;
    else if (strcmp(key, "suite") == 0) target = &line->row->suite;
    else if (strcmp(key, "section") == 0) target = &line->row->section;
    else if (strcmp(key, "workload") == 0) target = &line->row->workload;
    else if (strcmp(key, "tool") == 0) target = &line->row->tool;
    else if (strcmp(key, "tool_family") == 0) target = &line->row->tool_family;
    else if (strcmp(key, "command") == 0) target = &line->row->command;
    if (target) {
        free(*target);
        *target = value;
    } else {
        free(value);
    }
    return 1;
}

void sidecar_row_free(SidecarRow *row) {
    if (!row) return;
    free(row->raw_json);
    free(row->suite);
    free(row->section);
    free(row->workload);
    free(row->tool);
    free(row->tool_family);
    free(row->command);
    free(row);
}

int sidecar_parse_file(const char *path, Vec *out, char *error, size_t error_size) {
    unsigned char *data;
    size_t length, start = 0, line_number = 0;
    const char *why = NULL;
    int rows = 0;
    if (!pf_read_file(path, &data, &length, MAX_INPUT_BYTES, &why)) {
        snprintf(error, error_size, "%s: cannot read: %s", path, why);
        return -1;
    }
    while (start < length) {
        size_t end = start;
        while (end < length && data[end] != '\n') end++;
        line_number++;
        Parser parser;
        memset(&parser, 0, sizeof(parser));
        parser.data = data + start;
        parser.length = end - start;
        skip_whitespace(&parser);
        if (parser.position < parser.length) {
            SidecarLine line = {xcalloc(1, sizeof(SidecarRow)), 0};
            if (parser.data[parser.position] != '{' || !parse_object(&parser, sidecar_field, &line)) {
                snprintf(error, error_size, "%s:%zu: %s", path, line_number, parser.error[0] ? parser.error : "expected a JSON object");
                sidecar_row_free(line.row);
                free(data);
                return -1;
            }
            if (line.is_meta && line.row->raw_json && line.row->workload && line.row->tool) {
                vec_push(out, line.row);
                rows++;
            } else {
                sidecar_row_free(line.row);
            }
        }
        start = end + 1;
    }
    free(data);
    return rows;
}
