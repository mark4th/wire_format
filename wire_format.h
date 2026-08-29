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

// ⚠ HOW DEEP %[n] MAY NEST.  each frame is a return address, the
// caller's index, the called format's start and a loop counter - 28
// bytes, so eight frames is 224.
//
// ★ this is NOT the table size.  an N entry table CAN nest N deep, since
// every call strictly decreases the index - but almost none do, and
// refusing a forty message protocol because it theoretically could is
// wrong.  wi_set_formats() computes the table's ACTUAL depth in one pass
// (the ordering rule makes it a pre-sorted DAG) and refuses only a table
// that really would overflow this.

// ⚠ OVERRIDE THIS FOR YOUR PROTOCOL: -DWI_CALL_DEPTH=n.  the right value
// is how deep YOUR message table actually nests, which wi_set_formats()
// measures and enforces - not a number this header can know.

#ifndef WI_CALL_DEPTH
#define WI_CALL_DEPTH   8
#endif

// the largest format table wi_set_formats() will look at.  it bounds a
// scratch array there, not the protocol - a table this big may still
// only nest two or three deep, and that is what is actually checked.

#ifndef WI_MAX_FORMATS
#define WI_MAX_FORMATS  64
#endif

// -----------------------------------------------------------------------

typedef struct
{
    const uint8_t *f_str;               // current position in format string

    int64_t  fstack[WI_STACK_DEPTH];    // RPN stack
    int      fsp;                       // stack pointer

    int64_t  params[WI_MAX_PARAMS];     // caller-supplied parameters
    int64_t  atoz[WI_MAX_VARS];         // dynamic variables a-z
    int64_t  AtoZ[WI_MAX_VARS];         // dynamic variables A-Z

    uint8_t       *out;                 // output buffer (encode)
    size_t         out_size;            // output buffer capacity
    size_t         out_len;             // bytes written so far

    const uint8_t *in;                  // input buffer (decode)
    size_t         in_size;             // input buffer capacity
    size_t         in_pos;              // bytes consumed so far

    int      digits;                    // for %2d / %3d

    uint8_t  bit_acc;                   // encode bit accumulator
    uint8_t  in_acc;                    // decode bit accumulator
    uint8_t  in_loaded;                 // decode: byte loaded into in_acc

    // %[n] - a format string CALL.  fmts is the caller's table of
    // formats; rstack holds where to resume when a called format ends.

    const char **fmts;                  // table indexed by %[n]
    int          nfmts;
    int          cur_fmt;               // index of the format executing
    int64_t      pending;               // %: repeat count for the next %[n]

    const uint8_t *rstack[WI_CALL_DEPTH];   // where to resume
    const uint8_t *rstart[WI_CALL_DEPTH];   // start of the called format
    int64_t      rleft[WI_CALL_DEPTH];      // repeats still owed
    int          rfmt[WI_CALL_DEPTH];       // cur_fmt to restore
    int          rsp;

    // ⚠ SET WHEN A READ OR AN EMIT RAN OUT OF BUFFER.  it stops the
    // parse.  it was an assert() in both places, which aborts the
    // process with asserts on and walks off the end with -DNDEBUG -
    // neither being an option for a parser fed by a network.

    int          overrun;
} wi_vars_t;

// -----------------------------------------------------------------------

// encode: set output buffer and parameters, then call wi_parse()
void    wi_init(wi_vars_t *v, uint8_t *buf, size_t bufsize,
                int64_t *params, int nparams);

// decode: set input buffer and parameters, then call wi_parse()
// in_pos advances with each call; multiple wi_parse() calls continue
// from where the previous left off.  Results land in v->atoz[].
void    wi_decode_init(wi_vars_t *v, const uint8_t *in, size_t in_size,
                       int64_t *params, int nparams);

// %[n] embeds format n of this table into the format being parsed - a
// subroutine call, so a message that contains another message is written
// once and referenced rather than copied.
//
// ⚠ CALL THIS AFTER wi_init() / wi_decode_init().  both memset the whole
// struct, so setting the table first silently loses it.
//
// ★★ NO FORWARD REFERENCES.  format n may only call formats with an
// index STRICTLY LESS than n.  every call therefore decreases the index,
// the index is bounded below by zero, and a cycle becomes impossible to
// WRITE rather than merely bounded when it runs - which is the whole
// difference between a table that cannot be wrong and one that fails
// quietly.  a self call %[n] from inside format n is a cycle of length
// one and is refused by the same rule.
//
// the string handed to wi_parse() is not in the table and ranks above
// all of it, so a top level message may call anything.
//
// ⚠ the called format shares the caller's params and its a-z variables.
// that is deliberate for encode - a sub-format reads the same parameter
// array - but on DECODE a callee that stores into %Pa overwrites the
// caller's a.  give parent and child disjoint letters, or read the
// child's values out before calling again.

// returns -1 if the table's ACTUAL nesting depth would overflow
// WI_CALL_DEPTH, or if nfmts exceeds WI_MAX_FORMATS.  depth is measured,
// not assumed, so a large flat table is fine.

int     wi_set_formats(wi_vars_t *v, const char **fmts, int nfmts);

// ⚠ CHECK v->overrun AFTER PARSING.  the return value is the length
// produced, which for a truncated encode is a short but plausible
// number; overrun is the only thing that says it is short because the
// buffer ran out.

size_t  wi_parse(wi_vars_t *v, const char *fmt);

// -----------------------------------------------------------------------

#endif // WIRE_FORMAT_H

// =======================================================================
