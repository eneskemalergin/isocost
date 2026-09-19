/*
 * A strict TOML 1.0 subset for isocost.toml: comments, [table], [[array of
 * tables]], bare and quoted keys, dotted table headers, basic and literal
 * strings, integers, floats, booleans, arrays, and inline tables.
 * Not accepted: multi-line strings, dates and times, dotted keys in
 * assignments. Every value records its line for error messages.
 */
#ifndef ISOCOST_TOML_H
#define ISOCOST_TOML_H

#include <stddef.h>

typedef enum { TOML_STRING, TOML_INTEGER, TOML_FLOAT, TOML_BOOL, TOML_ARRAY, TOML_TABLE } TomlType;

typedef struct TomlValue {
    TomlType type;
    int line;
    int column;
    char *string;
    double number;           /* integers too */
    int boolean;
    struct TomlValue **items; /* array elements or table values */
    char **keys;              /* table keys, parallel to items */
    size_t count;
    int array_of_tables;      /* defined with [[...]] */
    int defined;              /* table header seen */
} TomlValue;

/* Returns the root table, or NULL with "line:column: message" in error. */
TomlValue *toml_parse(const char *text, size_t length, char *error, size_t error_size);
void toml_free(TomlValue *value);
const char *toml_type_name(TomlType type);

#endif
