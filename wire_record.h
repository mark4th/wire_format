#ifndef WIRE_RECORD_H
#define WIRE_RECORD_H
#include <stddef.h>
#include <stdint.h>

typedef struct wire_info_record wire_info_record_t;
typedef struct
{
    uint64_t number;
    const uint8_t *bytes;
    size_t length;
    wire_info_record_t *records;
    size_t count;
    size_t capacity;
} wire_info_value_t;
struct wire_info_record
{
    wire_info_value_t *values;
    size_t capacity;
};

typedef enum
{
    WIRE_INFO_OK, WIRE_INFO_ARGUMENT, WIRE_INFO_SCHEMA, WIRE_INFO_RANGE,
    WIRE_INFO_TRUNCATED, WIRE_INFO_CAPACITY, WIRE_INFO_CONSTANT,
    WIRE_INFO_CHECKSUM, WIRE_INFO_CONSTRAINT
} wire_info_status_t;

/* Named-record context for format-string operands. Metadata callbacks only
 * resolve names/child layouts; they never encode or decode protocol data. */
typedef struct wi_record_scope
{
    struct wi_record_scope *parent;
    wire_info_record_t *record;
    const void *metadata;
    int (*field)(const void *, unsigned, const char *);
    int (*child)(const void *, unsigned, unsigned);
    size_t (*count)(const void *, unsigned);
    unsigned message;
    unsigned depth;
    size_t base, index;
    const uint8_t *input;
    uint8_t *output;
    wire_info_status_t status;
    size_t crc_at, crc_from;
    unsigned crc_algorithm;
    wire_info_value_t *crc_value;
} wi_record_scope_t;
#endif
