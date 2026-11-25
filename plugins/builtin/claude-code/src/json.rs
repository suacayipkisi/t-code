//! Just enough JSON to read claude's stream-json output: a value tree, parsed from one line.

#[derive(Debug, Clone, PartialEq, Default)]
pub enum Json {
    #[default]
    Null,
    Bool(bool),
    Number(f64),
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

static NULL: Json = Json::Null;

impl Json {
    /// The member called key of an object; Null when missing or not an object.
    pub fn get(&self, key: &str) -> &Json {
        match self {
            Json::Object(members) => members.iter().find(|(k, _)| k == key).map(|(_, v)| v).unwrap_or(&NULL),
            _ => &NULL,
        }
    }

    pub fn str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn number(&self) -> Option<f64> {
        match self {
            Json::Number(n) => Some(*n),
            _ => None,
        }
    }

    pub fn array(&self) -> &[Json] {
        match self {
            Json::Array(items) => items,
            _ => &[],
        }
    }

    pub fn is_true(&self) -> bool {
        matches!(self, Json::Bool(true))
    }
}

/// The JSON value text holds (whitespace around it allowed); None when it is not JSON.
pub fn parse(text: &str) -> Option<Json> {
    let mut p = Parser { bytes: text.as_bytes(), text, at: 0 };
    let value = p.value(0)?;
    p.space();
    (p.at == p.bytes.len()).then_some(value)
}

struct Parser<'a> {
    bytes: &'a [u8],
    text: &'a str,
    at: usize,
}

impl Parser<'_> {
    fn space(&mut self) {
        while self.bytes.get(self.at).is_some_and(|b| b.is_ascii_whitespace()) {
            self.at += 1;
        }
    }

    fn eat(&mut self, word: &str) -> bool {
        if self.text[self.at..].starts_with(word) {
            self.at += word.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self, depth: usize) -> Option<Json> {
        if depth > 200 {
            return None;
        }
        self.space();
        match *self.bytes.get(self.at)? {
            b'{' => {
                self.at += 1;
                let mut members = Vec::new();
                self.space();
                if self.eat("}") {
                    return Some(Json::Object(members));
                }
                loop {
                    self.space();
                    let key = self.string()?;
                    self.space();
                    if !self.eat(":") {
                        return None;
                    }
                    members.push((key, self.value(depth + 1)?));
                    self.space();
                    if self.eat("}") {
                        return Some(Json::Object(members));
                    }
                    if !self.eat(",") {
                        return None;
                    }
                }
            }
            b'[' => {
                self.at += 1;
                let mut items = Vec::new();
                self.space();
                if self.eat("]") {
                    return Some(Json::Array(items));
                }
                loop {
                    items.push(self.value(depth + 1)?);
                    self.space();
                    if self.eat("]") {
                        return Some(Json::Array(items));
                    }
                    if !self.eat(",") {
                        return None;
                    }
                }
            }
            b'"' => self.string().map(Json::Str),
            b't' => self.eat("true").then_some(Json::Bool(true)),
            b'f' => self.eat("false").then_some(Json::Bool(false)),
            b'n' => self.eat("null").then_some(Json::Null),
            _ => {
                let start = self.at;
                while self.bytes.get(self.at).is_some_and(|b| b"+-.eE0123456789".contains(b)) {
                    self.at += 1;
                }
                self.text[start..self.at].parse().ok().map(Json::Number)
            }
        }
    }

    fn hex4(&mut self) -> Option<u32> {
        let hex = self.text.get(self.at..self.at + 4)?;
        self.at += 4;
        u32::from_str_radix(hex, 16).ok()
    }

    fn string(&mut self) -> Option<String> {
        if !self.eat("\"") {
            return None;
        }
        let mut out = String::new();
        loop {
            let start = self.at;
            while self.bytes.get(self.at).is_some_and(|&b| b != b'"' && b != b'\\') {
                self.at += 1;
            }
            out.push_str(&self.text[start..self.at]);
            match *self.bytes.get(self.at)? {
                b'"' => {
                    self.at += 1;
                    return Some(out);
                }
                _ => {
                    self.at += 1;
                    let escape = *self.bytes.get(self.at)?;
                    self.at += 1;
                    match escape {
                        b'n' => out.push('\n'),
                        b't' => out.push('\t'),
                        b'r' => out.push('\r'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'u' => {
                            let mut code = self.hex4()?;
                            if (0xD800..0xDC00).contains(&code) && self.eat("\\u") {
                                let low = self.hex4()?;
                                code = 0x10000 + ((code - 0xD800) << 10) + (low.checked_sub(0xDC00)?);
                            }
                            out.push(char::from_u32(code).unwrap_or('\u{fffd}'));
                        }
                        other => out.push(other as char),
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values() {
        let v = parse(r#" {"a":[1,-2.5e1,true,null,"x\"\nç🙂"],"b":{"c":"d"},"e":{}} "#).unwrap();
        assert_eq!(v.get("a").array()[1].number(), Some(-25.0));
        assert_eq!(v.get("a").array()[4].str(), Some("x\"\nç🙂"));
        assert!(v.get("a").array()[2].is_true());
        assert_eq!(v.get("b").get("c").str(), Some("d"));
        assert_eq!(v.get("missing").get("deeper"), &Json::Null);
        assert_eq!(parse("[]"), Some(Json::Array(vec![])));
    }

    #[test]
    fn not_json() {
        for text in ["", "{", "[1,]", "{\"a\" 1}", "nul", "\"open", "[1] 2", "not json"] {
            assert_eq!(parse(text), None, "{text}");
        }
    }
}
