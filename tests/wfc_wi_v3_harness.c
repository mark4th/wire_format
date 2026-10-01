#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "v3.h"
#include "wire_format.h"

extern const uint8_t wfc_v3_wi_start[];
extern const uint8_t wfc_v3_wi_end[];

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

static const char *get_string(uint32_t section, uint32_t slot)
{
    uint32_t relative = get_u32(wfc_v3_wi_start + section + slot * 4);

    return (const char *)(wfc_v3_wi_start
                        + RECORD_DEMO_WI_STRING_TABLE_OFFSET
                        + relative);
}

static void load_formats(const uint32_t *sections, const uint32_t *slots,
                         const char **formats)
{
    size_t index;

    for (index = 0; index < RECORD_DEMO_WI_MESSAGE_COUNT; index++)
    {
        formats[index] = get_string(sections[index], slots[index]);
    }
}

int main(void)
{
    static const uint8_t expected[] = {
        0x21, 0x11, 0x01, 0x22, 0x95, 0x3c, 0x44, 0x55
    };
    static const uint32_t sections[] = {
        RECORD_DEMO_ITEM_STRINGS_OFFSET,
        RECORD_DEMO_SHORT_BODY_STRINGS_OFFSET,
        RECORD_DEMO_LONG_BODY_STRINGS_OFFSET,
        RECORD_DEMO_CONTAINER_STRINGS_OFFSET,
    };
    static const uint32_t encode_slots[] = {
        RECORD_DEMO_ITEM_STRING_ENCODE,
        RECORD_DEMO_SHORT_BODY_STRING_ENCODE,
        RECORD_DEMO_LONG_BODY_STRING_ENCODE,
        RECORD_DEMO_CONTAINER_STRING_ENCODE,
    };
    static const uint32_t decode_slots[] = {
        RECORD_DEMO_ITEM_STRING_DECODE,
        RECORD_DEMO_SHORT_BODY_STRING_DECODE,
        RECORD_DEMO_LONG_BODY_STRING_DECODE,
        RECORD_DEMO_CONTAINER_STRING_DECODE,
    };
    const char *formats[RECORD_DEMO_WI_MESSAGE_COUNT];
    int64_t params[RECORD_DEMO_CONTAINER_CALLER_FIELD_COUNT] = {0};
    wi_record_t item_rows[2] = {0};
    wi_record_t body_rows[1] = {0};
    wi_record_list_t items = {item_rows, 2, 2, 2};
    wi_record_list_t body = {body_rows, 1, 1, 1};
    uint8_t wire[sizeof expected] = {0};
    wi_vars_t vars;

    if ((size_t)(wfc_v3_wi_end - wfc_v3_wi_start) !=
            RECORD_DEMO_WI_FILE_SIZE ||
        !wi_file_crc32c_valid(wfc_v3_wi_start,
                              (size_t)(wfc_v3_wi_end - wfc_v3_wi_start)))
    {
        return 5;
    }

    item_rows[0].values[0] = 0x11;
    item_rows[0].values[1] = 1;
    item_rows[1].values[0] = 0x22;
    item_rows[1].values[1] = 0xabc;
    body_rows[0].values[0] = 0x4455;
    params[RECORD_DEMO_CONTAINER_ORDINAL_BODY_KIND] = 1;
    load_formats(sections, encode_slots, formats);
    wi_init(&vars, wire, sizeof wire, params,
            RECORD_DEMO_CONTAINER_CALLER_FIELD_COUNT);
    if (wi_set_formats(&vars, formats, RECORD_DEMO_WI_MESSAGE_COUNT) != 0 ||
        wi_set_record_list(&vars, RECORD_DEMO_CONTAINER_ORDINAL_ITEMS,
                           &items) != 0 ||
        wi_set_record_list(&vars, RECORD_DEMO_CONTAINER_ORDINAL_BODY,
                           &body) != 0)
    {
        return 1;
    }
    if (wi_parse(&vars, formats[RECORD_DEMO_MESSAGE_CONTAINER]) !=
            sizeof expected || vars.overrun != 0 ||
        memcmp(wire, expected, sizeof expected) != 0)
    {
        return 2;
    }

    memset(item_rows, 0, sizeof item_rows);
    memset(body_rows, 0, sizeof body_rows);
    items.count = 0;
    body.count = 0;
    load_formats(sections, decode_slots, formats);
    wi_decode_init(&vars, wire, sizeof wire, NULL, 0);
    if (wi_set_formats(&vars, formats, RECORD_DEMO_WI_MESSAGE_COUNT) != 0 ||
        wi_set_record_list(&vars, RECORD_DEMO_CONTAINER_ORDINAL_ITEMS,
                           &items) != 0 ||
        wi_set_record_list(&vars, RECORD_DEMO_CONTAINER_ORDINAL_BODY,
                           &body) != 0)
    {
        return 3;
    }
    wi_parse(&vars, formats[RECORD_DEMO_MESSAGE_CONTAINER]);
    if (vars.overrun != 0 || vars.faults != 0 || vars.in_pos != sizeof wire ||
        vars.vars[RECORD_DEMO_CONTAINER_ORDINAL_BODY_KIND] != 1 ||
        items.count != 2 || body.count != 1 ||
        item_rows[1].values[1] != 0xabc ||
        body_rows[0].values[0] != 0x4455)
    {
        return 4;
    }
    return 0;
}
