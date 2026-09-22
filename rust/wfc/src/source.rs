use std::collections::HashSet;
use std::fmt;

use serde::de::{self, MapAccess, Visitor};
use serde::{Deserialize, Deserializer};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Location {
    pub line: usize,
    pub column: usize,
}

const DOCUMENT: Location = Location { line: 1, column: 1 };

#[derive(Debug, Eq, PartialEq)]
pub struct Diagnostic {
    pub location: Location,
    pub message: String,
}

impl Diagnostic {
    pub(crate) fn new(location: Location, message: impl Into<String>) -> Self {
        Self {
            location,
            message: message.into(),
        }
    }
}

pub type Result<T> = std::result::Result<T, Diagnostic>;

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq)]
#[serde(rename_all = "kebab-case")]
pub enum ByteOrder {
    BigEndian,
    LittleEndian,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq)]
#[serde(rename_all = "kebab-case")]
pub enum BitOrder {
    MsbFirst,
    LsbFirst,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FieldType {
    Bits(u8),
    U8,
    U16,
    U32,
    U64,
}

impl FieldType {
    pub fn width(self) -> u8 {
        match self {
            Self::Bits(width) => width,
            Self::U8 => 8,
            Self::U16 => 16,
            Self::U32 => 32,
            Self::U64 => 64,
        }
    }

    pub fn is_scalar(self) -> bool {
        !matches!(self, Self::Bits(_))
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FieldKind {
    Supplied,
    Constant(u64),
}

#[derive(Debug)]
pub struct Field {
    pub name: String,
    pub location: Location,
    pub field_type: FieldType,
    pub kind: FieldKind,
}

#[derive(Debug)]
pub struct Assignment {
    pub name: String,
    pub location: Location,
    pub value: u64,
}

#[derive(Debug)]
pub struct WireByte {
    pub location: Location,
    pub value: u8,
}

#[derive(Debug)]
pub struct Vector {
    pub name: String,
    pub location: Location,
    pub assignments: Vec<Assignment>,
    pub wire: Vec<WireByte>,
}

#[derive(Debug)]
pub struct Message {
    pub name: String,
    pub description: String,
    pub location: Location,
    pub fields: Vec<Field>,
    pub vectors: Vec<Vector>,
}

#[derive(Debug)]
pub struct Protocol {
    pub name: String,
    pub location: Location,
    pub description: String,
    pub standard: Option<String>,
    pub reference: Option<String>,
    pub byte_order: ByteOrder,
    pub bit_order: BitOrder,
    pub messages: Vec<Message>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceFile {
    wire_format: u64,
    protocol: SourceProtocol,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceProtocol {
    name: String,
    description: String,
    #[serde(default)]
    standard: Option<String>,
    #[serde(default)]
    reference: Option<String>,
    byte_order: ByteOrder,
    bit_order: BitOrder,
    messages: Vec<SourceMessage>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceMessage {
    name: String,
    description: String,
    fields: Vec<SourceField>,
    #[serde(default)]
    vectors: Vec<SourceVector>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceField {
    name: String,
    #[serde(rename = "type")]
    field_type: String,
    #[serde(default)]
    width: Option<u64>,
    #[serde(default)]
    constant: Option<u64>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceVector {
    name: String,
    values: Assignments,
    wire: Vec<u8>,
}

#[derive(Debug)]
struct Assignments(Vec<(String, u64)>);

impl<'de> Deserialize<'de> for Assignments {
    fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AssignmentsVisitor;

        impl<'de> Visitor<'de> for AssignmentsVisitor {
            type Value = Assignments;

            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("an object containing field values")
            }

            fn visit_map<A>(self, mut map: A) -> std::result::Result<Self::Value, A::Error>
            where
                A: MapAccess<'de>,
            {
                let mut values = Vec::new();
                let mut names = HashSet::new();
                while let Some((name, value)) = map.next_entry::<String, u64>()? {
                    if !names.insert(name.clone()) {
                        return Err(de::Error::custom(format!(
                            "duplicate vector value `{name}`"
                        )));
                    }
                    values.push((name, value));
                }
                Ok(Assignments(values))
            }
        }

        deserializer.deserialize_map(AssignmentsVisitor)
    }
}

pub fn parse(text: &str) -> Result<Protocol> {
    json_five::model_from_str(text).map_err(|error| {
        Diagnostic::new(
            Location {
                line: error.lineno,
                column: error.colno,
            },
            error.message,
        )
    })?;

    let source: SourceFile =
        json_five::from_str(text).map_err(|error| Diagnostic::new(DOCUMENT, error.to_string()))?;
    if source.wire_format != 1 {
        return Err(Diagnostic::new(
            DOCUMENT,
            format!(
                "unsupported wire_format version {}; expected 1",
                source.wire_format
            ),
        ));
    }
    source.protocol.compile()
}

impl SourceProtocol {
    fn compile(self) -> Result<Protocol> {
        validate_identifier(&self.name)?;
        if self.messages.is_empty() {
            return Err(Diagnostic::new(
                DOCUMENT,
                "a protocol must contain at least one message",
            ));
        }

        let mut names = HashSet::new();
        let mut messages = Vec::with_capacity(self.messages.len());
        for message in self.messages {
            let message = message.compile()?;
            if !names.insert(message.name.clone()) {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!("duplicate message `{}`", message.name),
                ));
            }
            messages.push(message);
        }

        Ok(Protocol {
            name: self.name,
            location: DOCUMENT,
            description: self.description,
            standard: self.standard,
            reference: self.reference,
            byte_order: self.byte_order,
            bit_order: self.bit_order,
            messages,
        })
    }
}

impl SourceMessage {
    fn compile(self) -> Result<Message> {
        validate_identifier(&self.name)?;
        if self.fields.is_empty() {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!("message `{}` must contain at least one field", self.name),
            ));
        }

        let mut field_names = HashSet::new();
        let mut fields = Vec::with_capacity(self.fields.len());
        for field in self.fields {
            let field = field.compile()?;
            if !field_names.insert(field.name.clone()) {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!("duplicate field `{}`", field.name),
                ));
            }
            fields.push(field);
        }

        let mut vector_names = HashSet::new();
        let mut vectors = Vec::with_capacity(self.vectors.len());
        for vector in self.vectors {
            let vector = vector.compile()?;
            if !vector_names.insert(vector.name.clone()) {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!("duplicate vector `{}`", vector.name),
                ));
            }
            vectors.push(vector);
        }

        Ok(Message {
            name: self.name,
            description: self.description,
            location: DOCUMENT,
            fields,
            vectors,
        })
    }
}

impl SourceField {
    fn compile(self) -> Result<Field> {
        validate_identifier(&self.name)?;
        let field_type = match self.field_type.as_str() {
            "bits" => {
                let width = self.width.ok_or_else(|| {
                    Diagnostic::new(
                        DOCUMENT,
                        format!("bit field `{}` requires `width`", self.name),
                    )
                })?;
                if !(1..=64).contains(&width) {
                    return Err(Diagnostic::new(
                        DOCUMENT,
                        format!("bit field `{}` width must be from 1 through 64", self.name),
                    ));
                }
                FieldType::Bits(width as u8)
            }
            "u8" | "u16" | "u32" | "u64" => {
                if self.width.is_some() {
                    return Err(Diagnostic::new(
                        DOCUMENT,
                        format!("scalar field `{}` may not specify `width`", self.name),
                    ));
                }
                match self.field_type.as_str() {
                    "u8" => FieldType::U8,
                    "u16" => FieldType::U16,
                    "u32" => FieldType::U32,
                    "u64" => FieldType::U64,
                    _ => unreachable!(),
                }
            }
            _ => {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!(
                        "field `{}` type must be `bits`, `u8`, `u16`, `u32`, or `u64`",
                        self.name
                    ),
                ));
            }
        };

        let kind = match self.constant {
            Some(value) => {
                ensure_value_fits(value, field_type, DOCUMENT)?;
                FieldKind::Constant(value)
            }
            None => FieldKind::Supplied,
        };
        Ok(Field {
            name: self.name,
            location: DOCUMENT,
            field_type,
            kind,
        })
    }
}

impl SourceVector {
    fn compile(self) -> Result<Vector> {
        validate_identifier(&self.name)?;
        if self.wire.is_empty() {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!(
                    "vector `{}` must contain at least one wire octet",
                    self.name
                ),
            ));
        }
        let assignments = self
            .values
            .0
            .into_iter()
            .map(|(name, value)| {
                validate_identifier(&name)?;
                Ok(Assignment {
                    name,
                    location: DOCUMENT,
                    value,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let wire = self
            .wire
            .into_iter()
            .map(|value| WireByte {
                location: DOCUMENT,
                value,
            })
            .collect();
        Ok(Vector {
            name: self.name,
            location: DOCUMENT,
            assignments,
            wire,
        })
    }
}

fn validate_identifier(identifier: &str) -> Result<()> {
    let bytes = identifier.as_bytes();
    let valid = bytes.first().is_some_and(u8::is_ascii_lowercase)
        && bytes.last() != Some(&b'-')
        && !bytes.windows(2).any(|pair| pair == b"--")
        && bytes
            .iter()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || *byte == b'-');

    if valid {
        Ok(())
    } else {
        Err(Diagnostic::new(
            DOCUMENT,
            format!("invalid identifier `{identifier}`"),
        ))
    }
}

pub fn ensure_value_fits(value: u64, field_type: FieldType, location: Location) -> Result<()> {
    let width = field_type.width();
    if width < 64 && value >= (1_u64 << width) {
        return Err(Diagnostic::new(
            location,
            format!("value {value} does not fit in a {width}-bit field"),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXAMPLE: &str = r#"
{
  // Comments, hexadecimal values, unquoted keys and trailing commas are all intentional.
  wire_format: 1,
  protocol: {
    name: 'example',
    description: 'a protocol',
    standard: 'Example 1',
    reference: 'local',
    byte_order: 'big-endian',
    bit_order: 'msb-first',
    messages: [{
      name: 'status',
      description: 'status\nrecord',
      fields: [
        { name: 'version', type: 'bits', width: 3 },
        { name: 'reserved', type: 'bits', width: 1, constant: 0 },
        { name: 'mode', type: 'bits', width: 4 },
        { name: 'count', type: 'u16' },
      ],
      vectors: [{
        name: 'nominal',
        values: { count: 0x1234, version: 1, mode: 2 },
        wire: [0x22, 0x12, 0x34],
      }],
    }],
  },
}
"#;

    #[test]
    fn parses_complete_json5_source() {
        let protocol = parse(EXAMPLE).unwrap();
        assert_eq!(protocol.name, "example");
        assert_eq!(protocol.standard.as_deref(), Some("Example 1"));
        assert_eq!(protocol.reference.as_deref(), Some("local"));
        assert_eq!(protocol.messages.len(), 1);
        assert_eq!(protocol.messages[0].description, "status\nrecord");
        assert_eq!(protocol.messages[0].fields.len(), 4);
        assert_eq!(protocol.messages[0].vectors[0].wire.len(), 3);
    }

    #[test]
    fn parses_layout_for_the_checker_to_validate() {
        let source = EXAMPLE.replace("width: 3", "width: 2");
        let protocol = parse(&source).unwrap();
        assert_eq!(
            protocol.messages[0].fields[0].field_type,
            FieldType::Bits(2)
        );
    }

    #[test]
    fn accepts_full_width_hexadecimal_values() {
        let source = r#"{
          wire_format: 1,
          protocol: {
            name: 'wide',
            description: 'wide constant',
            byte_order: 'big-endian',
            bit_order: 'msb-first',
            messages: [{
              name: 'record',
              description: 'record',
              fields: [{
                name: 'value',
                type: 'u64',
                constant: 0xfedcba9876543210,
              }],
            }],
          },
        }"#;
        let protocol = parse(source).unwrap();
        assert_eq!(
            protocol.messages[0].fields[0].kind,
            FieldKind::Constant(0xfedcba9876543210)
        );
    }

    #[test]
    fn rejects_duplicate_field_names() {
        let source = EXAMPLE.replace("name: 'mode'", "name: 'version'");
        let error = parse(&source).unwrap_err();
        assert!(error.message.contains("duplicate field `version`"));
    }

    #[test]
    fn rejects_unknown_properties() {
        let source = EXAMPLE.replace("wire_format: 1", "wire_format: 1, surprise: 2");
        let error = parse(&source).unwrap_err();
        assert!(error.message.contains("unknown field `surprise`"));
    }

    #[test]
    fn reports_json5_syntax_location() {
        let source = EXAMPLE.replace("wire_format: 1", "wire_format: @");
        let error = parse(&source).unwrap_err();
        assert!(error.location.line > 1);
        assert!(error.location.column > 1);
    }
}
