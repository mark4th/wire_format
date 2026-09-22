// wire_format.h  - wire format info parser
// -----------------------------------------------------------------------

#ifndef WIRE_FORMAT_H
#define WIRE_FORMAT_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------

#define WI_STACK_DEPTH  16
#define WI_MAX_PARAMS   16
#define WI_MAX_VARS     26

// -----------------------------------------------------------------------

typedef struct
{
    const uint8_t *f_str;               // current position in format string

    int64_t  fstack[WI_STACK_DEPTH];    // RPN stack
    int      fsp;                       // stack pointer

    int64_t  params[WI_MAX_PARAMS];     // caller-supplied parameters
    int64_t  vars[WI_MAX_VARS];         // variables a-z; A-Z are aliases

    uint8_t       *out;                 // output buffer (encode)
    size_t         out_size;            // output buffer capacity
    size_t         out_len;             // bytes written so far

    const uint8_t *in;                  // input buffer (decode)
    size_t         in_size;             // input buffer capacity
    size_t         in_pos;              // bytes consumed so far

    uint8_t  bit_acc;                   // encode bit accumulator
    uint8_t  in_acc;                    // decode bit accumulator
    uint8_t  in_loaded;                 // decode: byte loaded into in_acc

    uint32_t faults;                    // fault bits raised during this parse
    uint32_t abort_mask;                // raised faults matching this mask abort parse
} wi_vars_t;

// -----------------------------------------------------------------------

// encode: set output buffer and parameters, then call wi_parse()
void    wi_init(wi_vars_t *v, uint8_t *buf, size_t bufsize,
                int64_t *params, int nparams);

// decode: set input buffer and parameters, then call wi_parse()
// in_pos advances with each call; multiple wi_parse() calls continue
// from where the previous left off.  Results land in v->vars[].
void    wi_decode_init(wi_vars_t *v, const uint8_t *in, size_t in_size,
                       int64_t *params, int nparams);

size_t  wi_parse(wi_vars_t *v, const char *fmt);

// -----------------------------------------------------------------------

#endif // WIRE_FORMAT_H

// =======================================================================
