// wire_format.c  - wire format info parser
// -----------------------------------------------------------------------
// Derived from uCurses terminfo format string parser.
// RPN stack, arithmetic/logic/conditionals, binary byte emission.
//
// New specifiers vs terminfo:
//   %b  - emit 1 byte  (low byte of TOS)
//   %w  - emit 2 bytes big-endian (uint16)
//   %W  - emit 4 bytes big-endian (uint32)
// -----------------------------------------------------------------------

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "wire_format.h"

// -----------------------------------------------------------------------

static void b_emit(wi_vars_t *wi, uint8_t byte)
{
    assert(wi->out_len < wi->out_size);
    wi->out[wi->out_len++] = byte;
}

static uint8_t b_read(wi_vars_t *wi)
{
    assert(wi->in_pos < wi->in_size);
    return wi->in[wi->in_pos++];
}

// -----------------------------------------------------------------------
// RPN stack

static void fs_push(wi_vars_t *wi, int64_t n)
{
    assert(wi->fsp < WI_STACK_DEPTH);
    wi->fstack[wi->fsp++] = n;
}

static int64_t fs_pop(wi_vars_t *wi)
{
    assert(wi->fsp > 0);
    return wi->fstack[--wi->fsp];
}

// -----------------------------------------------------------------------
// variable access

static int64_t *get_var_addr(wi_vars_t *wi)
{
    uint8_t c1 = *wi->f_str++ | 0x20;
    return &wi->vars[c1 - 'a'];
}

// -----------------------------------------------------------------------
// scan to next % specifier (used by conditionals)

static char scan(wi_vars_t *wi)
{
    while (*wi->f_str++ != '%')
        ;
    return (char)*wi->f_str++;
}

// -----------------------------------------------------------------------
// specifier implementations
// -----------------------------------------------------------------------

static void _percent(wi_vars_t *wi) { b_emit(wi, '%'); }

static void _and  (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b &  a); }
static void _andl (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b && a); }
static void _or   (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b |  a); }
static void _orl  (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b || a); }
static void _xor  (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b ^  a); }
static void _not  (wi_vars_t *wi) { fs_push(wi, ~fs_pop(wi)); }
static void _notl (wi_vars_t *wi) { fs_push(wi, !fs_pop(wi)); }
static void _plus (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b + a); }
static void _minus(wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b - a); }
static void _star (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b * a); }
static void _div  (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, a ? b / a : 0); }
static void _mod  (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, a ? b % a : 0); }

static void _equals (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b == a); }
static void _greater(wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b >  a); }
static void _less   (wi_vars_t *wi) { int64_t a = fs_pop(wi), b = fs_pop(wi); fs_push(wi, b <  a); }

// -----------------------------------------------------------------------
// %'x'  push literal character value

static void _tick(wi_vars_t *wi)
{
    fs_push(wi, (char)*wi->f_str);
    wi->f_str += 2;             // skip char and closing '
}

// -----------------------------------------------------------------------
// %{123}  push decimal literal

static void _brace(wi_vars_t *wi)
{
    int64_t n = 0;
    char c1;

    while ((c1 = (char)*wi->f_str++) != '}')
    {
        n *= 10;
        n += c1 - '0';
    }

    fs_push(wi, n);
}

// -----------------------------------------------------------------------
// %p1..%p9  push parameter

static void _p(wi_vars_t *wi)
{
    uint8_t c1 = *wi->f_str++ & 0x0f;
    fs_push(wi, wi->params[c1 - 1]);
}

// -----------------------------------------------------------------------
// %Px / %gx  store/load named variable

static void _P(wi_vars_t *wi) { *get_var_addr(wi) = fs_pop(wi); }
static void _g(wi_vars_t *wi) { fs_push(wi, *get_var_addr(wi)); }

// %E  raise fault bit(s); abort policy is caller supplied via abort_mask
static void _E(wi_vars_t *wi)
{
    uint32_t fault = (uint32_t)fs_pop(wi);
    wi->faults |= fault;
}

// -----------------------------------------------------------------------
// %?..%t..%e..%;  conditional

static void _t(wi_vars_t *wi)
{
    char c1;

    if (fs_pop(wi) != 0)
        return;

    for (;;)
    {
        c1 = scan(wi);
        if ((c1 == 'e') || (c1 == ';'))
            break;
    }
}

static void _e(wi_vars_t *wi)
{
    char c1;
    do { c1 = scan(wi); } while (c1 != ';');
}

// -----------------------------------------------------------------------
// emit specifiers

// %c  emit low byte of TOS (terminfo compatible)
static void _c(wi_vars_t *wi) { b_emit(wi, (uint8_t)fs_pop(wi)); }

// %b  emit 1 byte (alias for %c, explicit binary intent)
static void _b(wi_vars_t *wi) { b_emit(wi, (uint8_t)fs_pop(wi)); }

// %w  emit 2 bytes big-endian
static void _w(wi_vars_t *wi)
{
    uint16_t v = (uint16_t)fs_pop(wi);
    b_emit(wi, (uint8_t)(v >> 8));
    b_emit(wi, (uint8_t)(v & 0xff));
}

// %W  emit 4 bytes big-endian
static void _bW(wi_vars_t *wi)
{
    uint32_t v = (uint32_t)fs_pop(wi);
    b_emit(wi, (uint8_t)(v >> 24));
    b_emit(wi, (uint8_t)(v >> 16));
    b_emit(wi, (uint8_t)(v >>  8));
    b_emit(wi, (uint8_t)(v & 0xff));
}

// %B  read 1 byte from input → push
static void _rB(wi_vars_t *wi) { fs_push(wi, b_read(wi)); }

// %S  read 2 bytes big-endian → push as uint16
static void _rS(wi_vars_t *wi)
{
    uint16_t v = (uint16_t)b_read(wi) << 8;
    v |= b_read(wi);
    fs_push(wi, v);
}

// %L  read 4 bytes big-endian → push as uint32
static void _rL(wi_vars_t *wi)
{
    uint32_t v = (uint32_t)b_read(wi) << 24;
    v |= (uint32_t)b_read(wi) << 16;
    v |= (uint32_t)b_read(wi) << 8;
    v |= b_read(wi);
    fs_push(wi, v);
}

// %x  encode bit field: pop position, width, value → bit_acc |= (value & mask) << position
static void _bx(wi_vars_t *wi)
{
    int      pos   = (int)fs_pop(wi);
    int      width = (int)fs_pop(wi);
    uint8_t  val   = (uint8_t)fs_pop(wi);
    uint8_t  mask  = (uint8_t)((1u << width) - 1u);

    wi->bit_acc |= (val & mask) << pos;
}

// %X  decode bit field: pop position, width → extract from in_acc → push
static void _bX(wi_vars_t *wi)
{
    int     pos   = (int)fs_pop(wi);
    int     width = (int)fs_pop(wi);
    uint8_t mask  = (uint8_t)((1u << width) - 1u);

    if (!wi->in_loaded)
    {
        wi->in_acc    = b_read(wi);
        wi->in_loaded = 1;
    }

    fs_push(wi, (wi->in_acc >> pos) & mask);
}

// %f  flush: encode emits bit_acc and resets; decode discards current in_acc byte
static void _f(wi_vars_t *wi)
{
    if (wi->out)
    {
        b_emit(wi, wi->bit_acc);
        wi->bit_acc = 0;
    }
    else
    {
        wi->in_loaded = 0;
    }
}

// %r  emit raw buffer: TOS = length, next = pointer
static void _r(wi_vars_t *wi)
{
    size_t   len = (size_t)fs_pop(wi);
    uint8_t *ptr = (uint8_t *)(uintptr_t)fs_pop(wi);
    size_t   i;

    for (i = 0; i < len; i++)
        b_emit(wi, ptr[i]);
}

// -----------------------------------------------------------------------
// dispatch table

typedef void (*wi_fn_t)(wi_vars_t *wi);

typedef struct
{
    int32_t  op;
    wi_fn_t  fn;
} wi_op_t;

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
    { 'g', _g       }, { 'E', _E      }, { '?', NULL    }, { 't', _t      },
    { 'e', _e       }, { ';', NULL    },
};

#define OPS_COUNT  (sizeof(ops) / sizeof(ops[0]))

// -----------------------------------------------------------------------

static int wi_switch(wi_vars_t *wi, int32_t op)
{
    size_t i;

    for (i = 0; i < OPS_COUNT; i++)
    {
        if (ops[i].op == op)
        {
            if (ops[i].fn)
                ops[i].fn(wi);
            return 0;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------

static int next_c(wi_vars_t *wi)
{
    return *wi->f_str++;
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
    v->f_str   = (const uint8_t *)fmt;
    v->out_len = 0;
    v->faults  = 0;

    while (*v->f_str && ((v->faults & v->abort_mask) == 0))
    {
        int c1 = *v->f_str++;

        if (c1 == '%')
            wi_switch(v, next_c(v));
        else
            b_emit(v, (uint8_t)c1);
    }

    return v->out_len;
}

// =======================================================================
