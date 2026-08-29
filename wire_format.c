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

static void b_emit(uint8_t byte)
{
    assert(wi->out_len < wi->out_size);
    wi->out[wi->out_len++] = byte;
}

static uint8_t b_read(void)
{
    assert(wi->in_pos < wi->in_size);
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
static void _r(void)
{
    size_t   len = (size_t)fs_pop();
    uint8_t *ptr = (uint8_t *)(uintptr_t)fs_pop();
    size_t   i;

    for (i = 0; i < len; i++)
        b_emit(ptr[i]);
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

static void _call(void)
{
    int n = 0;
    int digits = 0;

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
        return;
    }

    wi->rstack[wi->rsp] = wi->f_str;
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

    // the top level string is not in the table, so it outranks all of it

    wi->cur_fmt = wi->nfmts;

    for (;;)
    {
        // ⚠ end of string is a RETURN, not the end of the parse, unless
        // there is nothing to return to.  this is the only reason %[n]
        // needs no matching terminator in the called format.

        if (*wi->f_str == 0)
        {
            if (wi->rsp == 0)
            {
                break;
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

void wi_set_formats(wi_vars_t *v, const char **fmts, int nfmts)
{
    v->fmts  = fmts;
    v->nfmts = nfmts;
}

// =======================================================================
