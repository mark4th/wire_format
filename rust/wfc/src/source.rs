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
    Sdnv,
    Bytes,
    Records,
    Choice,
}

impl FieldType {
    pub fn width(self) -> u8 {
        match self {
            Self::Bits(width) => width,
            Self::U8 => 8,
            Self::U16 => 16,
            Self::U32 => 32,
            Self::U64 => 64,
            Self::Sdnv => 8,
            Self::Bytes => 0,
            Self::Records | Self::Choice => 0,
        }
    }

    pub fn is_scalar(self) -> bool {
        matches!(
            self,
            Self::U8 | Self::U16 | Self::U32 | Self::U64 | Self::Sdnv
        )
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
    pub length_from: Option<String>,
    pub length_of: Option<String>,
    pub message: Option<String>,
    pub count_from: Option<String>,
    pub count_of: Option<String>,
    pub select_from: Option<String>,
    pub cases: Vec<ChoiceCase>,
}

#[derive(Debug)]
pub struct ChoiceCase {
    pub value: u64,
    pub message: String,
    pub location: Location,
}

#[derive(Debug)]
pub struct Assignment {
    pub name: String,
    pub location: Location,
    pub value: AssignmentValue,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum AssignmentValue {
    Unsigned(u64),
    Bytes(Vec<u8>),
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
    pub version: u16,
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
    #[serde(default)]
    length_from: Option<String>,
    #[serde(default)]
    length_of: Option<String>,
    #[serde(default)]
    message: Option<String>,
    #[serde(default)]
    count_from: Option<String>,
    #[serde(default)]
    count_of: Option<String>,
    #[serde(default)]
    select_from: Option<String>,
    #[serde(default)]
    cases: Vec<SourceChoiceCase>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceChoiceCase {
    value: u64,
    message: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SourceVector {
    name: String,
    values: Assignments,
    wire: Vec<u8>,
}

#[derive(Debug, Deserialize)]
#[serde(untagged)]
enum SourceAssignmentValue {
    Unsigned(u64),
    Bytes(Vec<u8>),
}

#[derive(Debug)]
struct Assignments(Vec<(String, SourceAssignmentValue)>);

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
                while let Some((name, value)) = map.next_entry::<String, SourceAssignmentValue>()? {
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
    if source.wire_format != 1 && source.wire_format != 2 && source.wire_format != 3 {
        return Err(Diagnostic::new(
            DOCUMENT,
            format!(
                "unsupported wire_format version {}; expected 1, 2, or 3",
                source.wire_format
            ),
        ));
    }
    source.protocol.compile(source.wire_format as u16)
}

impl SourceProtocol {
    fn compile(self, version: u16) -> Result<Protocol> {
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
            let message = message.compile(version)?;
            if !names.insert(message.name.clone()) {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!("duplicate message `{}`", message.name),
                ));
            }
            messages.push(message);
        }

        for (message_index, message) in messages.iter().enumerate() {
            for field in &message.fields {
                if let Some(target_name) = &field.message {
                    let target_index = messages
                        .iter()
                        .position(|candidate| candidate.name == *target_name)
                        .ok_or_else(|| {
                            Diagnostic::new(
                                field.location,
                                format!(
                                    "record field `{}` references unknown message `{target_name}`",
                                    field.name
                                ),
                            )
                        })?;
                    if target_index >= message_index {
                        return Err(Diagnostic::new(
                            field.location,
                            format!(
                                "record field `{}` must reference an earlier message",
                                field.name
                            ),
                        ));
                    }
                }
                for case in &field.cases {
                    let target_index = messages
                        .iter()
                        .position(|candidate| candidate.name == case.message)
                        .ok_or_else(|| {
                            Diagnostic::new(
                                case.location,
                                format!(
                                    "choice field `{}` references unknown message `{}`",
                                    field.name, case.message
                                ),
                            )
                        })?;
                    if target_index >= message_index {
                        return Err(Diagnostic::new(
                            case.location,
                            format!(
                                "choice field `{}` must reference an earlier message",
                                field.name
                            ),
                        ));
                    }
                }
            }
        }

        Ok(Protocol {
            version,
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
    fn compile(self, version: u16) -> Result<Message> {
        validate_identifier(&self.name)?;
        if self.fields.is_empty() {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!("message `{}` must contain at least one field", self.name),
            ));
        }

        let mut field_names = HashSet::new();
        let mut fields = Vec::with_capacity(self.fields.len());
        let field_count = self.fields.len();
        for (field_index, field) in self.fields.into_iter().enumerate() {
            let field = field.compile(version)?;
            if matches!(field.field_type, FieldType::Bytes)
                && field.length_from.is_none()
                && field_index + 1 != field_count
            {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!(
                        "byte field `{}` must be the final field in its message",
                        field.name
                    ),
                ));
            }
            if !field_names.insert(field.name.clone()) {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!("duplicate field `{}`", field.name),
                ));
            }
            fields.push(field);
        }

        if version >= 3 {
            for (index, field) in fields.iter().enumerate() {
                let Some(reference) = field
                    .length_of
                    .as_ref()
                    .or(field.length_from.as_ref())
                    .or(field.count_of.as_ref())
                    .or(field.count_from.as_ref())
                else {
                    continue;
                };
                let Some((target_index, target)) = fields
                    .iter()
                    .enumerate()
                    .find(|(_, candidate)| candidate.name == *reference)
                else {
                    return Err(Diagnostic::new(
                        field.location,
                        format!(
                            "field `{}` references unknown field `{reference}`",
                            field.name
                        ),
                    ));
                };
                if field.length_of.is_some() {
                    if !matches!(target.field_type, FieldType::Bytes)
                        || target_index <= index
                        || target.length_from.as_deref() != Some(field.name.as_str())
                    {
                        return Err(Diagnostic::new(
                            field.location,
                            format!(
                                "length field `{}` and byte field `{}` must reference each other, with the length first",
                                field.name, target.name
                            ),
                        ));
                    }
                } else if field.length_from.is_some() {
                    if matches!(target.field_type, FieldType::Bytes)
                        || target_index >= index
                        || target.length_of.as_deref() != Some(field.name.as_str())
                    {
                        return Err(Diagnostic::new(
                            field.location,
                            format!(
                                "byte field `{}` and length field `{}` must reference each other, with the length first",
                                field.name, target.name
                            ),
                        ));
                    }
                } else if field.count_of.is_some() {
                    if !matches!(target.field_type, FieldType::Records)
                        || target_index <= index
                        || target.count_from.as_deref() != Some(field.name.as_str())
                    {
                        return Err(Diagnostic::new(
                            field.location,
                            format!(
                                "count field `{}` and record field `{}` must reference each other, with the count first",
                                field.name, target.name
                            ),
                        ));
                    }
                } else if matches!(
                    target.field_type,
                    FieldType::Bytes | FieldType::Records | FieldType::Choice
                ) || target_index >= index
                    || target.count_of.as_deref() != Some(field.name.as_str())
                {
                    return Err(Diagnostic::new(
                        field.location,
                        format!(
                            "record field `{}` and count field `{}` must reference each other, with the count first",
                            field.name, target.name
                        ),
                    ));
                }
            }

            for field in &fields {
                if let Some(selector) = &field.select_from {
                    let Some((selector_index, selector_field)) = fields
                        .iter()
                        .enumerate()
                        .find(|(_, candidate)| candidate.name == *selector)
                    else {
                        return Err(Diagnostic::new(
                            field.location,
                            format!(
                                "choice field `{}` references unknown selector `{selector}`",
                                field.name
                            ),
                        ));
                    };
                    if selector_index
                        >= fields
                            .iter()
                            .position(|candidate| candidate.name == field.name)
                            .unwrap()
                        || !matches!(
                            selector_field.field_type,
                            FieldType::Bits(_)
                                | FieldType::U8
                                | FieldType::U16
                                | FieldType::U32
                                | FieldType::U64
                                | FieldType::Sdnv
                        )
                        || !matches!(selector_field.kind, FieldKind::Supplied)
                        || selector_field.length_of.is_some()
                        || selector_field.count_of.is_some()
                    {
                        return Err(Diagnostic::new(
                            field.location,
                            format!(
                                "choice field `{}` requires an earlier scalar selector",
                                field.name
                            ),
                        ));
                    }
                    for case in &field.cases {
                        ensure_value_fits(case.value, selector_field.field_type, case.location)?;
                    }
                }
            }
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
    fn compile(self, version: u16) -> Result<Field> {
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
            "u8" | "u16" | "u32" | "u64" | "sdnv" => {
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
                    "sdnv" => FieldType::Sdnv,
                    _ => unreachable!(),
                }
            }
            "bytes" => {
                if self.width.is_some() {
                    return Err(Diagnostic::new(
                        DOCUMENT,
                        format!("byte field `{}` may not specify `width`", self.name),
                    ));
                }
                if self.constant.is_some() {
                    return Err(Diagnostic::new(
                        DOCUMENT,
                        format!("byte field `{}` may not specify `constant`", self.name),
                    ));
                }
                FieldType::Bytes
            }
            "records" | "choice" => {
                if version < 3 {
                    return Err(Diagnostic::new(
                        DOCUMENT,
                        format!("field `{}` requires wire_format version 3", self.name),
                    ));
                }
                if self.width.is_some() || self.constant.is_some() {
                    return Err(Diagnostic::new(
                        DOCUMENT,
                        format!(
                            "record field `{}` may not specify `width` or `constant`",
                            self.name
                        ),
                    ));
                }
                if self.field_type == "records" {
                    FieldType::Records
                } else {
                    FieldType::Choice
                }
            }
            _ => {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!(
                        "field `{}` type must be `bits`, `u8`, `u16`, `u32`, `u64`, `sdnv`, `bytes`, `records`, or `choice`",
                        self.name
                    ),
                ));
            }
        };

        if matches!(field_type, FieldType::Bytes) && version < 2 {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!("byte field `{}` requires wire_format version 2", self.name),
            ));
        }
        if matches!(field_type, FieldType::Sdnv) && version < 3 {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!("SDNV field `{}` requires wire_format version 3", self.name),
            ));
        }
        if (self.length_from.is_some()
            || self.length_of.is_some()
            || self.count_from.is_some()
            || self.count_of.is_some()
            || self.select_from.is_some())
            && version < 3
        {
            return Err(Diagnostic::new(
                DOCUMENT,
                "length relationships require wire_format version 3",
            ));
        }
        if self.length_from.is_some() && !matches!(field_type, FieldType::Bytes) {
            return Err(Diagnostic::new(
                DOCUMENT,
                "only a byte field may specify `length_from`",
            ));
        }
        if self.length_of.is_some()
            && matches!(
                field_type,
                FieldType::Bytes | FieldType::Bits(_) | FieldType::Records | FieldType::Choice
            )
        {
            return Err(Diagnostic::new(
                DOCUMENT,
                "a byte or bit field may not specify `length_of`",
            ));
        }
        if self.constant.is_some() && self.length_of.is_some() {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!(
                    "field `{}` may not specify both `constant` and `length_of`",
                    self.name
                ),
            ));
        }
        if self.count_from.is_some() != matches!(field_type, FieldType::Records) {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!(
                    "record field `{}` requires exactly one `count_from`",
                    self.name
                ),
            ));
        }
        if self.message.is_some() != matches!(field_type, FieldType::Records) {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!(
                    "record field `{}` requires exactly one `message`",
                    self.name
                ),
            ));
        }
        if self.count_of.is_some()
            && !matches!(
                field_type,
                FieldType::Bits(_)
                    | FieldType::U8
                    | FieldType::U16
                    | FieldType::U32
                    | FieldType::U64
                    | FieldType::Sdnv
            )
        {
            return Err(Diagnostic::new(
                DOCUMENT,
                "only a scalar field may specify `count_of`",
            ));
        }
        if self.constant.is_some() && self.count_of.is_some() {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!(
                    "field `{}` may not specify both `constant` and `count_of`",
                    self.name
                ),
            ));
        }
        if matches!(field_type, FieldType::Choice) {
            if self.select_from.is_none() || self.cases.is_empty() {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!(
                        "choice field `{}` requires `select_from` and at least one case",
                        self.name
                    ),
                ));
            }
        } else if self.select_from.is_some() || !self.cases.is_empty() {
            return Err(Diagnostic::new(
                DOCUMENT,
                format!("only a choice field may specify `select_from` or `cases`"),
            ));
        }
        if let Some(name) = &self.length_from {
            validate_identifier(name)?;
        }
        if let Some(name) = &self.length_of {
            validate_identifier(name)?;
        }
        if let Some(name) = &self.message {
            validate_identifier(name)?;
        }
        if let Some(name) = &self.count_from {
            validate_identifier(name)?;
        }
        if let Some(name) = &self.count_of {
            validate_identifier(name)?;
        }
        if let Some(name) = &self.select_from {
            validate_identifier(name)?;
        }
        let mut case_values = HashSet::new();
        let mut cases = Vec::with_capacity(self.cases.len());
        for case in self.cases {
            validate_identifier(&case.message)?;
            if !case_values.insert(case.value) {
                return Err(Diagnostic::new(
                    DOCUMENT,
                    format!(
                        "choice field `{}` repeats selector value {}",
                        self.name, case.value
                    ),
                ));
            }
            cases.push(ChoiceCase {
                value: case.value,
                message: case.message,
                location: DOCUMENT,
            });
        }

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
            length_from: self.length_from,
            length_of: self.length_of,
            message: self.message,
            count_from: self.count_from,
            count_of: self.count_of,
            select_from: self.select_from,
            cases,
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
                    value: match value {
                        SourceAssignmentValue::Unsigned(value) => AssignmentValue::Unsigned(value),
                        SourceAssignmentValue::Bytes(value) => AssignmentValue::Bytes(value),
                    },
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
    if matches!(field_type, FieldType::Bytes) {
        return Err(Diagnostic::new(
            location,
            "a byte field requires an octet array",
        ));
    }
    if matches!(field_type, FieldType::Sdnv) {
        return Ok(());
    }
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
