// dns.h  - DNS protocol definitions and format strings for winfo demo
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

// -----------------------------------------------------------------------
// winfo format strings
//
// wi_dns_header:
//   p1 = transaction ID  (uint16)
//   p2 = flags           (uint16)
//   p3 = QDCOUNT         (uint16)
//   ANCOUNT/NSCOUNT/ARCOUNT fixed at 0
//
// wi_dns_question:
//   p1 = encoded QNAME pointer  (uintptr_t)
//   p2 = encoded QNAME length   (size_t)
//   p3 = QTYPE                  (uint16)
//   p4 = QCLASS                 (uint16)
//
// wi_dns_resp_hdr  (decode):
//   reads 6 x uint16; results in atoz[]: a=txid b=flags c=qdcount
//   d=ancount e=nscount f=arcount
//
// wi_dns_rr  (decode):
//   reads RR fixed fields after name; results in atoz[]:
//   a=type b=class c=ttl(uint32) d=rdlength

extern const char wi_dns_header[];
extern const char wi_dns_question[];
extern const char wi_dns_resp_hdr[];
extern const char wi_dns_rr[];

// -----------------------------------------------------------------------

size_t dns_encode_name(const char *name, uint8_t *out);
size_t dns_build_query(const char *name, uint16_t qtype, uint16_t txid,
                       uint8_t *out, size_t outsize);
void   dns_print_response(const uint8_t *buf, size_t len);

// -----------------------------------------------------------------------

#endif // DNS_H

// =======================================================================
