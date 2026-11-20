//! Documents: which file is the main one, which program builds it, running it, and reading its
//! errors.

use std::env;
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Kind {
    Latex,
    Typst,
}

impl Kind {
    pub fn of(path: &Path) -> Option<Kind> {
        match path.extension()?.to_str()? {
            "tex" | "ltx" => Some(Kind::Latex),
            "typ" => Some(Kind::Typst),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Kind::Latex => "LaTeX",
            Kind::Typst => "Typst",
        }
    }
}

fn read(path: &Path) -> String {
    fs::read_to_string(path).unwrap_or_default()
}

fn is_latex_main(path: &Path) -> bool {
    read(path).lines().any(|l| !l.trim_start().starts_with('%') && l.contains("\\documentclass"))
}

/// "% !TEX root = ../main.tex" (any case): the main file a chapter names.
fn tex_root(path: &Path) -> Option<PathBuf> {
    for line in read(path).lines().take(20) {
        let line = line.trim_start();
        let Some(comment) = line.strip_prefix('%') else { continue };
        let comment = comment.trim_start();
        if comment.len() >= 5 && comment[..5].eq_ignore_ascii_case("!tex ") {
            let rest = comment[5..].trim_start();
            if rest.len() >= 4 && rest[..4].eq_ignore_ascii_case("root") {
                if let Some(target) = rest[4..].trim_start().strip_prefix('=') {
                    return Some(path.parent()?.join(target.trim()));
                }
            }
        }
    }
    None
}

/// The document a saved file belongs to: a LaTeX file names it ("% !TEX root"), is one
/// (\documentclass), or is part of the main.tex (else the only file with a \documentclass) of
/// its folder or the project's; a Typst file is part of a main.typ there, else its own document.
pub fn main_file(path: &Path, root: &Path) -> PathBuf {
    let folders: Vec<&Path> = [path.parent(), Some(root)].into_iter().flatten().collect();
    match Kind::of(path) {
        Some(Kind::Latex) => {
            if let Some(named) = tex_root(path) {
                return named;
            }
            if is_latex_main(path) {
                return path.to_path_buf();
            }
            for folder in &folders {
                let main = folder.join("main.tex");
                if main.is_file() && is_latex_main(&main) {
                    return main;
                }
                let mut mains: Vec<PathBuf> = fs::read_dir(folder)
                    .into_iter()
                    .flatten()
                    .flatten()
                    .map(|e| e.path())
                    .filter(|p| Kind::of(p) == Some(Kind::Latex) && is_latex_main(p))
                    .collect();
                if mains.len() == 1 {
                    return mains.remove(0);
                }
            }
            path.to_path_buf()
        }
        _ => {
            for folder in &folders {
                let main = folder.join("main.typ");
                if main.is_file() {
                    return main;
                }
            }
            path.to_path_buf()
        }
    }
}

fn on_path(program: &str) -> bool {
    env::var_os("PATH").map(|paths| env::split_paths(&paths).any(|dir| dir.join(program).is_file())).unwrap_or(false)
}

/// The program and arguments that build main (run in its folder), or None when none is installed.
pub fn build_command(kind: Kind, main: &Path) -> Option<(String, Vec<String>)> {
    let name = main.file_name()?.to_string_lossy().to_string();
    let args = |list: &[&str]| -> Vec<String> {
        list.iter().map(|s| s.to_string()).chain(std::iter::once(name.clone())).collect()
    };
    let choices: Vec<(&str, Vec<String>)> = match kind {
        Kind::Latex => vec![
            ("latexmk", args(&["-pdf", "-interaction=nonstopmode", "-file-line-error", "-synctex=1"])),
            ("tectonic", args(&["--synctex"])),
            ("pdflatex", args(&["-interaction=nonstopmode", "-file-line-error", "-synctex=1"])),
        ],
        Kind::Typst => vec![("typst", args(&["compile", "--diagnostic-format", "short"]))],
    };
    choices.into_iter().find(|(program, _)| on_path(program)).map(|(p, a)| (p.to_string(), a))
}

pub fn install_hint(kind: Kind) -> &'static str {
    match kind {
        Kind::Latex => "install latexmk (with a TeX Live), tectonic or pdflatex",
        Kind::Typst => "install typst",
    }
}

/// Runs program in folder with stdout and stderr in one log; (finished well, the log). Ended after
/// timeout.
pub fn run(program: &str, args: &[String], folder: &Path, timeout: Duration) -> (bool, String) {
    static RUNS: AtomicUsize = AtomicUsize::new(0);
    let n = RUNS.fetch_add(1, Ordering::Relaxed);
    let log_path = env::temp_dir().join(format!("t-code-latex-{}-{n}.log", std::process::id()));
    let result = (|| -> std::io::Result<(bool, String)> {
        let log = File::create(&log_path)?;
        let mut child = Command::new(program)
            .args(args)
            .current_dir(folder)
            .stdin(Stdio::null())
            .stdout(log.try_clone()?)
            .stderr(log)
            .spawn()?;
        let started = Instant::now();
        let status = loop {
            if let Some(status) = child.try_wait()? {
                break Some(status);
            }
            if started.elapsed() > timeout {
                let _ = child.kill();
                let _ = child.wait();
                break None;
            }
            thread::sleep(Duration::from_millis(100));
        };
        let text = String::from_utf8_lossy(&fs::read(&log_path)?).to_string();
        Ok((status.is_some_and(|s| s.success()), text))
    })();
    let _ = fs::remove_file(&log_path);
    result.unwrap_or_else(|e| (false, format!("{program}: {e}")))
}

#[derive(Debug, PartialEq)]
pub struct Problem {
    pub path: PathBuf,
    pub line: usize,
    pub column: usize,
    pub error: bool,
    pub message: String,
}

const TEX_FILES: [&str; 5] = [".tex", ".ltx", ".sty", ".cls", ".bib"];

/// "<file>:<line>:" at the start of text, where file ends like one of exts: (file, line, the rest).
fn file_line<'a>(text: &'a str, exts: &[&str]) -> Option<(&'a str, usize, &'a str)> {
    for (colon, _) in text.match_indices(':') {
        let file = &text[..colon];
        if !exts.iter().any(|e| file.ends_with(e)) {
            continue;
        }
        let rest = &text[colon + 1..];
        let digits = rest.chars().take_while(char::is_ascii_digit).count();
        if digits > 0 && rest[digits..].starts_with(':') {
            return Some((file, rest[..digits].parse().ok()?, &rest[digits + 1..]));
        }
    }
    None
}

/// The errors and warnings in a build's log; relative paths start at folder, a LaTeX warning
/// without a file ("... on input line 5.") is main's.
pub fn problems(kind: Kind, log: &str, folder: &Path, main: &Path) -> Vec<Problem> {
    let mut found = Vec::new();
    let resolve = |file: &str| {
        let file = file.trim_start_matches("./");
        if Path::new(file).is_absolute() {
            PathBuf::from(file)
        } else {
            folder.join(file)
        }
    };
    for line in log.lines() {
        match kind {
            Kind::Latex => {
                // pdflatex / latexmk with -file-line-error; tectonic puts "error: " first
                let (error, text) = match line.strip_prefix("warning: ") {
                    Some(rest) => (false, rest),
                    None => (true, line.strip_prefix("error: ").unwrap_or(line)),
                };
                if let Some((file, number, message)) = file_line(text, &TEX_FILES) {
                    found.push(Problem {
                        path: resolve(file),
                        line: number,
                        column: 1,
                        error,
                        message: message.trim().to_string(),
                    });
                } else if let Some(at) = line.find(" on input line ") {
                    if line.starts_with("LaTeX Warning: ") || line.starts_with("Package ") {
                        let number = line[at + 15..].trim_end_matches('.').trim().parse().unwrap_or(1);
                        let message = line[..at].trim_start_matches("LaTeX Warning: ").to_string();
                        found.push(Problem {
                            path: main.to_path_buf(),
                            line: number,
                            column: 1,
                            error: false,
                            message,
                        });
                    }
                }
            }
            Kind::Typst => {
                // --diagnostic-format short: "file.typ:3:5: error: unknown variable: x"
                let Some((file, number, rest)) = file_line(line, &[".typ"]) else { continue };
                let digits = rest.chars().take_while(char::is_ascii_digit).count();
                let column = rest[..digits].parse().unwrap_or(1);
                let rest = rest[digits..].trim_start_matches(':').trim_start();
                let (error, message) = match (rest.strip_prefix("error: "), rest.strip_prefix("warning: ")) {
                    (Some(m), _) => (true, m),
                    (_, Some(m)) => (false, m),
                    _ => continue,
                };
                found.push(Problem { path: resolve(file), line: number, column, error, message: message.to_string() });
            }
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = env::temp_dir().join(format!("t-code-latex-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn finds_the_main_file() {
        let dir = temp_dir("main");
        fs::create_dir_all(dir.join("chapters")).unwrap();
        fs::write(dir.join("thesis.tex"), "\\documentclass{book}\n\\input{chapters/one}\n").unwrap();
        fs::write(dir.join("chapters/one.tex"), "\\chapter{One}\n").unwrap();
        fs::write(dir.join("chapters/two.tex"), "% !TeX root = ../thesis.tex\n\\chapter{Two}\n").unwrap();
        fs::write(dir.join("notes.typ"), "= Notes\n").unwrap();
        assert_eq!(main_file(&dir.join("chapters/two.tex"), &dir), dir.join("chapters/../thesis.tex"));
        assert_eq!(main_file(&dir.join("chapters/one.tex"), &dir), dir.join("thesis.tex")); // the only one at the root
        assert_eq!(main_file(&dir.join("thesis.tex"), &dir), dir.join("thesis.tex"));
        assert_eq!(main_file(&dir.join("notes.typ"), &dir), dir.join("notes.typ"));
        fs::write(dir.join("main.typ"), "#include \"notes.typ\"\n").unwrap();
        assert_eq!(main_file(&dir.join("notes.typ"), &dir), dir.join("main.typ"));
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn reads_latex_errors() {
        let log = "This is pdfTeX\n./main.tex:12: Undefined control sequence.\n\
                   chapters/one.tex:3: Missing $ inserted.\n\
                   LaTeX Warning: Reference `fig:x' on page 1 undefined on input line 7.\n\
                   error: main.tex:20: Emergency stop\nwarning: main.tex:4: Overfull \\hbox\n\
                   (./main.aux) no problem: here\n";
        let found = problems(Kind::Latex, log, Path::new("/d"), Path::new("/d/main.tex"));
        let short: Vec<_> = found.iter().map(|p| (p.path.to_str().unwrap(), p.line, p.error)).collect();
        assert_eq!(
            short,
            vec![
                ("/d/main.tex", 12, true),
                ("/d/chapters/one.tex", 3, true),
                ("/d/main.tex", 7, false),
                ("/d/main.tex", 20, true),
                ("/d/main.tex", 4, false),
            ]
        );
        assert_eq!(found[0].message, "Undefined control sequence.");
        assert_eq!(found[2].message, "Reference `fig:x' on page 1 undefined");
    }

    #[test]
    fn reads_typst_errors() {
        let log = "main.typ:3:5: error: unknown variable: foo\nlib/x.typ:10:1: warning: unused\nhelp: try this\n";
        let found = problems(Kind::Typst, log, Path::new("/d"), Path::new("/d/main.typ"));
        assert_eq!(
            found,
            vec![
                Problem {
                    path: "/d/main.typ".into(),
                    line: 3,
                    column: 5,
                    error: true,
                    message: "unknown variable: foo".into()
                },
                Problem { path: "/d/lib/x.typ".into(), line: 10, column: 1, error: false, message: "unused".into() },
            ]
        );
    }

    #[test]
    fn runs_with_a_log() {
        let dir = temp_dir("run");
        let (ok, log) =
            run("sh", &["-c".into(), "echo out; echo err >&2; exit 3".into()], &dir, Duration::from_secs(5));
        assert!(!ok);
        assert!(log.contains("out") && log.contains("err"));
        let (ok, _) = run("sleep", &["5".into()], &dir, Duration::from_millis(300));
        assert!(!ok); // ended at the timeout
        fs::remove_dir_all(&dir).unwrap();
    }
}
