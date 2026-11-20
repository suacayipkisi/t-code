//! LaTeX and Typst for t-code, in Rust (the standard library only).
//!
//! Started with the project ($1 = "start"), it builds a document when one of its files is saved
//! (latexmk, tectonic or pdflatex for LaTeX; typst for Typst), puts its errors and warnings in the
//! Problems panel and keeps a status bar item ("LaTeX: ok", "LaTeX: 2 errors").
//!
//!     build  builds the active file's document now
//!     open   opens its PDF in the desktop's viewer

mod doc;
mod tcode;

use std::collections::BTreeMap;
use std::env;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::time::Duration;

use doc::{Kind, Problem};
use tcode::{quote, Tcode};

const ITEM: &str = "document";
const TIMEOUT: Duration = Duration::from_secs(300);

fn diagnostics(problems: &[Problem]) -> BTreeMap<&Path, Vec<String>> {
    let mut by_file: BTreeMap<&Path, Vec<String>> = BTreeMap::new();
    for p in problems {
        by_file.entry(&p.path).or_default().push(format!(
            "{{\"range\":[{},{}],\"severity\":\"{}\",\"message\":{},\"source\":\"latex\"}}",
            p.line,
            p.column,
            if p.error { "error" } else { "warning" },
            quote(&p.message)
        ));
    }
    by_file
}

/// Builds the document file belongs to; the status bar item and the Problems panel say how it went.
fn build(t: &mut Tcode, file: &Path, root: &Path) -> io::Result<()> {
    let Some(kind) = Kind::of(file) else { return Ok(()) };
    let main = doc::main_file(file, root);
    let folder = main.parent().unwrap_or(root).to_path_buf();
    let Some((program, args)) = doc::build_command(kind, &main) else {
        t.request(&format!("status-set {ITEM} {}: {}", kind.name(), doc::install_hint(kind)))?;
        return Ok(());
    };
    t.request(&format!("status-set {ITEM} {}: building…", kind.name()))?;
    let (ok, log) = doc::run(&program, &args, &folder, TIMEOUT);
    let problems = doc::problems(kind, &log, &folder, &main);
    t.request("diagnostics-clear")?;
    for (path, items) in diagnostics(&problems) {
        t.request(&format!(
            "diagnostics-set {{\"path\":{},\"items\":[{}]}}",
            quote(&path.to_string_lossy()),
            items.join(",")
        ))?;
    }
    let errors = problems.iter().filter(|p| p.error).count();
    let warnings = problems.len() - errors;
    let state = match (ok, errors, warnings) {
        (true, 0, 0) => "ok".to_string(),
        (true, 0, w) => format!("ok, {w} warning{}", if w == 1 { "" } else { "s" }),
        (_, 0, _) => format!("{program} failed (its log has no line to point at)"),
        (_, e, _) => format!("{e} error{}", if e == 1 { "" } else { "s" }),
    };
    t.request(&format!("status-set {ITEM} {}: {state}", kind.name()))?;
    Ok(())
}

fn project(t: &mut Tcode) -> io::Result<PathBuf> {
    match env::var("TCODE_PROJECT_ROOT") {
        Ok(root) if !root.is_empty() => Ok(PathBuf::from(root)),
        _ => Ok(PathBuf::from(t.request("get-project")?)),
    }
}

fn listen(socket: &str, token: &str) -> io::Result<()> {
    let mut requests = Tcode::connect(socket, token)?;
    let root = project(&mut requests)?;
    let mut events = Tcode::connect(socket, token)?;
    events.request("subscribe")?;
    while let Some(line) = events.line()? {
        if tcode::member(&line, "event") == "did_save" {
            let path = PathBuf::from(tcode::member(&line, "path"));
            if Kind::of(&path).is_some() {
                build(&mut requests, &path, &root)?;
            }
        }
    }
    Ok(())
}

/// The active file, when it is a LaTeX or Typst one; else the status bar says so and None.
fn active_document(t: &mut Tcode) -> io::Result<Option<PathBuf>> {
    let file = PathBuf::from(t.request("get-file")?);
    if Kind::of(&file).is_none() {
        t.request("say LaTeX: open a .tex or .typ file first")?;
        return Ok(None);
    }
    Ok(Some(file))
}

fn build_now(socket: &str, token: &str) -> io::Result<()> {
    let mut t = Tcode::connect(socket, token)?;
    let root = project(&mut t)?;
    if let Some(file) = active_document(&mut t)? {
        t.request("save")?; // its did_save builds it too when the plugin runs: the same result
        build(&mut t, &file, &root)?;
    }
    Ok(())
}

fn open_pdf(socket: &str, token: &str) -> io::Result<()> {
    let mut t = Tcode::connect(socket, token)?;
    let root = project(&mut t)?;
    let Some(file) = active_document(&mut t)? else { return Ok(()) };
    let pdf = doc::main_file(&file, &root).with_extension("pdf");
    if !pdf.is_file() {
        t.request(&format!(
            "say LaTeX: no {} yet: build it first",
            pdf.file_name().unwrap_or_default().to_string_lossy()
        ))?;
        return Ok(());
    }
    let opened =
        Command::new("xdg-open").arg(&pdf).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn();
    if opened.is_err() {
        t.request("say LaTeX: xdg-open is missing: open the PDF by hand")?;
    }
    Ok(())
}

fn main() -> ExitCode {
    let command = env::args().nth(1).unwrap_or_default();
    let Ok(socket) = env::var("TCODE_SOCKET") else {
        eprintln!("latex: no $TCODE_SOCKET: t-code runs this plugin");
        return ExitCode::FAILURE;
    };
    let token = env::var("TCODE_PLUGIN_TOKEN").unwrap_or_default();
    let result = match command.as_str() {
        "start" => listen(&socket, &token),
        "build" => build_now(&socket, &token),
        "open" => open_pdf(&socket, &token),
        other => Err(io::Error::other(format!("unknown command {other:?}"))),
    };
    match result {
        Err(e) if !tcode::is_closed(&e) => {
            eprintln!("latex: {command}: {e}");
            ExitCode::FAILURE
        }
        _ => ExitCode::SUCCESS,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::io::{Read, Write};
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixListener;
    use std::thread;

    /// A fake pdflatex on PATH and a t-code on a socket: the build's error lands in Problems and the
    /// item says so.
    #[test]
    fn build_reports() {
        let dir = env::temp_dir().join(format!("t-code-latex-main-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("bin")).unwrap();
        let fake = dir.join("bin/pdflatex");
        fs::write(&fake, "#!/bin/sh\necho './paper.tex:2: Undefined control sequence.'\nexit 1\n").unwrap();
        fs::set_permissions(&fake, fs::Permissions::from_mode(0o755)).unwrap();
        fs::write(dir.join("paper.tex"), "\\documentclass{article}\n\\foo\n").unwrap();
        let old_path = env::var_os("PATH").unwrap_or_default();
        env::set_var("PATH", format!("{}:/usr/bin:/bin", dir.join("bin").display()));

        let socket = dir.join("t-code.sock");
        let listener = UnixListener::bind(&socket).unwrap();
        let server = thread::spawn(move || {
            let (mut conn, _) = listener.accept().unwrap();
            let (mut requests, mut pending, mut buffer) = (Vec::new(), Vec::new(), [0u8; 4096]);
            while let Ok(n) = conn.read(&mut buffer) {
                if n == 0 {
                    break;
                }
                pending.extend_from_slice(&buffer[..n]);
                while let Some(end) = pending.iter().position(|b| *b == 0) {
                    let request = String::from_utf8(pending.drain(..=end).collect()).unwrap();
                    let request = request.trim_end_matches('\0').to_string();
                    let answer = if request.starts_with("hello") { "ok tapi_1 t-code test\n" } else { "ok\n" };
                    conn.write_all(answer.as_bytes()).unwrap();
                    requests.push(request);
                }
            }
            requests
        });
        {
            let mut t = Tcode::connect(socket.to_str().unwrap(), "s").unwrap();
            build(&mut t, &dir.join("paper.tex"), &dir).unwrap();
        }
        let requests = server.join().unwrap();
        env::set_var("PATH", old_path);
        assert_eq!(requests[1], format!("status-set {ITEM} LaTeX: building…"));
        assert_eq!(requests[2], "diagnostics-clear");
        assert_eq!(
            requests[3],
            format!(
                "diagnostics-set {{\"path\":{},\"items\":[{{\"range\":[2,1],\"severity\":\"error\",\"message\":\"Undefined control sequence.\",\"source\":\"latex\"}}]}}",
                quote(&dir.join("paper.tex").to_string_lossy())
            )
        );
        assert_eq!(requests[4], format!("status-set {ITEM} LaTeX: 1 error"));
        fs::remove_dir_all(&dir).unwrap();
    }
}
