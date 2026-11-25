//! What the chat needs that does not talk to t-code: claude's command line for each mode, the
//! stream-json lines it writes turned into what the panel shows, and the lines written back to it
//! (the user's messages, the answers to its permission questions).

use crate::claude::{relative, tool_detail};
use crate::json::{self, Json};
use crate::tcode::quote;

/// The modes, in the order the panel lists them; the first is picked at first.
pub const MODES: [&str; 3] = ["Ask", "Read", "Edit"];

/// What the panel says before the first message.
pub const WELCOME: &str = "**Claude Code** in t-code. Pick a mode beside the box:\n\n\
- **Ask**: answers from what it knows and what you write, without looking at files.\n\
- **Read**: reads the file open in the editor, and other files when the question needs them.\n\
- **Edit**: reads and changes files.\n\n\
Commands, and anything your Claude Code settings do not allow already, ask you first.";

const PROMPT: &str = "You answer in a side panel of the t-code editor, which shows Markdown. Name places in the \
code as path:line, relative to the project folder.";

const READ_PROMPT: &str = " Start from the file open in the editor; read other files only when the question \
needs them.";

const ASK_PROMPT: &str = " You have no tools in this mode and cannot read files: answer from the conversation. When \
the answer needs a file's content, say so in one sentence and suggest the Read mode.";

/// claude's arguments for a conversation in mode ("Ask", "Read" or "Edit"), resuming session when
/// there is one.
pub fn arguments(mode: &str, session: Option<&str>) -> Vec<String> {
    let mut args: Vec<String> = [
        "-p",
        "--input-format",
        "stream-json",
        "--output-format",
        "stream-json",
        "--verbose",
        "--include-partial-messages",
        "--permission-prompt-tool",
        "stdio",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    let mut prompt = PROMPT.to_string();
    match mode {
        "Edit" => args.extend(["--permission-mode".into(), "acceptEdits".into()]),
        "Read" => {
            args.extend(["--tools".into(), "Read,Grep,Glob".into()]);
            prompt += READ_PROMPT;
        }
        _ => {
            args.extend(["--tools".into(), String::new()]);
            prompt += ASK_PROMPT;
        }
    }
    args.extend(["--append-system-prompt".into(), prompt]);
    if let Some(id) = session {
        args.extend(["--resume".into(), id.to_string()]);
    }
    args
}

/// The line that sends text to claude as the user's message.
pub fn user_message(text: &str) -> String {
    format!(r#"{{"type":"user","message":{{"role":"user","content":{}}}}}"#, quote(text))
}

/// The line that answers claude's permission question request: allowed with its input as it was,
/// or denied.
pub fn permission(request: &str, allow: bool, input: &Json) -> String {
    let answer = if allow {
        format!(r#"{{"behavior":"allow","updatedInput":{}}}"#, input.write())
    } else {
        r#"{"behavior":"deny","message":"The user said no in t-code."}"#.to_string()
    };
    format!(
        r#"{{"type":"control_response","response":{{"subtype":"success","request_id":{},"response":{answer}}}}}"#,
        quote(request)
    )
}

/// The user's message as the transcript shows it: a rule, who and in which mode, the text quoted.
pub fn user_block(text: &str, mode: &str) -> String {
    let quoted: Vec<String> = text.lines().map(|line| format!("> {line}")).collect();
    format!("\n\n---\n\n**You** · {mode}\n\n{}\n\n", quoted.join("\n"))
}

/// What the panel asks when claude wants to use a tool.
pub fn question(tool: &str, input: &Json) -> String {
    match tool_detail(input) {
        Some(detail) => format!("Claude wants to use {tool}:\n{detail}"),
        None => format!("Claude wants to use {tool}."),
    }
}

/// What a line of claude's output means for the panel.
#[derive(Debug, PartialEq)]
pub enum Action {
    /// Text to add to the transcript.
    Text(String),
    /// A permission question: its request id, the tool and its input.
    Ask { request: String, tool: String, input: Json },
    /// claude's session id, to resume the conversation with.
    Session(String),
    /// The turn ended, with claude's error when it failed.
    Done(Option<String>),
}

/// Reads claude's stream-json lines: the answer as it is written (--include-partial-messages), a
/// line for each tool it uses, its questions and the end of each turn.
#[derive(Default)]
pub struct ChatStream {
    root: String,          // the project's folder: paths in tool lines are shown relative to it
    current: String,       // the message being streamed
    streamed: Vec<String>, // messages whose text came as deltas
    wrote: bool,           // text was added this turn: the next message starts a new paragraph
}

impl ChatStream {
    /// A stream for a conversation in root (tool lines name paths relative to it).
    pub fn new(root: &str) -> ChatStream {
        ChatStream { root: root.to_string(), ..Default::default() }
    }

    pub fn feed(&mut self, raw: &str) -> Vec<Action> {
        let Some(line) = json::parse(raw) else { return Vec::new() };
        let mut out = Vec::new();
        if let Some(id) = line.get("session_id").str().filter(|id| !id.is_empty()) {
            if matches!(line.get("type").str(), Some("system") | Some("result")) {
                out.push(Action::Session(id.to_string()));
            }
        }
        if line.get("parent_tool_use_id").str().is_some_and(|id| !id.is_empty()) {
            return out; // a subagent's own work
        }
        match line.get("type").str().unwrap_or("") {
            "stream_event" => {
                let event = line.get("event");
                match event.get("type").str().unwrap_or("") {
                    "message_start" => {
                        self.current = event.get("message").get("id").str().unwrap_or("").to_string();
                        if self.wrote {
                            out.push(Action::Text("\n\n".into()));
                            self.wrote = false;
                        }
                    }
                    "content_block_delta" if event.get("delta").get("type").str() == Some("text_delta") => {
                        if !self.streamed.contains(&self.current) {
                            self.streamed.push(self.current.clone());
                        }
                        let text = event.get("delta").get("text").str().unwrap_or("");
                        if !text.is_empty() {
                            self.wrote = true;
                            out.push(Action::Text(text.to_string()));
                        }
                    }
                    _ => {}
                }
            }
            "assistant" => {
                let message = line.get("message");
                let id = message.get("id").str().unwrap_or("").to_string();
                for block in message.get("content").array() {
                    match block.get("type").str() {
                        Some("text") if !self.streamed.contains(&id) => {
                            self.wrote = true;
                            out.push(Action::Text(block.get("text").str().unwrap_or("").to_string()));
                        }
                        Some("tool_use") => {
                            let name = block.get("name").str().unwrap_or("?");
                            let detail = tool_detail(block.get("input"))
                                .map(|d| format!(" {}", relative(&self.root, &d)))
                                .unwrap_or_default();
                            out.push(Action::Text(format!("\n\n`› {name}{}`\n\n", detail.replace('`', "'"))));
                            self.wrote = false;
                        }
                        _ => {}
                    }
                }
            }
            "control_request" if line.get("request").get("subtype").str() == Some("can_use_tool") => {
                let request = line.get("request");
                out.push(Action::Ask {
                    request: line.get("request_id").str().unwrap_or("").to_string(),
                    tool: request.get("tool_name").str().unwrap_or("?").to_string(),
                    input: request.get("input").clone(),
                });
            }
            "result" => {
                self.wrote = false;
                let error = line
                    .get("is_error")
                    .is_true()
                    .then(|| line.get("result").str().or(line.get("subtype").str()).unwrap_or("failed").to_string());
                out.push(Action::Done(error));
            }
            _ => {}
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn modes() {
        let ask = arguments("Ask", None);
        assert!(ask.windows(2).any(|w| w[0] == "--tools" && w[1].is_empty()));
        assert!(!ask.contains(&"--resume".to_string()));
        let read = arguments("Read", Some("s1"));
        assert!(read.windows(2).any(|w| w[0] == "--tools" && w[1] == "Read,Grep,Glob"));
        assert!(read.windows(2).any(|w| w[0] == "--resume" && w[1] == "s1"));
        let edit = arguments("Edit", None);
        assert!(edit.windows(2).any(|w| w[0] == "--permission-mode" && w[1] == "acceptEdits"));
        assert!(!edit.contains(&"--tools".to_string()));
        assert!(edit.windows(2).any(|w| w[0] == "--permission-prompt-tool" && w[1] == "stdio"));
    }

    #[test]
    fn lines_written() {
        assert_eq!(user_message("hi \"you\""), r#"{"type":"user","message":{"role":"user","content":"hi \"you\""}}"#);
        let input = json::parse(r#"{"command":"ls -l"}"#).unwrap();
        assert_eq!(
            permission("r1", true, &input),
            r#"{"type":"control_response","response":{"subtype":"success","request_id":"r1","response":{"behavior":"allow","updatedInput":{"command":"ls -l"}}}}"#
        );
        assert!(permission("r1", false, &input).contains(r#""behavior":"deny""#));
        assert_eq!(user_block("a\nb", "Read"), "\n\n---\n\n**You** · Read\n\n> a\n> b\n\n");
        assert_eq!(question("Bash", &input), "Claude wants to use Bash:\nls -l");
    }

    #[test]
    fn stream() {
        let mut s = ChatStream::new("/p");
        let lines = [
            r#"{"type":"system","subtype":"init","session_id":"s1"}"#,
            r#"{"type":"stream_event","event":{"type":"message_start","message":{"id":"m1"}}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"Hel"}}}"#,
            r#"{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"lo "}}}"#,
            r#"{"type":"assistant","message":{"id":"m1","content":[{"type":"text","text":"Hello "}]}}"#,
            r#"{"type":"assistant","message":{"id":"m1","content":[{"type":"tool_use","name":"Bash","input":{"command":"ls `x`"}}]}}"#,
            r#"{"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"command":"ls"}}}"#,
            r#"{"type":"stream_event","event":{"type":"message_start","message":{"id":"m2"}}}"#,
            r#"{"type":"assistant","message":{"id":"m2","content":[{"type":"tool_use","name":"Read","input":{"file_path":"/p/src/a.c"}}]}}"#,
            r#"{"type":"assistant","message":{"id":"m2","content":[{"type":"text","text":"Done."}]}}"#,
            r#"{"type":"assistant","message":{"id":"m3","content":[{"type":"text","text":"sub"}]},"parent_tool_use_id":"t1"}"#,
            r#"{"type":"result","subtype":"success","is_error":false,"result":"Done.","session_id":"s1"}"#,
            r#"{"type":"result","subtype":"error_during_execution","is_error":true,"session_id":"s1"}"#,
            "not json",
        ];
        let out: Vec<Action> = lines.iter().flat_map(|line| s.feed(line)).collect();
        assert_eq!(
            out,
            [
                Action::Session("s1".into()),
                Action::Text("Hel".into()),
                Action::Text("lo ".into()),
                Action::Text("\n\n`› Bash ls 'x'`\n\n".into()),
                Action::Ask {
                    request: "r1".into(),
                    tool: "Bash".into(),
                    input: json::parse(r#"{"command":"ls"}"#).unwrap()
                },
                Action::Text("\n\n`› Read src/a.c`\n\n".into()),
                Action::Text("Done.".into()),
                Action::Session("s1".into()),
                Action::Done(None),
                Action::Session("s1".into()),
                Action::Done(Some("error_during_execution".into())),
            ]
        );
    }
}
