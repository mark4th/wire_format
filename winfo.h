// winfo.h  - wire format info parser
// -----------------------------------------------------------------------

#ifndef WINFO_H
#define WINFO_H

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
    int64_t  atoz[WI_MAX_VARS];         // dynamic variables a-z
    int64_t  AtoZ[WI_MAX_VARS];         // dynamic variables A-Z

    uint8_t *out;                       // output buffer
    size_t   out_size;                  // output buffer capacity
    size_t   out_len;                   // bytes written so far

    int      digits;                    // for %2d / %3d
} wi_vars_t;

// -----------------------------------------------------------------------

void    wi_init(wi_vars_t *v, uint8_t *buf, size_t bufsize,
                int64_t *params, int nparams);
size_t  wi_parse(wi_vars_t *v, const char *fmt);

// -----------------------------------------------------------------------

#endif // WINFO_H

// =======================================================================
