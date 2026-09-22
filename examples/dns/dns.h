// dns.h  - DNS protocol definitions for the wire_format demo
// -----------------------------------------------------------------------

#ifndef DNS_H
#define DNS_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------

#define DNS_QTYPE_A     1       // IPv4 address
#define DNS_QTYPE_AAAA  28      // IPv6 address
#define DNS_QTYPE_MX    15      // mail exchange
#define DNS_QTYPE_NS    2       // name server
#define DNS_QTYPE_TXT   16      // text record

#define DNS_QCLASS_IN   1       // internet

#define DNS_FLAG_RD     0x0100  // recursion desired

#define DNS_MAX_NAME    255     // max encoded name length
#define DNS_HDR_LEN     12      // fixed header size
#define DNS_MAX_ANSWERS 32      // bounded storage for the UDP demo

typedef struct
{
    uint16_t       type;
    uint16_t       class;
    uint32_t       ttl;
    uint16_t       data_length;
    const uint8_t *data;
} dns_answer_t;

typedef struct
{
    uint16_t     transaction_id;
    uint16_t     flags;
    uint16_t     question_count;
    uint16_t     answer_count;
    uint16_t     response_code;
    size_t       answers_decoded;
    dns_answer_t answers[DNS_MAX_ANSWERS];
} dns_response_t;

typedef enum
{
    DNS_DECODE_OK = 0,
    DNS_DECODE_INVALID_ARGUMENT,
    DNS_DECODE_INVALID_DATABASE,
    DNS_DECODE_TRUNCATED_HEADER,
    DNS_DECODE_TOO_MANY_ANSWERS,
    DNS_DECODE_TRUNCATED_QUESTION,
    DNS_DECODE_TRUNCATED_ANSWER,
    DNS_DECODE_TRUNCATED_ANSWER_DATA,
} dns_decode_status_t;

// -----------------------------------------------------------------------
size_t dns_encode_name(const char *name, uint8_t *out);
size_t dns_build_query(const char *name, uint16_t qtype, uint16_t txid,
                       uint8_t *out, size_t outsize);
dns_decode_status_t dns_decode_response(const uint8_t *buf, size_t len,
                                        dns_response_t *response);
void   dns_print_response(const uint8_t *buf, size_t len);

// -----------------------------------------------------------------------

#endif // DNS_H

// =======================================================================
