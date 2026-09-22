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

pub(crate) fn read_call_index(fmt: &[u8], pos: &mut usize) -> Option<usize> {
    let mut index = 0usize;
    let mut digits = 0usize;

    while *pos < fmt.len() && fmt[*pos].is_ascii_digit() && digits < 4 {
        index = index
            .saturating_mul(10)
            .saturating_add((fmt[*pos] - b'0') as usize);
        *pos += 1;
        digits += 1;
    }

    if *pos < fmt.len() && fmt[*pos] == b']' {
        *pos += 1;
    }

    if digits == 0 {
        None
    } else {
        Some(index)
    }
}

pub(crate) fn next_call(fmt: &[u8], pos: &mut usize) -> Option<usize> {
    while *pos < fmt.len() {
        let byte = fmt[*pos];
        *pos += 1;

        if byte != b'%' || *pos >= fmt.len() {
            continue;
        }

        let op = fmt[*pos];
        *pos += 1;

        if op == b'\'' {
            *pos = (*pos + 2).min(fmt.len());
            continue;
        }

        if op == b'{' {
            while *pos < fmt.len() && fmt[*pos] != b'}' {
                *pos += 1;
            }
            if *pos < fmt.len() {
                *pos += 1;
            }
            continue;
        }

        if op == b'[' {
            return read_call_index(fmt, pos);
        }
    }

    None
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
