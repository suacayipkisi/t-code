//! What does not talk to t-code: finding claude, reading its stream-json output, places in text,
//! and the small text helpers the commands share.

use crate::json::{self, Json};
use crate::tcode::quote;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

/// The claude program on PATH, or where its installers put it.
pub fn find_claude() -> Option<PathBuf> {
    let executable = |p: &Path| p.metadata().is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0);
    let path = std::env::var_os("PATH").unwrap_or_default();
    if let Some(found) = std::env::split_paths(&path).map(|dir| dir.join("claude")).find(|p| executable(p)) {
        return Some(found);
    }
    let home = PathBuf::from(std::env::var_os("HOME")?);
    [".local/bin/claude", ".claude/local/claude", ".npm-global/bin/claude"]
        .iter()
        .map(|candidate| home.join(candidate))
        .find(|p| executable(p))
}

/// path relative to root when it is inside it, else the path as it is.
pub fn relative(root: &str, path: &str) -> String {
    match Path::new(path).strip_prefix(root) {
        Ok(rel) if !root.is_empty() && !rel.as_os_str().is_empty() => rel.to_string_lossy().into_owned(),
        _ => path.to_string(),
    }
}

/// A selection: start line, column, end line, column (1-based; equal places: none).
pub type Range = [usize; 4];

/// Claude Code's reference to a file and its selected lines, "@src/main.c#L5-10". A selection that
/// ends at a line's first column does not take that line; no selection: the file alone.
pub fn mention(root: &str, file: &str, r: Range) -> String {
    let reference = format!("@{}", relative(root, file));
    let [l1, c1, mut l2, c2] = r;
    if l1 == l2 && c1 == c2 {
        return reference;
    }
    if l2 > l1 && c2 == 1 {
        l2 -= 1;
    }
    if l1 == l2 {
        format!("{reference}#L{l1}")
    } else {
        format!("{reference}#L{l1}-{l2}")
    }
}

/// s quoted for terminal-open's program line, which is split like a shell would.
pub fn shell_quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', r"'\''"))
}

/// The first "file:line[:column]" in text whose file exists (exists decides), relative paths taken
/// from root: "path:line[:column]" for t-code's open.
pub fn locate(root: &str, text: &str, exists: impl Fn(&Path) -> bool) -> Option<String> {
    let bytes = text.as_bytes();
    let digits = |from: usize| bytes[from..].iter().take_while(|b| b.is_ascii_digit()).count();
    for (colon, _) in text.match_indices(':') {
        let line_len = digits(colon + 1);
        if line_len == 0 {
            continue;
        }
        let start = text[..colon].rfind(|c: char| c.is_whitespace() || "([{<\"'`,;=".contains(c)).map_or(0, |i| i + 1);
        let name = &text[start..colon];
        let base = name.rsplit('/').next().unwrap_or("");
        let extension = base.rsplit_once(".");
        if !extension
            .is_some_and(|(_, ext)| !ext.is_empty() && ext.chars().all(|c| c.is_alphanumeric() || "_+-".contains(c)))
        {
            continue;
        }
        let path = if let Some(rest) = name.strip_prefix("~/") {
            PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(rest)
        } else {
            Path::new(root).join(name)
        };
        if !exists(&path) {
            continue;
        }
        let line_end = colon + 1 + line_len;
        let mut place = format!("{}:{}", path.display(), &text[colon + 1..line_end]);
        if bytes.get(line_end) == Some(&b':') && digits(line_end + 1) > 0 {
            place.push_str(&text[line_end..line_end + 1 + digits(line_end + 1)]);
        }
        return Some(place);
    }
    None
}

/// text with each line's number in front, so claude can name lines exactly.
pub fn numbered(text: &str) -> String {
    text.split('\n').enumerate().map(|(i, line)| format!("{:5}  {line}\n", i + 1)).collect()
}

/// The place after text inserted at line, column (1-based, columns in characters).
pub fn end_of(line: usize, column: usize, text: &str) -> (usize, usize) {
    match text.rsplit_once('\n') {
        None => (line, column + text.chars().count()),
        Some((before, last)) => (line + before.matches('\n').count() + 1, last.chars().count() + 1),
    }
}

/// claude's last stream-json line.
#[derive(Debug, Default)]
pub struct Outcome {
    pub is_error: bool,
    pub text: String,
    pub structured: Json,
    pub cost: f64,
}

/// Turns claude's --output-format stream-json lines into whole lines of text for a view: the
/// answer as it is written (with --include-partial-messages), one line per tool claude uses.
#[derive(Default)]
pub struct Stream {
    partial: String,
    current: String,       // the message being streamed
    streamed: Vec<String>, // messages whose text came as deltas
    pub outcome: Option<Outcome>,
}

impl Stream {
    /// Takes one line of claude's output; answers the lines it finished.
    pub fn feed(&mut self, raw: &str) -> Vec<String> {
        let Some(line) = json::parse(raw) else { return Vec::new() };
        if line.get("parent_tool_use_id").str().is_some_and(|id| !id.is_empty()) {
            return Vec::new(); // a subagent's own work
        }
        match line.get("type").str().unwrap_or("") {
            "stream_event" => {
                let event = line.get("event");
                match event.get("type").str().unwrap_or("") {
                    "message_start" => self.current = event.get("message").get("id").str().unwrap_or("").to_string(),
                    "content_block_delta" if event.get("delta").get("type").str() == Some("text_delta") => {
                        if !self.streamed.contains(&self.current) {
                            self.streamed.push(self.current.clone());
                        }
                        return self.add(event.get("delta").get("text").str().unwrap_or(""));
                    }
                    "content_block_stop" | "message_stop" => return self.flush(),
                    _ => {}
                }
                Vec::new()
            }
            "assistant" => {
                let message = line.get("message");
                let id = message.get("id").str().unwrap_or("").to_string();
                let mut out = Vec::new();
                for block in message.get("content").array() {
                    match block.get("type").str() {
                        Some("text") if !self.streamed.contains(&id) => {
                            out.extend(self.add(&format!("{}\n", block.get("text").str().unwrap_or(""))))
                        }
                        Some("tool_use") if block.get("name").str() != Some("StructuredOutput") => {
                            out.extend(self.flush());
                            out.push(tool_line(block.get("name").str().unwrap_or("?"), block.get("input")));
                        }
                        _ => {}
                    }
                }
                out
            }
            "result" => {
                self.outcome = Some(Outcome {
                    is_error: line.get("is_error").is_true(),
                    text: line.get("result").str().or(line.get("subtype").str()).unwrap_or("").to_string(),
                    structured: line.get("structured_output").clone(),
                    cost: line.get("total_cost_usd").number().unwrap_or(0.0),
                });
                self.flush()
            }
            _ => Vec::new(),
        }
    }

    fn add(&mut self, text: &str) -> Vec<String> {
        self.partial.push_str(text);
        let Some(cut) = self.partial.rfind('\n') else { return Vec::new() };
        let rest = self.partial.split_off(cut + 1);
        let done = std::mem::replace(&mut self.partial, rest);
        done[..cut].split('\n').map(str::to_string).collect()
    }

    /// The text written since the last line break, as a line.
    pub fn flush(&mut self) -> Vec<String> {
        if self.partial.is_empty() {
            Vec::new()
        } else {
            vec![std::mem::take(&mut self.partial)]
        }
    }
}

/// One line saying what claude looks at, "  › Read src/main.c".
pub fn tool_line(name: &str, input: &Json) -> String {
    for key in ["file_path", "pattern", "path", "command", "url", "query", "description"] {
        if let Some(value) = input.get(key).str().filter(|v| !v.is_empty()) {
            return match value.split_once('\n') {
                Some((first, _)) => format!("  › {name} {first} …"),
                None => format!("  › {name} {value}"),
            };
        }
    }
    format!("  › {name}")
}

/// One finding of a review.
#[derive(Debug, PartialEq)]
pub struct Issue {
    pub line: usize,
    pub end_line: usize,
    pub severity: String,
    pub message: String,
}

/// The --json-schema of a review.
pub const REVIEW_SCHEMA: &str = concat!(
    r#"{"type":"object","properties":{"issues":{"type":"array","items":{"type":"object","#,
    r#""properties":{"line":{"type":"integer","minimum":1},"end_line":{"type":"integer","minimum":1},"#,
    r#""severity":{"type":"string","enum":["error","warning","info","hint"]},"message":{"type":"string"}},"#,
    r#""required":["line","severity","message"]}}},"required":["issues"]}"#
);

/// The --json-schema of a rewrite.
pub const REWRITE_SCHEMA: &str = r#"{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}"#;

/// A review's issues, from its structured output or a JSON object in its text answer.
pub fn parse_review(structured: &Json, text: &str) -> Result<Vec<Issue>, String> {
    let from_text;
    let review = if *structured != Json::Null {
        structured
    } else {
        let (start, end) = (text.find('{'), text.rfind('}'));
        let (Some(start), Some(end)) = (start, end) else { return Err("claude answered no review".into()) };
        from_text = json::parse(text.get(start..=end).unwrap_or("")).ok_or("claude's review is not JSON")?;
        &from_text
    };
    let Json::Array(items) = review.get("issues") else { return Err("claude answered no review".into()) };
    let mut issues = Vec::new();
    for item in items {
        let line = item.get("line").number().unwrap_or(0.0) as usize;
        let message = item.get("message").str().unwrap_or("").trim();
        if line < 1 || message.is_empty() {
            continue;
        }
        let severity = item
            .get("severity")
            .str()
            .filter(|s| ["error", "warning", "info", "hint"].contains(s))
            .unwrap_or("warning");
        let end_line = (item.get("end_line").number().unwrap_or(0.0) as usize).max(line);
        issues.push(Issue { line, end_line, severity: severity.into(), message: message.into() });
    }
    Ok(issues)
}

/// diagnostics-set's argument: the issues over their whole lines.
pub fn diagnostics(path: &str, issues: &[Issue]) -> String {
    let items: Vec<String> = issues
        .iter()
        .map(|i| {
            format!(
                r#"{{"range":[{},1,{},1048576],"severity":{},"message":{},"source":"claude"}}"#,
                i.line,
                i.end_line,
                quote(&i.severity),
                quote(&i.message)
            )
        })
        .collect();
    format!(r#"{{"path":{},"items":[{}]}}"#, quote(path), items.join(","))
}

/// A rewrite's new text.
pub fn parse_rewrite(structured: &Json) -> Result<String, String> {
    structured.get("text").str().map(str::to_string).ok_or_else(|| "claude answered no text".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mentions() {
        let m = |r| mention("/p", "/p/src/a.c", r);
        assert_eq!(m([3, 4, 3, 4]), "@src/a.c");
        assert_eq!(m([3, 1, 3, 9]), "@src/a.c#L3");
        assert_eq!(m([3, 1, 7, 2]), "@src/a.c#L3-7");
        assert_eq!(m([3, 1, 8, 1]), "@src/a.c#L3-7");
        assert_eq!(m([3, 5, 4, 1]), "@src/a.c#L3");
        assert_eq!(mention("/p", "/elsewhere/b.c", [1, 1, 1, 1]), "@/elsewhere/b.c");
        assert_eq!(relative("/p", "/pp/x.c"), "/pp/x.c");
    }

    #[test]
    fn quoting() {
        assert_eq!(shell_quote("it's"), r"'it'\''s'");
    }

    #[test]
    fn places() {
        let exists = |p: &Path| p == Path::new("/p/src/main.c") || p == Path::new("/abs/x.go");
        let cases = [
            ("src/main.c:42: warning: leak", Some("/p/src/main.c:42")),
            ("see src/main.c:7:3 and more", Some("/p/src/main.c:7:3")),
            ("gone.c:3 then /abs/x.go:9", Some("/abs/x.go:9")),
            ("(src/main.c:12)", Some("/p/src/main.c:12")),
            ("no places here, version 1.2:3", None),
            ("— done ($0.0123)", None),
            ("Review of src/main.c: 2 finding(s)", None),
            ("ç src/main.c:5", Some("/p/src/main.c:5")),
        ];
        for (text, want) in cases {
            assert_eq!(locate("/p", text, exists).as_deref(), want, "{text}");
        }
    }

    #[test]
    fn ends_and_numbers() {
        assert_eq!(end_of(3, 5, "abç"), (3, 8));
        assert_eq!(end_of(3, 5, "a\nbç\n"), (5, 1));
        assert_eq!(end_of(3, 5, "a\nbçd"), (4, 4));
        assert_eq!(numbered("a\nb"), "    1  a\n    2  b\n");
    }

    #[test]
    fn stream() {
        let mut s = Stream::default();
        let lines = [
            r#"{"type":"system","subtype":"init"}"#,
            r#"{"type":"stream_event","event":{"type":"message_start","message":{"id":"m1"}}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"thinking_delta","thinking":"hm"}}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"Hel"}}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"lo\n\nsrc/a.c:3"}}}"#,
            r#"{"type":"assistant","message":{"id":"m1","content":[{"type":"text","text":"Hello\n\nsrc/a.c:3 is it"}]}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":" is it"}}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_stop"}}"#,
            r#"{"type":"assistant","message":{"id":"m2","content":[{"type":"tool_use","name":"Read","input":{"file_path":"/p/a.c"}}]}}"#,
            r#"{"type":"assistant","message":{"id":"m2","content":[{"type":"tool_use","name":"StructuredOutput","input":{"n":1}}]}}"#,
            r#"{"type":"assistant","message":{"id":"m3","content":[{"type":"text","text":"Not streamed"}]},"parent_tool_use_id":null}"#,
            r#"{"type":"assistant","message":{"id":"m4","content":[{"type":"text","text":"a subagent's"}]},"parent_tool_use_id":"toolu_1"}"#,
            "not json",
            r#"{"type":"result","subtype":"success","is_error":false,"result":"Hello","total_cost_usd":0.5,"structured_output":{"n":1}}"#,
        ];
        let out: Vec<String> = lines.iter().flat_map(|line| s.feed(line)).collect();
        assert_eq!(out, ["Hello", "", "src/a.c:3 is it", "  › Read /p/a.c", "Not streamed"]);
        let outcome = s.outcome.unwrap();
        assert_eq!(outcome.cost, 0.5);
        assert!(!outcome.is_error);
        assert_eq!(outcome.structured.get("n").number(), Some(1.0));
    }

    #[test]
    fn tool_lines() {
        assert_eq!(tool_line("Grep", &json::parse(r#"{"pattern":"foo\nbar"}"#).unwrap()), "  › Grep foo …");
        assert_eq!(tool_line("Odd", &Json::Null), "  › Odd");
    }

    #[test]
    fn reviews() {
        let structured = json::parse(concat!(
            r#"{"issues":[{"line":4,"severity":"error","message":"leak"},"#,
            r#"{"line":0,"severity":"error","message":"no line"},{"line":9,"end_line":11,"severity":"odd","message":"x \"y\""},"#,
            r#"{"line":5,"severity":"info","message":"  "}]}"#
        ))
        .unwrap();
        let issues = parse_review(&structured, "").unwrap();
        assert_eq!(
            issues,
            [
                Issue { line: 4, end_line: 4, severity: "error".into(), message: "leak".into() },
                Issue { line: 9, end_line: 11, severity: "warning".into(), message: "x \"y\"".into() },
            ]
        );
        let argument = diagnostics("/p/a.c", &issues);
        assert!(json::parse(&argument).is_some(), "{argument}");
        assert!(argument
            .contains(r#"{"range":[9,1,11,1048576],"severity":"warning","message":"x \"y\"","source":"claude"}"#));
        // without structured output: the JSON in the text answer
        assert_eq!(parse_review(&Json::Null, "Here:\n```json\n{\"issues\":[]}\n```"), Ok(vec![]));
        assert!(parse_review(&Json::Null, "no json").is_err());
    }

    #[test]
    fn rewrites() {
        assert_eq!(parse_rewrite(&json::parse(r#"{"text":"a\n  b"}"#).unwrap()), Ok("a\n  b".into()));
        assert_eq!(parse_rewrite(&json::parse(r#"{"text":""}"#).unwrap()), Ok(String::new()));
        assert!(parse_rewrite(&json::parse("{}").unwrap()).is_err());
    }
}
