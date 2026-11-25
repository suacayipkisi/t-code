//! Claude Code for t-code, in Rust (the standard library only). It needs the claude command.
//!
//! - `session`: opens Claude Code in a terminal tab of the project (one per project; again: says
//!   where it is). t-code types /exit into it when it closes.
//! - `send`: asks for a prompt and sends it to the session with the selection as a reference,
//!   "@src/main.c#L5-10 why is this slow?"; starts the session when there is none.
//! - `ask`: asks a question about the active file and streams the answer into the "Claude" view on
//!   the right. Closing the view stops claude.
//! - `review`: has claude review the active buffer: its findings go to Problems (diagnostics) and
//!   to the "Claude Review" view at the bottom.
//! - `rewrite`: rewrites the selection as asked, as one undo step (`edit`), and selects the new text.
//! - `clear`: takes the review's diagnostics and its view away.
//! - `start` (with the project): opens the place ("src/main.c:42") a line of the plugin's views
//!   names when the line is activated.

mod claude;
mod json;
mod tcode;

use claude::Range;
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tcode::{quote, Tcode};

const STATUS: &str = "claude";

const PANEL_PROMPT: &str = "You answer inside a plain text panel of the t-code editor: no Markdown tables, \
headings or bold text. Name places in the code as path:line (relative to the project folder), one per line \
when there are several, so the user can open them.";

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

// The session's terminal tab id, kept per project while the user's login lasts.

fn tab_file(root: &str) -> PathBuf {
    let dir = match std::env::var_os("XDG_RUNTIME_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => std::env::temp_dir(),
    };
    dir.join("t-code-claude-code").join(format!("{}.tab", root.trim_matches('/').replace('/', "%")))
}

/// The session's tab when it is still open. terminal-close-text answers bad-argument for a tab
/// that is gone, so it tells without typing anything into the tab.
fn session_tab(p: &mut Plugin) -> Option<String> {
    let tab = std::fs::read_to_string(tab_file(&p.root)).ok()?.trim().to_string();
    (!tab.is_empty() && p.request("terminal-close-text", &format!("{tab} /exit")).is_ok()).then_some(tab)
}

/// Opens Claude Code in a terminal tab, with prompt as its first message when given.
fn open_session(p: &mut Plugin, claude: &Path, prompt: Option<&str>) -> io::Result<String> {
    let mut line = format!("Claude -- {}", claude::shell_quote(&claude.to_string_lossy()));
    if let Some(prompt) = prompt {
        line = format!("{line} {}", claude::shell_quote(prompt));
    }
    let tab = p.request("terminal-open", &line)?;
    let _ = p.request("terminal-close-text", &format!("{tab} /exit"));
    let file = tab_file(&p.root);
    if let Some(dir) = file.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(file, &tab);
    Ok(tab)
}

fn session() -> io::Result<()> {
    let mut p = Plugin::new()?;
    if session_tab(&mut p).is_some() {
        p.say("the session is open in the \"Claude\" terminal tab");
        return Ok(());
    }
    let Some(claude) = p.claude() else { return Ok(()) };
    open_session(&mut p, &claude, None).map(|_| ())
}

fn send() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let Some(file) = p.active_file() else { return Ok(()) };
    let range = p.selection_range()?;
    let mention = claude::mention(&p.root, &file, range);
    let Some(prompt) = p.question(&format!("Ask Claude about {mention}"))? else { return Ok(()) };
    let message = format!("{mention} {prompt}");
    match session_tab(&mut p) {
        Some(tab) => {
            p.request("terminal-send", &format!("{tab} {message}"))?;
        }
        None => {
            let Some(claude) = p.claude() else { return Ok(()) };
            open_session(&mut p, &claude, Some(&message))?;
        }
    }
    p.say(&format!("sent {mention}"));
    Ok(())
}

/// A running claude -p that another thread may stop.
#[derive(Clone)]
struct Stopper {
    child: Arc<Mutex<Option<Child>>>,
    stopped: Arc<AtomicBool>,
}

impl Stopper {
    fn new() -> Stopper {
        Stopper { child: Arc::new(Mutex::new(None)), stopped: Arc::new(AtomicBool::new(false)) }
    }

    fn stop(&self) {
        self.stopped.store(true, Ordering::SeqCst);
        if let Some(child) = self.child.lock().unwrap().as_mut() {
            let _ = child.kill();
        }
    }

    fn stopped(&self) -> bool {
        self.stopped.load(Ordering::SeqCst)
    }

    /// Stops claude when the user closes the plugin's view id (a connection of its own).
    fn on_view_closed(&self, id: &'static str) {
        let stopper = self.clone();
        let Ok(mut events) = connect() else { return };
        std::thread::spawn(move || {
            if events.request("subscribe").is_err() {
                return;
            }
            let me = env("TCODE_PLUGIN_ID");
            while let Ok(Some(line)) = events.line() {
                if tcode::member(&line, "event") == "view_closed"
                    && tcode::member(&line, "plugin") == me
                    && tcode::member(&line, "id") == id
                {
                    stopper.stop();
                }
            }
        });
    }
}

/// Runs claude -p in root with the prompt on stdin, hands each finished line of its answer to
/// line and answers its outcome.
fn run_claude(
    claude: &Path,
    root: &str,
    prompt: &str,
    stopper: &Stopper,
    mut line: impl FnMut(&str),
    args: &[&str],
) -> Result<claude::Outcome, String> {
    let mut child = Command::new(claude)
        .args(["-p", "--output-format", "stream-json", "--verbose", "--include-partial-messages"])
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
    *stopper.child.lock().unwrap() = Some(child);
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
        for text in stream.feed(&raw) {
            line(&text);
        }
    }
    let status = stopper.child.lock().unwrap().take().map(|mut child| child.wait());
    let errors = errors.join().unwrap_or_default();
    if stopper.stopped() {
        return Err("stopped".into());
    }
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

fn file_context(p: &mut Plugin, file: &str) -> io::Result<String> {
    let mut out = format!("\n\nThe file open in the editor: {}", claude::relative(&p.root, file));
    let [l1, c1, l2, c2] = p.selection_range()?;
    if l1 == l2 && c1 == c2 {
        out += &format!(", the cursor on line {l1}");
    } else {
        out += &format!(", lines {l1}-{l2} selected");
        let selection = p.text("get-selection")?;
        if selection.len() < 64 * 1024 {
            out += &format!(":\n```\n{selection}\n```");
        }
    }
    Ok(out)
}

fn ask() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let Some(file) = p.active_file() else { return Ok(()) };
    let Some(claude) = p.claude() else { return Ok(()) };
    let name = Path::new(&file).file_name().unwrap_or_default().to_string_lossy().into_owned();
    let Some(query) = p.question(&format!("Ask Claude about {name}"))? else { return Ok(()) };
    let prompt = query.clone() + &file_context(&mut p, &file)?;
    let view =
        format!(r#"{{"id":"answer","title":"Claude","where":"right","text":{}}}"#, quote(&format!("› {query}\n")));
    p.request("view-open", &view)?;
    let stopper = Stopper::new();
    stopper.on_view_closed("answer");
    p.status("thinking…");
    let root = p.root.clone();
    let args = ["--tools", "Read,Grep,Glob", "--append-system-prompt", PANEL_PROMPT];
    let result = run_claude(
        &claude,
        &root,
        &prompt,
        &stopper,
        |line| {
            if let Err(e) = p.request("view-append", &format!("answer {line}")) {
                if is_code(&e, "no-view") {
                    stopper.stop();
                }
            }
        },
        &args,
    );
    p.status_clear();
    match result {
        Err(_) if stopper.stopped() => p.say("stopped"),
        Err(message) => {
            let _ = p.request("view-append", &format!("answer \n{message}"));
        }
        Ok(outcome) => {
            let _ = p.request("view-append", &format!("answer \n— done (${:.4})", outcome.cost));
        }
    }
    Ok(())
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
    let result = run_claude(
        &claude,
        &root,
        &prompt,
        &Stopper::new(),
        |_| {},
        &["--tools", "", "--json-schema", claude::REVIEW_SCHEMA],
    );
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
    let result = run_claude(
        &claude,
        &root,
        &prompt,
        &Stopper::new(),
        |_| {},
        &["--tools", "", "--json-schema", claude::REWRITE_SCHEMA],
    );
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

/// Listens for the lines of the plugin's views being activated, and opens their places. It ends
/// when t-code closes the connection or stops it (SIGTERM ends the process).
fn start() -> io::Result<()> {
    let mut p = Plugin::new()?;
    let mut events = connect()?;
    events.request("subscribe")?;
    let me = env("TCODE_PLUGIN_ID");
    let exists = |path: &Path| path.is_file();
    while let Some(line) = events.line()? {
        if tcode::member(&line, "event") != "view_activated" || tcode::member(&line, "plugin") != me {
            continue;
        }
        if let Some(place) = claude::locate(&p.root, &tcode::member(&line, "text"), exists) {
            p.request("open", &place)?;
        }
    }
    Ok(())
}

fn main() {
    let command = std::env::args().nth(1).unwrap_or_default();
    let result = match command.as_str() {
        "session" => session(),
        "send" => send(),
        "ask" => ask(),
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
