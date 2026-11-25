//! Claude Code for t-code, in Rust (the standard library only). It needs the claude command,
//! installed and signed in: the plugin only puts it into t-code.
//!
//! - `chat`: shows the "Claude" panel on the right (its tab comes with the project): a
//!   conversation in Markdown, a box to write in and the mode beside it (Ask: no tools; Read: reads
//!   files; Edit: changes them). What claude wants to do that needs permission shows as a question
//!   with Allow and Deny.
//! - `send`: puts a reference to the selection ("@src/main.c#L5-10") into the chat's box.
//! - `ask`: puts a reference to the active file into the chat's box.
//! - `review`: has claude review the active buffer: its findings go to Problems (diagnostics) and
//!   to the "Claude Review" view at the bottom.
//! - `rewrite`: rewrites the selection as asked, as one undo step (`edit`), and selects the new text.
//! - `clear`: takes the review's diagnostics and its view away.
//! - `start` (with the project): adds the chat's tab, runs its conversations with claude, and
//!   opens the place ("src/main.c:42") a line of the review's view names when the line is activated.

mod chat;
mod claude;
mod json;
mod tcode;

use chat::{Action, ChatStream};
use claude::Range;
use json::Json;
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::{Arc, Mutex};
use tcode::{quote, Tcode};

const STATUS: &str = "claude";

const CHAT: &str = "chat"; // the panel's view id

fn env(name: &str) -> String {
    std::env::var(name).unwrap_or_default()
}

fn connect() -> io::Result<Tcode> {
    Tcode::connect(&env("TCODE_SOCKET"), &env("TCODE_PLUGIN_TOKEN"))
}

/// Whether e is t-code's "error <code> …" answer.
fn is_code(e: &io::Error, code: &str) -> bool {
    e.to_string().strip_prefix("error ").is_some_and(|rest| rest.split(' ').next() == Some(code))
}

/// The plugin works on, says things to and asks one t-code connection.
struct Plugin {
    t: Tcode,
    root: String,
}

impl Plugin {
    fn new() -> io::Result<Plugin> {
        let mut t = connect()?;
        let mut root = env("TCODE_PROJECT_ROOT");
        if root.is_empty() {
            root = t.request("get-project")?;
        }
        Ok(Plugin { t, root })
    }

    fn request(&mut self, command: &str, argument: &str) -> io::Result<String> {
        self.t.request(&format!("{command} {argument}"))
    }

    fn say(&mut self, text: &str) {
        let _ = self.request("say", &format!("Claude Code: {text}"));
    }

    fn status(&mut self, text: &str) {
        let _ = self.request("status-set", &format!("{STATUS} Claude: {text}"));
    }

    fn status_clear(&mut self) {
        let _ = self.request("status-clear", STATUS);
    }

    /// get-text or get-selection: "ok", then the text up to a NUL byte.
    fn text(&mut self, command: &str) -> io::Result<String> {
        self.t.request(command)?;
        self.t.until_nul()
    }

    /// The active editor's file; says so and answers None without one.
    fn active_file(&mut self) -> Option<String> {
        let file = self.request("get-file", "").unwrap_or_default();
        if file.is_empty() {
            self.say("open a file first");
            return None;
        }
        Some(file)
    }

    fn claude(&mut self) -> Option<PathBuf> {
        let found = claude::find_claude();
        if found.is_none() {
            self.say("the claude command is not installed (see claude.com/claude-code)");
        }
        found
    }

    /// One line from the user; None when they closed the picker or typed nothing.
    fn question(&mut self, prompt: &str) -> io::Result<Option<String>> {
        match self.request("ask", prompt) {
            Ok(answer) if !answer.trim().is_empty() => Ok(Some(answer.trim().to_string())),
            Ok(_) => Ok(None),
            Err(e) if is_code(&e, "canceled") => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// The selection's start and end (the cursor twice without one).
    fn selection_range(&mut self) -> io::Result<Range> {
        let answer = self.request("get-selection-range", "")?;
        let numbers: Vec<usize> = answer.split(' ').filter_map(|n| n.parse().ok()).collect();
        numbers.try_into().map_err(|_| io::Error::other(format!("odd get-selection-range answer {answer:?}")))
    }
}

/// Runs claude -p in root with the prompt on stdin and answers its outcome.
fn run_claude(claude: &Path, root: &str, prompt: &str, args: &[&str]) -> Result<claude::Outcome, String> {
    let mut child = Command::new(claude)
        .args(["-p", "--output-format", "stream-json", "--verbose"])
        .args(args)
        .current_dir(root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("claude did not start: {e}"))?;
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let prompt = prompt.to_string();
    std::thread::spawn(move || {
        let _ = stdin.write_all(prompt.as_bytes());
    });
    let errors = std::thread::spawn(move || {
        let mut text = String::new();
        let _ = io::Read::read_to_string(&mut stderr, &mut text);
        text
    });
    let mut stream = claude::Stream::default();
    for raw in BufReader::new(stdout).lines() {
        let Ok(raw) = raw else { break };
        stream.feed(&raw);
    }
    let status = child.wait();
    let errors = errors.join().unwrap_or_default();
    match stream.outcome {
        Some(outcome) if outcome.is_error => Err(format!("claude: {}", first_line(&outcome.text))),
        Some(outcome) => Ok(outcome),
        None => {
            let mut message = errors.trim().lines().last().unwrap_or("").to_string();
            if message.is_empty() {
                message = format!("{:?}", status);
            }
            Err(format!("claude gave no answer: {message}"))
        }
    }
}

fn first_line(text: &str) -> &str {
    text.trim().lines().next().unwrap_or("")
}

fn review() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let Some(file) = p.active_file() else { return Ok(()) };
    let Some(claude) = p.claude() else { return Ok(()) };
    let text = p.text("get-text")?;
    let rel = claude::relative(&p.root, &file);
    let name = Path::new(&file).file_name().unwrap_or_default().to_string_lossy().into_owned();
    p.status(&format!("reviewing {name}…"));
    let prompt = format!(
        "Review this file for real problems: bugs, crashes, leaks, races, security holes, wrong edge cases. \
         Skip style and taste. Give each problem's line numbers (as numbered below), a severity (error: \
         certainly wrong, warning: likely wrong, info: worth a look) and one short sentence. No problems: an \
         empty list.\n\nFile: {rel}\n\n{}",
        claude::numbered(&text)
    );
    let root = p.root.clone();
    let result = run_claude(&claude, &root, &prompt, &["--tools", "", "--json-schema", claude::REVIEW_SCHEMA]);
    p.status_clear();
    let issues = match result.and_then(|outcome| claude::parse_review(&outcome.structured, &outcome.text)) {
        Ok(issues) => issues,
        Err(message) => {
            p.say(&message);
            return Ok(());
        }
    };
    p.request("diagnostics-set", &claude::diagnostics(&file, &issues))?;
    let mut lines = vec![format!("Review of {rel}: {} finding(s)", issues.len())];
    lines.extend(issues.iter().map(|i| format!("{rel}:{}: {}: {}", i.line, i.severity, i.message)));
    let view =
        format!(r#"{{"id":"review","title":"Claude Review","where":"bottom","text":{}}}"#, quote(&lines.join("\n")));
    p.request("view-open", &view)?;
    p.say(&format!("{} finding(s) in Problems", issues.len()));
    Ok(())
}

fn rewrite() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let Some(file) = p.active_file() else { return Ok(()) };
    let Some(claude) = p.claude() else { return Ok(()) };
    let range = p.selection_range()?;
    let [l1, c1, l2, c2] = range;
    if l1 == l2 && c1 == c2 {
        p.say("select the text to rewrite first");
        return Ok(());
    }
    let selection = p.text("get-selection")?;
    let before = p.text("get-text")?;
    let Some(how) = p.question("Rewrite the selection how?")? else { return Ok(()) };
    p.status("rewriting…");
    let mut prompt = format!(
        "Rewrite an excerpt of {} (lines {l1}-{l2}) as asked: {how}\n\nAnswer the new text of the excerpt alone \
         in \"text\": the same indentation, no Markdown fences, nothing around it.\n\nThe excerpt:\n```\n{selection}\n```\n",
        claude::relative(&p.root, &file)
    );
    if before.len() < 200 * 1024 {
        prompt += &format!("\nThe whole file, for context:\n```\n{before}\n```\n");
    }
    let root = p.root.clone();
    let result = run_claude(&claude, &root, &prompt, &["--tools", "", "--json-schema", claude::REWRITE_SCHEMA]);
    p.status_clear();
    let text = match result.and_then(|outcome| claude::parse_rewrite(&outcome.structured)) {
        Ok(text) => text,
        Err(message) => {
            p.say(&message);
            return Ok(());
        }
    };
    // The answer is for that text: not when another tab is active now or the text changed.
    if p.request("get-file", "").unwrap_or_default() != file {
        p.say(&format!("{} is no longer the active tab: nothing changed", claude::relative(&p.root, &file)));
        return Ok(());
    }
    if p.text("get-text")? != before {
        p.say("the file changed meanwhile: nothing changed");
        return Ok(());
    }
    p.request("edit", &format!(r#"[{{"range":[{l1},{c1},{l2},{c2}],"text":{}}}]"#, quote(&text)))?;
    let (line, column) = claude::end_of(l1, c1, &text);
    let _ = p.request("set-selection", &format!("{l1} {c1} {line} {column}"));
    p.say("rewritten (Ctrl+Z undoes it)");
    Ok(())
}

fn clear() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let _ = p.request("diagnostics-clear", "");
    if let Err(e) = p.request("view-close", "review") {
        if !is_code(&e, "no-view") {
            return Err(e);
        }
    }
    p.say("review cleared");
    Ok(())
}

// The chat -------------------------------------------------------------------------------------

/// The chat's panel with the welcome text, a new conversation's: its tab added beside the others, or
/// also switched to (show).
fn new_chat(p: &mut Plugin, show: bool) -> io::Result<()> {
    let modes: Vec<String> = chat::MODES.iter().map(|m| quote(m)).collect();
    let view = format!(
        r#"{{"id":"{CHAT}","title":"Claude","where":"right","icon":"tcode-chat-symbolic","markdown":true,"input":"Ask Claude…","modes":[{}],"mode":{},"text":{},"show":{show}}}"#,
        modes.join(","),
        quote(chat::MODES[0]),
        quote(chat::WELCOME)
    );
    p.request("view-open", &view).map(|_| ())
}

/// Shows the chat's panel, opening it when it is not open (the start program opens it with the project).
fn open_chat(p: &mut Plugin) -> io::Result<()> {
    match p.request("view-show", CHAT) {
        Ok(_) => Ok(()),
        Err(e) if is_code(&e, "no-view") => new_chat(p, true),
        Err(e) => Err(e),
    }
}

fn chat_command() -> io::Result<()> {
    let mut p = Plugin::new()?;
    open_chat(&mut p)
}

/// The chat's box gets a reference to the active file: its selected lines (selection) or the file.
fn mention_in_chat(selection: bool) -> io::Result<()> {
    let mut p = Plugin::new()?;
    let Some(file) = p.active_file() else { return Ok(()) };
    let range = if selection { p.selection_range()? } else { [1, 1, 1, 1] };
    let mention = claude::mention(&p.root, &file, range);
    open_chat(&mut p)?;
    p.request("view-input", &format!("{CHAT} {mention} ")).map(|_| ())
}

/// The chat's conversation with claude, shared by the event loop and the thread reading claude.
#[derive(Default)]
struct Session {
    child: Option<Child>,
    stdin: Option<ChildStdin>,
    mode: String,              // the mode claude runs in
    id: Option<String>,        // claude's session, resumed when claude starts again
    busy: bool,                // a turn runs
    asks: Vec<(String, Json)>, // claude's open permission questions: request id, tool input
    generation: u64,           // the claude run the reading thread belongs to
    signed_in: bool,           // claude auth status said so
}

type Shared = Arc<Mutex<Session>>;

fn extend(t: &mut Tcode, text: &str) {
    let _ = t.request(&format!("view-extend {CHAT} {text}"));
}

/// Whether claude is signed in (claude auth status); asked until it is. A claude without that
/// command is taken as signed in: it says so itself when it is not.
fn signed_in(shared: &Shared, claude: &Path) -> bool {
    if shared.lock().unwrap().signed_in {
        return true;
    }
    let yes = match Command::new(claude).args(["auth", "status"]).stdin(Stdio::null()).stderr(Stdio::null()).output() {
        Ok(out) => json::parse(&String::from_utf8_lossy(&out.stdout))
            .map(|status| status.get("loggedIn").is_true())
            .unwrap_or(!out.status.success() || out.stdout.is_empty()),
        Err(_) => true,
    };
    shared.lock().unwrap().signed_in = yes;
    yes
}

/// Where the user is: the active file and its cursor or selection, after the message.
fn chat_context(p: &mut Plugin) -> String {
    let file = p.request("get-file", "").unwrap_or_default();
    if file.is_empty() {
        return String::new();
    }
    let rel = claude::relative(&p.root, &file);
    match p.selection_range() {
        Ok([l1, c1, l2, c2]) if l1 == l2 && c1 == c2 => {
            format!("\n\n(Open in the editor: {rel}, the cursor on line {l1}.)")
        }
        Ok([l1, _, l2, _]) => format!("\n\n(Open in the editor: {rel}, lines {l1}-{l2} selected.)"),
        Err(_) => format!("\n\n(Open in the editor: {rel}.)"),
    }
}

/// The user sent text in mode: it goes to claude, started (again) in that mode when it does not run
/// in it, resuming the conversation.
fn chat_input(p: &mut Plugin, shared: &Shared, text: &str, mode: &str) -> io::Result<()> {
    let mode = if chat::MODES.contains(&mode) { mode } else { chat::MODES[0] };
    if shared.lock().unwrap().busy {
        extend(&mut p.t, "\n\n*Claude is still answering: wait for it, or press Stop.*\n\n");
        return Ok(());
    }
    extend(&mut p.t, &chat::user_block(text, mode));
    let Some(claude) = claude::find_claude() else {
        extend(&mut p.t, "**Claude Code is not installed.** Install it (the Plugins window shows how, under Needs), sign in, and ask again.");
        return Ok(());
    };
    if !signed_in(shared, &claude) {
        extend(&mut p.t, "**Claude Code is not signed in.** Run `claude auth login` in a terminal, then ask again.");
        return Ok(());
    }
    let context = if mode == "Ask" { String::new() } else { chat_context(p) }; // Ask reads no files
    let message = chat::user_message(&format!("{text}{context}"));
    let mut s = shared.lock().unwrap();
    let alive = s.child.as_mut().is_some_and(|c| c.try_wait().ok().flatten().is_none());
    if !alive || s.mode != mode {
        if let Some(mut child) = s.child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
        s.stdin = None;
        s.generation += 1;
        let spawned = Command::new(&claude)
            .args(chat::arguments(mode, s.id.as_deref()))
            .current_dir(&p.root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn();
        let mut child = match spawned {
            Ok(child) => child,
            Err(e) => {
                drop(s);
                extend(&mut p.t, &format!("**claude did not start:** {e}"));
                return Ok(());
            }
        };
        let (stdout, stderr) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
        s.stdin = child.stdin.take();
        s.child = Some(child);
        s.mode = mode.to_string();
        let (reader, generation, root) = (shared.clone(), s.generation, p.root.clone());
        std::thread::spawn(move || read_claude(reader, generation, root, stdout, stderr));
    }
    let written = match s.stdin.as_mut() {
        Some(w) => writeln!(w, "{message}").and_then(|_| w.flush()),
        None => Err(io::Error::other("claude's input is closed")),
    };
    s.busy = written.is_ok();
    drop(s);
    match written {
        Ok(()) => p.request("view-busy", &format!("{CHAT} Claude is working…")).map(|_| ()),
        Err(e) => {
            extend(&mut p.t, &format!("**claude did not take the message:** {e}"));
            Ok(())
        }
    }
}

/// Reads one claude run's output into the panel until it ends (a connection of its own).
fn read_claude(
    shared: Shared,
    generation: u64,
    root: String,
    stdout: std::process::ChildStdout,
    mut stderr: std::process::ChildStderr,
) {
    let errors = std::thread::spawn(move || {
        let mut text = String::new();
        let _ = io::Read::read_to_string(&mut stderr, &mut text);
        text
    });
    let Ok(mut t) = connect() else { return };
    let mut stream = ChatStream::new(&root);
    for raw in BufReader::new(stdout).lines() {
        let Ok(raw) = raw else { break };
        for action in stream.feed(&raw) {
            match action {
                Action::Text(text) => extend(&mut t, &text),
                Action::Session(id) => shared.lock().unwrap().id = Some(id),
                Action::Ask { request, tool, input } => {
                    let question = chat::question(&tool, &input);
                    shared.lock().unwrap().asks.push((request.clone(), input));
                    let _ = t.request(&format!(
                        r#"view-prompt {{"id":"{CHAT}","prompt":{},"text":{},"choices":["Allow","Deny"]}}"#,
                        quote(&request),
                        quote(&question)
                    ));
                }
                Action::Done(error) => {
                    shared.lock().unwrap().busy = false;
                    let _ = t.request(&format!("view-busy {CHAT}"));
                    if let Some(error) = error {
                        extend(&mut t, &format!("\n\n**Claude Code:** {error}\n\n"));
                    }
                }
            }
        }
    }
    let errors = errors.join().unwrap_or_default();
    let mut s = shared.lock().unwrap();
    if s.generation != generation {
        return; // stopped, or started again in another mode
    }
    let was_busy = std::mem::take(&mut s.busy);
    let asks = std::mem::take(&mut s.asks);
    s.stdin = None;
    if let Some(mut child) = s.child.take() {
        let _ = child.wait();
    }
    drop(s);
    for (request, _) in asks {
        let _ = t.request(&format!(r#"view-prompt {{"id":"{CHAT}","prompt":{},"choices":[]}}"#, quote(&request)));
    }
    if was_busy {
        let _ = t.request(&format!("view-busy {CHAT}"));
        let why = errors.trim().lines().last().unwrap_or("it ended without an answer").to_string();
        extend(&mut t, &format!("\n\n**Claude Code stopped:** {why}\n\n"));
    }
}

/// One of claude's permission questions was answered.
fn chat_choice(shared: &Shared, prompt: &str, choice: &str) {
    let mut s = shared.lock().unwrap();
    let Some(at) = s.asks.iter().position(|(request, _)| request == prompt) else { return };
    let (request, input) = s.asks.remove(at);
    let line = chat::permission(&request, choice == "Allow", &input);
    if let Some(w) = s.stdin.as_mut() {
        let _ = writeln!(w, "{line}").and_then(|_| w.flush());
    }
}

/// Stop: claude ends; the next message starts it again, resuming the conversation.
fn chat_stop(t: Option<&mut Tcode>, shared: &Shared) {
    let mut s = shared.lock().unwrap();
    s.generation += 1;
    if let Some(mut child) = s.child.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
    s.stdin = None;
    let was_busy = std::mem::take(&mut s.busy);
    let asks = std::mem::take(&mut s.asks);
    drop(s);
    let Some(t) = t else { return };
    for (request, _) in asks {
        let _ = t.request(&format!(r#"view-prompt {{"id":"{CHAT}","prompt":{},"choices":[]}}"#, quote(&request)));
    }
    let _ = t.request(&format!("view-busy {CHAT}"));
    if was_busy {
        extend(t, "\n\n*Stopped.*\n\n");
    }
}

/// Runs the chat's conversations and opens the places the review's lines name when they are
/// activated. It ends when t-code closes the connection or stops it (SIGTERM ends the process;
/// claude then sees its input close).
fn start() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let mut events = connect()?;
    events.request("subscribe")?;
    let me = env("TCODE_PLUGIN_ID");
    let shared: Shared = Arc::default();
    new_chat(&mut p, false)?; // the Claude tab is there with the project, a new conversation
    let exists = |path: &Path| path.is_file();
    while let Some(line) = events.line()? {
        if tcode::member(&line, "plugin") != me {
            continue;
        }
        let chat = tcode::member(&line, "id") == CHAT;
        match tcode::member(&line, "event").as_str() {
            "view_activated" if !chat => {
                if let Some(place) = claude::locate(&p.root, &tcode::member(&line, "text"), exists) {
                    p.request("open", &place)?;
                }
            }
            "view_input" if chat => {
                chat_input(&mut p, &shared, &tcode::member(&line, "text"), &tcode::member(&line, "mode"))?
            }
            "view_choice" if chat => {
                chat_choice(&shared, &tcode::member(&line, "prompt"), &tcode::member(&line, "choice"))
            }
            "view_stop" if chat => chat_stop(Some(&mut p.t), &shared),
            _ => {}
        }
    }
    chat_stop(None, &shared);
    Ok(())
}

fn main() {
    let command = std::env::args().nth(1).unwrap_or_default();
    let result = match command.as_str() {
        "chat" => chat_command(),
        "send" => mention_in_chat(true),
        "ask" => mention_in_chat(false),
        "review" => review(),
        "rewrite" => rewrite(),
        "clear" => clear(),
        "start" => start(),
        _ => {
            eprintln!("claude-code: unknown command {command:?}");
            std::process::exit(2);
        }
    };
    if let Err(e) = result {
        eprintln!("claude-code: {e}");
        std::process::exit(1);
    }
}
