use crate::format::{
    next_call, next_op, read_call_index, read_fmt_byte, scan_to_else_or_end, scan_to_end,
};
use crate::ops::{bit_mask, checked_usize, var_index, BinaryOp};
use crate::param::{Param, Value};
use crate::{Error, CALL_DEPTH, MAX_FORMATS, MAX_PARAMS, MAX_VARS, STACK_DEPTH};

fn table_depth(formats: &[&str]) -> usize {
    let mut depths = [0usize; MAX_FORMATS];
    let mut deepest = 0usize;
    let mut format_index = 0usize;

    while format_index < formats.len() {
        let mut depth = 1usize;
        let mut pos = 0usize;

        while let Some(called_index) = next_call(formats[format_index].as_bytes(), &mut pos) {
            if called_index < format_index {
                depth = depth.max(depths[called_index] + 1);
            }
        }

        depths[format_index] = depth;
        deepest = deepest.max(depth);
        format_index += 1;
    }

    deepest
}

pub struct WireFormat<'a, 'out> {
    stack: [Value; STACK_DEPTH],
    sp: usize,
    params: [Value; MAX_PARAMS],
    raw: [Option<&'a [u8]>; MAX_PARAMS],
    u16s: [Option<&'a [u16]>; MAX_PARAMS],
    u32s: [Option<&'a [u32]>; MAX_PARAMS],
    vars: [Value; MAX_VARS],
    formats: Option<&'a [&'a str]>,
    out: Option<&'out mut [u8]>,
    out_len: usize,
    input: Option<&'a [u8]>,
    in_pos: usize,
    bit_acc: u8,
    in_acc: u8,
    in_loaded: bool,
    faults: u32,
    abort_mask: u32,
}

impl<'a, 'out> WireFormat<'a, 'out> {
    #[inline(never)]
    fn empty() -> Self {
        Self {
            stack: [Value::Int(0); STACK_DEPTH],
            sp: 0,
            params: [Value::Int(0); MAX_PARAMS],
            raw: [None; MAX_PARAMS],
            u16s: [None; MAX_PARAMS],
            u32s: [None; MAX_PARAMS],
            vars: [Value::Int(0); MAX_VARS],
            formats: None,
            out: None,
            out_len: 0,
            input: None,
            in_pos: 0,
            bit_acc: 0,
            in_acc: 0,
            in_loaded: false,
            faults: 0,
            abort_mask: 0,
        }
    }

    fn load_params(&mut self, params: &[Param<'a>]) {
        let mut index = 0;
        while index < params.len() && index < MAX_PARAMS {
            match params[index] {
                Param::Int(value) => self.params[index] = Value::Int(value),
                Param::Raw(bytes) => {
                    self.raw[index] = Some(bytes);
                    self.params[index] = Value::Raw(index);
                }
                Param::U16s(values) => {
                    self.u16s[index] = Some(values);
                    self.params[index] = Value::U16s(index);
                }
                Param::U32s(values) => {
                    self.u32s[index] = Some(values);
                    self.params[index] = Value::U32s(index);
                }
            }
            index += 1;
        }
    }

    #[inline(never)]
    fn with_params(params: &[Param<'a>]) -> Self {
        let mut parser = Self::empty();
        parser.load_params(params);
        parser
    }

    fn push(&mut self, value: Value) -> Result<(), Error> {
        if self.sp >= STACK_DEPTH {
            return Err(Error::StackOverflow);
        }
        self.stack[self.sp] = value;
        self.sp += 1;
        Ok(())
    }

    fn pop_value(&mut self) -> Result<Value, Error> {
        if self.sp == 0 {
            return Err(Error::StackUnderflow);
        }
        self.sp -= 1;
        Ok(self.stack[self.sp])
    }

    fn pop_int(&mut self) -> Result<i64, Error> {
        match self.pop_value()? {
            Value::Int(value) => Ok(value),
            Value::Raw(_) | Value::U16s(_) | Value::U32s(_) => Err(Error::TypeMismatch),
        }
    }

    fn emit(&mut self, byte: u8) -> Result<(), Error> {
        let out = self.out.as_deref_mut().ok_or(Error::OutputUnavailable)?;
        if self.out_len >= out.len() {
            return Err(Error::OutputFull);
        }
        out[self.out_len] = byte;
        self.out_len += 1;
        Ok(())
    }

    fn read_byte(&mut self) -> Result<u8, Error> {
        let input = self.input.ok_or(Error::InputUnavailable)?;
        if self.in_pos >= input.len() {
            return Err(Error::InputEof);
        }
        let byte = input[self.in_pos];
        self.in_pos += 1;
        Ok(byte)
    }

    fn emit_be_u16(&mut self, value: u16) -> Result<(), Error> {
        self.emit((value >> 8) as u8)?;
        self.emit(value as u8)
    }

    fn emit_be_u32(&mut self, value: u32) -> Result<(), Error> {
        self.emit((value >> 24) as u8)?;
        self.emit((value >> 16) as u8)?;
        self.emit((value >> 8) as u8)?;
        self.emit(value as u8)
    }

    fn emit_be_u64(&mut self, value: u64) -> Result<(), Error> {
        let mut shift = 56;

        loop {
            self.emit((value >> shift) as u8)?;
            if shift == 0 {
                return Ok(());
            }
            shift -= 8;
        }
    }

    fn read_be_u16(&mut self) -> Result<u16, Error> {
        let hi = self.read_byte()? as u16;
        let lo = self.read_byte()? as u16;
        Ok((hi << 8) | lo)
    }

    fn read_be_u32(&mut self) -> Result<u32, Error> {
        let b0 = self.read_byte()? as u32;
        let b1 = self.read_byte()? as u32;
        let b2 = self.read_byte()? as u32;
        let b3 = self.read_byte()? as u32;
        Ok((b0 << 24) | (b1 << 16) | (b2 << 8) | b3)
    }

    fn read_be_u64(&mut self) -> Result<u64, Error> {
        let mut value = 0u64;
        let mut count = 0;

        while count < 8 {
            value = (value << 8) | self.read_byte()? as u64;
            count += 1;
        }

        Ok(value)
    }

    fn set_var(&mut self, name: u8, value: Value) -> Result<(), Error> {
        let index = var_index(name)?;
        self.vars[index] = value;
        Ok(())
    }

    fn var_value(&self, name: u8) -> Result<Value, Error> {
        let index = var_index(name)?;
        Ok(self.vars[index])
    }

    fn push_param(&mut self, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        let first = read_fmt_byte(fmt, pos)?;
        let index = if first == b'{' {
            let mut value = 0usize;
            let mut saw_digit = false;
            loop {
                let byte = read_fmt_byte(fmt, pos)?;
                if byte == b'}' {
                    break;
                }
                if !byte.is_ascii_digit() {
                    return Err(Error::InvalidParam);
                }
                saw_digit = true;
                value = value
                    .checked_mul(10)
                    .and_then(|number| number.checked_add((byte - b'0') as usize))
                    .ok_or(Error::InvalidParam)?;
                if value > MAX_PARAMS {
                    return Err(Error::InvalidParam);
                }
            }
            if !saw_digit {
                return Err(Error::InvalidParam);
            }
            value
        } else if (b'1'..=b'9').contains(&first) {
            (first - b'0') as usize
        } else {
            return Err(Error::InvalidParam);
        };

        if index == 0 || index > MAX_PARAMS {
            return Err(Error::InvalidParam);
        }
        self.push(self.params[index - 1])
    }

    fn push_char_literal(&mut self, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        let value = read_fmt_byte(fmt, pos)?;
        let close = read_fmt_byte(fmt, pos)?;
        if close != b'\'' {
            return Err(Error::UnterminatedLiteral);
        }
        self.push(Value::Int(value as i64))
    }

    fn push_decimal_literal(&mut self, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        let mut value = 0i64;
        let mut saw_digit = false;
        loop {
            let byte = read_fmt_byte(fmt, pos)?;
            if byte == b'}' {
                if !saw_digit {
                    return Err(Error::InvalidLiteral);
                }
                return self.push(Value::Int(value));
            }
            if !byte.is_ascii_digit() {
                return Err(Error::InvalidLiteral);
            }
            saw_digit = true;
            value = value
                .checked_mul(10)
                .and_then(|v| v.checked_add((byte - b'0') as i64))
                .ok_or(Error::LiteralOverflow)?;
        }
    }

    fn store_var(&mut self, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        let name = read_fmt_byte(fmt, pos)?;
        let value = self.pop_value()?;
        self.set_var(name, value)
    }

    fn load_var(&mut self, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        let name = read_fmt_byte(fmt, pos)?;
        self.push(self.var_value(name)?)
    }

    fn binary(&mut self, op: BinaryOp) -> Result<(), Error> {
        let a = self.pop_int()?;
        let b = self.pop_int()?;
        self.push(Value::Int(op.apply(a, b)))
    }

    fn emit_array(&mut self, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        let mut element_size = 1u8;

        if *pos < fmt.len() && fmt[*pos].is_ascii_digit() {
            element_size = fmt[*pos] - b'0';
            *pos += 1;
            if !matches!(element_size, 1 | 2 | 4) {
                return Err(Error::InvalidArrayElementSize(element_size));
            }
        }

        let raw_len = self.pop_int()?;
        let len = checked_usize(raw_len)?;
        let source = self.pop_value()?;

        match (element_size, source) {
            (1, Value::Raw(index)) => {
                let values = self
                    .raw
                    .get(index)
                    .and_then(|entry| *entry)
                    .ok_or(Error::RawUnavailable)?;
                if len > values.len() {
                    return Err(Error::RawLength);
                }
                let mut index = 0;
                while index < len {
                    self.emit(values[index])?;
                    index += 1;
                }
            }
            (2, Value::U16s(index)) => {
                let values = self
                    .u16s
                    .get(index)
                    .and_then(|entry| *entry)
                    .ok_or(Error::RawUnavailable)?;
                if len > values.len() {
                    return Err(Error::RawLength);
                }
                let mut index = 0;
                while index < len {
                    self.emit_be_u16(values[index])?;
                    index += 1;
                }
            }
            (4, Value::U32s(index)) => {
                let values = self
                    .u32s
                    .get(index)
                    .and_then(|entry| *entry)
                    .ok_or(Error::RawUnavailable)?;
                if len > values.len() {
                    return Err(Error::RawLength);
                }
                let mut index = 0;
                while index < len {
                    self.emit_be_u32(values[index])?;
                    index += 1;
                }
            }
            _ => return Err(Error::TypeMismatch),
        }

        Ok(())
    }

    fn encode_bit_field(&mut self) -> Result<(), Error> {
        let pos = self.pop_int()?;
        let width = self.pop_int()?;
        let value = self.pop_int()?;
        let (pos, mask) = bit_mask(pos, width)?;
        self.bit_acc |= ((value as u8) & mask) << pos;
        Ok(())
    }

    fn decode_bit_field(&mut self) -> Result<(), Error> {
        let pos = self.pop_int()?;
        let width = self.pop_int()?;
        let (pos, mask) = bit_mask(pos, width)?;
        if !self.in_loaded {
            self.in_acc = self.read_byte()?;
            self.in_loaded = true;
        }
        self.push(Value::Int(((self.in_acc >> pos) & mask) as i64))
    }

    fn flush_bits(&mut self) -> Result<(), Error> {
        if self.out.is_some() {
            let byte = self.bit_acc;
            self.bit_acc = 0;
            self.emit(byte)
        } else {
            self.in_loaded = false;
            Ok(())
        }
    }

    fn raise_fault(&mut self) -> Result<(), Error> {
        let raw = self.pop_int()?;
        if raw < 0 || raw > u32::MAX as i64 {
            return Err(Error::ValueOutOfRange);
        }
        let fault = raw as u32;
        self.faults |= fault;
        if (fault & self.abort_mask) != 0 {
            Err(Error::FaultAbort(fault))
        } else {
            Ok(())
        }
    }

    fn apply_op(&mut self, op: u8, fmt: &[u8], pos: &mut usize) -> Result<(), Error> {
        match op {
            b'%' => self.emit(b'%'),
            b'p' => self.push_param(fmt, pos),
            b'c' | b'b' => {
                let value = self.pop_int()?;
                self.emit(value as u8)
            }
            b'w' => {
                let raw = self.pop_int()?;
                self.emit_be_u16(raw as u16)
            }
            b'W' => {
                let raw = self.pop_int()?;
                self.emit_be_u32(raw as u32)
            }
            b'q' => {
                let raw = self.pop_int()?;
                self.emit_be_u64(raw as u64)
            }
            b'r' => self.emit_array(fmt, pos),
            b'B' => {
                let value = self.read_byte()?;
                self.push(Value::Int(value as i64))
            }
            b'S' => {
                let value = self.read_be_u16()?;
                self.push(Value::Int(value as i64))
            }
            b'L' => {
                let value = self.read_be_u32()?;
                self.push(Value::Int(value as i64))
            }
            b'Q' => {
                let value = self.read_be_u64()?;
                self.push(Value::Int(value as i64))
            }
            b'x' => self.encode_bit_field(),
            b'X' => self.decode_bit_field(),
            b'f' => self.flush_bits(),
            b'E' => self.raise_fault(),
            b'&' => self.binary(BinaryOp::And),
            b'A' => self.binary(BinaryOp::AndLogical),
            b'|' => self.binary(BinaryOp::Or),
            b'O' => self.binary(BinaryOp::OrLogical),
            b'^' => self.binary(BinaryOp::Xor),
            b'~' => {
                let value = self.pop_int()?;
                self.push(Value::Int(!value))
            }
            b'!' => {
                let value = self.pop_int()?;
                self.push(Value::Int(if value == 0 { 1 } else { 0 }))
            }
            b'+' => self.binary(BinaryOp::Add),
            b'-' => self.binary(BinaryOp::Sub),
            b'*' => self.binary(BinaryOp::Mul),
            b'/' => self.binary(BinaryOp::Div),
            b'm' => self.binary(BinaryOp::Mod),
            b'=' => self.binary(BinaryOp::Eq),
            b'>' => self.binary(BinaryOp::Gt),
            b'<' => self.binary(BinaryOp::Lt),
            b'\'' => self.push_char_literal(fmt, pos),
            b'{' => self.push_decimal_literal(fmt, pos),
            b'P' => self.store_var(fmt, pos),
            b'g' => self.load_var(fmt, pos),
            b'?' | b';' => Ok(()),
            b't' => {
                if self.pop_int()? != 0 {
                    Ok(())
                } else {
                    scan_to_else_or_end(fmt, pos)
                }
            }
            b'e' => scan_to_end(fmt, pos),
            _ => Err(Error::InvalidOperator(op)),
        }
    }

    pub fn new_encode(out: &'out mut [u8], params: &[Param<'a>]) -> Self {
        let mut parser = Self::with_params(params);
        parser.out = Some(out);
        parser
    }

    /// Sets the format table addressed by `%[n]`.
    ///
    /// A table entry may call only a lower-numbered entry. This makes cycles
    /// impossible and permits the actual maximum nesting depth to be checked
    /// here without recursion or allocation.
    pub fn set_formats(&mut self, formats: &'a [&'a str]) -> Result<(), Error> {
        if formats.len() > MAX_FORMATS {
            return Err(Error::TooManyFormats);
        }
        if !formats.is_empty() && table_depth(formats) + 1 > CALL_DEPTH {
            return Err(Error::FormatCallDepth);
        }
        self.formats = Some(formats);
        Ok(())
    }

    pub fn parse(&mut self, fmt: &str) -> Result<usize, Error> {
        self.out_len = 0;
        self.faults = 0;
        let formats = self.formats.unwrap_or(&[]);
        let mut current = fmt.as_bytes();
        let mut current_index = formats.len();
        let mut pos = 0usize;
        let mut pending = 1i64;
        let mut return_formats: [Option<&[u8]>; CALL_DEPTH] = [None; CALL_DEPTH];
        let mut return_positions = [0usize; CALL_DEPTH];
        let mut return_indices = [0usize; CALL_DEPTH];
        let mut start_formats: [Option<&[u8]>; CALL_DEPTH] = [None; CALL_DEPTH];
        let mut repeats_left = [0i64; CALL_DEPTH];
        let mut rsp = 0usize;

        loop {
            if pos >= current.len() {
                if rsp == 0 {
                    break;
                }

                if repeats_left[rsp - 1] > 0 {
                    repeats_left[rsp - 1] -= 1;
                    current = start_formats[rsp - 1].ok_or(Error::FormatCallDepth)?;
                    pos = 0;
                    continue;
                }

                rsp -= 1;
                current = return_formats[rsp].ok_or(Error::FormatCallDepth)?;
                pos = return_positions[rsp];
                current_index = return_indices[rsp];
                continue;
            }

            let byte = current[pos];
            pos += 1;
            if byte == b'%' {
                let op = next_op(current, &mut pos)?;

                if op == b':' {
                    pending = self.pop_int()?;
                    continue;
                }

                if op == b'[' {
                    let count = pending;
                    pending = 1;
                    let called_index = match read_call_index(current, &mut pos) {
                        Some(index) => index,
                        None => continue,
                    };
                    let called = match formats.get(called_index) {
                        Some(format) => format.as_bytes(),
                        None => continue,
                    };

                    if called_index >= current_index || count <= 0 {
                        continue;
                    }
                    if rsp >= CALL_DEPTH {
                        return Err(Error::FormatCallDepth);
                    }

                    return_formats[rsp] = Some(current);
                    return_positions[rsp] = pos;
                    return_indices[rsp] = current_index;
                    start_formats[rsp] = Some(called);
                    repeats_left[rsp] = count - 1;
                    rsp += 1;

                    current = called;
                    current_index = called_index;
                    pos = 0;
                    continue;
                }

                self.apply_op(op, current, &mut pos)?;
            } else {
                self.emit(byte)?;
            }
        }
        Ok(self.out_len)
    }

    pub fn set_abort_mask(&mut self, mask: u32) {
        self.abort_mask = mask;
    }

    pub fn abort_mask(&self) -> u32 {
        self.abort_mask
    }

    pub fn faults(&self) -> u32 {
        self.faults
    }

    pub fn output_len(&self) -> usize {
        self.out_len
    }

    pub fn output(&self) -> &[u8] {
        match &self.out {
            Some(out) => &out[..self.out_len],
            None => &[],
        }
    }

    pub fn input_pos(&self) -> usize {
        self.in_pos
    }

    pub fn int_var(&self, name: u8) -> Option<i64> {
        match self.var_value(name).ok()? {
            Value::Int(value) => Some(value),
            Value::Raw(_) | Value::U16s(_) | Value::U32s(_) => None,
        }
    }
}

impl<'a> WireFormat<'a, 'static> {
    pub fn new_decode(input: &'a [u8], params: &[Param<'a>]) -> Self {
        let mut parser = Self::with_params(params);
        parser.input = Some(input);
        parser
    }
}
