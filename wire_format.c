// wire_format.c  - wire format info parser
// -----------------------------------------------------------------------
// Derived from uCurses terminfo format string parser.
// RPN stack, arithmetic/logic/conditionals, binary byte emission.
//
// New specifiers vs terminfo:
//   %b  - emit 1 byte  (low byte of TOS)
//   %w  - emit 2 bytes big-endian (uint16)
//   %W  - emit 4 bytes big-endian (uint32)
//   %[n] - call format n of the caller supplied table (wi_set_formats),
//          strictly lower index only, so a cycle cannot be written
//   %:   - pop a repeat count for the following %[n]
// -----------------------------------------------------------------------

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "wire_format.h"

// -----------------------------------------------------------------------

static wi_vars_t *wi;           // current parse context

// -----------------------------------------------------------------------

// ⚠ NOT assert().  with asserts on this aborted the process and with
// -DNDEBUG it wrote past the buffer; a counted loop makes both reachable
// from a bad count.  overrun stops the parse and the caller checks it.

static void b_emit(uint8_t byte)
{
    if (wi->out_len >= wi->out_size)
    {
        wi->overrun = 1;
        return;
    }

    wi->out[wi->out_len++] = byte;
}

static uint8_t b_read(void)
{
    if (wi->in_pos >= wi->in_size)
    {
        wi->overrun = 1;
        return 0;
    }

    return wi->in[wi->in_pos++];
}

// -----------------------------------------------------------------------
// RPN stack

static void fs_push(int64_t n)
{
    assert(wi->fsp < WI_STACK_DEPTH);
    wi->fstack[wi->fsp++] = n;
}

static int64_t fs_pop(void)
{
    assert(wi->fsp > 0);
    return wi->fstack[--wi->fsp];
}

// -----------------------------------------------------------------------
// variable access

static int64_t *get_var_addr(void)
{
    char c1 = (char)*wi->f_str++;

    return ((c1 >= 'a') && (c1 <= 'z'))
        ? &wi->atoz[c1 - 'a']
        : &wi->AtoZ[c1 - 'A'];
}

// -----------------------------------------------------------------------
// scan to next % specifier (used by conditionals)

static char scan(void)
{
    while (*wi->f_str++ != '%')
        ;
    return (char)*wi->f_str++;
}

// -----------------------------------------------------------------------
// specifier implementations
// -----------------------------------------------------------------------

static void _percent(void) { b_emit('%'); }

static void _and  (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b &  a); }
static void _andl (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b && a); }
static void _or   (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b |  a); }
static void _orl  (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b || a); }
static void _xor  (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b ^  a); }
static void _not  (void) { fs_push(~fs_pop()); }
static void _notl (void) { fs_push(!fs_pop()); }
static void _plus (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b + a); }
static void _minus(void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b - a); }
static void _star (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b * a); }
static void _div  (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(a ? b / a : 0); }
static void _mod  (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(a ? b % a : 0); }

static void _equals (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b == a); }
static void _greater(void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b >  a); }
static void _less   (void) { int64_t a = fs_pop(), b = fs_pop(); fs_push(b <  a); }

// -----------------------------------------------------------------------
// %'x'  push literal character value

static void _tick(void)
{
    fs_push((char)*wi->f_str);
    wi->f_str += 2;             // skip char and closing '
}

// -----------------------------------------------------------------------
// %{123}  push decimal literal

static void _brace(void)
{
    int64_t n = 0;
    char c1;

    while ((c1 = (char)*wi->f_str++) != '}')
    {
        n *= 10;
        n += c1 - '0';
    }

    fs_push(n);
}

// -----------------------------------------------------------------------
// %p1..%p9  push parameter

static void _p(void)
{
    uint8_t c1 = *wi->f_str++ & 0x0f;
    fs_push(wi->params[c1 - 1]);
}

// -----------------------------------------------------------------------
// %Px / %gx  store/load named variable

static void _P(void) { *get_var_addr() = fs_pop(); }
static void _g(void) { fs_push(*get_var_addr()); }

// -----------------------------------------------------------------------
// %?..%t..%e..%;  conditional

static void _t(void)
{
    char c1;

    if (fs_pop() != 0)
        return;

    for (;;)
    {
        c1 = scan();
        if ((c1 == 'e') || (c1 == ';'))
            break;
    }
}

static void _e(void)
{
    char c1;
    do { c1 = scan(); } while (c1 != ';');
}

// -----------------------------------------------------------------------
// emit specifiers

// %c  emit low byte of TOS (terminfo compatible)
static void _c(void) { b_emit((uint8_t)fs_pop()); }

// %b  emit 1 byte (alias for %c, explicit binary intent)
static void _b(void) { b_emit((uint8_t)fs_pop()); }

// %w  emit 2 bytes big-endian
static void _w(void)
{
    uint16_t v = (uint16_t)fs_pop();
    b_emit((uint8_t)(v >> 8));
    b_emit((uint8_t)(v & 0xff));
}

// %W  emit 4 bytes big-endian
static void _bW(void)
{
    uint32_t v = (uint32_t)fs_pop();
    b_emit((uint8_t)(v >> 24));
    b_emit((uint8_t)(v >> 16));
    b_emit((uint8_t)(v >>  8));
    b_emit((uint8_t)(v & 0xff));
}

// %B  read 1 byte from input → push
static void _rB(void) { fs_push(b_read()); }

// %S  read 2 bytes big-endian → push as uint16
static void _rS(void)
{
    uint16_t v = (uint16_t)b_read() << 8;
    v |= b_read();
    fs_push(v);
}

// %L  read 4 bytes big-endian → push as uint32
static void _rL(void)
{
    uint32_t v = (uint32_t)b_read() << 24;
    v |= (uint32_t)b_read() << 16;
    v |= (uint32_t)b_read() << 8;
    v |= b_read();
    fs_push(v);
}

// %x  encode bit field: pop position, width, value → bit_acc |= (value & mask) << position
static void _bx(void)
{
    int      pos   = (int)fs_pop();
    int      width = (int)fs_pop();
    uint8_t  val   = (uint8_t)fs_pop();
    uint8_t  mask  = (uint8_t)((1u << width) - 1u);

    wi->bit_acc |= (val & mask) << pos;
}

// %X  decode bit field: pop position, width → extract from in_acc → push
static void _bX(void)
{
    int     pos   = (int)fs_pop();
    int     width = (int)fs_pop();
    uint8_t mask  = (uint8_t)((1u << width) - 1u);

    if (!wi->in_loaded)
    {
        wi->in_acc    = b_read();
        wi->in_loaded = 1;
    }

    fs_push((wi->in_acc >> pos) & mask);
}

// %f  flush: encode emits bit_acc and resets; decode discards current in_acc byte
static void _f(void)
{
    if (wi->out)
    {
        b_emit(wi->bit_acc);
        wi->bit_acc = 0;
    }
    else
    {
        wi->in_loaded = 0;
    }
}

// %r  emit raw buffer: TOS = length, next = pointer
// %rN  - emit a COUNT of N byte elements from a buffer, big endian.
//
//      %r1   bytes          (what %r used to be)
//      %r2   uint16 array
//      %r4   uint32 array
//
// ⚠ THE DIGIT IS REQUIRED.  bare %r would have to guess, and guessing
// wrong means silently misreading a literal digit that followed it as
// an element size.  Explicit, and it matches %p1.
//
// ★ this is why a repeat cannot walk an array: %: runs a format again
// with the SAME parameters, so it emits one element N times.  Walking
// is a property of the emitter, not of the loop.

static void _r(void)
{
    size_t   i, j, size;
    int      c1 = *wi->f_str++;
    size_t   len;
    uint8_t *ptr;

    size = (size_t)(c1 - '0');

    if ((size != 1) && (size != 2) && (size != 4))
    {
        wi->overrun = 1;        // not an element size - refuse loudly
        return;
    }

    len = (size_t)fs_pop();
    ptr = (uint8_t *)(uintptr_t)fs_pop();

    if (ptr == NULL)
    {
        wi->overrun = 1;
        return;
    }

    for (i = 0; i != len; i++)
    {
        // ⚠ big endian, like %w and %W - a raw byte copy would be right
        // only on a big endian host, which is the bug this prevents.

        for (j = size; j-- != 0; )
        {
            b_emit(ptr[(i * size) + j]);
        }
    }
}

// -----------------------------------------------------------------------
// dispatch table

typedef void (*wi_fn_t)(void);

typedef struct
{
    int32_t  op;
    wi_fn_t  fn;
} wi_op_t;

// -----------------------------------------------------------------------
// %[n]  - a FORMAT STRING CALL.
//
// n indexes the table given to wi_set_formats().  the rest of the
// current string is pushed and parsing continues inside the called one;
// wi_parse()'s loop pops back when it runs off the end.
//
// ★★ A FORMAT MAY ONLY CALL A LOWER INDEX.  every call strictly
// decreases cur_fmt and the index cannot go below zero, so the chain
// always terminates - a cyclic table is not something that runs badly,
// it is something that cannot be written.  the top level string is not
// in the table and ranks above all of it.
//
// ⚠ every failure here is silent BY DESIGN - a bad index, a forward
// reference, no table, or nesting past WI_CALL_DEPTH skips the call and
// carries on.  a wire encoder that aborts mid message leaves a half
// written buffer, which is worse than a short one the length field
// already describes.

// -----------------------------------------------------------------------
// %:  - pop a repeat count for the NEXT %[n].
//
// ★ the count comes off the RPN stack rather than being a digit in the
// string, so it is not limited to 0-9 and can be computed:
//
//      %{3}%:%[0]        three times
//      %p1%:%[0]         as many as the caller passed
//
// ⚠ and zero works, because the count is known BEFORE the call rather
// than after - %[n] simply does not happen.  a count baked in at the end
// of a loop body could only ever give do-while.
//
// ⚠ it arms the NEXT %[n] in this parse, however far ahead that is -
// not only an immediately adjacent one - so a count can be computed,
// a header emitted, and then the call made.  every %[n] clears it,
// INCLUDING one that is refused, so a single %: arms exactly one call
// and a stray one cannot leak past it.

static void _colon(void)
{
    wi->pending = fs_pop();
}

// -----------------------------------------------------------------------

static void _call(void)
{
    int n = 0;
    int digits = 0;

    // ⚠ take the repeat count FIRST, before any of the early returns
    // below.  a call that is refused - bad index, forward reference,
    // stack full - must still consume it, or the count silently arms
    // whichever call comes next instead.

    int64_t count = wi->pending;

    wi->pending = 1;

    while ((*wi->f_str >= '0') && (*wi->f_str <= '9') && (digits < 4))
    {
        n = (n * 10) + (*wi->f_str++ - '0');
        digits++;
    }

    if (*wi->f_str == ']')
    {
        wi->f_str++;
    }

    if ((digits == 0) || (wi->fmts == NULL) ||
        (n < 0) || (n >= wi->nfmts) || (wi->fmts[n] == NULL))
    {
        return;
    }

    // ★ the rule.  strictly less - equal would be a self call.

    if (n >= wi->cur_fmt)
    {
        return;
    }

    if (wi->rsp >= WI_CALL_DEPTH)
    {
        wi->overrun = 1;        // ⚠ loud.  wi_set_formats() should have
        return;                 // caught this, so reaching it is a bug
    }

    if (count <= 0)
    {
        return;             // %{0}%:%[n] - the call does not happen
    }

    wi->rstack[wi->rsp] = wi->f_str;
    wi->rstart[wi->rsp] = (const uint8_t *)wi->fmts[n];
    wi->rleft[wi->rsp]  = count - 1;
    wi->rfmt[wi->rsp]   = wi->cur_fmt;
    wi->rsp++;

    wi->cur_fmt = n;
    wi->f_str   = (const uint8_t *)wi->fmts[n];
}

// -----------------------------------------------------------------------

static const wi_op_t ops[] =
{
    { '%', _percent }, { 'p', _p      }, { 'c', _c      },
    { 'b', _b       }, { 'w', _w      }, { 'W', _bW     }, { 'r', _r      },
    { 'B', _rB      }, { 'S', _rS     }, { 'L', _rL     },
    { 'x', _bx      }, { 'X', _bX     }, { 'f', _f      },
    { '&', _and     }, { 'A', _andl   }, { '|', _or     },
    { 'O', _orl     }, { '^', _xor    }, { '~', _not    },
    { '!', _notl    }, { '+', _plus   }, { '-', _minus   },
    { '*', _star    }, { '/', _div    }, { 'm', _mod     },
    { '=', _equals  }, { '>', _greater }, { '<', _less   },
    { 0x27, _tick   }, { '{', _brace  }, { 'P', _P      },
    { 'g', _g       }, { '?', NULL    }, { 't', _t      },
    { 'e', _e       }, { ';', NULL    }, { '[', _call   },
    { ':', _colon   },
};

#define OPS_COUNT  (sizeof(ops) / sizeof(ops[0]))

// -----------------------------------------------------------------------

static int wi_switch(int32_t op)
{
    size_t i;

    for (i = 0; i < OPS_COUNT; i++)
    {
        if (ops[i].op == op)
        {
            if (ops[i].fn)
                ops[i].fn();
            return 0;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------

static int next_c(void)
{
    wi->digits = 1;
    int c1 = *wi->f_str++;

    if ((c1 == '2') || (c1 == '3'))
    {
        wi->digits = c1 & 0x0f;
        c1 = *wi->f_str++;
    }

    return c1;
}

// -----------------------------------------------------------------------

void wi_init(wi_vars_t *v, uint8_t *buf, size_t bufsize,
             int64_t *params, int nparams)
{
    memset(v, 0, sizeof(*v));

    v->out      = buf;
    v->out_size = bufsize;

    if (params && nparams > 0)
    {
        if (nparams > WI_MAX_PARAMS)
            nparams = WI_MAX_PARAMS;
        memcpy(v->params, params, (size_t)nparams * sizeof(int64_t));
    }
}

void wi_decode_init(wi_vars_t *v, const uint8_t *in, size_t in_size,
                    int64_t *params, int nparams)
{
    memset(v, 0, sizeof(*v));

    v->in      = in;
    v->in_size = in_size;

    if (params && nparams > 0)
    {
        if (nparams > WI_MAX_PARAMS)
            nparams = WI_MAX_PARAMS;
        memcpy(v->params, params, (size_t)nparams * sizeof(int64_t));
    }
}

// -----------------------------------------------------------------------

size_t wi_parse(wi_vars_t *v, const char *fmt)
{
    wi = v;
    wi->f_str  = (const uint8_t *)fmt;
    wi->out_len = 0;
    wi->rsp     = 0;
    wi->pending = 1;
    wi->overrun = 0;

    // the top level string is not in the table, so it outranks all of it

    wi->cur_fmt = wi->nfmts;

    for (;;)
    {
        // ⚠ end of string is a RETURN, not the end of the parse, unless
        // there is nothing to return to.  this is the only reason %[n]
        // needs no matching terminator in the called format.

        if (wi->overrun)
        {
            break;
        }

        if (*wi->f_str == 0)
        {
            if (wi->rsp == 0)
            {
                break;
            }

            // ★ REPEAT BEFORE RETURN.  another iteration owed means
            // restarting the called format rather than popping - which
            // is the whole of the loop, on top of the call that was
            // already there.

            if (wi->rleft[wi->rsp - 1] > 0)
            {
                wi->rleft[wi->rsp - 1]--;
                wi->f_str = wi->rstart[wi->rsp - 1];
                continue;
            }

            wi->rsp--;
            wi->f_str   = wi->rstack[wi->rsp];
            wi->cur_fmt = wi->rfmt[wi->rsp];
            continue;
        }

        int c1 = *wi->f_str++;

        if (c1 == '%')
            wi_switch(next_c());
        else
            b_emit((uint8_t)c1);
    }

    return wi->out_len;
}

// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// find the next %[n] in a format string, or -1.  *pp is advanced.
//
// ⚠ it has to step OVER the other bracket-ish forms or it will find a
// call inside them: %'[' pushes a literal bracket, %{12} holds digits,
// and a %[n] already counted must not be seen twice.

static int next_call(const char **pp)
{
    const char *p = *pp;

    while (*p != '\0')
    {
        if (*p++ != '%') { continue; }

        if (*p == '\0') { break; }

        if (*p == '\'')                      // %'x' - literal char
        {
            p++;
            if (*p) { p++; }
            if (*p) { p++; }
            continue;
        }

        if (*p == '{')                       // %{123} - literal number
        {
            while (*p && (*p != '}')) { p++; }
            if (*p) { p++; }
            continue;
        }

        if (*p == '[')                       // the one we want
        {
            int n = 0, d = 0;

            p++;
            while ((*p >= '0') && (*p <= '9') && (d < 4))
            { n = (n * 10) + (*p++ - '0'); d++; }
            if (*p == ']') { p++; }

            *pp = p;
            return (d == 0) ? -1 : n;
        }

        p++;                                 // any other specifier
    }

    *pp = p;
    return -1;
}

// -----------------------------------------------------------------------
// ★ THE ACTUAL NESTING DEPTH OF A TABLE, NOT ITS WORST CASE.
//
// an N entry table CAN nest N deep, but almost none do - refusing a
// forty message protocol because it theoretically could is wrong.  the
// no forward reference rule makes the exact answer cheap: a format only
// ever calls a lower index, so the table is a DAG already sorted, and
// one pass upward from zero gives every depth with no recursion and no
// cycle check.
//
//     depth[k] = 1 + max(depth[j]) over every %[j] in format k

static int table_depth(const char **fmts, int nfmts, int *depths)
{
    int k, j, deepest = 0;
    const char *p;

    for (k = 0; k != nfmts; k++)
    {
        depths[k] = 1;

        if (fmts[k] == NULL) { continue; }

        p = fmts[k];

        for (;;)
        {
            j = next_call(&p);

            if (j < 0) { break; }

            // a forward or self reference never runs, so it never adds
            // depth - it is refused at parse time by the same rule

            if ((j >= k) || (j >= nfmts) || (fmts[j] == NULL)) { continue; }

            if ((depths[j] + 1) > depths[k]) { depths[k] = depths[j] + 1; }
        }

        if (depths[k] > deepest) { deepest = depths[k]; }
    }

    return deepest;
}

// -----------------------------------------------------------------------

int wi_set_formats(wi_vars_t *v, const char **fmts, int nfmts)
{
    int depths[WI_MAX_FORMATS];

    if ((nfmts < 0) || (nfmts > WI_MAX_FORMATS)) { return -1; }

    // ⚠ the top level string calls into the table, so the deepest chain
    // costs one frame more than the table's own depth.

    if ((fmts != NULL) && (nfmts > 0) &&
        ((table_depth(fmts, nfmts, depths) + 1) > WI_CALL_DEPTH))
    {
        return -1;
    }

    v->fmts  = fmts;
    v->nfmts = nfmts;

    return 0;
}

// =======================================================================
