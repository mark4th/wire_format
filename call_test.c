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
    int64_t p[WI_MAX_PARAMS] = { 0 };
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

    // ---- braced parameter numbers extend the unambiguous one-digit form ----
    printf("PARAMETERS\n");
    p[9] = 0x5a;
    wi_init(&v, buf, sizeof buf, p, 10);
    n = wi_parse(&v, "%p{10}%b%p1%b");
    ck("braced parameter 10", n, 2);
    ck("  p10 then p1", (buf[0] == 0x5a) && (buf[1] == 0x22), 1);

    wi_init(&v, buf, sizeof buf, p, 10);
    wi_parse(&v, "%p{0}%b");
    ck("parameter zero refused", v.overrun, 1);

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
        ck("decode a", v.vars[0], 7);
        ck("decode b", v.vars[1], 9);
        ck("decode c after call", v.vars[2], 42);
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

        wi_init(&v, buf, sizeof buf, p, 2);
        n = wi_parse(&v, "%p1%p2%r");
        ck("bare %%r length", n, 3);
        ck("  bare %%r bytes", (buf[0]==0x11)&&(buf[1]==0x22)&&(buf[2]==0x33), 1);

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

    // ---- local fault reporting survives format calls ----------------
    printf("FAULT  %%E\n");
    {
        static const char f_fault[] = "%{4}%E%{88}%c";
        static const char *t[] = { f_fault };

        wi_init(&v, buf, sizeof buf, NULL, 0);
        ck("set fault format", wi_set_formats(&v, t, 1), 0);
        n = wi_parse(&v, "%[0]");
        ck("nonfatal fault emits", n, 1);
        ck("fault recorded", v.faults, 4);

        wi_init(&v, buf, sizeof buf, NULL, 0);
        ck("set abort format", wi_set_formats(&v, t, 1), 0);
        v.abort_mask = 4;
        n = wi_parse(&v, "%[0]");
        ck("fatal fault stops", n, 0);
        ck("fatal fault recorded", v.faults, 4);
    }

    // ---- version 2 zero-copy variable byte fields ------------------
    printf("VERSION 2 SLICES\n");
    {
        static const uint8_t tail[] = { 0xaa, 0xbb, 0xcc };
        static const uint8_t input[] = { 0x12, 0x34, 0xaa, 0xbb, 0xcc };

        wi_init(&v, buf, sizeof buf, NULL, 0);
        ck("set encode slice", wi_set_slice(&v, 2, tail, sizeof tail), 0);
        n = wi_parse(&v, "%{4660}%w%{2}%v");
        ck("slice encode length", n, sizeof input);
        ck("slice encode bytes", memcmp(buf, input, sizeof input), 0);

        wi_decode_init(&v, input, sizeof input, NULL, 0);
        wi_parse(&v, "%S%Pa%{2}%R");
        ck("slice decode scalar", v.vars[0], 0x1234);
        ck("slice decode length", v.slices[2].length, sizeof tail);
        ck("slice decode bytes", memcmp(v.slices[2].data, tail, sizeof tail), 0);
    }

    // ---- version 3 SDNV and bounded slices -------------------------
    printf("VERSION 3 SDNV / BOUNDED SLICES\n");
    {
        static const uint8_t value[] = { 0xde, 0xad, 0xbe };
        static const uint8_t input[] = {
            0x95, 0x3c, 0x03, 0xde, 0xad, 0xbe, 0x7f
        };

        p[0] = 0xabc;
        wi_init(&v, buf, sizeof buf, p, 1);
        ck("set bounded slice", wi_set_slice(&v, 1, value, sizeof value), 0);
        n = wi_parse(&v, "%p1%d%{1}%z%d%{1}%z%{1}%V%{127}%b");
        ck("bounded encode length", n, sizeof input);
        ck("bounded encode bytes", memcmp(buf, input, sizeof input), 0);

        wi_decode_init(&v, input, sizeof input, NULL, 0);
        wi_parse(&v, "%D%Pa%D%Pb%gb%{1}%N%B%Pc");
        ck("SDNV value", v.vars[0], 0xabc);
        ck("SDNV length", v.vars[1], sizeof value);
        ck("bounded decode length", v.slices[1].length, sizeof value);
        ck("bounded decode bytes", memcmp(v.slices[1].data, value, sizeof value), 0);
        ck("trailing byte remains parseable", v.vars[2], 0x7f);
    }

    // ---- version 3 counted record sequences -----------------------
    printf("VERSION 3 COUNTED RECORDS\n");
    {
        static const char child_encode[] = "%p1%b%p2%d";
        static const char child_decode[] = "%B%Pa%D%Pb";
        static const char *encode_formats[] = { child_encode };
        static const char *decode_formats[] = { child_decode };
        static const uint8_t expected[] = { 2, 0x11, 1, 0x22, 0x95, 0x3c, 0xee };
        wi_record_t encode_rows[2] = {0};
        wi_record_t decode_rows[2] = {0};
        wi_record_list_t encode_list = { encode_rows, 2, 2, 2 };
        wi_record_list_t decode_list = { decode_rows, 0, 2, 2 };

        encode_rows[0].values[0] = 0x11;
        encode_rows[0].values[1] = 1;
        encode_rows[1].values[0] = 0x22;
        encode_rows[1].values[1] = 0xabc;
        wi_init(&v, buf, sizeof buf, NULL, 0);
        ck("set record encode formats", wi_set_formats(&v, encode_formats, 1), 0);
        ck("set encode record list", wi_set_record_list(&v, 0, &encode_list), 0);
        n = wi_parse(&v, "%{0}%k%d%{0}%k%{0}%{2}%J[0]%{238}%b");
        ck("record encode length", n, sizeof expected);
        ck("record encode bytes", memcmp(buf, expected, sizeof expected), 0);

        wi_decode_init(&v, expected, sizeof expected, NULL, 0);
        ck("set record decode formats", wi_set_formats(&v, decode_formats, 1), 0);
        ck("set decode record list", wi_set_record_list(&v, 0, &decode_list), 0);
        wi_parse(&v, "%D%Pa%ga%{0}%{2}%J[0]%B%Pb");
        ck("record decode count", decode_list.count, 2);
        ck("record zero scalar", decode_rows[0].values[0], 0x11);
        ck("record one SDNV", decode_rows[1].values[1], 0xabc);
        ck("record trailing byte", v.vars[1], 0xee);
    }

    printf("\n%s\n", fails ? "*** FAILURES ***" : "%[n], %: and %rN work");
    return fails != 0;
}
