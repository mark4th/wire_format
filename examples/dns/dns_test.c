#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dns.h"

int main(void)
{
    static const uint8_t expected_query[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
    };
    static const uint8_t response[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        0xc0, 0x0c,
        0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x01, 0x2c,
        0x00, 0x04, 0xc0, 0x00, 0x02, 0x01,
    };
    static const uint8_t expected_address[] = { 0xc0, 0x00, 0x02, 0x01 };
    uint8_t query[64];
    dns_response_t decoded;
    dns_decode_status_t status;
    size_t length;

    length = dns_build_query("example.com", DNS_QTYPE_A, 0x1234,
                             query, sizeof(query));
    if (length != sizeof(expected_query)
        || memcmp(query, expected_query, sizeof(expected_query)) != 0)
    {
        fprintf(stderr, "compiled DNS query does not match the expected wire image\n");
        return 1;
    }

    status = dns_decode_response(response, sizeof(response), &decoded);
    if (status != DNS_DECODE_OK
        || decoded.transaction_id != 0x1234
        || decoded.flags != 0x8180
        || decoded.question_count != 1
        || decoded.answer_count != 1
        || decoded.response_code != 0
        || decoded.answers_decoded != 1)
    {
        fprintf(stderr, "compiled DNS response header did not decode correctly\n");
        return 1;
    }
    if (decoded.answers[0].type != DNS_QTYPE_A
        || decoded.answers[0].class != DNS_QCLASS_IN
        || decoded.answers[0].ttl != 300
        || decoded.answers[0].data_length != sizeof(expected_address)
        || memcmp(decoded.answers[0].data,
                  expected_address, sizeof(expected_address)) != 0)
    {
        fprintf(stderr, "compiled DNS answer did not decode correctly\n");
        return 1;
    }

    status = dns_decode_response(response, sizeof(response) - 1, &decoded);
    if (status != DNS_DECODE_TRUNCATED_ANSWER_DATA)
    {
        fprintf(stderr, "truncated DNS answer data was not rejected\n");
        return 1;
    }
    return 0;
}
