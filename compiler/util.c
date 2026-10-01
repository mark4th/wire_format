#include "wfc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wfc_set_error(wfc_error_t *error, wfc_location_t location,
                   const char *format, ...)
{
    va_list arguments;

    error->location = location;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

char *wfc_duplicate(const char *text, size_t length)
{
    char *copy = malloc(length + 1);

    if (copy == NULL)
        return NULL;
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

int wfc_identifier_valid(const char *identifier)
{
    const unsigned char *scan = (const unsigned char *)identifier;
    int previous_hyphen = 0;

    if (*scan < 'a' || *scan > 'z')
        return 0;
    for (; *scan != 0; scan++) {
        int hyphen = *scan == '-';
        if (!((*scan >= 'a' && *scan <= 'z') ||
              (*scan >= '0' && *scan <= '9') || hyphen))
            return 0;
        if (hyphen && previous_hyphen)
            return 0;
        previous_hyphen = hyphen;
    }
    return !previous_hyphen;
}

uint8_t wfc_field_width(const wfc_field_t *field)
{
    switch (field->type) {
    case WFC_BITS:
        return field->width;
    case WFC_U8:
        return 8;
    case WFC_U16:
        return 16;
    case WFC_U32:
        return 32;
    case WFC_U64:
        return 64;
    case WFC_SDNV:
        return 8;
    case WFC_BYTES:
    case WFC_RECORDS:
    case WFC_CHOICE:
        return 0;
    }
    return 0;
}

int wfc_value_fits(uint64_t value, uint8_t width)
{
    return width == 64 || value < (UINT64_C(1) << width);
}

/* CRC-32C over a complete .wi image.  The checksum field at bytes 60..63 is
 * defined as zero while calculating, allowing callers to verify an image
 * without modifying immutable storage. */
uint32_t wfc_wi_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t index;

    for (index = 0; index < length; index++) {
        uint8_t byte = index >= 60 && index < 64 ? 0 : data[index];
        unsigned bit;

        crc ^= byte;
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0x82f63b78U & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

void wfc_wi_set_crc32c(uint8_t *data, size_t length)
{
    uint32_t crc = wfc_wi_crc32c(data, length);

    data[60] = (uint8_t)crc;
    data[61] = (uint8_t)(crc >> 8);
    data[62] = (uint8_t)(crc >> 16);
    data[63] = (uint8_t)(crc >> 24);
}
