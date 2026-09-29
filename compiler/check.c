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
                          "%s field `%s` begins at bit %zu, not an octet boundary",
                          field->type == WFC_BYTES ? "byte" : "scalar",
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
                       uint64_t value, wfc_bit_order_t order);
static uint64_t read_bits(const uint8_t *input, size_t offset, uint8_t width,
                          wfc_bit_order_t order);
static void write_scalar(uint8_t *output, size_t offset, size_t octets,
                         uint64_t value, wfc_byte_order_t order);
static uint64_t read_scalar(const uint8_t *input, size_t offset,
                            size_t octets, wfc_byte_order_t order);

static int write_sdnv_vector(uint8_t *output, size_t capacity,
                             size_t *position, uint64_t value)
{
    uint8_t encoded[10];
    size_t length = 1;
    size_t index;
    uint64_t remaining = value;

    while (remaining > 0x7fU)
    {
        remaining >>= 7;
        length++;
    }
    if (length > capacity - *position)
    {
        return 0;
    }
    for (index = 0; index < length; index++)
    {
        size_t shift = (length - index - 1U) * 7U;
        encoded[index] = (uint8_t)((value >> shift) & 0x7fU);
        if (index + 1U != length)
        {
            encoded[index] |= 0x80U;
        }
    }
    memcpy(output + *position, encoded, length);
    *position += length;
    return 1;
}

static int read_sdnv_vector(const uint8_t *input, size_t length,
                            size_t *position, uint64_t *result)
{
    uint64_t value = 0;
    size_t index;

    for (index = 0; index < 10; index++)
    {
        uint8_t byte;

        if (*position == length)
        {
            return 0;
        }
        byte = input[(*position)++];
        if (value > (UINT64_MAX >> 7))
        {
            return 0;
        }
        value = (value << 7) | (uint64_t)(byte & 0x7fU);
        if ((byte & 0x80U) == 0)
        {
            *result = value;
            return 1;
        }
    }
    return 0;
}

static uint64_t vector_field_value(const wfc_vector_t *vector,
                                   const wfc_field_t *field)
{
    if (field->is_constant)
    {
        return field->constant;
    }
    if (field->length_of != NULL)
    {
        const wfc_assignment_t *target = find_assignment(vector,
                                                         field->length_of);
        return target == NULL ? 0 : target->byte_count;
    }
    return find_assignment(vector, field->name)->value;
}

static int check_vector_v3(const wfc_protocol_t *protocol,
                           const wfc_message_t *message,
                           const wfc_vector_t *vector,
                           wfc_error_t *error)
{
    uint8_t *encoded;
    uint64_t *decoded_values;
    size_t bit_position = 0;
    size_t input_position = 0;
    size_t index;
    int success = 0;

    for (index = 0; index < message->field_count; index++)
    {
        if (message->fields[index].type == WFC_RECORDS ||
            message->fields[index].type == WFC_CHOICE)
        {
            wfc_set_error(error, vector->location,
                          "message `%s` contains records or choices and must be tested through its generated runtime program",
                          message->name);
            return 0;
        }
    }

    for (index = 0; index < vector->assignment_count; index++)
    {
        const wfc_assignment_t *assignment = &vector->assignments[index];
        const wfc_field_t *field = find_field(message, assignment->name, NULL);

        if (field == NULL)
        {
            wfc_set_error(error, assignment->location,
                          "vector `%s` assigns unknown field `%s`",
                          vector->name, assignment->name);
            return 0;
        }
        if (field->is_constant || field->length_of != NULL ||
            field->count_of != NULL)
        {
            wfc_set_error(error, assignment->location,
                          "vector `%s` may not assign derived or constant field `%s`",
                          vector->name, field->name);
            return 0;
        }
        if (field->type == WFC_BYTES && !assignment->is_bytes)
        {
            wfc_set_error(error, assignment->location,
                          "byte field `%s` requires an octet array", field->name);
            return 0;
        }
        if (field->type == WFC_RECORDS || field->type == WFC_CHOICE)
        {
            wfc_set_error(error, assignment->location,
                          "record field `%s` is supplied through a record list",
                          field->name);
            return 0;
        }
        if (field->type != WFC_BYTES && assignment->is_bytes)
        {
            wfc_set_error(error, assignment->location,
                          "scalar field `%s` requires an unsigned integer",
                          field->name);
            return 0;
        }
        if (field->type != WFC_BYTES && field->type != WFC_SDNV &&
            !wfc_value_fits(assignment->value, wfc_field_width(field)))
        {
            wfc_set_error(error, assignment->location,
                          "value %llu does not fit in a %u-bit field",
                          (unsigned long long)assignment->value,
                          (unsigned)wfc_field_width(field));
            return 0;
        }
    }
    for (index = 0; index < message->field_count; index++)
    {
        const wfc_field_t *field = &message->fields[index];

        if (!field->is_constant && field->length_of == NULL &&
            field->count_of == NULL &&
            find_assignment(vector, field->name) == NULL)
        {
            wfc_set_error(error, vector->location,
                          "vector `%s` has no value for field `%s`",
                          vector->name, field->name);
            return 0;
        }
    }

    encoded = calloc(vector->wire_count == 0 ? 1 : vector->wire_count, 1);
    decoded_values = calloc(message->field_count, sizeof(*decoded_values));
    if (encoded == NULL || decoded_values == NULL)
    {
        wfc_set_error(error, vector->location, "out of memory");
        goto done;
    }
    for (index = 0; index < message->field_count; index++)
    {
        const wfc_field_t *field = &message->fields[index];
        uint64_t value = vector_field_value(vector, field);

        if (field->type == WFC_BITS)
        {
            if (bit_position + field->width > vector->wire_count * 8U)
            {
                goto size_error;
            }
            write_bits(encoded, bit_position, field->width, value,
                       protocol->bit_order);
            bit_position += field->width;
            continue;
        }
        if (bit_position % 8U != 0)
        {
            wfc_set_error(error, field->location,
                          "field `%s` is not octet-aligned", field->name);
            goto done;
        }
        {
            size_t position = bit_position / 8U;

            if (field->type == WFC_SDNV)
            {
                if (!write_sdnv_vector(encoded, vector->wire_count,
                                       &position, value))
                {
                    goto size_error;
                }
            }
            else if (field->type == WFC_BYTES)
            {
                const wfc_assignment_t *assignment = find_assignment(
                    vector, field->name);
                if (assignment->byte_count > vector->wire_count - position)
                {
                    goto size_error;
                }
                memcpy(encoded + position, assignment->bytes,
                       assignment->byte_count);
                position += assignment->byte_count;
            }
            else
            {
                size_t octets = wfc_field_width(field) / 8U;
                if (octets > vector->wire_count - position)
                {
                    goto size_error;
                }
                write_scalar(encoded, position, octets, value,
                             protocol->byte_order);
                position += octets;
            }
            bit_position = position * 8U;
        }
    }
    if (bit_position != vector->wire_count * 8U)
    {
        goto size_error;
    }
    if (memcmp(encoded, vector->wire, vector->wire_count) != 0)
    {
        for (index = 0; index < vector->wire_count; index++)
        {
            if (encoded[index] != vector->wire[index])
            {
                wfc_set_error(error, vector->location,
                              "vector `%s` encodes octet %zu as %02X, but `wire` contains %02X",
                              vector->name, index, encoded[index],
                              vector->wire[index]);
                goto done;
            }
        }
    }

    bit_position = 0;
    for (index = 0; index < message->field_count; index++)
    {
        const wfc_field_t *field = &message->fields[index];
        uint64_t decoded = 0;
        uint64_t wanted = vector_field_value(vector, field);

        if (field->type == WFC_BITS)
        {
            decoded = read_bits(vector->wire, bit_position, field->width,
                                protocol->bit_order);
            bit_position += field->width;
        }
        else
        {
            input_position = bit_position / 8U;
            if (field->type == WFC_SDNV)
            {
                if (!read_sdnv_vector(vector->wire, vector->wire_count,
                                      &input_position, &decoded))
                {
                    wfc_set_error(error, vector->location,
                                  "vector `%s` contains an invalid SDNV",
                                  vector->name);
                    goto done;
                }
            }
            else if (field->type == WFC_BYTES)
            {
                const wfc_assignment_t *assignment = find_assignment(
                    vector, field->name);
                size_t length_index = 0;
                size_t byte_count;

                if (field->length_from == NULL)
                {
                    byte_count = vector->wire_count - input_position;
                }
                else
                {
                    find_field(message, field->length_from, &length_index);
                    byte_count = (size_t)decoded_values[length_index];
                }
                if (byte_count > vector->wire_count - input_position ||
                    byte_count != assignment->byte_count ||
                    memcmp(vector->wire + input_position, assignment->bytes,
                           byte_count) != 0)
                {
                    wfc_set_error(error, vector->location,
                                  "vector `%s` decodes byte field `%s` incorrectly",
                                  vector->name, field->name);
                    goto done;
                }
                input_position += byte_count;
                bit_position = input_position * 8U;
                continue;
            }
            else
            {
                size_t octets = wfc_field_width(field) / 8U;
                if (octets > vector->wire_count - input_position)
                {
                    goto size_error;
                }
                decoded = read_scalar(vector->wire, input_position, octets,
                                      protocol->byte_order);
                input_position += octets;
            }
            bit_position = input_position * 8U;
        }
        decoded_values[index] = decoded;
        if (decoded != wanted)
        {
            wfc_set_error(error, vector->location,
                          "vector `%s` decodes field `%s` as %llu, expected %llu",
                          vector->name, field->name,
                          (unsigned long long)decoded,
                          (unsigned long long)wanted);
            goto done;
        }
    }
    if (bit_position != vector->wire_count * 8U)
    {
        goto size_error;
    }
    success = 1;
    goto done;

size_error:
    wfc_set_error(error, vector->location,
                  "vector `%s` wire length does not match message `%s`",
                  vector->name, message->name);
done:
    free(encoded);
    free(decoded_values);
    return success;
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
    else if (field->type == WFC_BYTES)
        return;
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
    if (field->type == WFC_BYTES)
        return 0;
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
    size_t variable_octets = 0;
    size_t wire_octets;

    if (protocol->version >= 3)
    {
        return check_vector_v3(protocol, message, vector, error);
    }

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
        if (field->type == WFC_BYTES && !assignment->is_bytes) {
            wfc_set_error(error, assignment->location,
                          "byte field `%s` requires an octet array", field->name);
            return 0;
        }
        if (field->type != WFC_BYTES && assignment->is_bytes) {
            wfc_set_error(error, assignment->location,
                          "scalar field `%s` requires an unsigned integer", field->name);
            return 0;
        }
        if (field->type == WFC_BYTES) {
            variable_octets = assignment->byte_count;
        } else if (!wfc_value_fits(assignment->value, wfc_field_width(field))) {
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
    if (octets > SIZE_MAX - variable_octets) {
        wfc_set_error(error, vector->location, "vector wire size exceeds host limit");
        return 0;
    }
    wire_octets = octets + variable_octets;
    if (vector->wire_count != wire_octets) {
        wfc_set_error(error, vector->location,
                      "vector `%s` has %zu wire octets; message `%s` requires %zu",
                      vector->name, vector->wire_count, message->name, wire_octets);
        return 0;
    }

    encoded = calloc(wire_octets == 0 ? 1 : wire_octets, 1);
    if (encoded == NULL) {
        wfc_set_error(error, vector->location, "out of memory");
        return 0;
    }
    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        const wfc_assignment_t *assignment;
        if (field->type == WFC_BYTES) {
            assignment = find_assignment(vector, field->name);
            memcpy(encoded + octets, assignment->bytes, assignment->byte_count);
            continue;
        }
        uint64_t value = field->is_constant
                             ? field->constant
                             : find_assignment(vector, field->name)->value;
        write_field(encoded, offsets[index], field, value, protocol);
    }
    for (index = 0; index < wire_octets; index++)
        if (encoded[index] != vector->wire[index]) {
            mismatch = index;
            break;
        }

    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        if (field->type == WFC_BYTES) {
            const wfc_assignment_t *wanted = find_assignment(vector, field->name);
            if (wanted->byte_count != vector->wire_count - octets ||
                memcmp(wanted->bytes, vector->wire + octets,
                       wanted->byte_count) != 0) {
                wfc_set_error(error, vector->location,
                              "vector `%s` decodes byte field `%s` incorrectly",
                              vector->name, field->name);
                free(encoded);
                return 0;
            }
            continue;
        }
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
    if (protocol->version == 4) { return wfc_records_check(protocol, summary, error); }
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
