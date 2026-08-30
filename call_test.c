// call_test.c   %[n] - the format string call
#include <stdio.h>
#include <string.h>
#include "wire_format.h"

static int fails;
static void ck(const char *what, long long got, long long want)
{
    if (got != want) { printf("  %-30s got %lld want %lld  *** FAIL ***\n",
                              what, got, want); fails++; }
    else             { printf("  %-30s %lld\n", what, got); }
}

// a "coordinate" sub-message, referenced rather than copied
static const char f_coord[] = "%p1%w%p2%w";
static const char f_pad[]   = "%{255}%c";
static const char f_self[]  = "%{1}%c%[3]";        // index 3 calls ITSELF

// ⚠ MUTUAL recursion - 4 calls 5, 5 calls 4.  neither is self referential
// so neither looks wrong on its own; the cycle only exists in the table.
// the FORWARD call 4 -> 5 is what the rule refuses, which breaks it.

static const char f_ping[]  = "%{2}%c%[5]";
static const char f_pong[]  = "%{3}%c%[4]";

static const char *table[] = { f_coord, f_pad, NULL, f_self, f_ping, f_pong };

int main(void)
{
    wi_vars_t v;
    uint8_t buf[64];
    int64_t p[4];
    size_t n;

    // ---- a call in the middle of a format ----
    p[0] = 0x1122; p[1] = 0x3344;
    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%{170}%c%[0]%{187}%c");     // AA <coord> BB
    ck("len (1 + 4 + 1)", n, 6);
    ck("byte 0 = 0xAA", buf[0], 0xAA);
    ck("coord hi", (buf[1] << 8) | buf[2], 0x1122);
    ck("coord lo", (buf[3] << 8) | buf[4], 0x3344);
    ck("byte 5 = 0xBB", buf[5], 0xBB);

    // ---- two calls, and a call after a call ----
    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%[1]%[1]%[0]%[1]");
    ck("len (1+1+4+1)", n, 7);
    ck("pad, pad", (buf[0] == 255) && (buf[1] == 255), 1);
    ck("resumed after call", buf[6], 255);

    // ---- a NULL slot and an out of range index are skipped ----
    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%{7}%c%[2]%[99]%{8}%c");
    ck("bad index skipped", n, 2);
    ck("still emitted 7,8", (buf[0] == 7) && (buf[1] == 8), 1);

    // ---- no table at all ----
    wi_init(&v, buf, sizeof buf, p, 2);
    n = wi_parse(&v, "%{9}%c%[0]");
    ck("no table skipped", n, 1);

    // ---- SELF reference is a cycle of length one: refused ----
    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%[3]");
    ck("self call refused", n, 1);          // its byte, then nothing
    ck("  emitted its byte", buf[0], 1);

    // ---- MUTUAL cycle: the FORWARD half is refused ----
    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%[4]");               // 4 emits 2, then 4->5 refused
    ck("forward ref refused", n, 1);
    ck("  emitted its byte", buf[0], 2);

    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%[5]");               // 5 emits 3, 5->4 IS allowed
    ck("backward ref allowed", n, 2);
    ck("  5 then 4", (buf[0]==3) && (buf[1]==2), 1);

    // ---- a legal chain still nests properly: top -> 5 -> 4 ----
    wi_init(&v, buf, sizeof buf, p, 2);
    wi_set_formats(&v, table, 6);
    n = wi_parse(&v, "%{9}%c%[5]%{9}%c");
    ck("call returns to caller", n, 4);
    ck("  9,3,2,9", (buf[0]==9)&&(buf[1]==3)&&(buf[2]==2)&&(buf[3]==9), 1);

    // ---- DECODE side: %[n] works there too ----
    {
        static const char d_pair[] = "%S%Pa%S%Pb";
        static const char *dt[] = { d_pair };
        uint8_t in[6] = { 0x00, 0x07, 0x00, 0x09, 0x00, 0x2a };
        wi_decode_init(&v, in, sizeof in, NULL, 0);
        wi_set_formats(&v, dt, 1);
        wi_parse(&v, "%[0]%S%Pc");
        ck("decode a", v.atoz[0], 7);
        ck("decode b", v.atoz[1], 9);
        ck("decode c after call", v.atoz[2], 42);
        ck("in_pos", (long long)v.in_pos, 6);
    }

    // ---- %: the counted call -------------------------------------
    printf("LOOP  %%:\n");
    {
        static const char f_byte[] = "%{88}%c";
        static const char *t[] = { f_byte };

        wi_init(&v, buf, sizeof buf, p, 2);
        ck("set_formats", wi_set_formats(&v, t, 1), 0);
        n = wi_parse(&v, "%{3}%:%[0]");
        ck("three times", n, 3);
        ck("  all 88", (buf[0]==88)&&(buf[1]==88)&&(buf[2]==88), 1);

        wi_init(&v, buf, sizeof buf, p, 2);  wi_set_formats(&v, t, 1);
        n = wi_parse(&v, "%{0}%:%[0]");
        ck("zero times", n, 0);

        wi_init(&v, buf, sizeof buf, p, 2);  wi_set_formats(&v, t, 1);
        n = wi_parse(&v, "%[0]");
        ck("no %%: means once", n, 1);

        // the count is computable, not a baked in digit
        p[0] = 5;
        wi_init(&v, buf, sizeof buf, p, 1);  wi_set_formats(&v, t, 1);
        n = wi_parse(&v, "%p1%:%[0]");
        ck("count from a param", n, 5);

        // %: arms the NEXT call, not only an adjacent one
        wi_init(&v, buf, sizeof buf, p, 2);  wi_set_formats(&v, t, 1);
        n = wi_parse(&v, "%{4}%:%{9}%c%[0]");   // 9, then 4 x 88
        ck("arms the next call", n, 5);
        ck("  9 then four 88", (buf[0]==9)&&(buf[1]==88)&&(buf[4]==88), 1);

        // and a REFUSED call still consumes it - one %: arms one call
        wi_init(&v, buf, sizeof buf, p, 2);  wi_set_formats(&v, t, 1);
        n = wi_parse(&v, "%{4}%:%[9]%[0]");     // %[9] out of range
        ck("refused call eats it", n, 1);

        // loop and call together: the loop body may itself call down
        {
            static const char f_in[]  = "%{1}%c";
            static const char f_out[] = "%{2}%c%[0]";
            static const char *t2[] = { f_in, f_out };
            wi_init(&v, buf, sizeof buf, p, 2);
            ck("depth 2 accepted", wi_set_formats(&v, t2, 2), 0);
            n = wi_parse(&v, "%{3}%:%[1]");
            ck("3 x (2 then 1)", n, 6);
            ck("  2,1,2,1,2,1", (buf[0]==2)&&(buf[1]==1)&&(buf[4]==2)&&(buf[5]==1), 1);
        }
    }

    // ---- buffer overrun is reported, not asserted -------------------
    printf("OVERRUN\n");
    {
        static const char f_byte[] = "%{88}%c";
        static const char *t[] = { f_byte };
        uint8_t small[4];

        wi_init(&v, small, sizeof small, p, 2);  wi_set_formats(&v, t, 1);
        n = wi_parse(&v, "%{100}%:%[0]");
        ck("stopped at buffer end", n, sizeof small);
        ck("overrun flagged", v.overrun, 1);

        uint8_t in2[2] = { 0, 7 };
        wi_decode_init(&v, in2, sizeof in2, NULL, 0);
        wi_parse(&v, "%S%Pa%S%Pb");         // asks for 4, has 2
        ck("short input flagged", v.overrun, 1);
    }

    // ---- depth is MEASURED, not assumed -----------------------------
    printf("TABLE DEPTH\n");
    {
        // 40 formats, none of which call anything: depth 1, must be fine
        static const char flat[] = "%{1}%c";
        const char *big[40];
        for (int i = 0; i != 40; i++) { big[i] = flat; }
        wi_init(&v, buf, sizeof buf, p, 2);
        ck("40 flat formats accepted", wi_set_formats(&v, big, 40), 0);

        // a genuine chain deeper than WI_CALL_DEPTH is refused
        static char deep[WI_CALL_DEPTH + 2][16];
        const char *chain[WI_CALL_DEPTH + 2];
        chain[0] = "%{1}%c";
        for (int i = 1; i != WI_CALL_DEPTH + 2; i++)
        {
            snprintf(deep[i], sizeof deep[i], "%%[%d]", i - 1);
            chain[i] = deep[i];
        }
        wi_init(&v, buf, sizeof buf, p, 2);
        ck("deep chain refused", wi_set_formats(&v, chain, WI_CALL_DEPTH + 2), -1);
    }

    // ---- %rN - an array emitter that WALKS, which %: cannot --------
    printf("ARRAY  %%rN\n");
    {
        uint8_t  b8[3]  = { 0x11, 0x22, 0x33 };
        uint16_t b16[3] = { 0x1122, 0x3344, 0x5566 };
        uint32_t b32[2] = { 0x11223344u, 0x55667788u };

        wi_init(&v, buf, sizeof buf, p, 2);
        p[0] = (int64_t)(uintptr_t)b8;  p[1] = 3;
        wi_init(&v, buf, sizeof buf, p, 2);
        n = wi_parse(&v, "%p1%p2%r1");
        ck("%%r1 length", n, 3);
        ck("  bytes", (buf[0]==0x11)&&(buf[1]==0x22)&&(buf[2]==0x33), 1);

        p[0] = (int64_t)(uintptr_t)b16;  p[1] = 3;
        wi_init(&v, buf, sizeof buf, p, 2);
        n = wi_parse(&v, "%p1%p2%r2");
        ck("%%r2 length", n, 6);
        ck("  big endian", (buf[0]==0x11)&&(buf[1]==0x22)&&
                           (buf[2]==0x33)&&(buf[3]==0x44), 1);

        p[0] = (int64_t)(uintptr_t)b32;  p[1] = 2;
        wi_init(&v, buf, sizeof buf, p, 2);
        n = wi_parse(&v, "%p1%p2%r4");
        ck("%%r4 length", n, 8);
        ck("  big endian", (buf[0]==0x11)&&(buf[3]==0x44)&&
                           (buf[4]==0x55)&&(buf[7]==0x88), 1);

        // a size that is not 1, 2 or 4 is refused rather than guessed
        p[0] = (int64_t)(uintptr_t)b8;  p[1] = 3;
        wi_init(&v, buf, sizeof buf, p, 2);
        wi_parse(&v, "%p1%p2%r3");
        ck("%%r3 refused", v.overrun, 1);
    }

    printf("\n%s\n", fails ? "*** FAILURES ***" : "%[n], %: and %rN work");
    return fails != 0;
}
