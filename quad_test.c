// quad_test.c   %q and %Q - the 64 bit pair
//
// ★ THE POINT IS THE ROUND TRIP AND THE BYTE ORDER, separately.  a pair
// that agreed with each other while both being little-endian would round
// trip perfectly and be wrong on the wire - so the emitted bytes are
// checked against a hand written big-endian expectation as well.

#include <stdio.h>
#include <string.h>
#include "wire_format.h"

static int fails;

static void ck(const char *what, long long got, long long want)
{
    if (got != want) { printf("  %-34s got %lld want %lld  *** FAIL ***\n",
                              what, got, want); fails++; }
    else             { printf("  %-34s %lld\n", what, got); }
}

static void ck_bytes(const char *what, const uint8_t *got,
                     const uint8_t *want, size_t n)
{
    if (memcmp(got, want, n) != 0)
    {
        size_t i;

        printf("  %-34s *** FAIL ***\n    got ", what);
        for (i = 0; i != n; i++) { printf("%02x", got[i]); }
        printf("\n    want ");
        for (i = 0; i != n; i++) { printf("%02x", want[i]); }
        printf("\n");
        fails++;
    }
    else { printf("  %-34s ok\n", what); }
}

int main(void)
{
    wi_vars_t v;
    uint8_t buf[64];
    int64_t p[4];
    size_t n;

    // ---- eight bytes, big-endian, in that order ----

    {
        static const uint8_t want[] =
            { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };

        p[0] = (int64_t)0x0123456789abcdefULL;
        wi_init(&v, buf, sizeof buf, p, 1);
        n = wi_parse(&v, "%p1%q");

        ck("emitted length", (long long)n, 8);
        ck_bytes("big-endian, high byte first", buf, want, 8);
    }

    // ---- and back ----

    {
        wi_decode_init(&v, buf, 8, NULL, 0);
        wi_parse(&v, "%Q%Pa");

        ck("round trip", (long long)(uint64_t)v.atoz[0],
           (long long)0x0123456789abcdefULL);
        ck("consumed", (long long)v.in_pos, 8);
    }

    // ---- ⚠ THE TOP BIT.  a u64 above INT64_MAX rides as a negative
    // int64 on the value stack; the BYTES must still be exact, which is
    // the property callers actually depend on.

    {
        static const uint8_t want[] =
            { 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88 };
        uint64_t back;

        p[0] = (int64_t)0xffeeddccbbaa9988ULL;
        wi_init(&v, buf, sizeof buf, p, 1);
        n = wi_parse(&v, "%p1%q");

        ck("top bit set - length", (long long)n, 8);
        ck_bytes("top bit set - bytes exact", buf, want, 8);

        wi_decode_init(&v, buf, 8, NULL, 0);
        wi_parse(&v, "%Q%Pa");

        back = (uint64_t)v.atoz[0];

        ck("top bit set - round trips",
           (long long)(back == 0xffeeddccbbaa9988ULL), 1);
    }

    // ---- zero, and the all-ones value ----

    {
        uint64_t back;

        p[0] = 0;
        wi_init(&v, buf, sizeof buf, p, 1);
        wi_parse(&v, "%p1%q");
        wi_decode_init(&v, buf, 8, NULL, 0);
        wi_parse(&v, "%Q%Pa");
        ck("zero round trips", (long long)v.atoz[0], 0);

        p[0] = (int64_t)0xffffffffffffffffULL;
        wi_init(&v, buf, sizeof buf, p, 1);
        wi_parse(&v, "%p1%q");
        wi_decode_init(&v, buf, 8, NULL, 0);
        wi_parse(&v, "%Q%Pa");
        back = (uint64_t)v.atoz[0];
        ck("all ones round trips",
           (long long)(back == 0xffffffffffffffffULL), 1);
    }

    // ---- ⚠ it must not be confused with two %W.  the same eight bytes
    // read as a pair of uint32 is a DIFFERENT pair of values, and a
    // format mixing the two must still agree on where it is.

    {
        p[0] = (int64_t)0x0011223344556677ULL;
        wi_init(&v, buf, sizeof buf, p, 1);
        wi_parse(&v, "%p1%q");

        wi_decode_init(&v, buf, 8, NULL, 0);
        wi_parse(&v, "%L%Pa%L%Pb");

        ck("high word as %L", (long long)v.atoz[0], 0x00112233);
        ck("low word as %L",  (long long)v.atoz[1], 0x44556677);
    }

    // ---- ⚠ A SHORT BUFFER MUST OVERRUN, not write seven bytes and
    // claim success.  emit and read alike.

    {
        p[0] = (int64_t)0x0123456789abcdefULL;
        wi_init(&v, buf, 4, p, 1);
        wi_parse(&v, "%p1%q");
        ck("emit into 4 bytes sets overrun", (long long)(v.overrun != 0), 1);

        wi_decode_init(&v, buf, 4, NULL, 0);
        wi_parse(&v, "%Q%Pa");
        ck("read from 4 bytes sets overrun", (long long)(v.overrun != 0), 1);
    }

    // ---- ⓘ and it composes: a quad in the middle of a record ----

    {
        p[0] = 0xaa; p[1] = (int64_t)0x1122334455667788ULL; p[2] = 0xbbcc;
        wi_init(&v, buf, sizeof buf, p, 3);
        n = wi_parse(&v, "%p1%c%p2%q%p3%w");

        ck("1 + 8 + 2", (long long)n, 11);

        wi_decode_init(&v, buf, n, NULL, 0);
        wi_parse(&v, "%B%Pa%Q%Pb%S%Pc");

        ck("leading byte", (long long)v.atoz[0], 0xaa);
        ck("embedded quad", (long long)(uint64_t)v.atoz[1],
           (long long)0x1122334455667788ULL);
        ck("trailing short", (long long)v.atoz[2], 0xbbcc);
    }

    printf("%s\n", fails ? "FAILURES" : "all pass: %q / %Q");

    return fails ? 1 : 0;
}
