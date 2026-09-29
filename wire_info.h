#ifndef WIRE_INFO_H
#define WIRE_INFO_H

#include <stddef.h>
#include <stdint.h>
#include "wire_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A compiled schema is borrowed, immutable data. No protocol code is loaded. */
typedef struct
{
    const uint8_t *data;
    size_t size;
    uint32_t messages;
    uint32_t fields;
} wire_info_t;

typedef enum
{
    WIRE_INFO_UINT = 1,
    WIRE_INFO_BYTES,
    WIRE_INFO_SDNV,
    WIRE_INFO_RECORDS,
    WIRE_INFO_PARAMETER,
    WIRE_INFO_COMPUTED,
    WIRE_INFO_ASSERT,
    WIRE_INFO_MARK,
    WIRE_INFO_CRC,
    WIRE_INFO_CBOR_UINT,
    WIRE_INFO_CBOR_BYTES,
    WIRE_INFO_CBOR_TEXT,
    WIRE_INFO_CBOR_ARRAY,
    WIRE_INFO_DECIMAL,
    WIRE_INFO_BCD,
    WIRE_INFO_TERMINATED_UINT
} wire_info_type_t;

const char *wire_info_format(const wire_info_t *info, unsigned message, int decode);
wire_info_status_t wire_info_open(wire_info_t *info, const void *data, size_t size);
int wire_info_find(const wire_info_t *info, const char *message);
const char *wire_info_message_name(const wire_info_t *info, unsigned message);
size_t wire_info_field_count(const wire_info_t *info, unsigned message);
int wire_info_field(const wire_info_t *info, unsigned message, const char *name);
const char *wire_info_field_name(const wire_info_t *info, unsigned message, unsigned field);
wire_info_type_t wire_info_field_type(const wire_info_t *info, unsigned message, unsigned field);
int wire_info_child(const wire_info_t *info, unsigned message, unsigned field);

/* Caller owns all records and byte buffers. Decode slices borrow input.
 * On error outputs may be partial and must not be used. used is zero on error.
 * Encoding fills computed field values in the supplied record as well.
 * Parameters are caller-supplied on BOTH encode and decode (out-of-band data).
 */
wire_info_status_t wire_info_encode(const wire_info_t *info, unsigned message,
    wire_info_record_t *record, uint8_t *output, size_t capacity, size_t *used);
wire_info_status_t wire_info_decode(const wire_info_t *info, unsigned message,
    wire_info_record_t *record, const uint8_t *input, size_t length, size_t *used);
const char *wire_info_error(wire_info_status_t status);

#ifdef __cplusplus
}
#endif
#endif
