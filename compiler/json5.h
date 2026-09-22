#ifndef WFC_JSON5_H
#define WFC_JSON5_H

#include <stddef.h>

typedef enum {
    JSON5_NULL,
    JSON5_BOOLEAN,
    JSON5_NUMBER,
    JSON5_STRING,
    JSON5_ARRAY,
    JSON5_OBJECT
} json5_type_t;

typedef struct json5_value json5_value_t;

typedef struct {
    char *name;
    size_t name_length;
    json5_value_t *value;
    size_t line;
    size_t column;
} json5_member_t;

struct json5_value {
    json5_type_t type;
    size_t line;
    size_t column;
    union {
        int boolean;
        struct {
            char *text;
            size_t length;
        } scalar;
        struct {
            json5_value_t **items;
            size_t count;
        } array;
        struct {
            json5_member_t *members;
            size_t count;
        } object;
    } as;
};

typedef struct {
    size_t line;
    size_t column;
    char message[256];
} json5_error_t;

json5_value_t *json5_parse(const char *text, size_t length, json5_error_t *error);
void json5_free(json5_value_t *value);

#endif
