//! A small client for t-code's plugin socket (tapi_1), Rust's standard library only, and the bits
//! of JSON the plugin needs. See t-code's docs/plugins.md for the protocol.

use std::io::{self, BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;

pub struct Tcode {
    reader: BufReader<UnixStream>,
    writer: UnixStream,
}

impl Tcode {
    /// A tapi_1 connection, said hello to with the plugin's token.
    pub fn connect(socket: &str, token: &str) -> io::Result<Tcode> {
        let stream = UnixStream::connect(socket)?;
        let mut t = Tcode { reader: BufReader::new(stream.try_clone()?), writer: stream };
        t.request(format!("hello tapi_1 {token}").trim_end())?;
        Ok(t)
    }

    /// One request, ended by a NUL (an argument may hold line breaks); the value after "ok".
    pub fn request(&mut self, request: &str) -> io::Result<String> {
        self.writer.write_all(request.as_bytes())?;
        self.writer.write_all(b"\0")?;
        let line = self.line()?.ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "t-code went away"))?;
        if line == "ok" {
            return Ok(String::new());
        }
        match line.strip_prefix("ok ") {
            Some(value) => Ok(value.to_string()),
            None => Err(io::Error::other(line)),
        }
    }

    /// The text after get-text's or get-selection's "ok", up to its NUL byte.
    pub fn until_nul(&mut self) -> io::Result<String> {
        let mut text = Vec::new();
        if self.reader.read_until(0, &mut text)? == 0 || text.pop() != Some(0) {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "t-code went away"));
        }
        String::from_utf8(text).map_err(io::Error::other)
    }

    /// The next line without its line break; None when t-code went away.
    pub fn line(&mut self) -> io::Result<Option<String>> {
        let mut line = String::new();
        if self.reader.read_line(&mut line)? == 0 {
            return Ok(None);
        }
        Ok(Some(line.trim_end_matches('\n').to_string()))
    }
}

/// A JSON string.
pub fn quote(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// The JSON string starting at text[*at] (its quote), unescaped; *at ends past its closing quote.
fn string_at(text: &str, at: &mut usize) -> Option<String> {
    let bytes = text.as_bytes();
    if bytes.get(*at) != Some(&b'"') {
        return None;
    }
    let mut out = String::new();
    let mut chars = text[*at + 1..].char_indices();
    while let Some((i, c)) = chars.next() {
        match c {
            '"' => {
                *at += 1 + i + 1;
                return Some(out);
            }
            '\\' => match chars.next()?.1 {
                'n' => out.push('\n'),
                't' => out.push('\t'),
                'r' => out.push('\r'),
                'b' => out.push('\u{8}'),
                'f' => out.push('\u{c}'),
                'u' => {
                    let hex: String = (0..4).filter_map(|_| chars.next().map(|(_, c)| c)).collect();
                    let mut code = u32::from_str_radix(&hex, 16).ok()?;
                    if (0xD800..0xDC00).contains(&code) {
                        let rest = chars.as_str();
                        if let Some(low) = rest.strip_prefix("\\u").and_then(|r| r.get(..4)) {
                            let low = u32::from_str_radix(low, 16).ok()?;
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            for _ in 0..6 {
                                chars.next();
                            }
                        }
                    }
                    out.push(char::from_u32(code)?);
                }
                other => out.push(other),
            },
            c => out.push(c),
        }
    }
    None
}

/// The string member called name of an event line (a flat JSON object); "" when missing.
pub fn member(json: &str, name: &str) -> String {
    let key = quote(name);
    let mut from = 0;
    while let Some(found) = json[from..].find(&key) {
        let mut at = from + found;
        // a key starts after "{" or ",", not inside a value
        let before = json[..at].trim_end();
        let is_key = before.ends_with('{') || before.ends_with(',');
        at += key.len();
        let rest = json[at..].trim_start();
        if is_key && rest.starts_with(':') {
            let mut value_at = json.len() - rest.len() + 1;
            while json.as_bytes().get(value_at).is_some_and(|b| b.is_ascii_whitespace()) {
                value_at += 1;
            }
            return string_at(json, &mut value_at).unwrap_or_default();
        }
        from = at;
    }
    String::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn members() {
        let event = r#"{"event":"did_save","path":"/p/a \"b\".tex","by":"plugin"}"#;
        assert_eq!(member(event, "event"), "did_save");
        assert_eq!(member(event, "path"), "/p/a \"b\".tex");
        assert_eq!(member(event, "by"), "plugin");
        assert_eq!(member(event, "none"), "");
        assert_eq!(member(r#"{"path":"x\"event\":\"no","event":"yes"}"#, "event"), "yes");
        assert_eq!(member(r#"{"path": "ç🙂"}"#, "path"), "ç🙂");
    }

    #[test]
    fn quoting() {
        assert_eq!(quote("a\"b\\c\nd\u{1}"), "\"a\\\"b\\\\c\\nd\\u0001\"");
    }
}
