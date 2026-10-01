#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "v2.h"
#include "wire_format.h"

extern const uint8_t wfc_v2_wi_start[];
extern const uint8_t wfc_v2_wi_end[];

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

static const char *get_string(uint32_t section, uint32_t slot)
{
    uint32_t relative = get_u32(wfc_v2_wi_start + section + slot * 4);

    return (const char *)(wfc_v2_wi_start
                        + VARIABLE_TAIL_WI_STRING_TABLE_OFFSET
                        + relative);
}

int main(void)
{
    static const uint8_t tail[] = {0xaa, 0xbb, 0xcc};
    static const uint8_t expected[] = {0x22, 0x12, 0x34, 0xaa, 0xbb, 0xcc};
    const char *encode;
    const char *decode;
    int64_t params[VARIABLE_TAIL_PACKET_CALLER_FIELD_COUNT] = {0};
    uint8_t wire[sizeof expected] = {0};
    wi_vars_t vars;

    if ((size_t)(wfc_v2_wi_end - wfc_v2_wi_start) !=
        VARIABLE_TAIL_WI_FILE_SIZE)
        return 1;
    if (!wi_file_crc32c_valid(wfc_v2_wi_start,
                              (size_t)(wfc_v2_wi_end - wfc_v2_wi_start)))
        return 11;
    if (get_u16(wfc_v2_wi_start + 4) != 2)
        return 2;
    if (get_u32(wfc_v2_wi_start + VARIABLE_TAIL_PACKET_RECORD_OFFSET + 24) != 1)
        return 3;
    if (VARIABLE_TAIL_PACKET_WIRE_SIZE != 3 ||
        VARIABLE_TAIL_PACKET_VARIABLE_WIRE_SIZE != 1)
        return 4;

    encode = get_string(VARIABLE_TAIL_PACKET_STRINGS_OFFSET,
                        VARIABLE_TAIL_PACKET_STRING_ENCODE);
    decode = get_string(VARIABLE_TAIL_PACKET_STRINGS_OFFSET,
                        VARIABLE_TAIL_PACKET_STRING_DECODE);
    params[VARIABLE_TAIL_PACKET_ORDINAL_KIND] = 2;
    params[VARIABLE_TAIL_PACKET_ORDINAL_IDENTIFIER] = 0x1234;
    wi_init(&vars, wire, sizeof wire, params,
            VARIABLE_TAIL_PACKET_CALLER_FIELD_COUNT);
    if (wi_set_slice(&vars, VARIABLE_TAIL_PACKET_ORDINAL_DATA,
                     tail, sizeof tail) != 0)
        return 5;
    if (wi_parse(&vars, encode) != sizeof expected || vars.overrun != 0)
        return 6;
    if (memcmp(wire, expected, sizeof expected) != 0)
        return 7;

    wi_decode_init(&vars, wire, sizeof wire, NULL, 0);
    wi_parse(&vars, decode);
    if (vars.overrun != 0 || vars.faults != 0 || vars.in_pos != sizeof wire)
        return 8;
    if (vars.vars[VARIABLE_TAIL_PACKET_ORDINAL_KIND] != 2 ||
        vars.vars[VARIABLE_TAIL_PACKET_ORDINAL_IDENTIFIER] != 0x1234)
        return 9;
    if (vars.slices[VARIABLE_TAIL_PACKET_ORDINAL_DATA].length != sizeof tail ||
        memcmp(vars.slices[VARIABLE_TAIL_PACKET_ORDINAL_DATA].data,
               tail, sizeof tail) != 0)
        return 10;

    return 0;
}
