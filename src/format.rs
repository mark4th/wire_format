use crate::Error;

pub(crate) fn read_fmt_byte(fmt: &[u8], pos: &mut usize) -> Result<u8, Error> {
    if *pos >= fmt.len() {
        return Err(Error::UnterminatedLiteral);
    }
    let byte = fmt[*pos];
    *pos += 1;
    Ok(byte)
}

pub(crate) fn next_op(fmt: &[u8], pos: &mut usize) -> Result<u8, Error> {
    read_fmt_byte(fmt, pos)
}

pub(crate) fn scan_to_else_or_end(fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
    while *pos < fmt.len() {
        let byte = fmt[*pos];
        *pos += 1;
        if byte == b'%' {
            let op = next_op(fmt, pos)?;
            if op == b'e' || op == b';' {
                return Ok(());
            }
        }
    }
    Err(Error::UnterminatedConditional)
}

pub(crate) fn scan_to_end(fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
    while *pos < fmt.len() {
        let byte = fmt[*pos];
        *pos += 1;
        if byte == b'%' {
            let op = next_op(fmt, pos)?;
            if op == b';' {
                return Ok(());
            }
        }
    }
    Err(Error::UnterminatedConditional)
}
