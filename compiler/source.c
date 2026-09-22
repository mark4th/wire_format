#include "json5.h"
#include "wfc.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static wfc_location_t location_of(const json5_value_t *value)
{
    wfc_location_t location = {value->line, value->column};
    return location;
}

static int member_named(const json5_member_t *member, const char *name)
{
    size_t length = strlen(name);
    return member->name_length == length && memcmp(member->name, name, length) == 0;
}

static int allowed_name(const json5_member_t *member, const char *const *allowed,
                        size_t allowed_count)
{
    size_t index;
    for (index = 0; index < allowed_count; index++)
        if (member_named(member, allowed[index]))
            return 1;
    return 0;
}

static int object_shape(const json5_value_t *object, const char *what,
                        const char *const *allowed, size_t allowed_count,
                        wfc_error_t *error)
{
    size_t left;
    size_t right;

    if (object->type != JSON5_OBJECT) {
        wfc_set_error(error, location_of(object), "%s must be an object", what);
        return 0;
    }
    for (left = 0; left < object->as.object.count; left++) {
        const json5_member_t *member = &object->as.object.members[left];
        wfc_location_t location = {member->line, member->column};
        if (memchr(member->name, 0, member->name_length) != NULL) {
            wfc_set_error(error, location, "%s contains a NUL in a property name", what);
            return 0;
        }
        if (!allowed_name(member, allowed, allowed_count)) {
            wfc_set_error(error, location, "unknown property `%s` in %s",
                          member->name, what);
            return 0;
        }
        for (right = 0; right < left; right++) {
            const json5_member_t *earlier = &object->as.object.members[right];
            if (member->name_length == earlier->name_length &&
                memcmp(member->name, earlier->name, member->name_length) == 0) {
                wfc_set_error(error, location, "duplicate property `%s` in %s",
                              member->name, what);
                return 0;
            }
        }
    }
    return 1;
}

static const json5_value_t *find_member(const json5_value_t *object, const char *name)
{
    size_t index;
    for (index = 0; index < object->as.object.count; index++)
        if (member_named(&object->as.object.members[index], name))
            return object->as.object.members[index].value;
    return NULL;
}

static const json5_value_t *required_member(const json5_value_t *object,
                                             const char *name, const char *what,
                                             wfc_error_t *error)
{
    const json5_value_t *value = find_member(object, name);
    if (value == NULL)
        wfc_set_error(error, location_of(object), "%s requires `%s`", what, name);
    return value;
}

static char *string_value(const json5_value_t *value, const char *what,
                          wfc_error_t *error)
{
    char *result;
    if (value->type != JSON5_STRING) {
        wfc_set_error(error, location_of(value), "%s must be a string", what);
        return NULL;
    }
    if (memchr(value->as.scalar.text, 0, value->as.scalar.length) != NULL) {
        wfc_set_error(error, location_of(value), "%s may not contain a NUL octet", what);
        return NULL;
    }
    result = wfc_duplicate(value->as.scalar.text, value->as.scalar.length);
    if (result == NULL)
        wfc_set_error(error, location_of(value), "out of memory");
    return result;
}

static int parse_u64(const json5_value_t *value, const char *what,
                     uint64_t *result, wfc_error_t *error)
{
    const char *scan;
    const char *end;
    unsigned base = 10;
    uint64_t number = 0;

    if (value->type != JSON5_NUMBER) {
        wfc_set_error(error, location_of(value), "%s must be an unsigned integer", what);
        return 0;
    }
    scan = value->as.scalar.text;
    end = scan + value->as.scalar.length;
    if (scan != end && *scan == '+')
        scan++;
    if (end - scan >= 2 && scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X')) {
        base = 16;
        scan += 2;
    }
    if (scan == end) {
        wfc_set_error(error, location_of(value), "%s must be an unsigned integer", what);
        return 0;
    }
    while (scan != end) {
        unsigned digit;
        unsigned char character = (unsigned char)*scan++;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (base == 16 && character >= 'a' && character <= 'f')
            digit = character - 'a' + 10;
        else if (base == 16 && character >= 'A' && character <= 'F')
            digit = character - 'A' + 10;
        else {
            wfc_set_error(error, location_of(value),
                          "%s must be an unsigned integer", what);
            return 0;
        }
        if (digit >= base || number > (UINT64_MAX - digit) / base) {
            wfc_set_error(error, location_of(value), "%s exceeds 64 bits", what);
            return 0;
        }
        number = number * base + digit;
    }
    *result = number;
    return 1;
}

static int name_value(const json5_value_t *object, const char *what,
                      char **name, wfc_location_t *location, wfc_error_t *error)
{
    const json5_value_t *value = required_member(object, "name", what, error);
    if (value == NULL)
        return 0;
    *name = string_value(value, "name", error);
    if (*name == NULL)
        return 0;
    *location = location_of(value);
    if (!wfc_identifier_valid(*name)) {
        wfc_set_error(error, *location, "invalid identifier `%s`", *name);
        return 0;
    }
    return 1;
}

static int parse_field(const json5_value_t *source, wfc_field_t *field,
                       wfc_error_t *error)
{
    static const char *const allowed[] = {"name", "type", "width", "constant"};
    const json5_value_t *type_value;
    const json5_value_t *width_value;
    const json5_value_t *constant_value;
    char *type = NULL;
    uint64_t width;

    if (!object_shape(source, "field", allowed, 4, error) ||
        !name_value(source, "field", &field->name, &field->location, error))
        return 0;
    type_value = required_member(source, "type", "field", error);
    if (type_value == NULL)
        return 0;
    type = string_value(type_value, "field type", error);
    if (type == NULL)
        return 0;
    width_value = find_member(source, "width");
    if (strcmp(type, "bits") == 0) {
        field->type = WFC_BITS;
        if (width_value == NULL) {
            wfc_set_error(error, field->location, "bit field `%s` requires `width`",
                          field->name);
            free(type);
            return 0;
        }
        if (!parse_u64(width_value, "field width", &width, error)) {
            free(type);
            return 0;
        }
        if (width < 1 || width > 64) {
            wfc_set_error(error, location_of(width_value),
                          "bit field `%s` width must be from 1 through 64", field->name);
            free(type);
            return 0;
        }
        field->width = (uint8_t)width;
    } else {
        if (width_value != NULL) {
            wfc_set_error(error, location_of(width_value),
                          "scalar field `%s` may not specify `width`", field->name);
            free(type);
            return 0;
        }
        if (strcmp(type, "u8") == 0)
            field->type = WFC_U8;
        else if (strcmp(type, "u16") == 0)
            field->type = WFC_U16;
        else if (strcmp(type, "u32") == 0)
            field->type = WFC_U32;
        else if (strcmp(type, "u64") == 0)
            field->type = WFC_U64;
        else {
            wfc_set_error(error, location_of(type_value),
                          "field `%s` type must be `bits`, `u8`, `u16`, `u32`, or `u64`",
                          field->name);
            free(type);
            return 0;
        }
    }
    free(type);

    constant_value = find_member(source, "constant");
    if (constant_value != NULL) {
        field->is_constant = 1;
        if (!parse_u64(constant_value, "field constant", &field->constant, error))
            return 0;
        if (!wfc_value_fits(field->constant, wfc_field_width(field))) {
            wfc_set_error(error, location_of(constant_value),
                          "value %llu does not fit in a %u-bit field",
                          (unsigned long long)field->constant,
                          (unsigned)wfc_field_width(field));
            return 0;
        }
    }
    return 1;
}

static int parse_vector(const json5_value_t *source, wfc_vector_t *vector,
                        wfc_error_t *error)
{
    static const char *const allowed[] = {"name", "values", "wire"};
    const json5_value_t *values;
    const json5_value_t *wire;
    size_t index;

    if (!object_shape(source, "vector", allowed, 3, error) ||
        !name_value(source, "vector", &vector->name, &vector->location, error))
        return 0;
    values = required_member(source, "values", "vector", error);
    wire = required_member(source, "wire", "vector", error);
    if (values == NULL || wire == NULL)
        return 0;
    if (values->type != JSON5_OBJECT) {
        wfc_set_error(error, location_of(values), "vector `values` must be an object");
        return 0;
    }
    vector->assignment_count = values->as.object.count;
    vector->assignments = calloc(vector->assignment_count, sizeof(*vector->assignments));
    if (vector->assignment_count != 0 && vector->assignments == NULL) {
        wfc_set_error(error, location_of(values), "out of memory");
        return 0;
    }
    for (index = 0; index < vector->assignment_count; index++) {
        const json5_member_t *member = &values->as.object.members[index];
        wfc_assignment_t *assignment = &vector->assignments[index];
        size_t previous;
        assignment->location.line = member->line;
        assignment->location.column = member->column;
        if (memchr(member->name, 0, member->name_length) != NULL) {
            wfc_set_error(error, assignment->location,
                          "vector field name may not contain a NUL octet");
            return 0;
        }
        assignment->name = wfc_duplicate(member->name, member->name_length);
        if (assignment->name == NULL) {
            wfc_set_error(error, assignment->location, "out of memory");
            return 0;
        }
        if (!wfc_identifier_valid(assignment->name)) {
            wfc_set_error(error, assignment->location, "invalid identifier `%s`",
                          assignment->name);
            return 0;
        }
        for (previous = 0; previous < index; previous++)
            if (strcmp(assignment->name, vector->assignments[previous].name) == 0) {
                wfc_set_error(error, assignment->location,
                              "duplicate vector value `%s`", assignment->name);
                return 0;
            }
        if (!parse_u64(member->value, "vector value", &assignment->value, error))
            return 0;
    }

    if (wire->type != JSON5_ARRAY) {
        wfc_set_error(error, location_of(wire), "vector `wire` must be an array");
        return 0;
    }
    if (wire->as.array.count == 0) {
        wfc_set_error(error, location_of(wire),
                      "vector `%s` must contain at least one wire octet", vector->name);
        return 0;
    }
    vector->wire_count = wire->as.array.count;
    vector->wire = malloc(vector->wire_count);
    if (vector->wire == NULL) {
        wfc_set_error(error, location_of(wire), "out of memory");
        return 0;
    }
    for (index = 0; index < vector->wire_count; index++) {
        uint64_t octet;
        if (!parse_u64(wire->as.array.items[index], "wire octet", &octet, error))
            return 0;
        if (octet > 255) {
            wfc_set_error(error, location_of(wire->as.array.items[index]),
                          "wire octet exceeds 255");
            return 0;
        }
        vector->wire[index] = (uint8_t)octet;
    }
    return 1;
}

static int parse_message(const json5_value_t *source, wfc_message_t *message,
                         wfc_error_t *error)
{
    static const char *const allowed[] = {"name", "description", "fields", "vectors"};
    const json5_value_t *description;
    const json5_value_t *fields;
    const json5_value_t *vectors;
    size_t index;

    if (!object_shape(source, "message", allowed, 4, error) ||
        !name_value(source, "message", &message->name, &message->location, error))
        return 0;
    description = required_member(source, "description", "message", error);
    fields = required_member(source, "fields", "message", error);
    if (description == NULL || fields == NULL)
        return 0;
    message->description = string_value(description, "message description", error);
    if (message->description == NULL)
        return 0;
    if (fields->type != JSON5_ARRAY || fields->as.array.count == 0) {
        wfc_set_error(error, location_of(fields),
                      "message `%s` must contain at least one field", message->name);
        return 0;
    }
    message->field_count = fields->as.array.count;
    message->fields = calloc(message->field_count, sizeof(*message->fields));
    if (message->fields == NULL) {
        wfc_set_error(error, location_of(fields), "out of memory");
        return 0;
    }
    for (index = 0; index < message->field_count; index++) {
        size_t previous;
        if (!parse_field(fields->as.array.items[index], &message->fields[index], error))
            return 0;
        for (previous = 0; previous < index; previous++)
            if (strcmp(message->fields[index].name, message->fields[previous].name) == 0) {
                wfc_set_error(error, message->fields[index].location,
                              "duplicate field `%s`", message->fields[index].name);
                return 0;
            }
    }

    vectors = find_member(source, "vectors");
    if (vectors == NULL)
        return 1;
    if (vectors->type != JSON5_ARRAY) {
        wfc_set_error(error, location_of(vectors), "message `vectors` must be an array");
        return 0;
    }
    message->vector_count = vectors->as.array.count;
    message->vectors = calloc(message->vector_count, sizeof(*message->vectors));
    if (message->vector_count != 0 && message->vectors == NULL) {
        wfc_set_error(error, location_of(vectors), "out of memory");
        return 0;
    }
    for (index = 0; index < message->vector_count; index++) {
        size_t previous;
        if (!parse_vector(vectors->as.array.items[index], &message->vectors[index], error))
            return 0;
        for (previous = 0; previous < index; previous++)
            if (strcmp(message->vectors[index].name, message->vectors[previous].name) == 0) {
                wfc_set_error(error, message->vectors[index].location,
                              "duplicate vector `%s`", message->vectors[index].name);
                return 0;
            }
    }
    return 1;
}

static int parse_protocol(const json5_value_t *source, wfc_protocol_t *protocol,
                          wfc_error_t *error)
{
    static const char *const allowed[] = {
        "name", "description", "standard", "reference",
        "byte_order", "bit_order", "messages"
    };
    const json5_value_t *description;
    const json5_value_t *standard;
    const json5_value_t *reference;
    const json5_value_t *byte_order;
    const json5_value_t *bit_order;
    const json5_value_t *messages;
    char *order;
    size_t index;

    if (!object_shape(source, "protocol", allowed, 7, error) ||
        !name_value(source, "protocol", &protocol->name, &protocol->location, error))
        return 0;
    description = required_member(source, "description", "protocol", error);
    byte_order = required_member(source, "byte_order", "protocol", error);
    bit_order = required_member(source, "bit_order", "protocol", error);
    messages = required_member(source, "messages", "protocol", error);
    if (description == NULL || byte_order == NULL || bit_order == NULL || messages == NULL)
        return 0;
    protocol->description = string_value(description, "protocol description", error);
    if (protocol->description == NULL)
        return 0;
    standard = find_member(source, "standard");
    reference = find_member(source, "reference");
    if (standard != NULL &&
        (protocol->standard = string_value(standard, "standard", error)) == NULL)
        return 0;
    if (reference != NULL &&
        (protocol->reference = string_value(reference, "reference", error)) == NULL)
        return 0;

    order = string_value(byte_order, "byte_order", error);
    if (order == NULL)
        return 0;
    if (strcmp(order, "big-endian") == 0)
        protocol->byte_order = WFC_BIG_ENDIAN;
    else if (strcmp(order, "little-endian") == 0)
        protocol->byte_order = WFC_LITTLE_ENDIAN;
    else {
        wfc_set_error(error, location_of(byte_order),
                      "byte_order must be `big-endian` or `little-endian`");
        free(order);
        return 0;
    }
    free(order);

    order = string_value(bit_order, "bit_order", error);
    if (order == NULL)
        return 0;
    if (strcmp(order, "msb-first") == 0)
        protocol->bit_order = WFC_MSB_FIRST;
    else if (strcmp(order, "lsb-first") == 0)
        protocol->bit_order = WFC_LSB_FIRST;
    else {
        wfc_set_error(error, location_of(bit_order),
                      "bit_order must be `msb-first` or `lsb-first`");
        free(order);
        return 0;
    }
    free(order);

    if (messages->type != JSON5_ARRAY || messages->as.array.count == 0) {
        wfc_set_error(error, location_of(messages),
                      "a protocol must contain at least one message");
        return 0;
    }
    protocol->message_count = messages->as.array.count;
    protocol->messages = calloc(protocol->message_count, sizeof(*protocol->messages));
    if (protocol->messages == NULL) {
        wfc_set_error(error, location_of(messages), "out of memory");
        return 0;
    }
    for (index = 0; index < protocol->message_count; index++) {
        size_t previous;
        if (!parse_message(messages->as.array.items[index], &protocol->messages[index], error))
            return 0;
        for (previous = 0; previous < index; previous++)
            if (strcmp(protocol->messages[index].name, protocol->messages[previous].name) == 0) {
                wfc_set_error(error, protocol->messages[index].location,
                              "duplicate message `%s`", protocol->messages[index].name);
                return 0;
            }
    }
    return 1;
}

int wfc_source_parse(const char *text, size_t length, wfc_protocol_t *protocol,
                     wfc_error_t *error)
{
    static const char *const root_allowed[] = {"wire_format", "protocol"};
    json5_error_t json_error;
    json5_value_t *root;
    const json5_value_t *version;
    const json5_value_t *source_protocol;
    uint64_t version_number;
    int success = 0;

    memset(protocol, 0, sizeof(*protocol));
    memset(error, 0, sizeof(*error));
    root = json5_parse(text, length, &json_error);
    if (root == NULL) {
        wfc_location_t location = {json_error.line, json_error.column};
        wfc_set_error(error, location, "%s", json_error.message);
        return 0;
    }
    if (!object_shape(root, "source file", root_allowed, 2, error))
        goto done;
    version = required_member(root, "wire_format", "source file", error);
    source_protocol = required_member(root, "protocol", "source file", error);
    if (version == NULL || source_protocol == NULL ||
        !parse_u64(version, "wire_format", &version_number, error))
        goto done;
    if (version_number != 1) {
        wfc_set_error(error, location_of(version),
                      "unsupported wire_format version %llu; expected 1",
                      (unsigned long long)version_number);
        goto done;
    }
    success = parse_protocol(source_protocol, protocol, error);

done:
    json5_free(root);
    if (!success)
        wfc_protocol_free(protocol);
    return success;
}

void wfc_protocol_free(wfc_protocol_t *protocol)
{
    size_t message_index;
    free(protocol->name);
    free(protocol->description);
    free(protocol->standard);
    free(protocol->reference);
    for (message_index = 0; message_index < protocol->message_count; message_index++) {
        wfc_message_t *message = &protocol->messages[message_index];
        size_t field_index;
        size_t vector_index;
        free(message->name);
        free(message->description);
        for (field_index = 0; field_index < message->field_count; field_index++)
            free(message->fields[field_index].name);
        free(message->fields);
        for (vector_index = 0; vector_index < message->vector_count; vector_index++) {
            wfc_vector_t *vector = &message->vectors[vector_index];
            size_t assignment_index;
            free(vector->name);
            for (assignment_index = 0; assignment_index < vector->assignment_count;
                 assignment_index++)
                free(vector->assignments[assignment_index].name);
            free(vector->assignments);
            free(vector->wire);
        }
        free(message->vectors);
    }
    free(protocol->messages);
    memset(protocol, 0, sizeof(*protocol));
}
