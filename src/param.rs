/// One caller-supplied format parameter.
///
/// Integer parameters work with the ordinary RPN operators. Slice parameters
/// retain their element type so `%r`, `%r2`, and `%r4` cannot silently apply
/// the wrong element width.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Param<'a> {
    /// Integer parameter carried on the RPN stack.
    Int(i64),
    /// Byte slice used by `%r` and `%r1`.
    Raw(&'a [u8]),
    /// Native `u16` values emitted in big-endian order by `%r2`.
    U16s(&'a [u16]),
    /// Native `u32` values emitted in big-endian order by `%r4`.
    U32s(&'a [u32]),
}

impl From<i64> for Param<'_> {
    fn from(value: i64) -> Self {
        Self::Int(value)
    }
}

impl From<u8> for Param<'_> {
    fn from(value: u8) -> Self {
        Self::Int(value as i64)
    }
}

impl From<u16> for Param<'_> {
    fn from(value: u16) -> Self {
        Self::Int(value as i64)
    }
}

impl From<u32> for Param<'_> {
    fn from(value: u32) -> Self {
        Self::Int(value as i64)
    }
}

impl From<u64> for Param<'_> {
    fn from(value: u64) -> Self {
        Self::Int(value as i64)
    }
}

impl From<usize> for Param<'_> {
    fn from(value: usize) -> Self {
        Self::Int(value as i64)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Value {
    Int(i64),
    Raw(usize),
    U16s(usize),
    U32s(usize),
}
