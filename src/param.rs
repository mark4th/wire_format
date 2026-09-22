#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Param<'a> {
    Int(i64),
    Raw(&'a [u8]),
}

impl<'a> From<i64> for Param<'a> {
    fn from(value: i64) -> Self {
        Self::Int(value)
    }
}

impl<'a> From<u8> for Param<'a> {
    fn from(value: u8) -> Self {
        Self::Int(value as i64)
    }
}

impl<'a> From<u16> for Param<'a> {
    fn from(value: u16) -> Self {
        Self::Int(value as i64)
    }
}

impl<'a> From<u32> for Param<'a> {
    fn from(value: u32) -> Self {
        Self::Int(value as i64)
    }
}

impl<'a> From<usize> for Param<'a> {
    fn from(value: usize) -> Self {
        Self::Int(value as i64)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Value {
    Int(i64),
    Raw(usize),
}
