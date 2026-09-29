#![no_std]

//! Format-string driven binary protocol encoder/decoder.
//!
//! The Rust port keeps the original fixed parser idea: message layouts live in
//! compact format strings, while a single interpreter handles encoding and
//! decoding. The implementation is allocation-free and suitable for embedded
//! use; examples may use `std` for printing.
//!
//! The C and Rust parsers implement the same format operations, including
//! nested format calls, counted calls, typed integer arrays, 64-bit values,
//! bit fields, conditionals, and caller-controlled faults. Rust represents raw
//! and typed arrays with [`Param`] slice variants and reports bounds failures
//! through [`Error`] rather than an overrun flag.

mod error;
mod format;
mod ops;
mod param;
mod parser;

pub use error::Error;
pub use param::Param;
pub use parser::{Record, RecordList, WireFormat};

/// Maximum depth of the RPN value stack.
pub const STACK_DEPTH: usize = 16;
/// Maximum number of caller parameters retained by one parser.
pub const MAX_PARAMS: usize = 16;
/// Number of variables addressed by `a` through `z`.
pub const MAX_VARS: usize = 26;
/// Maximum depth of nested `%[n]` format calls.
pub const CALL_DEPTH: usize = 8;
/// Maximum number of entries accepted by a format table.
pub const MAX_FORMATS: usize = 64;

#[cfg(test)]
extern crate std;

#[cfg(test)]
mod tests;
