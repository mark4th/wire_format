#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "example_telemetry.h"
#include "wire_format.h"

extern const uint8_t wfc_test_wfb_start[];
extern const uint8_t wfc_test_wfb_end[];

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

static const char *get_string(uint32_t section, uint32_t slot)
{
    uint32_t relative = get_u32(wfc_test_wfb_start + section + slot * 4);

    return (const char *)(wfc_test_wfb_start
                        + EXAMPLE_TELEMETRY_WFB_STRING_TABLE_OFFSET
                        + relative);
}

int main(void)
{
    static const uint8_t expected[] = { 0x22, 0x12, 0x34 };
    const char *encode;
    const char *decode;
    int64_t params[EXAMPLE_TELEMETRY_STATUS_CALLER_FIELD_COUNT] = { 0 };
    uint8_t wire[EXAMPLE_TELEMETRY_STATUS_WIRE_SIZE] = { 0 };
    wi_vars_t vars;

    if ((size_t)(wfc_test_wfb_end - wfc_test_wfb_start) !=
        EXAMPLE_TELEMETRY_WFB_FILE_SIZE)
        return 1;
    if (memcmp(wfc_test_wfb_start, "WFB\0", 4) != 0)
        return 2;
    if (get_u32(wfc_test_wfb_start + 8) != EXAMPLE_TELEMETRY_WFB_FILE_SIZE)
        return 3;
    if (strcmp(get_string(EXAMPLE_TELEMETRY_WFB_PROTOCOL_STRINGS_OFFSET,
                          EXAMPLE_TELEMETRY_PROTOCOL_STRING_NAME),
               "example-telemetry") != 0)
        return 4;
    if (strcmp(get_string(EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET,
                          EXAMPLE_TELEMETRY_STATUS_STRING_NAME),
               "status") != 0)
        return 5;
    if (strcmp(get_string(EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET,
                          EXAMPLE_TELEMETRY_STATUS_STRING_FIELD_VERSION),
               "version") != 0)
        return 6;

    encode = get_string(EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET,
                        EXAMPLE_TELEMETRY_STATUS_STRING_ENCODE);
    decode = get_string(EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET,
                        EXAMPLE_TELEMETRY_STATUS_STRING_DECODE);
    params[EXAMPLE_TELEMETRY_STATUS_ORDINAL_VERSION] = 1;
    params[EXAMPLE_TELEMETRY_STATUS_ORDINAL_MODE] = 2;
    params[EXAMPLE_TELEMETRY_STATUS_ORDINAL_SAMPLE_COUNT] = 0x1234;
    wi_init(&vars, wire, sizeof wire, params,
            EXAMPLE_TELEMETRY_STATUS_CALLER_FIELD_COUNT);
    if (wi_parse(&vars, encode) != sizeof expected || vars.overrun != 0)
        return 7;
    if (memcmp(wire, expected, sizeof expected) != 0)
        return 8;

    wi_decode_init(&vars, wire, sizeof wire, NULL, 0);
    wi_parse(&vars, decode);
    if (vars.overrun != 0 || vars.faults != 0)
        return 9;
    if (vars.vars[EXAMPLE_TELEMETRY_STATUS_ORDINAL_VERSION] != 1 ||
        vars.vars[EXAMPLE_TELEMETRY_STATUS_ORDINAL_MODE] != 2 ||
        vars.vars[EXAMPLE_TELEMETRY_STATUS_ORDINAL_SAMPLE_COUNT] != 0x1234)
        return 10;

    wire[0] |= 0x10;
    wi_decode_init(&vars, wire, sizeof wire, NULL, 0);
    wi_parse(&vars, decode);
    if ((vars.faults & EXAMPLE_TELEMETRY_WFB_FAULT_FIXED_FIELD) == 0)
        return 11;

    return 0;
}
