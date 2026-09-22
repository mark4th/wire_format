#include "json5.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
    json5_error_t *error;
    int failed;
} parser_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} buffer_t;

static json5_value_t *parse_value(parser_t *parser);

static void fail(parser_t *parser, const char *format, ...)
{
    va_list arguments;

    if (parser->failed)
        return;
    parser->failed = 1;
    parser->error->line = parser->line;
    parser->error->column = parser->column;
    va_start(arguments, format);
    vsnprintf(parser->error->message, sizeof(parser->error->message), format, arguments);
    va_end(arguments);
}

static int at_end(const parser_t *parser)
{
    return parser->offset >= parser->length;
}

static unsigned char peek(const parser_t *parser)
{
    return at_end(parser) ? 0 : (unsigned char)parser->text[parser->offset];
}

static unsigned char peek_next(const parser_t *parser)
{
    return parser->offset + 1 >= parser->length
               ? 0
               : (unsigned char)parser->text[parser->offset + 1];
}

static unsigned char take(parser_t *parser)
{
    unsigned char character;

    if (at_end(parser))
        return 0;
    character = (unsigned char)parser->text[parser->offset++];
    if (character == '\n') {
        parser->line++;
        parser->column = 1;
    } else {
        parser->column++;
    }
    return character;
}

static int grow(void **items, size_t *capacity, size_t count, size_t item_size)
{
    size_t previous = *capacity;
    size_t next;
    void *replacement;

    if (count < *capacity)
        return 1;
    next = *capacity == 0 ? 8 : *capacity * 2;
    if (next < *capacity || next > SIZE_MAX / item_size)
        return 0;
    replacement = realloc(*items, next * item_size);
    if (replacement == NULL)
        return 0;
    memset((unsigned char *)replacement + previous * item_size, 0,
           (next - previous) * item_size);
    *items = replacement;
    *capacity = next;
    return 1;
}

static int append_byte(buffer_t *buffer, unsigned char byte)
{
    if (!grow((void **)&buffer->data, &buffer->capacity, buffer->length, 1))
        return 0;
    buffer->data[buffer->length++] = (char)byte;
    return 1;
}

static int append_utf8(buffer_t *buffer, uint32_t codepoint)
{
    if (codepoint <= 0x7f)
        return append_byte(buffer, (unsigned char)codepoint);
    if (codepoint <= 0x7ff)
        return append_byte(buffer, 0xc0 | (codepoint >> 6)) &&
               append_byte(buffer, 0x80 | (codepoint & 0x3f));
    if (codepoint <= 0xffff)
        return append_byte(buffer, 0xe0 | (codepoint >> 12)) &&
               append_byte(buffer, 0x80 | ((codepoint >> 6) & 0x3f)) &&
               append_byte(buffer, 0x80 | (codepoint & 0x3f));
    if (codepoint <= 0x10ffff)
        return append_byte(buffer, 0xf0 | (codepoint >> 18)) &&
               append_byte(buffer, 0x80 | ((codepoint >> 12) & 0x3f)) &&
               append_byte(buffer, 0x80 | ((codepoint >> 6) & 0x3f)) &&
               append_byte(buffer, 0x80 | (codepoint & 0x3f));
    return 0;
}

static int valid_utf8(const char *text, size_t length, size_t *bad_offset)
{
    size_t offset = 0;
    while (offset < length) {
        const unsigned char *bytes = (const unsigned char *)text + offset;
        size_t count;
        uint32_t codepoint;
        uint32_t minimum;
        size_t index;

        if (bytes[0] < 0x80) {
            offset++;
            continue;
        }
        if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
            count = 2;
            codepoint = bytes[0] & 0x1f;
            minimum = 0x80;
        } else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
            count = 3;
            codepoint = bytes[0] & 0x0f;
            minimum = 0x800;
        } else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) {
            count = 4;
            codepoint = bytes[0] & 0x07;
            minimum = 0x10000;
        } else {
            *bad_offset = offset;
            return 0;
        }
        if (count > length - offset) {
            *bad_offset = offset;
            return 0;
        }
        for (index = 1; index < count; index++) {
            if ((bytes[index] & 0xc0) != 0x80) {
                *bad_offset = offset + index;
                return 0;
            }
            codepoint = (codepoint << 6) | (bytes[index] & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            *bad_offset = offset;
            return 0;
        }
        offset += count;
    }
    return 1;
}

static json5_value_t *new_value(parser_t *parser, json5_type_t type,
                                size_t line, size_t column)
{
    json5_value_t *value = calloc(1, sizeof(*value));

    if (value == NULL) {
        fail(parser, "out of memory");
        return NULL;
    }
    value->type = type;
    value->line = line;
    value->column = column;
    return value;
}

static void skip_space(parser_t *parser)
{
    for (;;) {
        while (!at_end(parser) && isspace(peek(parser)))
            take(parser);

        if (peek(parser) == '/' && peek_next(parser) == '/') {
            take(parser);
            take(parser);
            while (!at_end(parser) && peek(parser) != '\n' && peek(parser) != '\r')
                take(parser);
            continue;
        }
        if (peek(parser) == '/' && peek_next(parser) == '*') {
            take(parser);
            take(parser);
            while (!at_end(parser) && !(peek(parser) == '*' && peek_next(parser) == '/'))
                take(parser);
            if (at_end(parser)) {
                fail(parser, "unterminated block comment");
                return;
            }
            take(parser);
            take(parser);
            continue;
        }
        return;
    }
}

static int hex_value(unsigned char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int take_hex(parser_t *parser, unsigned digits, uint32_t *value)
{
    unsigned index;
    int digit;

    *value = 0;
    for (index = 0; index < digits; index++) {
        digit = hex_value(peek(parser));
        if (digit < 0) {
            fail(parser, "expected %u hexadecimal digits", digits);
            return 0;
        }
        *value = (*value << 4) | (uint32_t)digit;
        take(parser);
    }
    return 1;
}

static char *parse_string_bytes(parser_t *parser, size_t *length)
{
    unsigned char quote = take(parser);
    buffer_t buffer = {0};

    while (!at_end(parser) && !parser->failed) {
        unsigned char character = take(parser);
        uint32_t codepoint;

        if (character == quote) {
            if (!append_byte(&buffer, 0)) {
                fail(parser, "out of memory");
                break;
            }
            *length = buffer.length - 1;
            return buffer.data;
        }
        if (character == '\n' || character == '\r') {
            fail(parser, "unescaped newline in string");
            break;
        }
        if (character != '\\') {
            if (!append_byte(&buffer, character))
                fail(parser, "out of memory");
            continue;
        }

        if (at_end(parser)) {
            fail(parser, "unterminated string escape");
            break;
        }
        character = take(parser);
        switch (character) {
        case '\n':
            break;
        case '\r':
            if (peek(parser) == '\n')
                take(parser);
            break;
        case 'b':
            if (!append_byte(&buffer, '\b'))
                fail(parser, "out of memory");
            break;
        case 'f':
            if (!append_byte(&buffer, '\f'))
                fail(parser, "out of memory");
            break;
        case 'n':
            if (!append_byte(&buffer, '\n'))
                fail(parser, "out of memory");
            break;
        case 'r':
            if (!append_byte(&buffer, '\r'))
                fail(parser, "out of memory");
            break;
        case 't':
            if (!append_byte(&buffer, '\t'))
                fail(parser, "out of memory");
            break;
        case 'v':
            if (!append_byte(&buffer, '\v'))
                fail(parser, "out of memory");
            break;
        case '0':
            if (isdigit(peek(parser))) {
                fail(parser, "a zero escape may not be followed by a digit");
                break;
            }
            if (!append_byte(&buffer, 0))
                fail(parser, "out of memory");
            break;
        case 'x':
            if (take_hex(parser, 2, &codepoint) && !append_utf8(&buffer, codepoint))
                fail(parser, "out of memory");
            break;
        case 'u': {
            uint32_t low;
            if (!take_hex(parser, 4, &codepoint))
                break;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (take(parser) != '\\' || take(parser) != 'u' ||
                    !take_hex(parser, 4, &low) || low < 0xdc00 || low > 0xdfff) {
                    fail(parser, "invalid UTF-16 surrogate pair");
                    break;
                }
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                fail(parser, "unpaired UTF-16 low surrogate");
                break;
            }
            if (!append_utf8(&buffer, codepoint))
                fail(parser, "invalid Unicode code point");
            break;
        }
        default:
            if (!append_byte(&buffer, character))
                fail(parser, "out of memory");
            break;
        }
    }

    if (!parser->failed)
        fail(parser, "unterminated string");
    free(buffer.data);
    return NULL;
}

static int identifier_start(unsigned char character)
{
    return isalpha(character) || character == '_' || character == '$';
}

static int identifier_continue(unsigned char character)
{
    return isalnum(character) || character == '_' || character == '$';
}

static char *parse_identifier(parser_t *parser, size_t *length)
{
    size_t start = parser->offset;
    char *result;

    if (!identifier_start(peek(parser))) {
        fail(parser, "expected an object property name");
        return NULL;
    }
    take(parser);
    while (identifier_continue(peek(parser)))
        take(parser);
    *length = parser->offset - start;
    result = malloc(*length + 1);
    if (result == NULL) {
        fail(parser, "out of memory");
        return NULL;
    }
    memcpy(result, parser->text + start, *length);
    result[*length] = 0;
    return result;
}

static int is_delimiter(unsigned char character)
{
    return character == 0 || isspace(character) || character == ',' ||
           character == ']' || character == '}' || character == '/';
}

static json5_value_t *parse_number(parser_t *parser, size_t line, size_t column)
{
    size_t start = parser->offset;
    size_t length;
    json5_value_t *value;

    while (!is_delimiter(peek(parser)))
        take(parser);
    length = parser->offset - start;
    if (length == 0) {
        fail(parser, "expected a value");
        return NULL;
    }
    value = new_value(parser, JSON5_NUMBER, line, column);
    if (value == NULL)
        return NULL;
    value->as.scalar.text = malloc(length + 1);
    if (value->as.scalar.text == NULL) {
        json5_free(value);
        fail(parser, "out of memory");
        return NULL;
    }
    memcpy(value->as.scalar.text, parser->text + start, length);
    value->as.scalar.text[length] = 0;
    value->as.scalar.length = length;
    return value;
}

static json5_value_t *parse_array(parser_t *parser, size_t line, size_t column)
{
    json5_value_t *value = new_value(parser, JSON5_ARRAY, line, column);
    size_t capacity = 0;

    if (value == NULL)
        return NULL;
    take(parser);
    skip_space(parser);
    if (peek(parser) == ']') {
        take(parser);
        return value;
    }

    for (;;) {
        json5_value_t *item = parse_value(parser);
        if (item == NULL)
            break;
        if (!grow((void **)&value->as.array.items, &capacity,
                  value->as.array.count, sizeof(*value->as.array.items))) {
            json5_free(item);
            fail(parser, "out of memory");
            break;
        }
        value->as.array.items[value->as.array.count++] = item;
        skip_space(parser);
        if (peek(parser) == ']') {
            take(parser);
            return value;
        }
        if (take(parser) != ',') {
            fail(parser, "expected `,` or `]` in array");
            break;
        }
        skip_space(parser);
        if (peek(parser) == ']') {
            take(parser);
            return value;
        }
    }
    json5_free(value);
    return NULL;
}

static json5_value_t *parse_object(parser_t *parser, size_t line, size_t column)
{
    json5_value_t *value = new_value(parser, JSON5_OBJECT, line, column);
    size_t capacity = 0;

    if (value == NULL)
        return NULL;
    take(parser);
    skip_space(parser);
    if (peek(parser) == '}') {
        take(parser);
        return value;
    }

    for (;;) {
        json5_member_t member = {0};
        member.line = parser->line;
        member.column = parser->column;
        if (peek(parser) == '\'' || peek(parser) == '"')
            member.name = parse_string_bytes(parser, &member.name_length);
        else
            member.name = parse_identifier(parser, &member.name_length);
        if (member.name == NULL)
            break;
        skip_space(parser);
        if (take(parser) != ':') {
            free(member.name);
            fail(parser, "expected `:` after object property");
            break;
        }
        member.value = parse_value(parser);
        if (member.value == NULL) {
            free(member.name);
            break;
        }
        if (!grow((void **)&value->as.object.members, &capacity,
                  value->as.object.count, sizeof(*value->as.object.members))) {
            free(member.name);
            json5_free(member.value);
            fail(parser, "out of memory");
            break;
        }
        value->as.object.members[value->as.object.count++] = member;
        skip_space(parser);
        if (peek(parser) == '}') {
            take(parser);
            return value;
        }
        if (take(parser) != ',') {
            fail(parser, "expected `,` or `}` in object");
            break;
        }
        skip_space(parser);
        if (peek(parser) == '}') {
            take(parser);
            return value;
        }
    }
    json5_free(value);
    return NULL;
}

static int match_word(parser_t *parser, const char *word)
{
    size_t length = strlen(word);

    if (parser->length - parser->offset < length ||
        memcmp(parser->text + parser->offset, word, length) != 0 ||
        (parser->offset + length < parser->length &&
         identifier_continue((unsigned char)parser->text[parser->offset + length])))
        return 0;
    while (length-- != 0)
        take(parser);
    return 1;
}

static json5_value_t *parse_value(parser_t *parser)
{
    size_t line;
    size_t column;
    json5_value_t *value;

    skip_space(parser);
    if (parser->failed)
        return NULL;
    line = parser->line;
    column = parser->column;

    if (peek(parser) == '{')
        return parse_object(parser, line, column);
    if (peek(parser) == '[')
        return parse_array(parser, line, column);
    if (peek(parser) == '\'' || peek(parser) == '"') {
        value = new_value(parser, JSON5_STRING, line, column);
        if (value == NULL)
            return NULL;
        value->as.scalar.text = parse_string_bytes(parser, &value->as.scalar.length);
        if (value->as.scalar.text == NULL) {
            json5_free(value);
            return NULL;
        }
        return value;
    }
    if (match_word(parser, "null"))
        return new_value(parser, JSON5_NULL, line, column);
    if (match_word(parser, "true")) {
        value = new_value(parser, JSON5_BOOLEAN, line, column);
        if (value != NULL)
            value->as.boolean = 1;
        return value;
    }
    if (match_word(parser, "false")) {
        value = new_value(parser, JSON5_BOOLEAN, line, column);
        if (value != NULL)
            value->as.boolean = 0;
        return value;
    }
    if (at_end(parser)) {
        fail(parser, "expected a value");
        return NULL;
    }
    return parse_number(parser, line, column);
}

json5_value_t *json5_parse(const char *text, size_t length, json5_error_t *error)
{
    parser_t parser = {
        .text = text,
        .length = length,
        .line = 1,
        .column = 1,
        .error = error,
    };
    json5_value_t *value;
    size_t bad_offset;

    memset(error, 0, sizeof(*error));
    if (!valid_utf8(text, length, &bad_offset)) {
        size_t offset;
        error->line = 1;
        error->column = 1;
        for (offset = 0; offset < bad_offset; offset++) {
            if (text[offset] == '\n') {
                error->line++;
                error->column = 1;
            } else {
                error->column++;
            }
        }
        snprintf(error->message, sizeof(error->message), "source is not valid UTF-8");
        return NULL;
    }
    if (length >= 3 && (unsigned char)text[0] == 0xef &&
        (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) {
        parser.offset = 3;
        parser.column = 1;
    }
    value = parse_value(&parser);
    if (value == NULL)
        return NULL;
    skip_space(&parser);
    if (!at_end(&parser)) {
        fail(&parser, "unexpected text after the top-level value");
        json5_free(value);
        return NULL;
    }
    return value;
}

void json5_free(json5_value_t *value)
{
    size_t index;

    if (value == NULL)
        return;
    switch (value->type) {
    case JSON5_NUMBER:
    case JSON5_STRING:
        free(value->as.scalar.text);
        break;
    case JSON5_ARRAY:
        for (index = 0; index < value->as.array.count; index++)
            json5_free(value->as.array.items[index]);
        free(value->as.array.items);
        break;
    case JSON5_OBJECT:
        for (index = 0; index < value->as.object.count; index++) {
            free(value->as.object.members[index].name);
            json5_free(value->as.object.members[index].value);
        }
        free(value->as.object.members);
        break;
    case JSON5_NULL:
    case JSON5_BOOLEAN:
        break;
    }
    free(value);
}
