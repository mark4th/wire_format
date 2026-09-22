use crate::source::{Diagnostic, Location, Result};

#[derive(Clone, Debug)]
pub struct Token {
    pub text: String,
    pub location: Location,
    pub quoted: bool,
}

#[derive(Debug)]
pub struct Statement {
    pub tokens: Vec<Token>,
}

impl Statement {
    pub fn location(&self) -> Location {
        self.tokens[0].location
    }

    pub fn keyword(&self) -> &str {
        &self.tokens[0].text
    }

    pub fn is_keyword(&self, keyword: &str) -> bool {
        !self.tokens[0].quoted && self.tokens[0].text == keyword
    }
}

pub fn lex(text: &str) -> Result<Vec<Statement>> {
    let mut statements = Vec::new();

    for (line_index, raw_line) in text.lines().enumerate() {
        let line_number = line_index + 1;
        let bytes = raw_line.as_bytes();
        let mut tokens = Vec::new();
        let mut index = 0;

        while index < bytes.len() {
            while index < bytes.len() && bytes[index].is_ascii_whitespace() {
                index += 1;
            }

            if index == bytes.len() || bytes[index] == b'#' {
                break;
            }

            let location = Location {
                line: line_number,
                column: index + 1,
            };

            if bytes[index] == b'=' {
                tokens.push(Token {
                    text: "=".to_owned(),
                    location,
                    quoted: false,
                });
                index += 1;
                continue;
            }

            if bytes[index] == b'"' {
                let (value, next) = lex_string(bytes, index, line_number)?;
                tokens.push(Token {
                    text: value,
                    location,
                    quoted: true,
                });
                index = next;
                continue;
            }

            let start = index;
            while index < bytes.len()
                && !bytes[index].is_ascii_whitespace()
                && bytes[index] != b'#'
                && bytes[index] != b'='
            {
                index += 1;
            }

            let value = std::str::from_utf8(&bytes[start..index])
                .map_err(|_| Diagnostic::new(location, "invalid UTF-8 outside a quoted string"))?;
            tokens.push(Token {
                text: value.to_owned(),
                location,
                quoted: false,
            });
        }

        if !tokens.is_empty() {
            statements.push(Statement { tokens });
        }
    }

    Ok(statements)
}

fn lex_string(bytes: &[u8], start: usize, line: usize) -> Result<(String, usize)> {
    let mut value = Vec::new();
    let mut index = start + 1;

    while index < bytes.len() {
        match bytes[index] {
            b'"' => {
                return String::from_utf8(value)
                    .map(|text| (text, index + 1))
                    .map_err(|_| {
                        Diagnostic::new(
                            Location {
                                line,
                                column: start + 1,
                            },
                            "quoted string is not valid UTF-8",
                        )
                    });
            }
            b'\\' => {
                let escape_column = index + 1;
                index += 1;
                if index == bytes.len() {
                    return Err(Diagnostic::new(
                        Location {
                            line,
                            column: escape_column,
                        },
                        "unfinished escape at the end of a string",
                    ));
                }
                match bytes[index] {
                    b'\\' => value.push(b'\\'),
                    b'"' => value.push(b'"'),
                    b'n' => value.push(b'\n'),
                    b't' => value.push(b'\t'),
                    other => {
                        return Err(Diagnostic::new(
                            Location {
                                line,
                                column: index + 1,
                            },
                            format!("unknown string escape `\\{}`", char::from(other)),
                        ));
                    }
                }
                index += 1;
            }
            byte => {
                value.push(byte);
                index += 1;
            }
        }
    }

    Err(Diagnostic::new(
        Location {
            line,
            column: start + 1,
        },
        "unterminated quoted string",
    ))
}
