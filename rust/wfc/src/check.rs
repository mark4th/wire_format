use std::collections::HashMap;

use crate::source::{
    ensure_value_fits, BitOrder, ByteOrder, Diagnostic, Field, FieldKind, FieldType, Message,
    Protocol, Result, Vector,
};

#[derive(Debug, Eq, PartialEq)]
pub struct Summary {
    pub messages: usize,
    pub vectors: usize,
    pub layout_octets: usize,
}

pub fn check(protocol: &Protocol) -> Result<Summary> {
    ensure_text(
        &protocol.description,
        "protocol description",
        protocol.location,
    )?;
    if let Some(standard) = &protocol.standard {
        ensure_text(standard, "standard", protocol.location)?;
    }
    if let Some(reference) = &protocol.reference {
        ensure_text(reference, "reference", protocol.location)?;
    }

    let mut vectors = 0;
    let mut layout_octets = 0usize;

    for message in &protocol.messages {
        ensure_text(
            &message.description,
            "message description",
            message.location,
        )?;
        let layout = Layout::new(message)?;
        layout_octets = layout_octets
            .checked_add(layout.octets)
            .ok_or_else(|| Diagnostic {
                location: message.location,
                message: "combined message sizes exceed the host size limit".to_owned(),
            })?;

        for vector in &message.vectors {
            check_vector(protocol, message, &layout, vector)?;
            vectors += 1;
        }
    }

    Ok(Summary {
        messages: protocol.messages.len(),
        vectors,
        layout_octets,
    })
}

fn ensure_text(text: &str, what: &str, location: crate::source::Location) -> Result<()> {
    if text.trim().is_empty() {
        return Err(Diagnostic {
            location,
            message: format!("{what} may not be empty"),
        });
    }
    if text.as_bytes().contains(&0) {
        return Err(Diagnostic {
            location,
            message: format!("{what} may not contain a NUL octet"),
        });
    }
    Ok(())
}

#[derive(Debug)]
pub struct Layout {
    pub offsets: Vec<usize>,
    pub octets: usize,
}

impl Layout {
    pub fn new(message: &Message) -> Result<Self> {
        let mut offsets = Vec::with_capacity(message.fields.len());
        let mut bit_offset = 0usize;

        for field in &message.fields {
            if field.field_type.is_scalar() && bit_offset % 8 != 0 {
                return Err(Diagnostic {
                    location: field.location,
                    message: format!(
                        "scalar field `{}` begins at bit {}, not an octet boundary",
                        field.name, bit_offset
                    ),
                });
            }
            offsets.push(bit_offset);
            bit_offset = bit_offset
                .checked_add(usize::from(field.field_type.width()))
                .ok_or_else(|| Diagnostic {
                    location: field.location,
                    message: "message size exceeds the host size limit".to_owned(),
                })?;
        }

        if bit_offset % 8 != 0 {
            return Err(Diagnostic {
                location: message.location,
                message: format!(
                    "message `{}` ends after {} bits, not on an octet boundary",
                    message.name, bit_offset
                ),
            });
        }

        Ok(Self {
            offsets,
            octets: bit_offset / 8,
        })
    }
}

fn check_vector(
    protocol: &Protocol,
    message: &Message,
    layout: &Layout,
    vector: &Vector,
) -> Result<()> {
    let fields: HashMap<&str, &Field> = message
        .fields
        .iter()
        .map(|field| (field.name.as_str(), field))
        .collect();
    let mut assignments = HashMap::new();

    for assignment in &vector.assignments {
        let field = fields
            .get(assignment.name.as_str())
            .ok_or_else(|| Diagnostic {
                location: assignment.location,
                message: format!(
                    "vector `{}` assigns unknown field `{}`",
                    vector.name, assignment.name
                ),
            })?;
        if matches!(field.kind, FieldKind::Constant(_)) {
            return Err(Diagnostic {
                location: assignment.location,
                message: format!(
                    "vector `{}` may not assign constant field `{}`",
                    vector.name, assignment.name
                ),
            });
        }
        if assignments
            .insert(assignment.name.as_str(), assignment)
            .is_some()
        {
            return Err(Diagnostic {
                location: assignment.location,
                message: format!(
                    "vector `{}` assigns field `{}` more than once",
                    vector.name, assignment.name
                ),
            });
        }
        ensure_value_fits(assignment.value, field.field_type, assignment.location)?;
    }

    for field in &message.fields {
        if matches!(field.kind, FieldKind::Supplied)
            && !assignments.contains_key(field.name.as_str())
        {
            return Err(Diagnostic {
                location: vector.location,
                message: format!(
                    "vector `{}` has no value for field `{}`",
                    vector.name, field.name
                ),
            });
        }
    }

    if vector.wire.len() != layout.octets {
        return Err(Diagnostic {
            location: vector.location,
            message: format!(
                "vector `{}` has {} wire octets; message `{}` requires {}",
                vector.name,
                vector.wire.len(),
                message.name,
                layout.octets
            ),
        });
    }

    let mut encoded = vec![0u8; layout.octets];
    for (index, field) in message.fields.iter().enumerate() {
        let value = match field.kind {
            FieldKind::Supplied => assignments[field.name.as_str()].value,
            FieldKind::Constant(value) => value,
        };
        write_field(
            &mut encoded,
            layout.offsets[index],
            field.field_type,
            value,
            protocol.byte_order,
            protocol.bit_order,
        );
    }

    let expected: Vec<u8> = vector.wire.iter().map(|byte| byte.value).collect();
    let encode_mismatch = encoded
        .iter()
        .zip(&expected)
        .position(|(actual, wanted)| actual != wanted);

    for (index, field) in message.fields.iter().enumerate() {
        let decoded = read_field(
            &expected,
            layout.offsets[index],
            field.field_type,
            protocol.byte_order,
            protocol.bit_order,
        );
        match field.kind {
            FieldKind::Constant(value) if decoded != value => {
                return Err(Diagnostic {
                    location: vector.wire[layout.offsets[index] / 8].location,
                    message: format!(
                        "vector `{}` decodes constant `{}` as {decoded}, expected {value}",
                        vector.name, field.name
                    ),
                });
            }
            FieldKind::Supplied => {
                let wanted = assignments[field.name.as_str()].value;
                if decoded != wanted {
                    return Err(Diagnostic {
                        location: vector.wire[layout.offsets[index] / 8].location,
                        message: format!(
                            "vector `{}` decodes field `{}` as {decoded}, expected {wanted}",
                            vector.name, field.name
                        ),
                    });
                }
            }
            FieldKind::Constant(_) => {}
        }
    }

    if let Some(index) = encode_mismatch {
        return Err(Diagnostic {
            location: vector.wire[index].location,
            message: format!(
                "vector `{}` encodes octet {} as {:02X}, but `wire` contains {:02X}",
                vector.name, index, encoded[index], expected[index]
            ),
        });
    }

    Ok(())
}

fn write_field(
    output: &mut [u8],
    bit_offset: usize,
    field_type: FieldType,
    value: u64,
    byte_order: ByteOrder,
    bit_order: BitOrder,
) {
    match field_type {
        FieldType::Bits(width) => write_bits(output, bit_offset, width, value, bit_order),
        _ => write_scalar(
            output,
            bit_offset / 8,
            usize::from(field_type.width() / 8),
            value,
            byte_order,
        ),
    }
}

fn read_field(
    input: &[u8],
    bit_offset: usize,
    field_type: FieldType,
    byte_order: ByteOrder,
    bit_order: BitOrder,
) -> u64 {
    match field_type {
        FieldType::Bits(width) => read_bits(input, bit_offset, width, bit_order),
        _ => read_scalar(
            input,
            bit_offset / 8,
            usize::from(field_type.width() / 8),
            byte_order,
        ),
    }
}

fn write_bits(output: &mut [u8], offset: usize, width: u8, value: u64, order: BitOrder) {
    for index in 0..usize::from(width) {
        let (source_bit, target_bit) = match order {
            BitOrder::MsbFirst => (usize::from(width) - 1 - index, 7 - ((offset + index) % 8)),
            BitOrder::LsbFirst => (index, (offset + index) % 8),
        };
        if ((value >> source_bit) & 1) != 0 {
            output[(offset + index) / 8] |= 1 << target_bit;
        }
    }
}

fn read_bits(input: &[u8], offset: usize, width: u8, order: BitOrder) -> u64 {
    let mut value = 0u64;
    for index in 0..usize::from(width) {
        let (target_bit, source_bit) = match order {
            BitOrder::MsbFirst => (usize::from(width) - 1 - index, 7 - ((offset + index) % 8)),
            BitOrder::LsbFirst => (index, (offset + index) % 8),
        };
        if ((input[(offset + index) / 8] >> source_bit) & 1) != 0 {
            value |= 1 << target_bit;
        }
    }
    value
}

fn write_scalar(output: &mut [u8], offset: usize, octets: usize, value: u64, order: ByteOrder) {
    for index in 0..octets {
        let shift = match order {
            ByteOrder::BigEndian => (octets - 1 - index) * 8,
            ByteOrder::LittleEndian => index * 8,
        };
        output[offset + index] = (value >> shift) as u8;
    }
}

fn read_scalar(input: &[u8], offset: usize, octets: usize, order: ByteOrder) -> u64 {
    let mut value = 0u64;
    for index in 0..octets {
        let shift = match order {
            ByteOrder::BigEndian => (octets - 1 - index) * 8,
            ByteOrder::LittleEndian => index * 8,
        };
        value |= u64::from(input[offset + index]) << shift;
    }
    value
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::source::parse;

    fn checked(source: &str) -> std::result::Result<Summary, Diagnostic> {
        let protocol = parse(source).unwrap();
        check(&protocol)
    }

    fn source(fields: &str, vectors: &str) -> String {
        source_with_orders(fields, vectors, "big-endian", "msb-first")
    }

    fn source_with_orders(
        fields: &str,
        vectors: &str,
        byte_order: &str,
        bit_order: &str,
    ) -> String {
        format!(
            r#"{{
  wire_format: 1,
  protocol: {{
    name: 'test',
    description: 'test protocol',
    byte_order: '{byte_order}',
    bit_order: '{bit_order}',
    messages: [{{
      name: 'header',
      description: 'header',
      fields: [{fields}],
      vectors: [{vectors}],
    }}],
  }},
}}"#
        )
    }

    #[test]
    fn checks_msb_fields_which_cross_octets() {
        let source = source(
            "{ name: 'first', type: 'bits', width: 5 },
             { name: 'second', type: 'bits', width: 11 }",
            "{ name: 'nominal', values: { first: 0x15, second: 0x567 },
               wire: [0xad, 0x67] }",
        );
        assert_eq!(
            checked(&source).unwrap(),
            Summary {
                messages: 1,
                vectors: 1,
                layout_octets: 2
            }
        );
    }

    #[test]
    fn checks_lsb_fields_and_little_endian_scalars() {
        let source = source_with_orders(
            "{ name: 'low', type: 'bits', width: 3 },
             { name: 'middle', type: 'bits', width: 2, constant: 2 },
             { name: 'high', type: 'bits', width: 3 },
             { name: 'count', type: 'u16' }",
            "{ name: 'nominal', values: { low: 5, high: 3, count: 0x1234 },
               wire: [0x75, 0x34, 0x12] }",
            "little-endian",
            "lsb-first",
        );
        checked(&source).unwrap();
    }

    #[test]
    fn rejects_unaligned_scalar() {
        let source = source(
            "{ name: 'flags', type: 'bits', width: 3 },
             { name: 'count', type: 'u16' }",
            "",
        );
        let error = checked(&source).unwrap_err();
        assert!(error.message.contains("not an octet boundary"));
    }

    #[test]
    fn rejects_missing_assignment() {
        let source = source(
            "{ name: 'value', type: 'u8' }",
            "{ name: 'empty', values: {}, wire: [0] }",
        );
        let error = checked(&source).unwrap_err();
        assert!(error.message.contains("has no value for field `value`"));
    }

    #[test]
    fn rejects_a_bad_constant_on_the_wire() {
        let source = source(
            "{ name: 'version', type: 'bits', width: 3, constant: 0 },
             { name: 'value', type: 'bits', width: 5 }",
            "{ name: 'wrong-version', values: { value: 1 }, wire: [0xe1] }",
        );
        let error = checked(&source).unwrap_err();
        assert!(error.message.contains("decodes constant `version` as 7"));
    }

    #[test]
    fn rejects_out_of_range_vector_value() {
        let source = source(
            "{ name: 'value', type: 'bits', width: 3 },
             { name: 'padding', type: 'bits', width: 5, constant: 0 }",
            "{ name: 'range', values: { value: 8 }, wire: [0] }",
        );
        let error = checked(&source).unwrap_err();
        assert!(error.message.contains("does not fit in a 3-bit field"));
    }

    #[test]
    fn rejects_nul_in_compiled_text() {
        let source =
            source("{ name: 'value', type: 'u8' }", "").replace("test protocol", "bad\\u0000text");
        let error = checked(&source).unwrap_err();
        assert!(error.message.contains("may not contain a NUL octet"));
    }
}
