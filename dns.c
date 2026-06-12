// dns.c  - DNS query construction and response parsing via wire_format
// -----------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "wire_format.h"
#include "dns.h"

// -----------------------------------------------------------------------
// wire_format format strings for DNS message construction
//
// Header: 6 x uint16 big-endian
//   ID | flags | QDCOUNT | ANCOUNT | NSCOUNT | ARCOUNT

const char wi_dns_header[] =
    "%p1%w"         // ID
    "%p2%w"         // flags
    "%p3%w"         // QDCOUNT
    "%{0}%w"        // ANCOUNT = 0
    "%{0}%w"        // NSCOUNT = 0
    "%{0}%w";       // ARCOUNT = 0

// Question section: pre-encoded name + QTYPE + QCLASS
//   p1 = pointer to encoded name, p2 = name length

const char wi_dns_question[] =
    "%p1%p2%r"      // encoded QNAME (raw bytes via pointer + length)
    "%p3%w"         // QTYPE
    "%p4%w";        // QCLASS

// -----------------------------------------------------------------------
// encode a dotted hostname to DNS wire format
// "www.example.com" -> \x03www\x07example\x03com\x00
// returns total bytes written including root label

size_t dns_encode_name(const char *name, uint8_t *out)
{
    size_t      total = 0;
    const char *p     = name;

    while (*p)
    {
        const char *dot = strchr(p, '.');
        size_t      len = dot ? (size_t)(dot - p) : strlen(p);

        out[total++] = (uint8_t)len;
        memcpy(out + total, p, len);
        total += len;

        p += len;
        if (*p == '.') p++;
    }

    out[total++] = 0;           // root label

    return total;
}

// -----------------------------------------------------------------------
// build a complete DNS query into out[], returns total length

size_t dns_build_query(const char *name, uint16_t qtype, uint16_t txid,
                       uint8_t *out, size_t outsize)
{
    uint8_t   encoded[DNS_MAX_NAME];
    wi_vars_t v;
    int64_t   params[4];
    size_t    namelen;
    size_t    total;

    namelen = dns_encode_name(name, encoded);

    // --- header ---
    params[0] = txid;
    params[1] = DNS_FLAG_RD;
    params[2] = 1;              // one question

    wi_init(&v, out, outsize, params, 3);
    total = wi_parse(&v, wi_dns_header);

    // --- question ---
    params[0] = (int64_t)(uintptr_t)encoded;
    params[1] = (int64_t)namelen;
    params[2] = qtype;
    params[3] = DNS_QCLASS_IN;

    wi_init(&v, out + total, outsize - total, params, 4);
    total += wi_parse(&v, wi_dns_question);

    return total;
}

// -----------------------------------------------------------------------
// wire_format format strings for DNS response decoding
//
// wi_dns_resp_hdr: decode 6 x uint16 header fields
//   results in atoz[]: a=txid b=flags c=qdcount d=ancount e=nscount f=arcount

const char wi_dns_resp_hdr[] =
    "%S%Pa"         // txid    -> a
    "%S%Pb"         // flags   -> b
    "%S%Pc"         // qdcount -> c
    "%S%Pd"         // ancount -> d
    "%S%Pe"         // nscount -> e
    "%S%Pf";        // arcount -> f

// wi_dns_rr: decode RR fixed fields (call after name has been skipped)
//   results in atoz[]: a=type b=class c=ttl(uint32) d=rdlength

const char wi_dns_rr[] =
    "%S%Pa"         // type     -> a
    "%S%Pb"         // class    -> b
    "%L%Pc"         // ttl      -> c
    "%S%Pd";        // rdlength -> d

// -----------------------------------------------------------------------
// skip a DNS name field; handles compression pointers
// returns pointer to first byte after the name

static const uint8_t *skip_name(const uint8_t *p)
{
    while (*p)
    {
        if ((*p & 0xc0) == 0xc0)
            return p + 2;       // compression pointer: 2 bytes total
        p += *p + 1;            // skip label
    }
    return p + 1;               // skip root label
}

void dns_print_response(const uint8_t *buf, size_t len)
{
    if (len < DNS_HDR_LEN)
    {
        printf("response too short\n");
        return;
    }

    wi_vars_t v;
    wi_decode_init(&v, buf, len, NULL, 0);
    wi_parse(&v, wi_dns_resp_hdr);

    uint16_t txid    = (uint16_t)v.atoz[0];   // a
    uint16_t flags   = (uint16_t)v.atoz[1];   // b
    uint16_t qdcount = (uint16_t)v.atoz[2];   // c
    uint16_t ancount = (uint16_t)v.atoz[3];   // d
    uint16_t rcode   = flags & 0x000f;

    printf("txid=0x%04x  flags=0x%04x  questions=%u  answers=%u\n",
           txid, flags, qdcount, ancount);

    if (rcode)
    {
        printf("error: rcode=%u\n", rcode);
        return;
    }

    // skip question section (names are variable-length; not suitable for wire_format)
    const uint8_t *p = buf + v.in_pos;
    uint16_t i;

    for (i = 0; i < qdcount && (size_t)(p - buf) < len; i++)
    {
        p = skip_name(p);
        p += 4;                 // QTYPE + QCLASS
    }

    // decode answer records
    for (i = 0; i < ancount && (size_t)(p - buf) < len; i++)
    {
        p = skip_name(p);

        v.in_pos = (size_t)(p - buf);
        wi_parse(&v, wi_dns_rr);
        p = buf + v.in_pos;

        uint16_t type     = (uint16_t)v.atoz[0];   // a
        uint32_t ttl      = (uint32_t)v.atoz[2];   // c
        uint16_t rdlength = (uint16_t)v.atoz[3];   // d

        if (type == DNS_QTYPE_A && rdlength == 4)
        {
            struct in_addr addr;
            memcpy(&addr, p, 4);
            printf("  A  ttl=%-6u  %s\n", ttl, inet_ntoa(addr));
        }
        else if (type == DNS_QTYPE_AAAA && rdlength == 16)
        {
            char buf6[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, p, buf6, sizeof(buf6));
            printf("  AAAA ttl=%-6u  %s\n", ttl, buf6);
        }
        else
        {
            printf("  type=%-5u ttl=%-6u  rdlength=%u\n", type, ttl, rdlength);
        }

        p += rdlength;
    }
}

// =======================================================================
