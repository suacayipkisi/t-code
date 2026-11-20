//! t-code's TODO finder: the TODO, FIXME, XXX and HACK comments of the project, in t-code's picker
//! (the chosen one opens) or as hints in the Problems panel. It speaks tapi_1 over $TCODE_SOCKET
//! (docs/plugins.md) and needs nothing but Rust's standard library.
//!
//!     list      pick a TODO, go to it
//!     problems  every TODO as an "info" diagnostic, in place of the last list
//!     clear     the diagnostics go

use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::{self, BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const TAGS: [&str; 4] = ["TODO", "FIXME", "XXX", "HACK"];
/// Folders that hold what a build or a package manager made, not the project's own text.
const SKIP_DIRS: [&str; 7] = ["target", "build", "node_modules", "out", "dist", "__pycache__", "vendor"];
const MAX_FILE_BYTES: u64 = 1 << 20;
const MAX_FILES: usize = 20_000;
const MAX_TODOS: usize = 5_000;

#[derive(Debug, PartialEq)]
struct Todo {
    path: PathBuf,
    line: usize,   // 1-based
    column: usize, // 1-based, in characters
    tag: &'static str,
    text: String,
}

impl Todo {
    fn message(&self) -> String {
        if self.text.is_empty() {
            self.tag.to_string()
        } else {
            format!("{}: {}", self.tag, self.text)
        }
    }
}

// Finding them ----------------------------------------------------------------------------------

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// The first tag in a line as a word of its own, then an optional "(who)", then ":", a space or
/// the line's end: (its column, the tag, the text after it). "TODO_LIST" and "todo" are not tags.
fn tag_in(line: &str) -> Option<(usize, &'static str, String)> {
    for (at, _) in line.char_indices() {
        let Some(tag) = TAGS.iter().find(|t| line[at..].starts_with(**t)) else { continue };
        if line[..at].chars().next_back().is_some_and(is_word) {
            continue;
        }
        let mut rest = &line[at + tag.len()..];
        if rest.chars().next().is_some_and(is_word) {
            continue;
        }
        if rest.starts_with('(') {
            match rest.find(')') {
                Some(end) => rest = &rest[end + 1..],
                None => continue,
            }
        }
        if !(rest.is_empty() || rest.starts_with(':') || rest.starts_with(char::is_whitespace)) {
            continue;
        }
        let text = rest.trim_start_matches(':').trim();
        let text = text.trim_end_matches("*/").trim_end_matches("-->").trim_end();
        return Some((line[..at].chars().count() + 1, *tag, text.to_string()));
    }
    None
}

fn todos_in(path: &Path, text: &str) -> Vec<Todo> {
    text.lines()
        .enumerate()
        .filter_map(|(i, line)| {
            tag_in(line).map(|(column, tag, text)| Todo { path: path.to_path_buf(), line: i + 1, column, tag, text })
        })
        .collect()
}

/// The project's files: git's list (tracked and new, without the ignored ones) in a repository,
/// else a walk that leaves out hidden folders and build output.
fn files(root: &Path) -> Vec<PathBuf> {
    let git = Command::new("git")
        .arg("-C")
        .arg(root)
        .args(["ls-files", "-z", "--cached", "--others", "--exclude-standard"])
        .output();
    if let Ok(out) = git {
        if out.status.success() && !out.stdout.is_empty() {
            return out
                .stdout
                .split(|b| *b == 0)
                .filter(|name| !name.is_empty())
                .map(|name| root.join(String::from_utf8_lossy(name).as_ref()))
                .take(MAX_FILES)
                .collect();
        }
    }
    let mut found = Vec::new();
    walk(root, &mut found);
    found
}

fn walk(folder: &Path, found: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(folder) else { return };
    let mut entries: Vec<_> = entries.flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        if found.len() >= MAX_FILES {
            return;
        }
        let name = entry.file_name();
        let name = name.to_string_lossy();
        let Ok(kind) = entry.file_type() else { continue };
        if kind.is_dir() {
            if !name.starts_with('.') && !SKIP_DIRS.contains(&name.as_ref()) {
                walk(&entry.path(), found);
            }
        } else if kind.is_file() {
            found.push(entry.path());
        }
    }
}

/// Every TODO of the project, by file and line; binary, huge and non-UTF-8 files are skipped.
fn scan(root: &Path) -> Vec<Todo> {
    let mut todos = Vec::new();
    for path in files(root) {
        let small = fs::metadata(&path).map(|m| m.is_file() && m.len() <= MAX_FILE_BYTES).unwrap_or(false);
        let Ok(bytes) = (if small { fs::read(&path) } else { continue }) else { continue };
        if bytes.contains(&0) {
            continue;
        }
        let Ok(text) = String::from_utf8(bytes) else { continue };
        todos.extend(todos_in(&path, &text));
        if todos.len() >= MAX_TODOS {
            todos.truncate(MAX_TODOS);
            break;
        }
    }
    todos.sort_by(|a, b| (&a.path, a.line).cmp(&(&b.path, b.line)));
    todos
}

// Talking to t-code -----------------------------------------------------------------------------

/// A JSON string.
fn quote(text: &str) -> String {
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

struct Tcode {
    reader: BufReader<UnixStream>,
    writer: UnixStream,
}

impl Tcode {
    /// A tapi_1 connection, said hello to with the plugin's token.
    fn connect(socket: &str, token: &str) -> io::Result<Tcode> {
        let stream = UnixStream::connect(socket)?;
        let mut tcode = Tcode { reader: BufReader::new(stream.try_clone()?), writer: stream };
        let answer = tcode.request(format!("hello tapi_1 {token}").trim_end())?;
        if !answer.starts_with("ok") {
            return Err(io::Error::other(format!("t-code said {answer:?} to hello")));
        }
        Ok(tcode)
    }

    /// One request, ended by a NUL (a path or a message may hold anything), and its answer line.
    fn request(&mut self, request: &str) -> io::Result<String> {
        self.writer.write_all(request.as_bytes())?;
        self.writer.write_all(b"\0")?;
        let mut answer = String::new();
        if self.reader.read_line(&mut answer)? == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "t-code closed the connection"));
        }
        Ok(answer.trim_end_matches('\n').to_string())
    }

    /// The answer's value after "ok"; an "error ..." answer becomes an error.
    fn ok(&mut self, request: &str) -> io::Result<String> {
        let answer = self.request(request)?;
        match answer.strip_prefix("ok") {
            Some(value) => Ok(value.trim_start().to_string()),
            None => Err(io::Error::other(answer)),
        }
    }
}

fn relative<'a>(path: &'a Path, root: &Path) -> &'a Path {
    path.strip_prefix(root).unwrap_or(path)
}

fn list(tcode: &mut Tcode, root: &Path) -> io::Result<()> {
    let todos = scan(root);
    if todos.is_empty() {
        tcode.ok("say TODO Finder: nothing to do here")?;
        return Ok(());
    }
    let items: Vec<String> = todos
        .iter()
        .map(|t| {
            let place = format!("{}:{}", relative(&t.path, root).display(), t.line);
            format!("{{\"label\":{},\"detail\":{}}}", quote(&t.message()), quote(&place))
        })
        .collect();
    let title = format!("{} TODO{}", todos.len(), if todos.len() == 1 { "" } else { "s" });
    let answer = tcode.request(&format!("pick {{\"title\":{},\"items\":[{}]}}", quote(&title), items.join(",")))?;
    if answer.starts_with("error canceled") {
        return Ok(());
    }
    let index: usize = answer
        .strip_prefix("ok ")
        .and_then(|rest| rest.split(' ').next())
        .and_then(|n| n.parse().ok())
        .ok_or_else(|| io::Error::other(answer.clone()))?;
    let todo = todos.get(index).ok_or_else(|| io::Error::other(answer.clone()))?;
    tcode.ok(&format!("open {}:{}:{}", todo.path.display(), todo.line, todo.column))?;
    Ok(())
}

fn problems(tcode: &mut Tcode, root: &Path) -> io::Result<()> {
    let todos = scan(root);
    let mut by_file: BTreeMap<&Path, Vec<String>> = BTreeMap::new();
    for t in &todos {
        by_file.entry(&t.path).or_default().push(format!(
            "{{\"range\":[{},{},{},{}],\"severity\":\"info\",\"source\":\"todo\",\"message\":{}}}",
            t.line,
            t.column,
            t.line,
            t.column + t.tag.len(),
            quote(&t.message())
        ));
    }
    tcode.ok("diagnostics-clear")?;
    for (path, items) in &by_file {
        let path = path.to_string_lossy();
        tcode.ok(&format!("diagnostics-set {{\"path\":{},\"items\":[{}]}}", quote(&path), items.join(",")))?;
    }
    let files = by_file.len();
    tcode.ok(&format!(
        "say TODO Finder: {} in {} file{}, in the Problems panel",
        todos.len(),
        files,
        if files == 1 { "" } else { "s" }
    ))?;
    Ok(())
}

fn run(command: &str, tcode: &mut Tcode) -> io::Result<()> {
    let root = match env::var("TCODE_PROJECT_ROOT") {
        Ok(root) if !root.is_empty() => PathBuf::from(root),
        _ => PathBuf::from(tcode.ok("get-project")?),
    };
    match command {
        "list" => list(tcode, &root),
        "problems" => problems(tcode, &root),
        "clear" => tcode.ok("diagnostics-clear").map(|_| ()),
        other => Err(io::Error::other(format!("unknown command {other:?}"))),
    }
}

fn main() -> ExitCode {
    let command = env::args().nth(1).unwrap_or_default();
    let Ok(socket) = env::var("TCODE_SOCKET") else {
        eprintln!("todos: no $TCODE_SOCKET: t-code runs this plugin");
        return ExitCode::FAILURE;
    };
    let token = env::var("TCODE_PLUGIN_TOKEN").unwrap_or_default();
    let result = Tcode::connect(&socket, &token).and_then(|mut tcode| run(&command, &mut tcode));
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("todos: {command}: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use std::os::unix::net::UnixListener;
    use std::thread;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = env::temp_dir().join(format!("t-code-todos-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn tags() {
        assert_eq!(tag_in("  // TODO: free it"), Some((6, "TODO", "free it".into())));
        assert_eq!(tag_in("# FIXME(ana) the race"), Some((3, "FIXME", "the race".into())));
        assert_eq!(tag_in("/* XXX */"), Some((4, "XXX", "".into())));
        assert_eq!(tag_in("çğ HACK"), Some((4, "HACK", "".into()))); // columns in characters
        assert_eq!(tag_in("<!-- TODO: docs -->"), Some((6, "TODO", "docs".into())));
        assert_eq!(tag_in("TODO_LIST = 1"), None);
        assert_eq!(tag_in("MYTODO: x"), None);
        assert_eq!(tag_in("todo: lower case"), None);
        assert_eq!(tag_in("TODOs"), None);
        assert_eq!(tag_in("FIXME(unclosed"), None);
    }

    #[test]
    fn json() {
        assert_eq!(quote("a\"b\\c\nd\u{1}é"), "\"a\\\"b\\\\c\\nd\\u0001é\"");
    }

    #[test]
    fn walks_without_git() {
        let dir = temp_dir("walk");
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::create_dir_all(dir.join("target")).unwrap();
        fs::create_dir_all(dir.join(".hidden")).unwrap();
        fs::write(dir.join("src/b.rs"), "fn b() {}\n// FIXME: b\n").unwrap();
        fs::write(dir.join("a.c"), "// TODO: a\n").unwrap();
        fs::write(dir.join("target/out.rs"), "// TODO: built\n").unwrap();
        fs::write(dir.join(".hidden/x"), "TODO\n").unwrap();
        fs::write(dir.join("blob"), b"TODO\0\x01").unwrap();
        let found = scan(&dir);
        let places: Vec<_> = found.iter().map(|t| (relative(&t.path, &dir).to_path_buf(), t.line)).collect();
        assert_eq!(places, vec![(PathBuf::from("a.c"), 1), (PathBuf::from("src/b.rs"), 2)]);
        fs::remove_dir_all(&dir).unwrap();
    }

    /// A t-code that answers like the real one: hello, then pick (the second item), then open.
    #[test]
    fn list_opens_the_pick() {
        let dir = temp_dir("list");
        fs::write(dir.join("a.c"), "// TODO: one\nint x; // FIXME: two\n").unwrap();
        let socket = dir.join("t-code.sock");
        let listener = UnixListener::bind(&socket).unwrap();
        let server = thread::spawn(move || {
            let (mut conn, _) = listener.accept().unwrap();
            let mut requests = Vec::new();
            let mut pending = Vec::new();
            let mut buffer = [0u8; 4096];
            while let Ok(n) = conn.read(&mut buffer) {
                if n == 0 {
                    break;
                }
                pending.extend_from_slice(&buffer[..n]);
                while let Some(end) = pending.iter().position(|b| *b == 0) {
                    let request = String::from_utf8(pending.drain(..=end).collect::<Vec<_>>()).unwrap();
                    let request = request.trim_end_matches('\0').to_string();
                    let answer = match request.split(' ').next().unwrap() {
                        "hello" => "ok tapi_1 t-code test\n",
                        "pick" => "ok 1 FIXME: two\n",
                        _ => "ok\n",
                    };
                    conn.write_all(answer.as_bytes()).unwrap();
                    requests.push(request);
                }
            }
            requests
        });
        {
            let mut tcode = Tcode::connect(socket.to_str().unwrap(), "secret").unwrap();
            list(&mut tcode, &dir).unwrap();
        }
        let requests = server.join().unwrap();
        assert_eq!(requests[0], "hello tapi_1 secret");
        assert!(requests[1].starts_with("pick {\"title\":\"2 TODOs\",\"items\":[{\"label\":\"TODO: one\""));
        assert!(requests[1].contains("{\"label\":\"FIXME: two\",\"detail\":\"a.c:2\"}"));
        assert_eq!(requests[2], format!("open {}:2:11", dir.join("a.c").display()));
        fs::remove_dir_all(&dir).unwrap();
    }
}
