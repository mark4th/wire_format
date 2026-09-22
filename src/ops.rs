use crate::Error;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    AndLogical,
    Or,
    OrLogical,
    Xor,
    Eq,
    Gt,
    Lt,
}

impl BinaryOp {
    pub(crate) fn apply(self, a: i64, b: i64) -> i64 {
        match self {
            Self::Add => b.wrapping_add(a),
            Self::Sub => b.wrapping_sub(a),
            Self::Mul => b.wrapping_mul(a),
            Self::Div => {
                if a == 0 {
                    0
                } else {
                    b / a
                }
            }
            Self::Mod => {
                if a == 0 {
                    0
                } else {
                    b % a
                }
            }
            Self::And => b & a,
            Self::AndLogical => {
                if b != 0 && a != 0 {
                    1
                } else {
                    0
                }
            }
            Self::Or => b | a,
            Self::OrLogical => {
                if b != 0 || a != 0 {
                    1
                } else {
                    0
                }
            }
            Self::Xor => b ^ a,
            Self::Eq => {
                if b == a {
                    1
                } else {
                    0
                }
            }
            Self::Gt => {
                if b > a {
                    1
                } else {
                    0
                }
            }
            Self::Lt => {
                if b < a {
                    1
                } else {
                    0
                }
            }
        }
    }
}

pub(crate) fn checked_usize(value: i64) -> Result<usize, Error> {
    if value < 0 {
        return Err(Error::ValueOutOfRange);
    }
    Ok(value as usize)
}

pub(crate) fn var_index(name: u8) -> Result<usize, Error> {
    let lower = name | 0x20;
    if lower.is_ascii_lowercase() {
        Ok((lower - b'a') as usize)
    } else {
        Err(Error::InvalidVariable(name))
    }
}

pub(crate) fn bit_mask(pos: i64, width: i64) -> Result<(u8, u8), Error> {
    if pos < 0 || width <= 0 || pos > 7 || width > 8 || pos + width > 8 {
        return Err(Error::InvalidBitField);
    }
    let mask = if width == 8 {
        0xff
    } else {
        ((1u16 << width) - 1) as u8
    };
    Ok((pos as u8, mask))
}
