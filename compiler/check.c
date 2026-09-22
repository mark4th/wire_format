#include "wfc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int unicode_space(uint32_t codepoint)
{
    return (codepoint >= 0x09 && codepoint <= 0x0d) || codepoint == 0x20 ||
           codepoint == 0x85 || codepoint == 0xa0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200a) ||
           codepoint == 0x2028 || codepoint == 0x2029 || codepoint == 0x202f ||
           codepoint == 0x205f || codepoint == 0x3000;
}

static int text_present(const char *text)
{
    const unsigned char *scan = (const unsigned char *)text;
    while (*scan != 0) {
        uint32_t codepoint;
        if (*scan < 0x80) {
            codepoint = *scan++;
        } else if ((*scan & 0xe0) == 0xc0) {
            codepoint = (uint32_t)(*scan++ & 0x1f) << 6;
            codepoint |= *scan++ & 0x3f;
        } else if ((*scan & 0xf0) == 0xe0) {
            codepoint = (uint32_t)(*scan++ & 0x0f) << 12;
            codepoint |= (uint32_t)(*scan++ & 0x3f) << 6;
            codepoint |= *scan++ & 0x3f;
        } else {
            codepoint = (uint32_t)(*scan++ & 0x07) << 18;
            codepoint |= (uint32_t)(*scan++ & 0x3f) << 12;
            codepoint |= (uint32_t)(*scan++ & 0x3f) << 6;
            codepoint |= *scan++ & 0x3f;
        }
        if (!unicode_space(codepoint))
            return 1;
    }
    return 0;
}

static int message_layout(const wfc_message_t *message, size_t **offsets,
                          size_t *octets, wfc_error_t *error)
{
    size_t bit_offset = 0;
    size_t index;

    *offsets = calloc(message->field_count, sizeof(**offsets));
    if (*offsets == NULL) {
        wfc_set_error(error, message->location, "out of memory");
        return 0;
    }
    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        uint8_t width = wfc_field_width(field);
        if (field->type != WFC_BITS && bit_offset % 8 != 0) {
            wfc_set_error(error, field->location,
                          "scalar field `%s` begins at bit %zu, not an octet boundary",
                          field->name, bit_offset);
            free(*offsets);
            *offsets = NULL;
            return 0;
        }
        (*offsets)[index] = bit_offset;
        if (bit_offset > SIZE_MAX - width) {
            wfc_set_error(error, field->location,
                          "message size exceeds the host size limit");
            free(*offsets);
            *offsets = NULL;
            return 0;
        }
        bit_offset += width;
    }
    if (bit_offset % 8 != 0) {
        wfc_set_error(error, message->location,
                      "message `%s` ends after %zu bits, not on an octet boundary",
                      message->name, bit_offset);
        free(*offsets);
        *offsets = NULL;
        return 0;
    }
    *octets = bit_offset / 8;
    return 1;
}

static const wfc_field_t *find_field(const wfc_message_t *message, const char *name,
                                     size_t *index)
{
    size_t position;
    for (position = 0; position < message->field_count; position++)
        if (strcmp(message->fields[position].name, name) == 0) {
            if (index != NULL)
                *index = position;
            return &message->fields[position];
        }
    return NULL;
}

static const wfc_assignment_t *find_assignment(const wfc_vector_t *vector,
                                                const char *name)
{
    size_t index;
    for (index = 0; index < vector->assignment_count; index++)
        if (strcmp(vector->assignments[index].name, name) == 0)
            return &vector->assignments[index];
    return NULL;
}

static void write_bits(uint8_t *output, size_t offset, uint8_t width,
                       uint64_t value, wfc_bit_order_t order)
{
    size_t index;
    for (index = 0; index < width; index++) {
        size_t source_bit;
        size_t target_bit;
        if (order == WFC_MSB_FIRST) {
            source_bit = width - 1 - index;
            target_bit = 7 - ((offset + index) % 8);
        } else {
            source_bit = index;
            target_bit = (offset + index) % 8;
        }
        if (((value >> source_bit) & 1) != 0)
            output[(offset + index) / 8] |= (uint8_t)(1U << target_bit);
    }
}

static uint64_t read_bits(const uint8_t *input, size_t offset, uint8_t width,
                          wfc_bit_order_t order)
{
    uint64_t value = 0;
    size_t index;
    for (index = 0; index < width; index++) {
        size_t target_bit;
        size_t source_bit;
        if (order == WFC_MSB_FIRST) {
            target_bit = width - 1 - index;
            source_bit = 7 - ((offset + index) % 8);
        } else {
            target_bit = index;
            source_bit = (offset + index) % 8;
        }
        if (((input[(offset + index) / 8] >> source_bit) & 1) != 0)
            value |= UINT64_C(1) << target_bit;
    }
    return value;
}

static void write_scalar(uint8_t *output, size_t offset, size_t octets,
                         uint64_t value, wfc_byte_order_t order)
{
    size_t index;
    for (index = 0; index < octets; index++) {
        size_t shift = order == WFC_BIG_ENDIAN
                           ? (octets - 1 - index) * 8
                           : index * 8;
        output[offset + index] = (uint8_t)(value >> shift);
    }
}

static uint64_t read_scalar(const uint8_t *input, size_t offset, size_t octets,
                            wfc_byte_order_t order)
{
    uint64_t value = 0;
    size_t index;
    for (index = 0; index < octets; index++) {
        size_t shift = order == WFC_BIG_ENDIAN
                           ? (octets - 1 - index) * 8
                           : index * 8;
        value |= (uint64_t)input[offset + index] << shift;
    }
    return value;
}

static void write_field(uint8_t *output, size_t offset, const wfc_field_t *field,
                        uint64_t value, const wfc_protocol_t *protocol)
{
    if (field->type == WFC_BITS)
        write_bits(output, offset, field->width, value, protocol->bit_order);
    else
        write_scalar(output, offset / 8, wfc_field_width(field) / 8, value,
                     protocol->byte_order);
}

static uint64_t read_field(const uint8_t *input, size_t offset,
                           const wfc_field_t *field,
                           const wfc_protocol_t *protocol)
{
    if (field->type == WFC_BITS)
        return read_bits(input, offset, field->width, protocol->bit_order);
    return read_scalar(input, offset / 8, wfc_field_width(field) / 8,
                       protocol->byte_order);
}

static int check_vector(const wfc_protocol_t *protocol,
                        const wfc_message_t *message, const size_t *offsets,
                        size_t octets, const wfc_vector_t *vector,
                        wfc_error_t *error)
{
    uint8_t *encoded;
    size_t index;
    size_t mismatch = SIZE_MAX;

    for (index = 0; index < vector->assignment_count; index++) {
        const wfc_assignment_t *assignment = &vector->assignments[index];
        const wfc_field_t *field = find_field(message, assignment->name, NULL);
        if (field == NULL) {
            wfc_set_error(error, assignment->location,
                          "vector `%s` assigns unknown field `%s`",
                          vector->name, assignment->name);
            return 0;
        }
        if (field->is_constant) {
            wfc_set_error(error, assignment->location,
                          "vector `%s` may not assign constant field `%s`",
                          vector->name, assignment->name);
            return 0;
        }
        if (!wfc_value_fits(assignment->value, wfc_field_width(field))) {
            wfc_set_error(error, assignment->location,
                          "value %llu does not fit in a %u-bit field",
                          (unsigned long long)assignment->value,
                          (unsigned)wfc_field_width(field));
            return 0;
        }
    }
    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        if (!field->is_constant && find_assignment(vector, field->name) == NULL) {
            wfc_set_error(error, vector->location,
                          "vector `%s` has no value for field `%s`",
                          vector->name, field->name);
            return 0;
        }
    }
    if (vector->wire_count != octets) {
        wfc_set_error(error, vector->location,
                      "vector `%s` has %zu wire octets; message `%s` requires %zu",
                      vector->name, vector->wire_count, message->name, octets);
        return 0;
    }

    encoded = calloc(octets == 0 ? 1 : octets, 1);
    if (encoded == NULL) {
        wfc_set_error(error, vector->location, "out of memory");
        return 0;
    }
    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        uint64_t value = field->is_constant
                             ? field->constant
                             : find_assignment(vector, field->name)->value;
        write_field(encoded, offsets[index], field, value, protocol);
    }
    for (index = 0; index < octets; index++)
        if (encoded[index] != vector->wire[index]) {
            mismatch = index;
            break;
        }

    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        uint64_t decoded = read_field(vector->wire, offsets[index], field, protocol);
        if (field->is_constant && decoded != field->constant) {
            wfc_set_error(error, vector->location,
                          "vector `%s` decodes constant `%s` as %llu, expected %llu",
                          vector->name, field->name,
                          (unsigned long long)decoded,
                          (unsigned long long)field->constant);
            free(encoded);
            return 0;
        }
        if (!field->is_constant) {
            uint64_t wanted = find_assignment(vector, field->name)->value;
            if (decoded != wanted) {
                wfc_set_error(error, vector->location,
                              "vector `%s` decodes field `%s` as %llu, expected %llu",
                              vector->name, field->name,
                              (unsigned long long)decoded,
                              (unsigned long long)wanted);
                free(encoded);
                return 0;
            }
        }
    }
    if (mismatch != SIZE_MAX) {
        wfc_set_error(error, vector->location,
                      "vector `%s` encodes octet %zu as %02X, but `wire` contains %02X",
                      vector->name, mismatch, encoded[mismatch], vector->wire[mismatch]);
        free(encoded);
        return 0;
    }
    free(encoded);
    return 1;
}

int wfc_check(const wfc_protocol_t *protocol, wfc_summary_t *summary,
              wfc_error_t *error)
{
    size_t message_index;
    memset(summary, 0, sizeof(*summary));

    if (!text_present(protocol->description)) {
        wfc_set_error(error, protocol->location, "protocol description may not be empty");
        return 0;
    }
    if (protocol->standard != NULL && !text_present(protocol->standard)) {
        wfc_set_error(error, protocol->location, "standard may not be empty");
        return 0;
    }
    if (protocol->reference != NULL && !text_present(protocol->reference)) {
        wfc_set_error(error, protocol->location, "reference may not be empty");
        return 0;
    }

    for (message_index = 0; message_index < protocol->message_count; message_index++) {
        const wfc_message_t *message = &protocol->messages[message_index];
        size_t *offsets;
        size_t octets;
        size_t vector_index;
        if (!text_present(message->description)) {
            wfc_set_error(error, message->location,
                          "message description may not be empty");
            return 0;
        }
        if (!message_layout(message, &offsets, &octets, error))
            return 0;
        if (summary->layout_octets > SIZE_MAX - octets) {
            free(offsets);
            wfc_set_error(error, message->location,
                          "combined message sizes exceed the host size limit");
            return 0;
        }
        summary->layout_octets += octets;
        for (vector_index = 0; vector_index < message->vector_count; vector_index++) {
            if (!check_vector(protocol, message, offsets, octets,
                              &message->vectors[vector_index], error)) {
                free(offsets);
                return 0;
            }
            summary->vectors++;
        }
        free(offsets);
    }
    summary->messages = protocol->message_count;
    return 1;
}
