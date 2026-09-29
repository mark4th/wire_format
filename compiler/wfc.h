#ifndef WFC_H
#define WFC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t line;
    size_t column;
} wfc_location_t;

typedef struct {
    wfc_location_t location;
    char message[512];
} wfc_error_t;

typedef enum {
    WFC_BIG_ENDIAN,
    WFC_LITTLE_ENDIAN
} wfc_byte_order_t;

typedef enum {
    WFC_MSB_FIRST,
    WFC_LSB_FIRST
} wfc_bit_order_t;

typedef enum {
    WFC_BITS,
    WFC_U8,
    WFC_U16,
    WFC_U32,
    WFC_U64,
    WFC_SDNV,
    WFC_BYTES,
    WFC_RECORDS,
    WFC_CHOICE
} wfc_field_type_t;

typedef struct {
    uint64_t value;
    char *message;
    wfc_location_t location;
} wfc_choice_case_t;

typedef struct {
    char *name;
    wfc_location_t location;
    wfc_field_type_t type;
    uint8_t width;
    int is_constant;
    uint64_t constant;
    char *length_from;
    char *length_of;
    char *message;
    char *count_from;
    char *count_of;
    char *select_from;
    wfc_choice_case_t *cases;
    size_t case_count;
} wfc_field_t;

typedef struct {
    char *name;
    wfc_location_t location;
    uint64_t value;
    uint8_t *bytes;
    size_t byte_count;
    int is_bytes;
} wfc_assignment_t;

typedef struct {
    char *name;
    wfc_location_t location;
    wfc_assignment_t *assignments;
    size_t assignment_count;
    uint8_t *wire;
    size_t wire_count;
} wfc_vector_t;

typedef struct {
    char *name;
    char *description;
    wfc_location_t location;
    wfc_field_t *fields;
    size_t field_count;
    wfc_vector_t *vectors;
    size_t vector_count;
} wfc_message_t;

typedef struct {
    struct json5_value *record_source;
    uint16_t version;
    char *name;
    char *description;
    char *standard;
    char *reference;
    wfc_location_t location;
    wfc_byte_order_t byte_order;
    wfc_bit_order_t bit_order;
    wfc_message_t *messages;
    size_t message_count;
} wfc_protocol_t;

typedef struct {
    size_t messages;
    size_t vectors;
    size_t layout_octets;
} wfc_summary_t;

typedef struct {
    char *header;
    uint8_t *binary;
    size_t binary_size;
} wfc_generated_t;

int wfc_source_parse(const char *text, size_t length, wfc_protocol_t *protocol,
                     wfc_error_t *error);
void wfc_protocol_free(wfc_protocol_t *protocol);

int wfc_check(const wfc_protocol_t *protocol, wfc_summary_t *summary,
              wfc_error_t *error);

int wfc_generate(const wfc_protocol_t *protocol, wfc_generated_t *generated,
                 wfc_error_t *error);
void wfc_generated_free(wfc_generated_t *generated);

void wfc_set_error(wfc_error_t *error, wfc_location_t location,
                   const char *format, ...);
char *wfc_duplicate(const char *text, size_t length);
int wfc_identifier_valid(const char *identifier);
uint8_t wfc_field_width(const wfc_field_t *field);
int wfc_value_fits(uint64_t value, uint8_t width);

int wfc_records_generate(const wfc_protocol_t *, wfc_generated_t *, wfc_error_t *);
int wfc_records_check(const wfc_protocol_t *, wfc_summary_t *, wfc_error_t *);

#endif
