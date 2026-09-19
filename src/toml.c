#include "toml.h"
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
    int line;
    size_t line_start;
    char *error;
    size_t error_size;
    int failed;
} Lexer;

static int column_of(const Lexer *lx) { return (int)(lx->pos - lx->line_start) + 1; }

static int fail(Lexer *lx, const char *format, ...) ISO_PRINTF(2, 3);
static int fail(Lexer *lx, const char *format, ...) {
    va_list args;
    char message[256];
    if (lx->failed) return 0;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    snprintf(lx->error, lx->error_size, "%d:%d: %s", lx->line, column_of(lx), message);
    lx->failed = 1;
    return 0;
}

static int peek(const Lexer *lx) { return lx->pos < lx->length ? (unsigned char)lx->text[lx->pos] : -1; }

static void advance(Lexer *lx) {
    if (lx->pos < lx->length && lx->text[lx->pos] == '\n') {
        lx->line++;
        lx->line_start = lx->pos + 1;
    }
    lx->pos++;
}

static void skip_blank(Lexer *lx) {
    while (peek(lx) == ' ' || peek(lx) == '\t') advance(lx);
}

static void skip_comment(Lexer *lx) {
    if (peek(lx) == '#')
        while (peek(lx) != -1 && peek(lx) != '\n') advance(lx);
}

/* Whitespace, newlines, and comments (inside arrays). */
static void skip_space(Lexer *lx) {
    for (;;) {
        int c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') advance(lx);
        else if (c == '#') skip_comment(lx);
        else return;
    }
}

static int end_of_line(Lexer *lx) {
    skip_blank(lx);
    skip_comment(lx);
    if (peek(lx) == '\r') advance(lx);
    if (peek(lx) == '\n') {
        advance(lx);
        return 1;
    }
    if (peek(lx) == -1) return 1;
    return fail(lx, "expected the end of the line, found '%c'", peek(lx));
}

static TomlValue *new_value(TomlType type, const Lexer *lx) {
    TomlValue *v = xcalloc(1, sizeof(*v));
    v->type = type;
    v->line = lx->line;
    v->column = column_of(lx);
    return v;
}

void toml_free(TomlValue *v) {
    if (!v) return;
    for (size_t i = 0; i < v->count; i++) {
        toml_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->string);
    free(v);
}

const char *toml_type_name(TomlType type) {
    switch (type) {
    case TOML_STRING: return "string";
    case TOML_INTEGER: return "integer";
    case TOML_FLOAT: return "float";
    case TOML_BOOL: return "boolean";
    case TOML_ARRAY: return "array";
    case TOML_TABLE: return "table";
    }
    return "value";
}

static TomlValue *table_get(TomlValue *table, const char *key) {
    for (size_t i = 0; i < table->count; i++)
        if (strcmp(table->keys[i], key) == 0) return table->items[i];
    return NULL;
}

static void table_add(TomlValue *table, const char *key, TomlValue *value) {
    table->keys = xrealloc(table->keys, (table->count + 1) * sizeof(char *));
    table->items = xrealloc(table->items, (table->count + 1) * sizeof(TomlValue *));
    table->keys[table->count] = xstrdup(key);
    table->items[table->count] = value;
    table->count++;
}

static void array_add(TomlValue *array, TomlValue *value) {
    array->items = xrealloc(array->items, (array->count + 1) * sizeof(TomlValue *));
    array->items[array->count++] = value;
}

static void append_utf8(Str *s, uint32_t cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xe0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
    else { b[0] = (char)(0xf0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
    str_appendn(s, b, n);
}

static char *parse_string(Lexer *lx) {
    Str s = {0};
    int quote = peek(lx);
    if (lx->pos + 2 < lx->length && lx->text[lx->pos + 1] == quote && lx->text[lx->pos + 2] == quote) {
        fail(lx, "multi-line strings are not supported; use a single-line string");
        return NULL;
    }
    advance(lx);
    str_append(&s, "");
    for (;;) {
        int c = peek(lx);
        if (c == -1 || c == '\n') {
            fail(lx, "unterminated string");
            str_free(&s);
            return NULL;
        }
        if (c == quote) {
            advance(lx);
            return s.data;
        }
        if (quote == '"' && c == '\\') {
            advance(lx);
            c = peek(lx);
            advance(lx);
            switch (c) {
            case '"': str_append(&s, "\""); break;
            case '\\': str_append(&s, "\\"); break;
            case 'n': str_append(&s, "\n"); break;
            case 't': str_append(&s, "\t"); break;
            case 'r': str_append(&s, "\r"); break;
            case 'b': str_append(&s, "\b"); break;
            case 'f': str_append(&s, "\f"); break;
            case 'u':
            case 'U': {
                int digits = c == 'u' ? 4 : 8;
                uint32_t cp = 0;
                for (int i = 0; i < digits; i++) {
                    int h = peek(lx);
                    if (!isxdigit(h)) {
                        fail(lx, "invalid unicode escape");
                        str_free(&s);
                        return NULL;
                    }
                    cp = cp * 16 + (uint32_t)(isdigit(h) ? h - '0' : (tolower(h) - 'a' + 10));
                    advance(lx);
                }
                if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
                    fail(lx, "invalid unicode code point");
                    str_free(&s);
                    return NULL;
                }
                append_utf8(&s, cp);
                break;
            }
            default:
                fail(lx, "unknown escape '\\%c'", c > 0 ? c : '?');
                str_free(&s);
                return NULL;
            }
            continue;
        }
        if (c < 0x20 && c != '\t') {
            fail(lx, "control character in string");
            str_free(&s);
            return NULL;
        }
        char ch = (char)c;
        str_appendn(&s, &ch, 1);
        advance(lx);
    }
}

static int is_bare(int c) { return isalnum(c) || c == '_' || c == '-'; }

static char *parse_key(Lexer *lx) {
    int c = peek(lx);
    if (c == '"' || c == '\'') return parse_string(lx);
    if (!is_bare(c)) {
        fail(lx, c == -1 ? "expected a key" : "expected a key, found '%c'", c);
        return NULL;
    }
    size_t start = lx->pos;
    while (is_bare(peek(lx))) advance(lx);
    return xstrndup(lx->text + start, lx->pos - start);
}

static TomlValue *parse_value(Lexer *lx, int depth);

static TomlValue *parse_number_or_bool(Lexer *lx) {
    size_t start = lx->pos;
    TomlValue *v;
    char buffer[64];
    size_t n = 0;
    int is_float = 0;
    while (lx->pos < lx->length && (isalnum(peek(lx)) || peek(lx) == '.' || peek(lx) == '+' || peek(lx) == '-' || peek(lx) == '_')) {
        if (n + 1 < sizeof(buffer) && peek(lx) != '_') buffer[n++] = (char)peek(lx);
        advance(lx);
    }
    buffer[n] = '\0';
    size_t len = lx->pos - start;
    if (len == 4 && strncmp(lx->text + start, "true", 4) == 0) {
        v = new_value(TOML_BOOL, lx);
        v->boolean = 1;
        return v;
    }
    if (len == 5 && strncmp(lx->text + start, "false", 5) == 0) {
        v = new_value(TOML_BOOL, lx);
        return v;
    }
    if (!n) {
        fail(lx, "expected a value");
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
        if (buffer[i] == '.' || buffer[i] == 'e' || buffer[i] == 'E') is_float = 1;
    for (size_t i = 0; i < n; i++)
        if (!(isdigit((unsigned char)buffer[i]) || strchr("+-.eE", buffer[i]))) {
            fail(lx, "'%.*s' is not a number, boolean, or quoted string", (int)len, lx->text + start);
            return NULL;
        }
    if (strchr(buffer, ':') || (n >= 10 && buffer[4] == '-' && buffer[7] == '-')) {
        fail(lx, "dates and times are not supported");
        return NULL;
    }
    char *end = NULL;
    double value = strtod(buffer, &end);
    if (!end || *end || !isfinite(value)) {
        fail(lx, "'%s' is not a valid number", buffer);
        return NULL;
    }
    v = new_value(is_float ? TOML_FLOAT : TOML_INTEGER, lx);
    v->number = value;
    return v;
}

static TomlValue *parse_array(Lexer *lx, int depth) {
    TomlValue *array = new_value(TOML_ARRAY, lx);
    advance(lx); /* [ */
    for (;;) {
        skip_space(lx);
        if (peek(lx) == ']') {
            advance(lx);
            return array;
        }
        TomlValue *item = parse_value(lx, depth + 1);
        if (!item) {
            toml_free(array);
            return NULL;
        }
        array_add(array, item);
        skip_space(lx);
        if (peek(lx) == ',') {
            advance(lx);
            continue;
        }
        if (peek(lx) == ']') {
            advance(lx);
            return array;
        }
        fail(lx, "expected ',' or ']' in array");
        toml_free(array);
        return NULL;
    }
}

static TomlValue *parse_inline_table(Lexer *lx, int depth) {
    TomlValue *table = new_value(TOML_TABLE, lx);
    table->defined = 1;
    advance(lx); /* { */
    skip_blank(lx);
    if (peek(lx) == '}') {
        advance(lx);
        return table;
    }
    for (;;) {
        skip_blank(lx);
        char *key = parse_key(lx);
        if (!key) break;
        skip_blank(lx);
        if (peek(lx) != '=') {
            fail(lx, "expected '=' after key '%s'", key);
            free(key);
            break;
        }
        advance(lx);
        skip_blank(lx);
        if (table_get(table, key)) {
            fail(lx, "duplicate key '%s'", key);
            free(key);
            break;
        }
        TomlValue *value = parse_value(lx, depth + 1);
        if (!value) {
            free(key);
            break;
        }
        table_add(table, key, value);
        free(key);
        skip_blank(lx);
        if (peek(lx) == ',') {
            advance(lx);
            continue;
        }
        if (peek(lx) == '}') {
            advance(lx);
            return table;
        }
        fail(lx, "expected ',' or '}' in inline table");
        break;
    }
    toml_free(table);
    return NULL;
}

static TomlValue *parse_value(Lexer *lx, int depth) {
    int c = peek(lx);
    if (depth > 32) {
        fail(lx, "values are nested too deeply");
        return NULL;
    }
    if (c == '"' || c == '\'') {
        TomlValue *v = new_value(TOML_STRING, lx);
        v->string = parse_string(lx);
        if (!v->string) {
            toml_free(v);
            return NULL;
        }
        return v;
    }
    if (c == '[') return parse_array(lx, depth);
    if (c == '{') return parse_inline_table(lx, depth);
    return parse_number_or_bool(lx);
}

/* Resolve a [a.b] or [[a.b]] header to the table that following keys go into. */
static TomlValue *open_table(Lexer *lx, TomlValue *root, char **path, size_t parts, int array_of_tables) {
    TomlValue *table = root;
    for (size_t i = 0; i < parts; i++) {
        int last = i + 1 == parts;
        TomlValue *next = table_get(table, path[i]);
        if (last && array_of_tables) {
            if (!next) {
                next = new_value(TOML_ARRAY, lx);
                next->array_of_tables = 1;
                table_add(table, path[i], next);
            } else if (next->type != TOML_ARRAY || !next->array_of_tables) {
                fail(lx, "'%s' is already defined and is not an array of tables", path[i]);
                return NULL;
            }
            TomlValue *entry = new_value(TOML_TABLE, lx);
            entry->defined = 1;
            array_add(next, entry);
            return entry;
        }
        if (!next) {
            next = new_value(TOML_TABLE, lx);
            table_add(table, path[i], next);
        } else if (next->type == TOML_ARRAY && next->array_of_tables && next->count) {
            next = next->items[next->count - 1];
        } else if (next->type != TOML_TABLE) {
            fail(lx, "'%s' is already a %s, not a table", path[i], toml_type_name(next->type));
            return NULL;
        } else if (last && next->defined) {
            fail(lx, "table [%s] is defined twice", path[i]);
            return NULL;
        }
        if (last) next->defined = 1;
        table = next;
    }
    return table;
}

TomlValue *toml_parse(const char *text, size_t length, char *error, size_t error_size) {
    Lexer lx = {text, length, 0, 1, 0, error, error_size, 0};
    TomlValue *root = new_value(TOML_TABLE, &lx), *current = root;
    root->defined = 1;
    if (length >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) lx.pos = 3;
    while (!lx.failed) {
        skip_space(&lx);
        if (peek(&lx) == -1) return root;
        if (peek(&lx) == '[') {
            int array_of_tables = 0;
            char *path[16];
            size_t parts = 0;
            advance(&lx);
            if (peek(&lx) == '[') {
                array_of_tables = 1;
                advance(&lx);
            }
            for (;;) {
                skip_blank(&lx);
                char *part = parse_key(&lx);
                if (!part) break;
                if (parts == 16) {
                    free(part);
                    fail(&lx, "table header has too many parts");
                    break;
                }
                path[parts++] = part;
                skip_blank(&lx);
                if (peek(&lx) == '.') {
                    advance(&lx);
                    continue;
                }
                break;
            }
            if (!lx.failed) {
                if (peek(&lx) != ']' || (array_of_tables && (lx.pos + 1 >= length || text[lx.pos + 1] != ']')))
                    fail(&lx, array_of_tables ? "expected ']]' to close the table header" : "expected ']' to close the table header");
                else {
                    advance(&lx);
                    if (array_of_tables) advance(&lx);
                    current = open_table(&lx, root, path, parts, array_of_tables);
                    end_of_line(&lx);
                }
            }
            for (size_t i = 0; i < parts; i++) free(path[i]);
            continue;
        }
        char *key = parse_key(&lx);
        if (!key) break;
        skip_blank(&lx);
        if (peek(&lx) == '.') {
            fail(&lx, "dotted keys are not supported; use a [table] header");
            free(key);
            break;
        }
        if (peek(&lx) != '=') {
            fail(&lx, "expected '=' after key '%s'", key);
            free(key);
            break;
        }
        advance(&lx);
        skip_blank(&lx);
        if (table_get(current, key)) {
            fail(&lx, "duplicate key '%s'", key);
            free(key);
            break;
        }
        TomlValue *value = parse_value(&lx, 0);
        if (!value) {
            free(key);
            break;
        }
        table_add(current, key, value);
        free(key);
        end_of_line(&lx);
    }
    toml_free(root);
    return NULL;
}
