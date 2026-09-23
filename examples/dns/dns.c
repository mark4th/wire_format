// dns.c  - DNS query construction and response parsing via wire_format
// -----------------------------------------------------------------------

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "wire_format.h"
#include "dns.h"
#include "dns_protocol.h"

extern const uint8_t wi_dns_database_start[];
extern const uint8_t wi_dns_database_end[];

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

static int dns_database_valid(void)
{
    size_t size = (size_t)(wi_dns_database_end - wi_dns_database_start);

    return size == DNS_WI_FILE_SIZE
        && size >= 64
        && memcmp(wi_dns_database_start, "WI\0\0", 4) == 0
        && get_u32(wi_dns_database_start + 8) == size;
}

static const char *dns_string(uint32_t section, uint32_t slot)
{
    size_t size = (size_t)(wi_dns_database_end - wi_dns_database_start);
    size_t entry = (size_t)section + (size_t)slot * 4;
    uint32_t relative;
    size_t offset;

    if (!dns_database_valid() || entry > size || size - entry < 4)
        return NULL;
    relative = get_u32(wi_dns_database_start + entry);
    if (relative == DNS_WI_NO_STRING)
        return NULL;
    offset = (size_t)DNS_WI_STRING_TABLE_OFFSET + relative;
    if (offset >= size || memchr(wi_dns_database_start + offset, 0, size - offset) == NULL)
        return NULL;
    return (const char *)(wi_dns_database_start + offset);
}

// Encode a dotted hostname to DNS wire format.
// "www.example.com" becomes \x03www\x07example\x03com\x00.

size_t dns_encode_name(const char *name, uint8_t *out)
{
    size_t total = 0;
    const char *p = name;

    while (*p != 0)
    {
        const char *dot = strchr(p, '.');
        size_t length = dot != NULL ? (size_t)(dot - p) : strlen(p);

        if (length == 0 || length > 63 || total > DNS_MAX_NAME - length - 2)
            return 0;
        out[total++] = (uint8_t)length;
        memcpy(out + total, p, length);
        total += length;

        p += length;
        if (*p == '.')
            p++;
    }

    out[total++] = 0;
    return total;
}

size_t dns_build_query(const char *name, uint16_t qtype, uint16_t txid,
                       uint8_t *out, size_t outsize)
{
    uint8_t encoded[DNS_MAX_NAME];
    const char *header_format;
    const char *tail_format;
    int64_t header[DNS_QUERY_HEADER_CALLER_FIELD_COUNT];
    int64_t tail[DNS_QUESTION_TAIL_CALLER_FIELD_COUNT];
    wi_vars_t variables;
    size_t name_length;
    size_t total;

    header_format = dns_string(DNS_QUERY_HEADER_STRINGS_OFFSET,
                               DNS_QUERY_HEADER_STRING_ENCODE);
    tail_format = dns_string(DNS_QUESTION_TAIL_STRINGS_OFFSET,
                             DNS_QUESTION_TAIL_STRING_ENCODE);
    if (header_format == NULL || tail_format == NULL)
        return 0;

    name_length = dns_encode_name(name, encoded);
    if (name_length == 0)
        return 0;

    header[DNS_QUERY_HEADER_ORDINAL_TRANSACTION_ID] = txid;
    header[DNS_QUERY_HEADER_ORDINAL_FLAGS] = DNS_FLAG_RD;
    header[DNS_QUERY_HEADER_ORDINAL_QUESTION_COUNT] = 1;

    wi_init(&variables, out, outsize, header,
            DNS_QUERY_HEADER_CALLER_FIELD_COUNT);
    total = wi_parse(&variables, header_format);
    if (variables.overrun || total > outsize || name_length > outsize - total)
        return 0;

    memcpy(out + total, encoded, name_length);
    total += name_length;

    tail[DNS_QUESTION_TAIL_ORDINAL_QUERY_TYPE] = qtype;
    tail[DNS_QUESTION_TAIL_ORDINAL_QUERY_CLASS] = DNS_QCLASS_IN;
    wi_init(&variables, out + total, outsize - total, tail,
            DNS_QUESTION_TAIL_CALLER_FIELD_COUNT);
    total += wi_parse(&variables, tail_format);
    if (variables.overrun)
        return 0;
    return total;
}

static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end)
{
    while (p < end && *p != 0)
    {
        size_t length;
        if ((*p & 0xc0) == 0xc0)
            return end - p >= 2 ? p + 2 : NULL;
        length = *p;
        if (length > 63 || (size_t)(end - p) <= length)
            return NULL;
        p += length + 1;
    }
    return p < end ? p + 1 : NULL;
}

dns_decode_status_t dns_decode_response(const uint8_t *buffer, size_t length,
                                        dns_response_t *response)
{
    const char *header_format;
    const char *record_format;
    const uint8_t *p;
    const uint8_t *end;
    wi_vars_t variables;
    uint16_t index;

    if (buffer == NULL || response == NULL)
        return DNS_DECODE_INVALID_ARGUMENT;
    memset(response, 0, sizeof(*response));
    end = buffer + length;

    header_format = dns_string(DNS_RESPONSE_HEADER_STRINGS_OFFSET,
                               DNS_RESPONSE_HEADER_STRING_DECODE);
    record_format = dns_string(DNS_RESOURCE_RECORD_STRINGS_OFFSET,
                               DNS_RESOURCE_RECORD_STRING_DECODE);
    if (header_format == NULL || record_format == NULL)
        return DNS_DECODE_INVALID_DATABASE;
    if (length < DNS_RESPONSE_HEADER_WIRE_SIZE)
        return DNS_DECODE_TRUNCATED_HEADER;

    wi_decode_init(&variables, buffer, length, NULL, 0);
    wi_parse(&variables, header_format);
    if (variables.overrun)
        return DNS_DECODE_TRUNCATED_HEADER;

    response->transaction_id =
        (uint16_t)variables.vars[DNS_RESPONSE_HEADER_ORDINAL_TRANSACTION_ID];
    response->flags =
        (uint16_t)variables.vars[DNS_RESPONSE_HEADER_ORDINAL_FLAGS];
    response->question_count =
        (uint16_t)variables.vars[DNS_RESPONSE_HEADER_ORDINAL_QUESTION_COUNT];
    response->answer_count =
        (uint16_t)variables.vars[DNS_RESPONSE_HEADER_ORDINAL_ANSWER_COUNT];
    response->response_code = response->flags & 0x000f;

    if (response->answer_count > DNS_MAX_ANSWERS)
        return DNS_DECODE_TOO_MANY_ANSWERS;
    if (response->response_code != 0)
        return DNS_DECODE_OK;

    p = buffer + variables.in_pos;
    for (index = 0; index < response->question_count; index++)
    {
        p = skip_name(p, end);
        if (p == NULL || (size_t)(end - p) < DNS_QUESTION_TAIL_WIRE_SIZE)
            return DNS_DECODE_TRUNCATED_QUESTION;
        p += DNS_QUESTION_TAIL_WIRE_SIZE;
    }

    for (index = 0; index < response->answer_count; index++)
    {
        dns_answer_t *answer = &response->answers[index];

        p = skip_name(p, end);
        if (p == NULL || (size_t)(end - p) < DNS_RESOURCE_RECORD_WIRE_SIZE)
            return DNS_DECODE_TRUNCATED_ANSWER;

        variables.in_pos = (size_t)(p - buffer);
        wi_parse(&variables, record_format);
        if (variables.overrun)
            return DNS_DECODE_TRUNCATED_ANSWER;
        p = buffer + variables.in_pos;

        answer->type =
            (uint16_t)variables.vars[DNS_RESOURCE_RECORD_ORDINAL_RECORD_TYPE];
        answer->class =
            (uint16_t)variables.vars[DNS_RESOURCE_RECORD_ORDINAL_RECORD_CLASS];
        answer->ttl =
            (uint32_t)variables.vars[DNS_RESOURCE_RECORD_ORDINAL_TTL];
        answer->data_length =
            (uint16_t)variables.vars[DNS_RESOURCE_RECORD_ORDINAL_DATA_LENGTH];
        if ((size_t)(end - p) < answer->data_length)
            return DNS_DECODE_TRUNCATED_ANSWER_DATA;
        answer->data = p;
        response->answers_decoded++;
        p += answer->data_length;
    }
    return DNS_DECODE_OK;
}

void dns_print_response(const uint8_t *buffer, size_t length)
{
    dns_response_t response;
    dns_decode_status_t status;
    size_t index;

    status = dns_decode_response(buffer, length, &response);
    if (status != DNS_DECODE_OK)
    {
        static const char *const messages[] = {
            "",
            "invalid decode argument",
            "compiled DNS database is invalid",
            "response header is truncated",
            "response has too many answers",
            "truncated question section",
            "truncated answer record",
            "truncated answer data",
        };
        printf("%s\n", messages[status]);
        return;
    }

    printf("txid=0x%04x  flags=0x%04x  questions=%u  answers=%u\n",
           response.transaction_id, response.flags,
           response.question_count, response.answer_count);
    if (response.response_code != 0)
    {
        printf("error: rcode=%u\n", response.response_code);
        return;
    }

    for (index = 0; index < response.answers_decoded; index++)
    {
        const dns_answer_t *answer = &response.answers[index];

        if (answer->type == DNS_QTYPE_A && answer->data_length == 4)
        {
            struct in_addr address;
            memcpy(&address, answer->data, 4);
            printf("  A  ttl=%-6u  %s\n", answer->ttl, inet_ntoa(address));
        }
        else if (answer->type == DNS_QTYPE_AAAA && answer->data_length == 16)
        {
            char address[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, answer->data, address, sizeof(address));
            printf("  AAAA ttl=%-6u  %s\n", answer->ttl, address);
        }
        else
        {
            printf("  type=%-5u ttl=%-6u  rdlength=%u\n",
                   answer->type, answer->ttl, answer->data_length);
        }
    }
}
