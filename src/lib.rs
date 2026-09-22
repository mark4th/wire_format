#![no_std]

//! Format-string driven binary protocol encoder/decoder.
//!
//! The Rust port keeps the original fixed parser idea: message layouts live in
//! compact format strings, while a single interpreter handles encoding and
//! decoding. The implementation is allocation-free and suitable for embedded
//! use; examples may use `std` for printing.

mod error;
mod format;
mod ops;
mod param;
mod parser;

pub use error::Error;
pub use param::Param;
pub use parser::WireFormat;

pub const STACK_DEPTH: usize = 16;
pub const MAX_PARAMS: usize = 16;
pub const MAX_VARS: usize = 26;

#[cfg(test)]
extern crate std;

#[cfg(test)]
mod tests;
