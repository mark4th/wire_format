#include "wfc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WI_HEADER_SIZE 64U
#define WI_MESSAGE_RECORD_SIZE 32U
#define WI_NO_STRING UINT32_MAX
#define WI_MAX_CALLER_FIELDS 16U
#define WI_FIXED_FIELD_FAULT UINT64_C(1)
#define WI_PROTOCOL_STRING_COUNT 4U
#define WI_MESSAGE_FIXED_STRING_COUNT 4U
#define WI_FLAG_WIRE_BYTE_ORDER_LITTLE (1U << 0)
#define WI_FLAG_WIRE_BIT_ORDER_LSB (1U << 1)

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} bytes_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} string_t;

typedef struct {
    const wfc_message_t *message;
    size_t *offsets;
    size_t octets;
    char *encode;
    char *decode;
    const wfc_field_t **value_fields;
    uint8_t *widths;
    uint8_t *swaps;
    size_t value_count;
    int variable;
} compiled_message_t;

typedef struct {
    uint32_t string_section_offset;
    uint32_t string_count;
    uint32_t widths_offset;
    uint32_t swaps_offset;
} image_message_t;

typedef struct {
    uint8_t *bytes;
    size_t size;
    uint32_t flags;
    uint32_t message_table_offset;
    uint32_t value_table_offset;
    uint32_t value_table_size;
    uint32_t protocol_strings_offset;
    uint32_t string_offsets_offset;
    uint32_t string_offset_count;
    uint32_t string_table_offset;
    uint32_t string_table_size;
    image_message_t *messages;
} image_t;

static int reserve(void **data, size_t *capacity, size_t needed, size_t item_size)
{
    size_t next;
    void *replacement;
    if (needed <= *capacity)
        return 1;
    next = *capacity == 0 ? 64 : *capacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2)
            return 0;
        next *= 2;
    }
    if (next > SIZE_MAX / item_size)
        return 0;
    replacement = realloc(*data, next * item_size);
    if (replacement == NULL)
        return 0;
    *data = replacement;
    *capacity = next;
    return 1;
}

static int bytes_append(bytes_t *bytes, const void *data, size_t length)
{
    if (length > SIZE_MAX - bytes->length ||
        !reserve((void **)&bytes->data, &bytes->capacity,
                 bytes->length + length, 1))
        return 0;
    memcpy(bytes->data + bytes->length, data, length);
    bytes->length += length;
    return 1;
}

static int bytes_byte(bytes_t *bytes, uint8_t value)
{
    return bytes_append(bytes, &value, 1);
}

static int string_append_n(string_t *string, const char *text, size_t length)
{
    if (length > SIZE_MAX - string->length - 1 ||
        !reserve((void **)&string->data, &string->capacity,
                 string->length + length + 1, 1))
        return 0;
    memcpy(string->data + string->length, text, length);
    string->length += length;
    string->data[string->length] = 0;
    return 1;
}

static int string_append(string_t *string, const char *text)
{
    return string_append_n(string, text, strlen(text));
}

static int string_printf(string_t *string, const char *format, ...)
{
    va_list arguments;
    va_list copy;
    int length;

    va_start(arguments, format);
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0 || !reserve((void **)&string->data, &string->capacity,
                               string->length + (size_t)length + 1, 1)) {
        va_end(arguments);
        return 0;
    }
    vsnprintf(string->data + string->length, (size_t)length + 1, format, arguments);
    va_end(arguments);
    string->length += (size_t)length;
    return 1;
}

static int push_literal(string_t *output, uint64_t value)
{
    return string_printf(output, "%%{%llu}", (unsigned long long)value);
}

static uint64_t bit_mask(uint8_t width)
{
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

static int append_bit_operation(string_t *output, uint8_t width,
                                uint8_t position, char operation)
{
    return push_literal(output, width) && push_literal(output, position) &&
           string_printf(output, "%%%c", operation);
}

static int append_constant_check(string_t *output, uint64_t expected)
{
    return push_literal(output, expected) &&
           string_append(output, "%=%!%?%t") &&
           push_literal(output, WI_FIXED_FIELD_FAULT) &&
           string_append(output, "%E%;");
}

static int compile_scalar(const wfc_field_t *field, size_t supplied_index,
                          int supplied, wfc_byte_order_t byte_order,
                          string_t *encode, string_t *decode)
{
    const char *encode_op = NULL;
    const char *decode_op = NULL;
    uint8_t octets = wfc_field_width(field) / 8;
    uint8_t index;

    if (field->type == WFC_SDNV) {
        if (supplied) {
            return string_printf(encode, "%%p{%zu}%%d", supplied_index + 1) &&
                   string_printf(decode, "%%D%%P%c",
                                 (char)('a' + supplied_index));
        }
        return push_literal(encode, field->constant) &&
               string_append(encode, "%d") && string_append(decode, "%D") &&
               append_constant_check(decode, field->constant);
    }

    if (supplied) {
        switch (field->type) {
        case WFC_U8: encode_op = "%b"; decode_op = "%B"; break;
        case WFC_U16: encode_op = "%w"; decode_op = "%S"; break;
        case WFC_U32: encode_op = "%W"; decode_op = "%L"; break;
        case WFC_U64: encode_op = "%q"; decode_op = "%Q"; break;
        case WFC_SDNV: return 0;
        case WFC_BITS:
        case WFC_BYTES:
        case WFC_RECORDS:
        case WFC_CHOICE: return 0;
        }
        return string_printf(encode, "%%p{%zu}%s", supplied_index + 1, encode_op) &&
               string_printf(decode, "%s%%P%c", decode_op,
                             (char)('a' + supplied_index));
    }

    for (index = 0; index < octets; index++) {
        uint8_t shift = byte_order == WFC_BIG_ENDIAN
                            ? (uint8_t)((octets - 1 - index) * 8)
                            : (uint8_t)(index * 8);
        uint8_t byte = (uint8_t)(field->constant >> shift);
        if (!push_literal(encode, byte) || !string_append(encode, "%b") ||
            !string_append(decode, "%B") || !append_constant_check(decode, byte))
            return 0;
    }
    return 1;
}

static int compile_bits(const wfc_field_t *field, size_t bit_offset,
                        size_t supplied_index, int supplied,
                        wfc_bit_order_t bit_order,
                        string_t *encode, string_t *decode)
{
    uint8_t width = field->width;
    uint8_t consumed = 0;
    size_t cursor = bit_offset;
    int first_fragment = 1;

    while (consumed < width) {
        size_t within = cursor % 8;
        uint8_t remaining = width - consumed;
        uint8_t chunk = (uint8_t)((8 - within) < remaining ? (8 - within) : remaining);
        uint8_t source_shift;
        uint8_t position;
        uint64_t mask = bit_mask(chunk);
        if (bit_order == WFC_MSB_FIRST) {
            source_shift = width - consumed - chunk;
            position = (uint8_t)(8 - within - chunk);
        } else {
            source_shift = consumed;
            position = (uint8_t)within;
        }

        if (supplied) {
            if (!string_printf(encode, "%%p{%zu}", supplied_index + 1))
                return 0;
            if (source_shift != 0 &&
                (!push_literal(encode, UINT64_C(1) << source_shift) ||
                 !string_append(encode, "%/")))
                return 0;
            if (!push_literal(encode, mask) || !string_append(encode, "%&") ||
                !append_bit_operation(encode, chunk, position, 'x'))
                return 0;

            if (!first_fragment && bit_order == WFC_MSB_FIRST &&
                (!push_literal(decode, UINT64_C(1) << chunk) ||
                 !string_append(decode, "%*")))
                return 0;
            if (!append_bit_operation(decode, chunk, position, 'X'))
                return 0;
            if (!first_fragment) {
                if (bit_order == WFC_LSB_FIRST &&
                    (!push_literal(decode, UINT64_C(1) << consumed) ||
                     !string_append(decode, "%*")))
                    return 0;
                if (!string_append(decode, "%+"))
                    return 0;
            }
        } else {
            uint64_t fragment = (field->constant >> source_shift) & mask;
            if (!push_literal(encode, fragment) ||
                !append_bit_operation(encode, chunk, position, 'x') ||
                !append_bit_operation(decode, chunk, position, 'X') ||
                !append_constant_check(decode, fragment))
                return 0;
        }

        consumed += chunk;
        cursor += chunk;
        first_fragment = 0;
        if (cursor % 8 == 0 &&
            (!string_append(encode, "%f") || !string_append(decode, "%f")))
            return 0;
    }
    if (supplied && !string_printf(decode, "%%P%c", (char)('a' + supplied_index)))
        return 0;
    return 1;
}

static uint8_t scalar_swap(const wfc_field_t *field, wfc_byte_order_t order)
{
    if (order != WFC_LITTLE_ENDIAN)
        return 0;
    switch (field->type) {
    case WFC_U16: return 2;
    case WFC_U32: return 4;
    case WFC_U64: return 8;
    case WFC_SDNV: return 0;
    case WFC_BITS:
    case WFC_BYTES:
    case WFC_RECORDS:
    case WFC_CHOICE:
    case WFC_U8: return 0;
    }
    return 0;
}

static size_t field_position(const wfc_message_t *message, const char *name)
{
    size_t index;

    for (index = 0; index < message->field_count; index++) {
        if (strcmp(message->fields[index].name, name) == 0)
            return index;
    }
    return SIZE_MAX;
}

static size_t message_position(const wfc_protocol_t *protocol, const char *name)
{
    size_t index;

    for (index = 0; index < protocol->message_count; index++) {
        if (strcmp(protocol->messages[index].name, name) == 0)
            return index;
    }
    return SIZE_MAX;
}

static size_t caller_field_count(const wfc_message_t *message)
{
    size_t index;
    size_t count = 0;

    for (index = 0; index < message->field_count; index++) {
        const wfc_field_t *field = &message->fields[index];
        if (!field->is_constant && field->length_of == NULL &&
            field->count_of == NULL)
            count++;
    }
    return count;
}

static int compile_derived_length(const wfc_field_t *field,
                                  size_t slice_slot, size_t variable,
                                  string_t *encode, string_t *decode)
{
    const char *encode_op = NULL;
    const char *decode_op = NULL;

    switch (field->type) {
    case WFC_U8: encode_op = "%b"; decode_op = "%B"; break;
    case WFC_U16: encode_op = "%w"; decode_op = "%S"; break;
    case WFC_U32: encode_op = "%W"; decode_op = "%L"; break;
    case WFC_U64: encode_op = "%q"; decode_op = "%Q"; break;
    case WFC_SDNV: encode_op = "%d"; decode_op = "%D"; break;
    case WFC_BITS:
    case WFC_BYTES:
    case WFC_RECORDS:
    case WFC_CHOICE:
        return 0;
    }
    return string_printf(encode, "%%{%zu}%%z%s", slice_slot, encode_op) &&
           string_printf(decode, "%s%%P%c", decode_op,
                         (char)('a' + variable));
}

static int compile_derived_bits(const wfc_field_t *field, size_t bit_offset,
                                const char *source, size_t variable,
                                wfc_bit_order_t bit_order,
                                string_t *encode, string_t *decode)
{
    uint8_t width = field->width;
    uint8_t consumed = 0;
    size_t cursor = bit_offset;
    int first_fragment = 1;

    while (consumed < width) {
        size_t within = cursor % 8;
        uint8_t remaining = width - consumed;
        uint8_t chunk = (uint8_t)((8 - within) < remaining
                                      ? (8 - within) : remaining);
        uint8_t source_shift;
        uint8_t position;
        uint64_t mask = bit_mask(chunk);

        if (bit_order == WFC_MSB_FIRST) {
            source_shift = width - consumed - chunk;
            position = (uint8_t)(8 - within - chunk);
        } else {
            source_shift = consumed;
            position = (uint8_t)within;
        }
        if (!string_append(encode, source))
            return 0;
        if (source_shift != 0 &&
            (!push_literal(encode, UINT64_C(1) << source_shift) ||
             !string_append(encode, "%/")))
            return 0;
        if (!push_literal(encode, mask) || !string_append(encode, "%&") ||
            !append_bit_operation(encode, chunk, position, 'x'))
            return 0;

        if (!first_fragment && bit_order == WFC_MSB_FIRST &&
            (!push_literal(decode, UINT64_C(1) << chunk) ||
             !string_append(decode, "%*")))
            return 0;
        if (!append_bit_operation(decode, chunk, position, 'X'))
            return 0;
        if (!first_fragment) {
            if (bit_order == WFC_LSB_FIRST &&
                (!push_literal(decode, UINT64_C(1) << consumed) ||
                 !string_append(decode, "%*")))
                return 0;
            if (!string_append(decode, "%+"))
                return 0;
        }
        consumed += chunk;
        cursor += chunk;
        first_fragment = 0;
        if (cursor % 8 == 0 &&
            (!string_append(encode, "%f") || !string_append(decode, "%f")))
            return 0;
    }
    return string_printf(decode, "%%P%c", (char)('a' + variable));
}

static int compile_derived_count(const wfc_field_t *field, size_t bit_offset,
                                 size_t record_slot, size_t variable,
                                 wfc_bit_order_t bit_order,
                                 string_t *encode, string_t *decode)
{
    string_t source = {0};
    const char *encode_op = NULL;
    const char *decode_op = NULL;
    int success;

    if (!string_printf(&source, "%%{%zu}%%k", record_slot))
        return 0;
    if (field->type == WFC_BITS) {
        success = compile_derived_bits(field, bit_offset, source.data, variable,
                                       bit_order, encode, decode);
        free(source.data);
        return success;
    }
    switch (field->type) {
    case WFC_U8: encode_op = "%b"; decode_op = "%B"; break;
    case WFC_U16: encode_op = "%w"; decode_op = "%S"; break;
    case WFC_U32: encode_op = "%W"; decode_op = "%L"; break;
    case WFC_U64: encode_op = "%q"; decode_op = "%Q"; break;
    case WFC_SDNV: encode_op = "%d"; decode_op = "%D"; break;
    case WFC_BITS:
    case WFC_BYTES:
    case WFC_RECORDS:
    case WFC_CHOICE:
        free(source.data);
        return 0;
    }
    success = string_append(encode, source.data) &&
              string_append(encode, encode_op) &&
              string_printf(decode, "%s%%P%c", decode_op,
                            (char)('a' + variable));
    free(source.data);
    return success;
}

static int compile_message(const wfc_protocol_t *protocol,
                           const wfc_message_t *message,
                           compiled_message_t *compiled, wfc_error_t *error)
{
    size_t bit_offset = 0;
    size_t field_index;
    size_t next_variable;
    size_t *ordinals = NULL;
    size_t *variables = NULL;
    string_t encode = {0};
    string_t decode = {0};

    memset(compiled, 0, sizeof(*compiled));
    compiled->message = message;
    compiled->offsets = calloc(message->field_count, sizeof(*compiled->offsets));
    compiled->value_fields = calloc(message->field_count, sizeof(*compiled->value_fields));
    compiled->widths = malloc(message->field_count == 0 ? 1 : message->field_count);
    compiled->swaps = malloc(message->field_count == 0 ? 1 : message->field_count);
    ordinals = malloc(message->field_count * sizeof(*ordinals));
    variables = malloc(message->field_count * sizeof(*variables));
    if (compiled->offsets == NULL || compiled->value_fields == NULL ||
        compiled->widths == NULL || compiled->swaps == NULL ||
        ordinals == NULL || variables == NULL) {
        wfc_set_error(error, message->location, "out of memory");
        goto fail;
    }

    for (field_index = 0; field_index < message->field_count; field_index++) {
        const wfc_field_t *field = &message->fields[field_index];
        int supplied = !field->is_constant && field->length_of == NULL &&
                       field->count_of == NULL;

        if (field->type == WFC_BYTES || field->type == WFC_SDNV ||
            field->type == WFC_RECORDS || field->type == WFC_CHOICE)
            compiled->variable = 1;

        ordinals[field_index] = SIZE_MAX;
        variables[field_index] = SIZE_MAX;
        if (supplied) {
            if (compiled->value_count == WI_MAX_CALLER_FIELDS) {
                wfc_set_error(error, message->location,
                              "the compiled format supports at most %u caller fields per message; `%s` has more",
                              WI_MAX_CALLER_FIELDS, message->name);
                goto fail;
            }
            if (field->type == WFC_BITS && field->width == 64) {
                wfc_set_error(error, field->location,
                              "the compiled format cannot represent caller field `%s` as `bits 64`; use `u64` when it is octet-aligned",
                              field->name);
                goto fail;
            }
            ordinals[field_index] = compiled->value_count;
            compiled->value_fields[compiled->value_count] = field;
            compiled->widths[compiled->value_count] = wfc_field_width(field);
            compiled->swaps[compiled->value_count] = scalar_swap(field, protocol->byte_order);
            compiled->value_count++;
        }
    }
    next_variable = compiled->value_count;
    for (field_index = 0; field_index < message->field_count; field_index++) {
        const wfc_field_t *field = &message->fields[field_index];

        if (field->is_constant || field->type == WFC_BYTES ||
            field->type == WFC_RECORDS || field->type == WFC_CHOICE)
            continue;
        if (field->length_of == NULL && field->count_of == NULL) {
            variables[field_index] = ordinals[field_index];
        } else {
            if (next_variable >= 26) {
                wfc_set_error(error, field->location,
                              "message `%s` needs more than 26 decode variables",
                              message->name);
                goto fail;
            }
            variables[field_index] = next_variable++;
        }
    }

    for (field_index = 0; field_index < message->field_count; field_index++) {
        const wfc_field_t *field = &message->fields[field_index];
        size_t supplied_index = ordinals[field_index];
        int supplied = supplied_index != SIZE_MAX;

        compiled->offsets[field_index] = bit_offset;
        if (field->count_of != NULL) {
            size_t target_index = field_position(message, field->count_of);
            size_t target_slot = ordinals[target_index];

            if (!compile_derived_count(field, bit_offset, target_slot,
                                       variables[field_index],
                                       protocol->bit_order, &encode, &decode)) {
                wfc_set_error(error, field->location, "out of memory");
                goto fail;
            }
        } else if (field->type == WFC_BITS) {
            if (!compile_bits(field, bit_offset, supplied_index, supplied,
                              protocol->bit_order, &encode, &decode)) {
                wfc_set_error(error, field->location, "out of memory");
                goto fail;
            }
        } else if (field->type == WFC_BYTES) {
            if (field->length_from == NULL) {
                if (!string_printf(&encode, "%%{%zu}%%v", supplied_index) ||
                    !string_printf(&decode, "%%{%zu}%%R", supplied_index)) {
                    wfc_set_error(error, field->location, "out of memory");
                    goto fail;
                }
            } else {
                size_t length_index = field_position(message,
                                                     field->length_from);
                if (!string_printf(&encode, "%%{%zu}%%z%%{%zu}%%V",
                                   supplied_index, supplied_index) ||
                    !string_printf(&decode, "%%g%c%%{%zu}%%N",
                                   (char)('a' + variables[length_index]),
                                   supplied_index)) {
                    wfc_set_error(error, field->location, "out of memory");
                    goto fail;
                }
            }
        } else if (field->length_of != NULL) {
            size_t target_index = field_position(message, field->length_of);
            size_t target_slot = ordinals[target_index];

            if (!compile_derived_length(field, target_slot,
                                        variables[field_index],
                                        &encode, &decode)) {
                wfc_set_error(error, field->location, "out of memory");
                goto fail;
            }
        } else if (field->type == WFC_RECORDS) {
            size_t child_index = message_position(protocol, field->message);
            size_t count_index = field_position(message, field->count_from);
            size_t child_fields = caller_field_count(
                &protocol->messages[child_index]);

            if (!string_printf(&encode,
                               "%%{%zu}%%k%%{%zu}%%{%zu}%%J[%zu]",
                               supplied_index, supplied_index,
                               child_fields, child_index) ||
                !string_printf(&decode,
                               "%%g%c%%{%zu}%%{%zu}%%J[%zu]",
                               (char)('a' + variables[count_index]),
                               supplied_index, child_fields, child_index)) {
                wfc_set_error(error, field->location, "out of memory");
                goto fail;
            }
        } else if (field->type == WFC_CHOICE) {
            size_t selector_index = field_position(message, field->select_from);
            size_t case_index;

            for (case_index = 0; case_index < field->case_count; case_index++) {
                size_t child_index = message_position(
                    protocol, field->cases[case_index].message);
                size_t child_fields = caller_field_count(
                    &protocol->messages[child_index]);

                if (!string_printf(&encode,
                                   "%%?%%p{%zu}%%{%llu}%%=%%t%%{1}%%{%zu}%%{%zu}%%J[%zu]%%;",
                                   ordinals[selector_index] + 1,
                                   (unsigned long long)field->cases[case_index].value,
                                   supplied_index, child_fields, child_index) ||
                    !string_printf(&decode,
                                   "%%?%%g%c%%{%llu}%%=%%t%%{1}%%{%zu}%%{%zu}%%J[%zu]%%;",
                                   (char)('a' + variables[selector_index]),
                                   (unsigned long long)field->cases[case_index].value,
                                   supplied_index, child_fields, child_index)) {
                    wfc_set_error(error, field->location, "out of memory");
                    goto fail;
                }
            }
        } else if (!compile_scalar(field, supplied_index, supplied,
                                   protocol->byte_order, &encode, &decode)) {
            wfc_set_error(error, field->location, "out of memory");
            goto fail;
        }
        bit_offset += wfc_field_width(field);
    }
    compiled->octets = bit_offset / 8;
    compiled->encode = encode.data == NULL ? wfc_duplicate("", 0) : encode.data;
    compiled->decode = decode.data == NULL ? wfc_duplicate("", 0) : decode.data;
    if (compiled->encode == NULL || compiled->decode == NULL) {
        wfc_set_error(error, message->location, "out of memory");
        goto fail;
    }
    free(ordinals);
    free(variables);
    return 1;

fail:
    free(encode.data);
    free(decode.data);
    free(compiled->offsets);
    free(compiled->value_fields);
    free(compiled->widths);
    free(compiled->swaps);
    free(ordinals);
    free(variables);
    memset(compiled, 0, sizeof(*compiled));
    return 0;
}

static void compiled_free(compiled_message_t *messages, size_t count)
{
    size_t index;
    if (messages == NULL)
        return;
    for (index = 0; index < count; index++) {
        free(messages[index].offsets);
        free(messages[index].encode);
        free(messages[index].decode);
        free(messages[index].value_fields);
        free(messages[index].widths);
        free(messages[index].swaps);
    }
    free(messages);
}

static int push_u32(uint32_t **items, size_t *count, size_t *capacity, uint32_t value)
{
    if (!reserve((void **)items, capacity, *count + 1, sizeof(**items)))
        return 0;
    (*items)[(*count)++] = value;
    return 1;
}

static int add_string(bytes_t *table, const char *text, uint32_t *offset,
                      wfc_location_t location, wfc_error_t *error)
{
    size_t length = strlen(text);
    if (table->length > UINT32_MAX) {
        wfc_set_error(error, location, "string table offset exceeds the WI 32-bit limit");
        return 0;
    }
    *offset = (uint32_t)table->length;
    if (!bytes_append(table, text, length) || !bytes_byte(table, 0)) {
        wfc_set_error(error, location, "out of memory");
        return 0;
    }
    return 1;
}

static int checked_u32(size_t value, const char *what, wfc_location_t location,
                       uint32_t *result, wfc_error_t *error)
{
    if (value > UINT32_MAX) {
        wfc_set_error(error, location, "%s exceeds the WI 32-bit limit", what);
        return 0;
    }
    *result = (uint32_t)value;
    return 1;
}

static int put_u16(bytes_t *output, uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return bytes_append(output, bytes, sizeof(bytes));
}

static int put_u32(bytes_t *output, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };
    return bytes_append(output, bytes, sizeof(bytes));
}

static int build_image(const wfc_protocol_t *protocol,
                       const compiled_message_t *compiled, image_t *image,
                       wfc_error_t *error)
{
    bytes_t strings = {0};
    bytes_t values = {0};
    bytes_t output = {0};
    uint32_t *string_offsets = NULL;
    size_t string_count = 0;
    size_t string_capacity = 0;
    size_t *message_string_indices = NULL;
    size_t *width_indices = NULL;
    size_t *swap_indices = NULL;
    size_t message_index;
    size_t message_table_size;
    size_t value_table_offset;
    size_t string_offsets_offset;
    size_t string_table_offset;
    size_t file_size;
    uint32_t temporary;
    int success = 0;

    memset(image, 0, sizeof(*image));
    image->messages = calloc(protocol->message_count, sizeof(*image->messages));
    message_string_indices = calloc(protocol->message_count, sizeof(*message_string_indices));
    width_indices = calloc(protocol->message_count, sizeof(*width_indices));
    swap_indices = calloc(protocol->message_count, sizeof(*swap_indices));
    if (image->messages == NULL || message_string_indices == NULL ||
        width_indices == NULL || swap_indices == NULL) {
        wfc_set_error(error, protocol->location, "out of memory");
        goto done;
    }

#define ADD_PROTOCOL_STRING(text, absent) do { \
    uint32_t value; \
    if ((text) == NULL && (absent)) value = WI_NO_STRING; \
    else if (!add_string(&strings, (text), &value, protocol->location, error)) goto done; \
    if (!push_u32(&string_offsets, &string_count, &string_capacity, value)) { \
        wfc_set_error(error, protocol->location, "out of memory"); goto done; \
    } \
} while (0)
    ADD_PROTOCOL_STRING(protocol->name, 0);
    ADD_PROTOCOL_STRING(protocol->description, 0);
    ADD_PROTOCOL_STRING(protocol->standard, 1);
    ADD_PROTOCOL_STRING(protocol->reference, 1);
#undef ADD_PROTOCOL_STRING

    for (message_index = 0; message_index < protocol->message_count; message_index++) {
        const compiled_message_t *item = &compiled[message_index];
        const wfc_message_t *message = item->message;
        const char *texts[4] = {message->name, message->description,
                                item->encode, item->decode};
        size_t index;
        message_string_indices[message_index] = string_count;
        for (index = 0; index < 4; index++) {
            uint32_t offset;
            if (!add_string(&strings, texts[index], &offset, message->location, error) ||
                !push_u32(&string_offsets, &string_count, &string_capacity, offset)) {
                if (error->message[0] == 0)
                    wfc_set_error(error, message->location, "out of memory");
                goto done;
            }
        }
        for (index = 0; index < message->field_count; index++) {
            uint32_t offset;
            if (!add_string(&strings, message->fields[index].name, &offset,
                            message->fields[index].location, error) ||
                !push_u32(&string_offsets, &string_count, &string_capacity, offset)) {
                if (error->message[0] == 0)
                    wfc_set_error(error, message->location, "out of memory");
                goto done;
            }
        }
    }

    for (message_index = 0; message_index < protocol->message_count; message_index++) {
        const compiled_message_t *item = &compiled[message_index];
        if (item->value_count == 0)
            continue;
        width_indices[message_index] = values.length;
        if (!bytes_append(&values, item->widths, item->value_count)) {
            wfc_set_error(error, item->message->location, "out of memory");
            goto done;
        }
        swap_indices[message_index] = values.length;
        if (!bytes_append(&values, item->swaps, item->value_count)) {
            wfc_set_error(error, item->message->location, "out of memory");
            goto done;
        }
    }

    if (protocol->message_count > (SIZE_MAX - WI_HEADER_SIZE) / WI_MESSAGE_RECORD_SIZE) {
        wfc_set_error(error, protocol->location, "message table size exceeds the host size limit");
        goto done;
    }
    message_table_size = protocol->message_count * WI_MESSAGE_RECORD_SIZE;
    value_table_offset = WI_HEADER_SIZE + message_table_size;
    if (values.length > SIZE_MAX - value_table_offset) {
        wfc_set_error(error, protocol->location, "string-offset section offset exceeds the host size limit");
        goto done;
    }
    string_offsets_offset = value_table_offset + values.length;
    if (string_offsets_offset > SIZE_MAX - 3) {
        wfc_set_error(error, protocol->location, "compiled section alignment exceeds the host size limit");
        goto done;
    }
    string_offsets_offset = (string_offsets_offset + 3) & ~(size_t)3;
    if (string_count > (SIZE_MAX - string_offsets_offset) / 4) {
        wfc_set_error(error, protocol->location, "string table offset exceeds the host size limit");
        goto done;
    }
    string_table_offset = string_offsets_offset + string_count * 4;
    if (strings.length > SIZE_MAX - string_table_offset) {
        wfc_set_error(error, protocol->location, "compiled file size exceeds the host size limit");
        goto done;
    }
    file_size = string_table_offset + strings.length;

    if (!checked_u32(value_table_offset, "value table offset", protocol->location,
                     &image->value_table_offset, error) ||
        !checked_u32(values.length, "value table size", protocol->location,
                     &image->value_table_size, error) ||
        !checked_u32(string_offsets_offset, "string-offset section offset", protocol->location,
                     &image->string_offsets_offset, error) ||
        !checked_u32(string_count, "string-offset count", protocol->location,
                     &image->string_offset_count, error) ||
        !checked_u32(string_table_offset, "string table offset", protocol->location,
                     &image->string_table_offset, error) ||
        !checked_u32(strings.length, "string table size", protocol->location,
                     &image->string_table_size, error) ||
        !checked_u32(file_size, "compiled file size", protocol->location,
                     &temporary, error))
        goto done;

    image->flags = (protocol->byte_order == WFC_LITTLE_ENDIAN
                        ? WI_FLAG_WIRE_BYTE_ORDER_LITTLE : 0) |
                   (protocol->bit_order == WFC_LSB_FIRST
                        ? WI_FLAG_WIRE_BIT_ORDER_LSB : 0);
    image->message_table_offset = WI_HEADER_SIZE;
    image->protocol_strings_offset = image->string_offsets_offset;

    if (!bytes_append(&output, "WI\0\0", 4) ||
        !put_u16(&output, protocol->version) ||
        !put_u16(&output, WI_HEADER_SIZE) ||
        !put_u32(&output, temporary) ||
        !put_u32(&output, image->flags) ||
        !put_u32(&output, (uint32_t)protocol->message_count) ||
        !put_u32(&output, WI_MESSAGE_RECORD_SIZE) ||
        !put_u32(&output, WI_HEADER_SIZE) ||
        !put_u32(&output, image->value_table_offset) ||
        !put_u32(&output, image->value_table_size) ||
        !put_u32(&output, image->protocol_strings_offset) ||
        !put_u32(&output, WI_PROTOCOL_STRING_COUNT) ||
        !put_u32(&output, image->string_offsets_offset) ||
        !put_u32(&output, image->string_offset_count) ||
        !put_u32(&output, image->string_table_offset) ||
        !put_u32(&output, image->string_table_size) ||
        !put_u32(&output, 0)) {
        wfc_set_error(error, protocol->location, "out of memory");
        goto done;
    }

    for (message_index = 0; message_index < protocol->message_count; message_index++) {
        const compiled_message_t *item = &compiled[message_index];
        image_message_t *record = &image->messages[message_index];
        size_t string_offset = string_offsets_offset + message_string_indices[message_index] * 4;
        if (!checked_u32(string_offset, "message string section offset",
                         item->message->location, &record->string_section_offset, error) ||
            !checked_u32(WI_MESSAGE_FIXED_STRING_COUNT + item->message->field_count,
                         "message string count", item->message->location,
                         &record->string_count, error))
            goto done;
        if (item->value_count != 0) {
            if (!checked_u32(value_table_offset + width_indices[message_index],
                             "field-width table offset", item->message->location,
                             &record->widths_offset, error) ||
                !checked_u32(value_table_offset + swap_indices[message_index],
                             "field-swap table offset", item->message->location,
                             &record->swaps_offset, error))
                goto done;
        }
        if (!put_u32(&output, record->string_section_offset) ||
            !put_u32(&output, record->string_count) ||
            !put_u32(&output, (uint32_t)item->octets) ||
            !put_u32(&output, (uint32_t)item->value_count) ||
            !put_u32(&output, record->widths_offset) ||
            !put_u32(&output, record->swaps_offset) ||
            !put_u32(&output, (uint32_t)item->variable) ||
            !put_u32(&output, 0)) {
            wfc_set_error(error, item->message->location, "out of memory");
            goto done;
        }
    }
    if (!bytes_append(&output, values.data, values.length)) {
        wfc_set_error(error, protocol->location, "out of memory");
        goto done;
    }
    while (output.length < string_offsets_offset)
        if (!bytes_byte(&output, 0)) {
            wfc_set_error(error, protocol->location, "out of memory");
            goto done;
        }
    for (message_index = 0; message_index < string_count; message_index++)
        if (!put_u32(&output, string_offsets[message_index])) {
            wfc_set_error(error, protocol->location, "out of memory");
            goto done;
        }
    if (!bytes_append(&output, strings.data, strings.length)) {
        wfc_set_error(error, protocol->location, "out of memory");
        goto done;
    }
    image->bytes = output.data;
    image->size = output.length;
    output.data = NULL;
    success = 1;

done:
    free(output.data);
    free(strings.data);
    free(values.data);
    free(string_offsets);
    free(message_string_indices);
    free(width_indices);
    free(swap_indices);
    if (!success) {
        free(image->messages);
        memset(image, 0, sizeof(*image));
    }
    return success;
}

static char *c_name(const char *name)
{
    size_t length = strlen(name);
    char *result = malloc(length + 1);
    size_t index;
    if (result == NULL)
        return NULL;
    for (index = 0; index < length; index++) {
        unsigned char character = (unsigned char)name[index];
        result[index] = character == '-' ? '_' : (char)toupper(character);
    }
    result[length] = 0;
    return result;
}

static int header_define(string_t *header, const char *name, uint32_t value)
{
    return string_printf(header, "#define %-52s %uu\n", name, value);
}

static int header_named_define(string_t *header, const char *prefix,
                               const char *suffix, uint32_t value)
{
    string_t name = {0};
    int success = string_printf(&name, "%s%s", prefix, suffix) &&
                  header_define(header, name.data, value);
    free(name.data);
    return success;
}

static int append_comment(string_t *header, const char *text)
{
    const unsigned char *scan = (const unsigned char *)text;
    if (!string_append(header, "/* "))
        return 0;
    while (*scan != 0) {
        if (scan[0] == '*' && scan[1] == '/') {
            if (!string_append(header, "* /"))
                return 0;
            scan += 2;
        } else if (scan[0] == 0xc2 && scan[1] >= 0x80 && scan[1] <= 0x9f) {
            if (!string_append(header, " "))
                return 0;
            scan += 2;
        } else {
            unsigned char character = *scan++;
            char output = (character < 0x20 || character == 0x7f) ? ' ' : (char)character;
            if (!string_append_n(header, &output, 1))
                return 0;
        }
    }
    return string_append(header, " */\n");
}

static int generate_header(const wfc_protocol_t *protocol,
                           const compiled_message_t *compiled,
                           const image_t *image, char **result,
                           wfc_error_t *error)
{
    string_t header = {0};
    char *prefix = c_name(protocol->name);
    string_t guard = {0};
    size_t message_index;
    int success = 0;

    if (prefix == NULL || !string_printf(&guard, "WF_GENERATED_%s_H", prefix) ||
        !string_append(&header, "/* Generated by wfc.  Do not edit. */\n"))
        goto done;
    {
        string_t protocol_comment = {0};
        if (!string_printf(&protocol_comment, "Protocol %s: %s",
                           protocol->name, protocol->description) ||
            !append_comment(&header, protocol_comment.data)) {
            free(protocol_comment.data);
            goto done;
        }
        free(protocol_comment.data);
    }
    if (!string_printf(&header, "#ifndef %s\n#define %s\n\n", guard.data, guard.data))
        goto done;

#define DEFINE(suffix, value) \
    do { if (!header_named_define(&header, prefix, suffix, value)) goto done; } while (0)
    DEFINE("_WI_FORMAT_VERSION", protocol->version);
    DEFINE("_WI_FILE_SIZE", (uint32_t)image->size);
    DEFINE("_WI_FLAGS", image->flags);
    DEFINE("_WI_MESSAGE_COUNT", (uint32_t)protocol->message_count);
    DEFINE("_WI_MESSAGE_TABLE_OFFSET", image->message_table_offset);
    DEFINE("_WI_MESSAGE_RECORD_SIZE", WI_MESSAGE_RECORD_SIZE);
    DEFINE("_WI_VALUE_TABLE_OFFSET", image->value_table_offset);
    DEFINE("_WI_VALUE_TABLE_SIZE", image->value_table_size);
    DEFINE("_WI_PROTOCOL_STRINGS_OFFSET", image->protocol_strings_offset);
    DEFINE("_WI_STRING_OFFSETS_OFFSET", image->string_offsets_offset);
    DEFINE("_WI_STRING_OFFSET_COUNT", image->string_offset_count);
    DEFINE("_WI_STRING_TABLE_OFFSET", image->string_table_offset);
    DEFINE("_WI_STRING_TABLE_SIZE", image->string_table_size);
    DEFINE("_WI_NO_STRING", WI_NO_STRING);
    DEFINE("_WI_FAULT_FIXED_FIELD", (uint32_t)WI_FIXED_FIELD_FAULT);
    if (!string_append(&header, "\n"))
        goto done;
    DEFINE("_PROTOCOL_STRING_NAME", 0);
    DEFINE("_PROTOCOL_STRING_DESCRIPTION", 1);
    DEFINE("_PROTOCOL_STRING_STANDARD", 2);
    DEFINE("_PROTOCOL_STRING_REFERENCE", 3);
    if (!string_append(&header, "\n"))
        goto done;
#undef DEFINE

    for (message_index = 0; message_index < protocol->message_count; message_index++) {
        const compiled_message_t *item = &compiled[message_index];
        const image_message_t *record = &image->messages[message_index];
        char *message = c_name(item->message->name);
        string_t base = {0};
        string_t name = {0};
        size_t field_index;
        size_t ordinal = 0;
        if (message == NULL || !string_printf(&base, "%s_%s", prefix, message) ||
            !append_comment(&header, item->message->description)) {
            free(message);
            free(base.data);
            goto done;
        }
#define MESSAGE_DEFINE(format, value) do { \
    name.length = 0; if (name.data != NULL) name.data[0] = 0; \
    if (!string_printf(&name, format, prefix, message) || \
        !header_define(&header, name.data, value)) { \
        free(message); free(base.data); free(name.data); goto done; \
    } \
} while (0)
        MESSAGE_DEFINE("%s_MESSAGE_%s", (uint32_t)message_index);
        MESSAGE_DEFINE("%s_%s_RECORD_OFFSET",
                       image->message_table_offset + (uint32_t)message_index * WI_MESSAGE_RECORD_SIZE);
        MESSAGE_DEFINE("%s_%s_STRINGS_OFFSET", record->string_section_offset);
        MESSAGE_DEFINE("%s_%s_STRING_COUNT", record->string_count);
        MESSAGE_DEFINE("%s_%s_STRING_NAME", 0);
        MESSAGE_DEFINE("%s_%s_STRING_DESCRIPTION", 1);
        MESSAGE_DEFINE("%s_%s_STRING_ENCODE", 2);
        MESSAGE_DEFINE("%s_%s_STRING_DECODE", 3);
        for (field_index = 0; field_index < item->message->field_count; field_index++) {
            char *field = c_name(item->message->fields[field_index].name);
            if (field == NULL) {
                free(message); free(base.data); free(name.data); goto done;
            }
            name.length = 0;
            if (name.data != NULL) name.data[0] = 0;
            if (!string_printf(&name, "%s_STRING_FIELD_%s", base.data, field) ||
                !header_define(&header, name.data,
                               WI_MESSAGE_FIXED_STRING_COUNT + (uint32_t)field_index)) {
                free(field); free(message); free(base.data); free(name.data); goto done;
            }
            free(field);
        }
        for (field_index = 0; field_index < item->message->field_count; field_index++) {
            char *field;
            if (item->message->fields[field_index].is_constant ||
                item->message->fields[field_index].length_of != NULL ||
                item->message->fields[field_index].count_of != NULL)
                continue;
            field = c_name(item->message->fields[field_index].name);
            if (field == NULL) {
                free(message); free(base.data); free(name.data); goto done;
            }
            name.length = 0;
            if (name.data != NULL) name.data[0] = 0;
            if (!string_printf(&name, "%s_ORDINAL_%s", base.data, field) ||
                !header_define(&header, name.data, (uint32_t)ordinal++)) {
                free(field); free(message); free(base.data); free(name.data); goto done;
            }
            free(field);
        }
        name.length = 0; if (name.data != NULL) name.data[0] = 0;
        if (!string_printf(&name, "%s_CALLER_FIELD_COUNT", base.data) ||
            !header_define(&header, name.data, (uint32_t)item->value_count)) {
            free(message); free(base.data); free(name.data); goto done;
        }
        name.length = 0; if (name.data != NULL) name.data[0] = 0;
        if (!string_printf(&name, "%s_WIRE_SIZE", base.data) ||
            !header_define(&header, name.data, (uint32_t)item->octets)) {
            free(message); free(base.data); free(name.data); goto done;
        }
        name.length = 0; if (name.data != NULL) name.data[0] = 0;
        if (!string_printf(&name, "%s_VARIABLE_WIRE_SIZE", base.data) ||
            !header_define(&header, name.data, (uint32_t)item->variable) ||
            !string_append(&header, "\n")) {
            free(message); free(base.data); free(name.data); goto done;
        }
#undef MESSAGE_DEFINE
        free(message);
        free(base.data);
        free(name.data);
    }
    if (!string_printf(&header, "#endif /* %s */\n", guard.data))
        goto done;
    *result = header.data;
    header.data = NULL;
    success = 1;

done:
    if (!success && error->message[0] == 0)
        wfc_set_error(error, protocol->location, "out of memory");
    free(header.data);
    free(prefix);
    free(guard.data);
    return success;
}

int wfc_generate(const wfc_protocol_t *protocol, wfc_generated_t *generated,
                 wfc_error_t *error)
{
    compiled_message_t *compiled;
    image_t image;
    size_t index;
    int success = 0;

    if (protocol->version == 4) { return wfc_records_generate(protocol, generated, error); }
    memset(generated, 0, sizeof(*generated));
    memset(&image, 0, sizeof(image));
    compiled = calloc(protocol->message_count, sizeof(*compiled));
    if (compiled == NULL) {
        wfc_set_error(error, protocol->location, "out of memory");
        return 0;
    }
    for (index = 0; index < protocol->message_count; index++)
        if (!compile_message(protocol, &protocol->messages[index],
                             &compiled[index], error))
            goto done;
    if (!build_image(protocol, compiled, &image, error) ||
        !generate_header(protocol, compiled, &image, &generated->header, error))
        goto done;
    generated->binary = image.bytes;
    generated->binary_size = image.size;
    image.bytes = NULL;
    success = 1;

done:
    compiled_free(compiled, protocol->message_count);
    free(image.bytes);
    free(image.messages);
    if (!success)
        wfc_generated_free(generated);
    return success;
}

void wfc_generated_free(wfc_generated_t *generated)
{
    free(generated->header);
    free(generated->binary);
    memset(generated, 0, sizeof(*generated));
}
