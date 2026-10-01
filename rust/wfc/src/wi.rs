use std::fmt::Write;

use crate::check::Layout;
use crate::source::{
    BitOrder, ByteOrder, Diagnostic, Field, FieldKind, FieldType, Location, Message, Protocol,
    Result,
};

pub const MAGIC: &[u8; 4] = b"WI\0\0";
#[cfg(test)]
pub const FORMAT_VERSION: u16 = 1;
pub const HEADER_SIZE: u32 = 64;
pub const MESSAGE_RECORD_SIZE: u32 = 32;
pub const NO_STRING: u32 = u32::MAX;

const MAX_CALLER_FIELDS: usize = 16;
const FIXED_FIELD_FAULT: u64 = 1;
const PROTOCOL_STRING_COUNT: u32 = 4;
const MESSAGE_FIXED_STRING_COUNT: usize = 4;

const FLAG_WIRE_BYTE_ORDER_LITTLE: u32 = 1 << 0;
const FLAG_WIRE_BIT_ORDER_LSB: u32 = 1 << 1;
const FLAG_FILE_CRC32C: u32 = 1 << 31;

pub struct Generated {
    pub header: String,
    pub binary: Vec<u8>,
}

pub fn generate(protocol: &Protocol) -> Result<Generated> {
    let compiled = protocol
        .messages
        .iter()
        .map(|message| compile_message(protocol, message))
        .collect::<Result<Vec<_>>>()?;
    let image = build_image(protocol, &compiled)?;
    let header = generate_header(protocol, &compiled, &image);

    Ok(Generated {
        header,
        binary: image.bytes,
    })
}

struct CompiledMessage<'a> {
    message: &'a Message,
    layout: Layout,
    encode: String,
    decode: String,
    value_fields: Vec<&'a Field>,
    value_widths: Vec<u8>,
    value_swaps: Vec<u8>,
}

fn compile_message<'a>(protocol: &Protocol, message: &'a Message) -> Result<CompiledMessage<'a>> {
    let layout = Layout::new(message)?;
    let value_fields: Vec<&Field> = message
        .fields
        .iter()
        .filter(|field| {
            matches!(field.kind, FieldKind::Supplied)
                && field.length_of.is_none()
                && field.count_of.is_none()
        })
        .collect();

    if value_fields.len() > MAX_CALLER_FIELDS {
        return Err(Diagnostic::new(
            message.location,
            format!(
                "the compiled format supports at most {MAX_CALLER_FIELDS} caller fields per message; `{}` has {}",
                message.name,
                value_fields.len()
            ),
        ));
    }

    let mut encode = String::new();
    let mut decode = String::new();
    let mut value_widths = Vec::with_capacity(value_fields.len());
    let mut value_swaps = Vec::with_capacity(value_fields.len());
    let mut ordinals = vec![None; message.fields.len()];
    let mut variables = vec![None; message.fields.len()];
    let mut value_index = 0usize;

    for (field_index, field) in message.fields.iter().enumerate() {
        if matches!(field.kind, FieldKind::Supplied)
            && field.length_of.is_none()
            && field.count_of.is_none()
        {
            ordinals[field_index] = Some(value_index);
            value_widths.push(field.field_type.width());
            value_swaps.push(scalar_swap(field.field_type, protocol.byte_order));
            value_index += 1;
        }
    }
    let mut next_variable = value_index;
    for (field_index, field) in message.fields.iter().enumerate() {
        if matches!(field.kind, FieldKind::Constant(_))
            || matches!(
                field.field_type,
                FieldType::Bytes | FieldType::Records | FieldType::Choice
            )
        {
            continue;
        }
        if field.length_of.is_none() && field.count_of.is_none() {
            variables[field_index] = ordinals[field_index];
        } else {
            if next_variable >= 26 {
                return Err(Diagnostic::new(
                    field.location,
                    format!(
                        "message `{}` needs more than 26 decode variables",
                        message.name
                    ),
                ));
            }
            variables[field_index] = Some(next_variable);
            next_variable += 1;
        }
    }

    for (field_index, field) in message.fields.iter().enumerate() {
        let supplied_index = ordinals[field_index];

        if matches!(field.kind, FieldKind::Supplied)
            && matches!(field.field_type, FieldType::Bits(64))
        {
            return Err(Diagnostic::new(
                field.location,
                format!(
                    "the compiled format cannot represent caller field `{}` as `bits 64`; use `u64` when it is octet-aligned",
                    field.name
                ),
            ));
        }

        let (field_encode, field_decode) = if let Some(target_name) = &field.length_of {
            let target_index = message
                .fields
                .iter()
                .position(|candidate| candidate.name == *target_name)
                .expect("validated length target");
            compile_derived_length(
                field.field_type,
                ordinals[target_index].expect("byte field ordinal"),
                variables[field_index].expect("derived variable"),
            )
        } else if let Some(target_name) = &field.count_of {
            let target_index = message
                .fields
                .iter()
                .position(|candidate| candidate.name == *target_name)
                .expect("validated record target");
            compile_derived_count(
                field,
                layout.offsets[field_index],
                ordinals[target_index].expect("record field ordinal"),
                variables[field_index].expect("derived variable"),
                protocol.bit_order,
            )
        } else if matches!(field.field_type, FieldType::Bytes) && field.length_from.is_some() {
            let length_index = message
                .fields
                .iter()
                .position(|candidate| Some(candidate.name.as_str()) == field.length_from.as_deref())
                .expect("validated length source");
            let slot = supplied_index.expect("byte field ordinal");
            (
                format!("%{{{slot}}}%z%{{{slot}}}%V"),
                format!(
                    "%g{}%{{{slot}}}%N",
                    variable(variables[length_index].expect("length variable"))
                ),
            )
        } else if matches!(field.field_type, FieldType::Records) {
            let child_index = protocol
                .messages
                .iter()
                .position(|candidate| Some(candidate.name.as_str()) == field.message.as_deref())
                .expect("validated record message");
            let child_fields = caller_field_count(&protocol.messages[child_index]);
            let count_index = message
                .fields
                .iter()
                .position(|candidate| Some(candidate.name.as_str()) == field.count_from.as_deref())
                .expect("validated count source");
            let slot = supplied_index.expect("record field ordinal");
            (
                format!("%{{{slot}}}%k%{{{slot}}}%{{{child_fields}}}%J[{child_index}]"),
                format!(
                    "%g{}%{{{slot}}}%{{{child_fields}}}%J[{child_index}]",
                    variable(variables[count_index].expect("count variable"))
                ),
            )
        } else if matches!(field.field_type, FieldType::Choice) {
            let selector_index = message
                .fields
                .iter()
                .position(|candidate| Some(candidate.name.as_str()) == field.select_from.as_deref())
                .expect("validated selector");
            let selector_ordinal = ordinals[selector_index].expect("selector ordinal");
            let selector_variable = variables[selector_index].expect("selector variable");
            let slot = supplied_index.expect("choice field ordinal");
            let mut field_encode = String::new();
            let mut field_decode = String::new();
            for case in &field.cases {
                let child_index = protocol
                    .messages
                    .iter()
                    .position(|candidate| candidate.name == case.message)
                    .expect("validated choice message");
                let child_fields = caller_field_count(&protocol.messages[child_index]);
                field_encode.push_str(&format!(
                    "%?%p{{{}}}%{{{}}}%=%t%{{1}}%{{{slot}}}%{{{child_fields}}}%J[{child_index}]%;",
                    selector_ordinal + 1,
                    case.value,
                ));
                field_decode.push_str(&format!(
                    "%?%g{}%{{{}}}%=%t%{{1}}%{{{slot}}}%{{{child_fields}}}%J[{child_index}]%;",
                    variable(selector_variable),
                    case.value,
                ));
            }
            (field_encode, field_decode)
        } else {
            compile_field(
                field,
                layout.offsets[field_index],
                supplied_index,
                protocol.byte_order,
                protocol.bit_order,
            )
        };
        encode.push_str(&field_encode);
        decode.push_str(&field_decode);
    }

    Ok(CompiledMessage {
        message,
        layout,
        encode,
        decode,
        value_fields,
        value_widths,
        value_swaps,
    })
}

fn caller_field_count(message: &Message) -> usize {
    message
        .fields
        .iter()
        .filter(|field| {
            matches!(field.kind, FieldKind::Supplied)
                && field.length_of.is_none()
                && field.count_of.is_none()
        })
        .count()
}

fn compile_derived_length(
    field_type: FieldType,
    slice_slot: usize,
    variable_index: usize,
) -> (String, String) {
    let (encode_op, decode_op) = match field_type {
        FieldType::U8 => ("%b", "%B"),
        FieldType::U16 => ("%w", "%S"),
        FieldType::U32 => ("%W", "%L"),
        FieldType::U64 => ("%q", "%Q"),
        FieldType::Sdnv => ("%d", "%D"),
        FieldType::Bits(_) | FieldType::Bytes | FieldType::Records | FieldType::Choice => {
            unreachable!()
        }
    };
    (
        format!("%{{{slice_slot}}}%z{encode_op}"),
        format!("{decode_op}%P{}", variable(variable_index)),
    )
}

fn compile_derived_count(
    field: &Field,
    bit_offset: usize,
    record_slot: usize,
    variable_index: usize,
    bit_order: BitOrder,
) -> (String, String) {
    let source = format!("%{{{record_slot}}}%k");
    if let FieldType::Bits(width) = field.field_type {
        return compile_derived_bits(bit_offset, width, &source, variable_index, bit_order);
    }
    let (encode_op, decode_op) = match field.field_type {
        FieldType::U8 => ("%b", "%B"),
        FieldType::U16 => ("%w", "%S"),
        FieldType::U32 => ("%W", "%L"),
        FieldType::U64 => ("%q", "%Q"),
        FieldType::Sdnv => ("%d", "%D"),
        _ => unreachable!(),
    };
    (
        format!("{source}{encode_op}"),
        format!("{decode_op}%P{}", variable(variable_index)),
    )
}

fn compile_derived_bits(
    bit_offset: usize,
    width: u8,
    source: &str,
    variable_index: usize,
    bit_order: BitOrder,
) -> (String, String) {
    let mut encode = String::new();
    let mut decode = String::new();
    let mut consumed = 0u8;
    let mut cursor = bit_offset;
    let mut first_fragment = true;

    while consumed < width {
        let within = cursor % 8;
        let chunk = (8 - within).min(usize::from(width - consumed)) as u8;
        let (source_shift, position) = match bit_order {
            BitOrder::MsbFirst => (width - consumed - chunk, 8 - within as u8 - chunk),
            BitOrder::LsbFirst => (consumed, within as u8),
        };
        encode.push_str(source);
        if source_shift != 0 {
            push_literal(&mut encode, UINT64_ONE << source_shift);
            encode.push_str("%/");
        }
        push_literal(&mut encode, bit_mask(chunk));
        encode.push_str("%&");
        append_bit_operation(&mut encode, chunk, position, 'x');

        if !first_fragment && bit_order == BitOrder::MsbFirst {
            push_literal(&mut decode, UINT64_ONE << chunk);
            decode.push_str("%*");
        }
        append_bit_operation(&mut decode, chunk, position, 'X');
        if !first_fragment {
            if bit_order == BitOrder::LsbFirst {
                push_literal(&mut decode, UINT64_ONE << consumed);
                decode.push_str("%*");
            }
            decode.push_str("%+");
        }
        consumed += chunk;
        cursor += usize::from(chunk);
        first_fragment = false;
        if cursor % 8 == 0 {
            encode.push_str("%f");
            decode.push_str("%f");
        }
    }
    decode.push_str(&format!("%P{}", variable(variable_index)));
    (encode, decode)
}

fn scalar_swap(field_type: FieldType, byte_order: ByteOrder) -> u8 {
    if byte_order != ByteOrder::LittleEndian {
        return 0;
    }
    match field_type {
        FieldType::U16 => 2,
        FieldType::U32 => 4,
        FieldType::U64 => 8,
        FieldType::Bits(_)
        | FieldType::U8
        | FieldType::Sdnv
        | FieldType::Bytes
        | FieldType::Records
        | FieldType::Choice => 0,
    }
}

fn compile_field(
    field: &Field,
    bit_offset: usize,
    supplied_index: Option<usize>,
    byte_order: ByteOrder,
    bit_order: BitOrder,
) -> (String, String) {
    match field.field_type {
        FieldType::Bits(width) => compile_bits(field, bit_offset, width, supplied_index, bit_order),
        FieldType::Bytes => {
            let index = supplied_index.expect("a byte field cannot be constant");
            (format!("%{{{index}}}%v"), format!("%{{{index}}}%R"))
        }
        FieldType::Records | FieldType::Choice => unreachable!(),
        scalar => compile_scalar(field, scalar, supplied_index, byte_order),
    }
}

fn compile_scalar(
    field: &Field,
    field_type: FieldType,
    supplied_index: Option<usize>,
    byte_order: ByteOrder,
) -> (String, String) {
    if matches!(field_type, FieldType::Sdnv) {
        if let Some(index) = supplied_index {
            return (
                format!("%p{{{}}}%d", index + 1),
                format!("%D%P{}", variable(index)),
            );
        }
        let FieldKind::Constant(value) = field.kind else {
            unreachable!()
        };
        let mut encode = String::new();
        let mut decode = String::from("%D");
        push_literal(&mut encode, value);
        encode.push_str("%d");
        append_constant_check(&mut decode, value);
        return (encode, decode);
    }
    let octets = field_type.width() / 8;
    if let Some(index) = supplied_index {
        let encode_op = match field_type {
            FieldType::U8 => "%b",
            FieldType::U16 => "%w",
            FieldType::U32 => "%W",
            FieldType::U64 => "%q",
            FieldType::Sdnv => unreachable!(),
            FieldType::Bits(_) | FieldType::Bytes | FieldType::Records | FieldType::Choice => {
                unreachable!()
            }
        };
        let decode_op = match field_type {
            FieldType::U8 => "%B",
            FieldType::U16 => "%S",
            FieldType::U32 => "%L",
            FieldType::U64 => "%Q",
            FieldType::Sdnv => unreachable!(),
            FieldType::Bits(_) | FieldType::Bytes | FieldType::Records | FieldType::Choice => {
                unreachable!()
            }
        };
        return (
            format!("%p{{{}}}{encode_op}", index + 1),
            format!("{decode_op}%P{}", variable(index)),
        );
    }

    let FieldKind::Constant(value) = field.kind else {
        unreachable!()
    };
    let bytes = scalar_bytes(value, octets, byte_order);
    let mut encode = String::new();
    let mut decode = String::new();
    for byte in bytes {
        push_literal(&mut encode, u64::from(byte));
        encode.push_str("%b");
        decode.push_str("%B");
        append_constant_check(&mut decode, u64::from(byte));
    }
    (encode, decode)
}

fn compile_bits(
    field: &Field,
    bit_offset: usize,
    width: u8,
    supplied_index: Option<usize>,
    bit_order: BitOrder,
) -> (String, String) {
    let mut encode = String::new();
    let mut decode = String::new();
    let mut consumed = 0u8;
    let mut cursor = bit_offset;
    let mut first_fragment = true;

    while consumed < width {
        let within = cursor % 8;
        let chunk = (8 - within).min(usize::from(width - consumed)) as u8;
        let (source_shift, position) = match bit_order {
            BitOrder::MsbFirst => (width - consumed - chunk, 8 - within as u8 - chunk),
            BitOrder::LsbFirst => (consumed, within as u8),
        };
        let mask = bit_mask(chunk);

        match (field.kind, supplied_index) {
            (FieldKind::Supplied, Some(index)) => {
                encode.push_str(&format!("%p{{{}}}", index + 1));
                if source_shift != 0 {
                    push_literal(&mut encode, UINT64_ONE << source_shift);
                    encode.push_str("%/");
                }
                push_literal(&mut encode, mask);
                encode.push_str("%&");
                append_bit_operation(&mut encode, chunk, position, 'x');

                if !first_fragment && bit_order == BitOrder::MsbFirst {
                    push_literal(&mut decode, UINT64_ONE << chunk);
                    decode.push_str("%*");
                }
                append_bit_operation(&mut decode, chunk, position, 'X');
                if !first_fragment {
                    if bit_order == BitOrder::LsbFirst {
                        push_literal(&mut decode, UINT64_ONE << consumed);
                        decode.push_str("%*");
                    }
                    decode.push_str("%+");
                }
            }
            (FieldKind::Constant(value), None) => {
                let fragment = (value >> source_shift) & mask;
                push_literal(&mut encode, fragment);
                append_bit_operation(&mut encode, chunk, position, 'x');

                append_bit_operation(&mut decode, chunk, position, 'X');
                append_constant_check(&mut decode, fragment);
            }
            _ => unreachable!(),
        }

        consumed += chunk;
        cursor += usize::from(chunk);
        first_fragment = false;

        if cursor % 8 == 0 {
            encode.push_str("%f");
            decode.push_str("%f");
        }
    }

    if let Some(index) = supplied_index {
        decode.push_str(&format!("%P{}", variable(index)));
    }
    (encode, decode)
}

const UINT64_ONE: u64 = 1;

fn bit_mask(width: u8) -> u64 {
    if width == 64 {
        u64::MAX
    } else {
        (UINT64_ONE << width) - 1
    }
}

fn append_bit_operation(output: &mut String, width: u8, position: u8, operation: char) {
    push_literal(output, u64::from(width));
    push_literal(output, u64::from(position));
    output.push('%');
    output.push(operation);
}

fn append_constant_check(output: &mut String, expected: u64) {
    push_literal(output, expected);
    output.push_str("%=%!%?%t");
    push_literal(output, FIXED_FIELD_FAULT);
    output.push_str("%E%;");
}

fn push_literal(output: &mut String, value: u64) {
    write!(output, "%{{{value}}}").expect("writing to a String cannot fail");
}

fn variable(index: usize) -> char {
    char::from(b'a' + index as u8)
}

fn scalar_bytes(value: u64, octets: u8, order: ByteOrder) -> Vec<u8> {
    (0..octets)
        .map(|index| {
            let shift = match order {
                ByteOrder::BigEndian => (octets - 1 - index) * 8,
                ByteOrder::LittleEndian => index * 8,
            };
            (value >> shift) as u8
        })
        .collect()
}

struct ImageMessage {
    string_section_offset: u32,
    string_count: u32,
    value_widths_offset: u32,
    value_swaps_offset: u32,
}

struct Image {
    bytes: Vec<u8>,
    flags: u32,
    message_table_offset: u32,
    value_table_offset: u32,
    value_table_size: u32,
    protocol_strings_offset: u32,
    string_offsets_offset: u32,
    string_offset_count: u32,
    string_table_offset: u32,
    string_table_size: u32,
    crc32c: u32,
    messages: Vec<ImageMessage>,
}

#[derive(Default)]
struct StringTable {
    bytes: Vec<u8>,
}

impl StringTable {
    fn add(&mut self, text: &str, location: Location) -> Result<u32> {
        if text.as_bytes().contains(&0) {
            return Err(Diagnostic::new(
                location,
                "compiled strings may not contain a NUL octet",
            ));
        }
        let offset = to_u32(self.bytes.len(), location, "string table offset")?;
        self.bytes.extend_from_slice(text.as_bytes());
        self.bytes.push(0);
        Ok(offset)
    }
}

fn build_image(protocol: &Protocol, messages: &[CompiledMessage<'_>]) -> Result<Image> {
    let mut strings = StringTable::default();
    let mut string_offsets = vec![
        strings.add(&protocol.name, protocol.location)?,
        strings.add(&protocol.description, protocol.location)?,
        match &protocol.standard {
            Some(value) => strings.add(value, protocol.location)?,
            None => NO_STRING,
        },
        match &protocol.reference {
            Some(value) => strings.add(value, protocol.location)?,
            None => NO_STRING,
        },
    ];

    let mut message_string_indices = Vec::with_capacity(messages.len());
    for compiled in messages {
        message_string_indices.push(string_offsets.len());
        string_offsets.push(strings.add(&compiled.message.name, compiled.message.location)?);
        string_offsets.push(strings.add(&compiled.message.description, compiled.message.location)?);
        string_offsets.push(strings.add(&compiled.encode, compiled.message.location)?);
        string_offsets.push(strings.add(&compiled.decode, compiled.message.location)?);
        for field in &compiled.message.fields {
            string_offsets.push(strings.add(&field.name, field.location)?);
        }
    }

    let mut values = Vec::new();
    let mut value_offsets = Vec::with_capacity(messages.len());
    for compiled in messages {
        if compiled.value_fields.is_empty() {
            value_offsets.push((0usize, 0usize));
        } else {
            let widths = values.len();
            values.extend_from_slice(&compiled.value_widths);
            let swaps = values.len();
            values.extend_from_slice(&compiled.value_swaps);
            value_offsets.push((widths, swaps));
        }
    }

    let message_table_offset = HEADER_SIZE;
    let message_table_size = checked_mul(
        messages.len(),
        MESSAGE_RECORD_SIZE as usize,
        protocol.location,
        "message table size",
    )?;
    let value_table_offset = checked_add(
        message_table_offset as usize,
        message_table_size,
        protocol.location,
        "value table offset",
    )?;
    let value_table_size = to_u32(values.len(), protocol.location, "value table size")?;
    let string_offsets_offset = align4(
        checked_add(
            value_table_offset,
            values.len(),
            protocol.location,
            "string-offset section offset",
        )?,
        protocol.location,
    )?;
    let string_offsets_size = checked_mul(
        string_offsets.len(),
        4,
        protocol.location,
        "string-offset section size",
    )?;
    let string_table_offset = checked_add(
        string_offsets_offset,
        string_offsets_size,
        protocol.location,
        "string table offset",
    )?;
    let file_size = checked_add(
        string_table_offset,
        strings.bytes.len(),
        protocol.location,
        "compiled file size",
    )?;

    let value_table_offset_u32 =
        to_u32(value_table_offset, protocol.location, "value table offset")?;
    let string_offsets_offset_u32 = to_u32(
        string_offsets_offset,
        protocol.location,
        "string-offset section offset",
    )?;
    let string_table_offset_u32 = to_u32(
        string_table_offset,
        protocol.location,
        "string table offset",
    )?;
    let string_table_size = to_u32(strings.bytes.len(), protocol.location, "string table size")?;
    let string_offset_count = to_u32(
        string_offsets.len(),
        protocol.location,
        "string-offset count",
    )?;

    let image_messages = messages
        .iter()
        .enumerate()
        .map(|(index, compiled)| {
            let string_index = message_string_indices[index];
            let (widths, swaps) = value_offsets[index];
            Ok(ImageMessage {
                string_section_offset: to_u32(
                    string_offsets_offset + string_index * 4,
                    compiled.message.location,
                    "message string section offset",
                )?,
                string_count: to_u32(
                    MESSAGE_FIXED_STRING_COUNT + compiled.message.fields.len(),
                    compiled.message.location,
                    "message string count",
                )?,
                value_widths_offset: if compiled.value_fields.is_empty() {
                    0
                } else {
                    to_u32(
                        value_table_offset + widths,
                        compiled.message.location,
                        "field-width table offset",
                    )?
                },
                value_swaps_offset: if compiled.value_fields.is_empty() {
                    0
                } else {
                    to_u32(
                        value_table_offset + swaps,
                        compiled.message.location,
                        "field-swap table offset",
                    )?
                },
            })
        })
        .collect::<Result<Vec<_>>>()?;

    let flags = FLAG_FILE_CRC32C | match protocol.byte_order {
        ByteOrder::BigEndian => 0,
        ByteOrder::LittleEndian => FLAG_WIRE_BYTE_ORDER_LITTLE,
    } | match protocol.bit_order {
        BitOrder::MsbFirst => 0,
        BitOrder::LsbFirst => FLAG_WIRE_BIT_ORDER_LSB,
    };

    let mut bytes = Vec::with_capacity(file_size);
    bytes.extend_from_slice(MAGIC);
    put_u16(&mut bytes, protocol.version);
    put_u16(&mut bytes, HEADER_SIZE as u16);
    put_u32(
        &mut bytes,
        to_u32(file_size, protocol.location, "compiled file size")?,
    );
    put_u32(&mut bytes, flags);
    put_u32(
        &mut bytes,
        to_u32(messages.len(), protocol.location, "message count")?,
    );
    put_u32(&mut bytes, MESSAGE_RECORD_SIZE);
    put_u32(&mut bytes, message_table_offset);
    put_u32(&mut bytes, value_table_offset_u32);
    put_u32(&mut bytes, value_table_size);
    put_u32(&mut bytes, string_offsets_offset_u32);
    put_u32(&mut bytes, PROTOCOL_STRING_COUNT);
    put_u32(&mut bytes, string_offsets_offset_u32);
    put_u32(&mut bytes, string_offset_count);
    put_u32(&mut bytes, string_table_offset_u32);
    put_u32(&mut bytes, string_table_size);
    put_u32(&mut bytes, 0);
    debug_assert_eq!(bytes.len(), HEADER_SIZE as usize);

    for (compiled, record) in messages.iter().zip(&image_messages) {
        put_u32(&mut bytes, record.string_section_offset);
        put_u32(&mut bytes, record.string_count);
        put_u32(
            &mut bytes,
            to_u32(
                compiled.layout.octets,
                compiled.message.location,
                "message wire size",
            )?,
        );
        put_u32(
            &mut bytes,
            to_u32(
                compiled.value_fields.len(),
                compiled.message.location,
                "caller-field count",
            )?,
        );
        put_u32(&mut bytes, record.value_widths_offset);
        put_u32(&mut bytes, record.value_swaps_offset);
        put_u32(&mut bytes, u32::from(compiled.layout.variable));
        put_u32(&mut bytes, 0);
    }
    debug_assert_eq!(bytes.len(), value_table_offset);

    bytes.extend_from_slice(&values);
    bytes.resize(string_offsets_offset, 0);
    for offset in string_offsets {
        put_u32(&mut bytes, offset);
    }
    debug_assert_eq!(bytes.len(), string_table_offset);
    bytes.extend_from_slice(&strings.bytes);
    debug_assert_eq!(bytes.len(), file_size);
    let crc32c = wi_crc32c(&bytes);
    bytes[60..64].copy_from_slice(&crc32c.to_le_bytes());

    Ok(Image {
        bytes,
        flags,
        message_table_offset,
        value_table_offset: value_table_offset_u32,
        value_table_size,
        protocol_strings_offset: string_offsets_offset_u32,
        string_offsets_offset: string_offsets_offset_u32,
        string_offset_count,
        string_table_offset: string_table_offset_u32,
        string_table_size,
        crc32c,
        messages: image_messages,
    })
}

fn checked_add(left: usize, right: usize, location: Location, what: &str) -> Result<usize> {
    left.checked_add(right)
        .ok_or_else(|| Diagnostic::new(location, format!("{what} exceeds the host size limit")))
}

fn checked_mul(left: usize, right: usize, location: Location, what: &str) -> Result<usize> {
    left.checked_mul(right)
        .ok_or_else(|| Diagnostic::new(location, format!("{what} exceeds the host size limit")))
}

fn align4(value: usize, location: Location) -> Result<usize> {
    value.checked_add(3).map(|value| value & !3).ok_or_else(|| {
        Diagnostic::new(
            location,
            "compiled section alignment exceeds the host size limit",
        )
    })
}

fn to_u32(value: usize, location: Location, what: &str) -> Result<u32> {
    u32::try_from(value)
        .map_err(|_| Diagnostic::new(location, format!("{what} exceeds the WI 32-bit limit")))
}

fn put_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn put_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn wi_crc32c(data: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for (index, stored) in data.iter().copied().enumerate() {
        let byte = if (60..64).contains(&index) { 0 } else { stored };
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0x82f63b78 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

fn generate_header(protocol: &Protocol, messages: &[CompiledMessage<'_>], image: &Image) -> String {
    let prefix = c_identifier(&protocol.name).to_ascii_uppercase();
    let guard = format!("WF_GENERATED_{prefix}_H");
    let mut output = String::new();

    writeln!(output, "/* Generated by wfc.  Do not edit. */").unwrap();
    append_comment(
        &mut output,
        &format!("Protocol {}: {}", protocol.name, protocol.description),
    );
    writeln!(output, "#ifndef {guard}").unwrap();
    writeln!(output, "#define {guard}\n").unwrap();
    define(
        &mut output,
        &format!("{prefix}_WI_FORMAT_VERSION"),
        u32::from(protocol.version),
    );
    define(
        &mut output,
        &format!("{prefix}_WI_FILE_SIZE"),
        image.bytes.len() as u32,
    );
    define(&mut output, &format!("{prefix}_WI_FLAGS"), image.flags);
    define(&mut output, &format!("{prefix}_WI_CRC32C"), image.crc32c);
    define(
        &mut output,
        &format!("{prefix}_WI_MESSAGE_COUNT"),
        messages.len() as u32,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_MESSAGE_TABLE_OFFSET"),
        image.message_table_offset,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_MESSAGE_RECORD_SIZE"),
        MESSAGE_RECORD_SIZE,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_VALUE_TABLE_OFFSET"),
        image.value_table_offset,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_VALUE_TABLE_SIZE"),
        image.value_table_size,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_PROTOCOL_STRINGS_OFFSET"),
        image.protocol_strings_offset,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_STRING_OFFSETS_OFFSET"),
        image.string_offsets_offset,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_STRING_OFFSET_COUNT"),
        image.string_offset_count,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_STRING_TABLE_OFFSET"),
        image.string_table_offset,
    );
    define(
        &mut output,
        &format!("{prefix}_WI_STRING_TABLE_SIZE"),
        image.string_table_size,
    );
    define(&mut output, &format!("{prefix}_WI_NO_STRING"), NO_STRING);
    define(
        &mut output,
        &format!("{prefix}_WI_FAULT_FIXED_FIELD"),
        FIXED_FIELD_FAULT as u32,
    );
    output.push('\n');

    define(&mut output, &format!("{prefix}_PROTOCOL_STRING_NAME"), 0);
    define(
        &mut output,
        &format!("{prefix}_PROTOCOL_STRING_DESCRIPTION"),
        1,
    );
    define(
        &mut output,
        &format!("{prefix}_PROTOCOL_STRING_STANDARD"),
        2,
    );
    define(
        &mut output,
        &format!("{prefix}_PROTOCOL_STRING_REFERENCE"),
        3,
    );
    output.push('\n');

    for (index, (compiled, record)) in messages.iter().zip(&image.messages).enumerate() {
        let message = c_identifier(&compiled.message.name).to_ascii_uppercase();
        let base = format!("{prefix}_{message}");
        append_comment(&mut output, &compiled.message.description);
        define(
            &mut output,
            &format!("{prefix}_MESSAGE_{message}"),
            index as u32,
        );
        define(
            &mut output,
            &format!("{base}_RECORD_OFFSET"),
            image.message_table_offset + index as u32 * MESSAGE_RECORD_SIZE,
        );
        define(
            &mut output,
            &format!("{base}_STRINGS_OFFSET"),
            record.string_section_offset,
        );
        define(
            &mut output,
            &format!("{base}_STRING_COUNT"),
            record.string_count,
        );
        define(&mut output, &format!("{base}_STRING_NAME"), 0);
        define(&mut output, &format!("{base}_STRING_DESCRIPTION"), 1);
        define(&mut output, &format!("{base}_STRING_ENCODE"), 2);
        define(&mut output, &format!("{base}_STRING_DECODE"), 3);
        for (field_index, field) in compiled.message.fields.iter().enumerate() {
            define(
                &mut output,
                &format!(
                    "{base}_STRING_FIELD_{}",
                    c_identifier(&field.name).to_ascii_uppercase()
                ),
                (MESSAGE_FIXED_STRING_COUNT + field_index) as u32,
            );
        }
        for (ordinal, field) in compiled.value_fields.iter().enumerate() {
            define(
                &mut output,
                &format!(
                    "{base}_ORDINAL_{}",
                    c_identifier(&field.name).to_ascii_uppercase()
                ),
                ordinal as u32,
            );
        }
        define(
            &mut output,
            &format!("{base}_CALLER_FIELD_COUNT"),
            compiled.value_fields.len() as u32,
        );
        define(
            &mut output,
            &format!("{base}_WIRE_SIZE"),
            compiled.layout.octets as u32,
        );
        define(
            &mut output,
            &format!("{base}_VARIABLE_WIRE_SIZE"),
            u32::from(compiled.layout.variable),
        );
        output.push('\n');
    }

    writeln!(output, "#endif /* {guard} */").unwrap();
    output
}

fn define(output: &mut String, name: &str, value: u32) {
    writeln!(output, "#define {name:<52} {value}u").unwrap();
}

fn append_comment(output: &mut String, text: &str) {
    let cleaned = text
        .replace("*/", "* /")
        .chars()
        .map(|character| {
            if character.is_control() {
                ' '
            } else {
                character
            }
        })
        .collect::<String>();
    writeln!(output, "/* {cleaned} */").unwrap();
}

fn c_identifier(identifier: &str) -> String {
    identifier.replace('-', "_")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::check::check;
    use crate::source::parse;

    fn u16_at(bytes: &[u8], offset: usize) -> u16 {
        u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
    }

    fn u32_at(bytes: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }

    fn string_at(image: &Image, string_section: u32, slot: u32) -> &str {
        let offset_entry = string_section as usize + slot as usize * 4;
        let relative = u32_at(&image.bytes, offset_entry);
        assert_ne!(relative, NO_STRING);
        let start = image.string_table_offset as usize + relative as usize;
        let end = image.bytes[start..]
            .iter()
            .position(|byte| *byte == 0)
            .map(|length| start + length)
            .unwrap();
        std::str::from_utf8(&image.bytes[start..end]).unwrap()
    }

    #[test]
    fn writes_portable_sections_and_string_offsets() {
        let source = include_str!("../../../examples/source-format/example-telemetry.wf");
        let protocol = parse(source).unwrap();
        check(&protocol).unwrap();
        let compiled = protocol
            .messages
            .iter()
            .map(|message| compile_message(&protocol, message))
            .collect::<Result<Vec<_>>>()
            .unwrap();
        let image = build_image(&protocol, &compiled).unwrap();

        assert_eq!(&image.bytes[..4], MAGIC);
        assert_eq!(u16_at(&image.bytes, 4), FORMAT_VERSION);
        assert_eq!(u16_at(&image.bytes, 6), HEADER_SIZE as u16);
        assert_eq!(u32_at(&image.bytes, 8) as usize, image.bytes.len());
        assert_eq!(u32_at(&image.bytes, 12) & FLAG_FILE_CRC32C, FLAG_FILE_CRC32C);
        assert_eq!(u32_at(&image.bytes, 60), wi_crc32c(&image.bytes));
        assert_eq!(u32_at(&image.bytes, 16), 1);
        assert_eq!(u32_at(&image.bytes, 20), MESSAGE_RECORD_SIZE);
        assert_eq!(
            string_at(&image, image.protocol_strings_offset, 0),
            "example-telemetry"
        );
        assert_eq!(
            string_at(&image, image.messages[0].string_section_offset, 0),
            "status"
        );
        assert_eq!(
            string_at(&image, image.messages[0].string_section_offset, 4),
            "version"
        );
        assert!(string_at(&image, image.messages[0].string_section_offset, 2).contains("%p{1}"));
    }

    #[test]
    fn header_names_messages_fields_and_string_sections() {
        let source = include_str!("../../../examples/source-format/example-telemetry.wf");
        let protocol = parse(source).unwrap();
        check(&protocol).unwrap();
        let generated = generate(&protocol).unwrap();

        assert!(generated
            .header
            .contains("EXAMPLE_TELEMETRY_MESSAGE_STATUS"));
        assert!(generated
            .header
            .contains("EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET"));
        assert!(generated
            .header
            .contains("EXAMPLE_TELEMETRY_STATUS_STRING_FIELD_VERSION"));
        assert!(generated
            .header
            .contains("EXAMPLE_TELEMETRY_STATUS_ORDINAL_SAMPLE_COUNT"));
        assert!(!generated.header.contains("wire_format.h"));
    }

    #[test]
    fn rejects_seventeenth_caller_field() {
        let protocol = protocol_with_u8_fields(17);
        let error = generate(&protocol).err().unwrap();
        assert!(error.message.contains("at most 16 caller fields"));
    }

    #[test]
    fn compiles_sixteen_caller_fields() {
        let protocol = protocol_with_u8_fields(16);
        let generated = generate(&protocol).unwrap();
        let text = String::from_utf8_lossy(&generated.binary);
        assert!(text.contains("%p{10}%b"));
        assert!(text.contains("%p{16}%b"));
    }

    fn protocol_with_u8_fields(count: usize) -> Protocol {
        let mut fields = String::new();
        for index in 0..count {
            writeln!(fields, "{{ name: 'f{index}', type: 'u8' }},").unwrap();
        }
        let source = format!(
            r#"{{
  wire_format: 1,
  protocol: {{
    name: 'many',
    description: 'many',
    byte_order: 'big-endian',
    bit_order: 'msb-first',
    messages: [{{
      name: 'record',
      description: 'record',
      fields: [{fields}],
    }}],
  }},
}}"#
        );
        let protocol = parse(&source).unwrap();
        check(&protocol).unwrap();
        protocol
    }
}
