use std::collections::{HashSet, VecDeque};

use crate::lexer::{lex, Statement, Token};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Location {
    pub line: usize,
    pub column: usize,
}

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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ByteOrder {
    BigEndian,
    LittleEndian,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
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

pub fn parse(text: &str) -> Result<Protocol> {
    Parser::new(lex(text)?, text.lines().count() + 1).parse()
}

struct Parser {
    statements: VecDeque<Statement>,
    eof: Location,
}

impl Parser {
    fn new(statements: Vec<Statement>, eof_line: usize) -> Self {
        Self {
            statements: statements.into(),
            eof: Location {
                line: eof_line,
                column: 1,
            },
        }
    }

    fn parse(mut self) -> Result<Protocol> {
        self.parse_version()?;
        let (name, location) = self.parse_named_statement("protocol")?;
        let description = self.parse_string_statement("description")?;
        let standard = self.parse_optional_string_statement("standard")?;
        let reference = self.parse_optional_string_statement("reference")?;
        let byte_order = self.parse_byte_order()?;
        let bit_order = self.parse_bit_order()?;

        let mut messages = Vec::new();
        let mut names = HashSet::new();

        while self.peek_keyword() != Some("end-protocol") {
            if self.peek().is_none() {
                return Err(Diagnostic::new(
                    self.eof,
                    "expected `message` or `end-protocol`",
                ));
            }
            let message = self.parse_message()?;
            if !names.insert(message.name.clone()) {
                return Err(Diagnostic::new(
                    message.location,
                    format!("duplicate message `{}`", message.name),
                ));
            }
            messages.push(message);
        }

        if messages.is_empty() {
            return Err(Diagnostic::new(
                self.peek().map_or(self.eof, Statement::location),
                "a protocol must contain at least one message",
            ));
        }

        self.parse_bare_statement("end-protocol")?;
        if let Some(statement) = self.peek() {
            return Err(Diagnostic::new(
                statement.location(),
                "unexpected statement after `end-protocol`",
            ));
        }

        Ok(Protocol {
            name,
            location,
            description,
            standard,
            reference,
            byte_order,
            bit_order,
            messages,
        })
    }

    fn parse_version(&mut self) -> Result<()> {
        let statement = self.take_expected("wire-format")?;
        if statement.tokens.len() != 2
            || statement.tokens[1].quoted
            || statement.tokens[1].text != "1"
        {
            return Err(Diagnostic::new(
                statement.location(),
                "the first statement must be exactly `wire-format 1`",
            ));
        }
        Ok(())
    }

    fn parse_message(&mut self) -> Result<Message> {
        let (name, location) = self.parse_named_statement("message")?;
        let description = self.parse_string_statement("description")?;
        let mut fields = Vec::new();
        let mut field_names = HashSet::new();

        while matches!(self.peek_keyword(), Some("field" | "constant")) {
            let field = self.parse_field()?;
            if !field_names.insert(field.name.clone()) {
                return Err(Diagnostic::new(
                    field.location,
                    format!("duplicate field `{}`", field.name),
                ));
            }
            fields.push(field);
        }

        if fields.is_empty() {
            return Err(Diagnostic::new(
                self.peek().map_or(self.eof, Statement::location),
                format!("message `{name}` must contain at least one field"),
            ));
        }

        let mut vectors = Vec::new();
        let mut vector_names = HashSet::new();
        while self.peek_keyword() == Some("vector") {
            let vector = self.parse_vector()?;
            if !vector_names.insert(vector.name.clone()) {
                return Err(Diagnostic::new(
                    vector.location,
                    format!("duplicate vector `{}`", vector.name),
                ));
            }
            vectors.push(vector);
        }

        self.parse_bare_statement("end-message")?;
        Ok(Message {
            name,
            description,
            location,
            fields,
            vectors,
        })
    }

    fn parse_field(&mut self) -> Result<Field> {
        let statement = self.take().ok_or_else(|| {
            Diagnostic::new(self.eof, "expected a `field` or `constant` declaration")
        })?;
        let location = statement.location();
        let keyword = statement.keyword();
        let is_constant = keyword == "constant";

        if statement.tokens[0].quoted || (keyword != "field" && !is_constant) {
            return Err(Diagnostic::new(
                location,
                "expected a `field` or `constant` declaration",
            ));
        }
        if statement.tokens.len() < 3 {
            return Err(Diagnostic::new(
                location,
                format!("incomplete `{keyword}` declaration"),
            ));
        }

        let name_token = &statement.tokens[1];
        validate_identifier(name_token)?;
        let (field_type, next) = parse_field_type(&statement.tokens, 2)?;

        let kind = if is_constant {
            if statement.tokens.len() != next + 2
                || statement.tokens[next].quoted
                || statement.tokens[next].text != "="
            {
                return Err(Diagnostic::new(
                    location,
                    "a constant declaration is `constant name type = value`",
                ));
            }
            let value = parse_integer(&statement.tokens[next + 1])?;
            ensure_value_fits(value, field_type, statement.tokens[next + 1].location)?;
            FieldKind::Constant(value)
        } else {
            if statement.tokens.len() != next {
                return Err(Diagnostic::new(
                    statement.tokens[next].location,
                    "unexpected token after field type",
                ));
            }
            FieldKind::Supplied
        };

        Ok(Field {
            name: name_token.text.clone(),
            location: name_token.location,
            field_type,
            kind,
        })
    }

    fn parse_vector(&mut self) -> Result<Vector> {
        let (name, location) = self.parse_named_statement("vector")?;
        let mut assignments = Vec::new();
        let mut wire = Vec::new();
        let mut saw_wire = false;

        loop {
            let Some(statement) = self.peek() else {
                return Err(Diagnostic::new(
                    self.eof,
                    format!("vector `{name}` is missing `end-vector`"),
                ));
            };

            if statement.is_keyword("end-vector") {
                break;
            }
            if statement.is_keyword("wire") {
                saw_wire = true;
                let statement = self.take().expect("peeked statement must exist");
                if statement.tokens.len() == 1 {
                    return Err(Diagnostic::new(
                        statement.location(),
                        "a `wire` statement must contain at least one octet",
                    ));
                }
                for token in &statement.tokens[1..] {
                    wire.push(WireByte {
                        location: token.location,
                        value: parse_hex_octet(token)?,
                    });
                }
            } else {
                let statement = self.take().expect("peeked statement must exist");
                if saw_wire {
                    return Err(Diagnostic::new(
                        statement.location(),
                        "field assignments must precede `wire` statements",
                    ));
                }
                if statement.tokens.len() != 3
                    || statement.tokens[1].quoted
                    || statement.tokens[1].text != "="
                {
                    return Err(Diagnostic::new(
                        statement.location(),
                        "a vector assignment is `field-name = value`",
                    ));
                }
                validate_identifier(&statement.tokens[0])?;
                assignments.push(Assignment {
                    name: statement.tokens[0].text.clone(),
                    location: statement.tokens[0].location,
                    value: parse_integer(&statement.tokens[2])?,
                });
            }
        }

        if !saw_wire {
            return Err(Diagnostic::new(
                location,
                format!("vector `{name}` must contain at least one `wire` statement"),
            ));
        }

        self.parse_bare_statement("end-vector")?;
        Ok(Vector {
            name,
            location,
            assignments,
            wire,
        })
    }

    fn parse_byte_order(&mut self) -> Result<ByteOrder> {
        let statement = self.take_expected("byte-order")?;
        if statement.tokens.len() != 2 {
            return Err(Diagnostic::new(
                statement.location(),
                "`byte-order` requires exactly one value",
            ));
        }
        if statement.tokens[1].quoted {
            return Err(Diagnostic::new(
                statement.tokens[1].location,
                "byte order may not be quoted",
            ));
        }
        match statement.tokens[1].text.as_str() {
            "big-endian" => Ok(ByteOrder::BigEndian),
            "little-endian" => Ok(ByteOrder::LittleEndian),
            _ => Err(Diagnostic::new(
                statement.tokens[1].location,
                "byte order must be `big-endian` or `little-endian`",
            )),
        }
    }

    fn parse_bit_order(&mut self) -> Result<BitOrder> {
        let statement = self.take_expected("bit-order")?;
        if statement.tokens.len() != 2 {
            return Err(Diagnostic::new(
                statement.location(),
                "`bit-order` requires exactly one value",
            ));
        }
        if statement.tokens[1].quoted {
            return Err(Diagnostic::new(
                statement.tokens[1].location,
                "bit order may not be quoted",
            ));
        }
        match statement.tokens[1].text.as_str() {
            "msb-first" => Ok(BitOrder::MsbFirst),
            "lsb-first" => Ok(BitOrder::LsbFirst),
            _ => Err(Diagnostic::new(
                statement.tokens[1].location,
                "bit order must be `msb-first` or `lsb-first`",
            )),
        }
    }

    fn parse_named_statement(&mut self, keyword: &str) -> Result<(String, Location)> {
        let statement = self.take_expected(keyword)?;
        if statement.tokens.len() != 2 {
            return Err(Diagnostic::new(
                statement.location(),
                format!("`{keyword}` requires exactly one identifier"),
            ));
        }
        validate_identifier(&statement.tokens[1])?;
        Ok((
            statement.tokens[1].text.clone(),
            statement.tokens[1].location,
        ))
    }

    fn parse_string_statement(&mut self, keyword: &str) -> Result<String> {
        let statement = self.take_expected(keyword)?;
        if statement.tokens.len() != 2 || !statement.tokens[1].quoted {
            return Err(Diagnostic::new(
                statement.location(),
                format!("`{keyword}` requires exactly one quoted string"),
            ));
        }
        Ok(statement.tokens[1].text.clone())
    }

    fn parse_optional_string_statement(&mut self, keyword: &str) -> Result<Option<String>> {
        if self.peek_keyword() == Some(keyword) {
            self.parse_string_statement(keyword).map(Some)
        } else {
            Ok(None)
        }
    }

    fn parse_bare_statement(&mut self, keyword: &str) -> Result<()> {
        let statement = self.take_expected(keyword)?;
        if statement.tokens.len() != 1 {
            return Err(Diagnostic::new(
                statement.location(),
                format!("`{keyword}` takes no arguments"),
            ));
        }
        Ok(())
    }

    fn take_expected(&mut self, keyword: &str) -> Result<Statement> {
        let statement = self.take().ok_or_else(|| {
            Diagnostic::new(self.eof, format!("expected `{keyword}`, found end of file"))
        })?;
        if !statement.is_keyword(keyword) {
            return Err(Diagnostic::new(
                statement.location(),
                format!("expected `{keyword}`, found `{}`", statement.keyword()),
            ));
        }
        Ok(statement)
    }

    fn peek(&self) -> Option<&Statement> {
        self.statements.front()
    }

    fn peek_keyword(&self) -> Option<&str> {
        self.peek().map(Statement::keyword)
    }

    fn take(&mut self) -> Option<Statement> {
        self.statements.pop_front()
    }
}

fn parse_field_type(tokens: &[Token], position: usize) -> Result<(FieldType, usize)> {
    let token = &tokens[position];
    if token.quoted {
        return Err(Diagnostic::new(
            token.location,
            "a field type may not be quoted",
        ));
    }
    let field_type = match token.text.as_str() {
        "bits" => {
            let width_token = tokens.get(position + 1).ok_or_else(|| {
                Diagnostic::new(token.location, "`bits` requires a width from 1 through 64")
            })?;
            let width = parse_integer(width_token)?;
            if !(1..=64).contains(&width) {
                return Err(Diagnostic::new(
                    width_token.location,
                    "bit width must be from 1 through 64",
                ));
            }
            return Ok((FieldType::Bits(width as u8), position + 2));
        }
        "u8" => FieldType::U8,
        "u16" => FieldType::U16,
        "u32" => FieldType::U32,
        "u64" => FieldType::U64,
        _ => {
            return Err(Diagnostic::new(
                token.location,
                "field type must be `bits N`, `u8`, `u16`, `u32`, or `u64`",
            ));
        }
    };
    Ok((field_type, position + 1))
}

fn validate_identifier(token: &Token) -> Result<()> {
    if token.quoted {
        return Err(Diagnostic::new(
            token.location,
            "an identifier may not be quoted",
        ));
    }

    let bytes = token.text.as_bytes();
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
            token.location,
            format!("invalid identifier `{}`", token.text),
        ))
    }
}

fn parse_integer(token: &Token) -> Result<u64> {
    if token.quoted {
        return Err(Diagnostic::new(
            token.location,
            "an integer may not be quoted",
        ));
    }

    let (digits, radix) = token
        .text
        .strip_prefix("0x")
        .map_or((token.text.as_str(), 10), |rest| (rest, 16));
    let valid_digit = |byte: u8| match radix {
        10 => byte.is_ascii_digit(),
        16 => byte.is_ascii_hexdigit(),
        _ => false,
    };
    let bytes = digits.as_bytes();
    let valid = !bytes.is_empty()
        && valid_digit(bytes[0])
        && valid_digit(bytes[bytes.len() - 1])
        && bytes.iter().all(|byte| valid_digit(*byte) || *byte == b'_')
        && !bytes.windows(2).any(|pair| pair == b"__");

    if !valid {
        return Err(Diagnostic::new(
            token.location,
            format!("invalid unsigned integer `{}`", token.text),
        ));
    }

    let compact: String = digits
        .chars()
        .filter(|character| *character != '_')
        .collect();
    u64::from_str_radix(&compact, radix).map_err(|_| {
        Diagnostic::new(
            token.location,
            format!(
                "integer `{}` is larger than an unsigned 64-bit value",
                token.text
            ),
        )
    })
}

fn parse_hex_octet(token: &Token) -> Result<u8> {
    if token.quoted
        || token.text.len() != 2
        || !token.text.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return Err(Diagnostic::new(
            token.location,
            format!(
                "wire octet `{}` must be exactly two hexadecimal digits",
                token.text
            ),
        ));
    }

    u8::from_str_radix(&token.text, 16)
        .map_err(|_| Diagnostic::new(token.location, "invalid wire octet"))
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
# comments and blank lines are ignored
wire-format 1
protocol example
description "a # remains in the string"
standard "Example 1"
reference "local"
byte-order big-endian
bit-order msb-first
message status
description "status\nrecord"
field version bits 3
constant reserved bits 1 = 0
field mode bits 4
field count u16
vector nominal
count = 0x12_34
version = 1
mode = 2
wire 22 12 34
end-vector
end-message
end-protocol
"#;

    #[test]
    fn parses_complete_source() {
        let protocol = parse(EXAMPLE).unwrap();
        assert_eq!(protocol.name, "example");
        assert_eq!(protocol.description, "a # remains in the string");
        assert_eq!(protocol.standard.as_deref(), Some("Example 1"));
        assert_eq!(protocol.reference.as_deref(), Some("local"));
        assert_eq!(protocol.messages.len(), 1);
        assert_eq!(protocol.messages[0].description, "status\nrecord");
        assert_eq!(protocol.messages[0].fields.len(), 4);
        assert_eq!(protocol.messages[0].vectors[0].wire.len(), 3);
    }

    #[test]
    fn parses_layout_for_the_checker_to_validate() {
        let source = EXAMPLE.replace("field version bits 3", "field version bits 2");
        let protocol = parse(&source).unwrap();
        assert_eq!(
            protocol.messages[0].fields[0].field_type,
            FieldType::Bits(2)
        );
    }

    #[test]
    fn rejects_duplicate_field_names() {
        let source = EXAMPLE.replace("field mode bits 4", "field version bits 4");
        let error = parse(&source).unwrap_err();
        assert!(error.message.contains("duplicate field `version`"));
    }

    #[test]
    fn rejects_bad_integer_separators() {
        let source = EXAMPLE.replace("0x12_34", "0x_1234");
        let error = parse(&source).unwrap_err();
        assert!(error.message.contains("invalid unsigned integer"));
    }

    #[test]
    fn rejects_quoted_language_tokens() {
        for source in [
            EXAMPLE.replace("wire-format 1", "\"wire-format\" 1"),
            EXAMPLE.replace("wire-format 1", "wire-format \"1\""),
            EXAMPLE.replace("byte-order big-endian", "byte-order \"big-endian\""),
            EXAMPLE.replace("field count u16", "field count \"u16\""),
            EXAMPLE.replace("wire 22 12 34", "\"wire\" 22 12 34"),
        ] {
            assert!(parse(&source).is_err());
        }
    }
}
