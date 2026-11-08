use crate::config::{plain_text, Extension};
use std::{
    io::{self, Read},
    os::{
        fd::{AsRawFd, RawFd},
        unix::process::CommandExt,
    },
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

const LIMIT: usize = 4096;

fn nonblocking(fd: RawFd) -> io::Result<()> {
    // SAFETY: fd is an open pipe owned by the child handle; fcntl does not retain it.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags == -1 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: same valid fd, with flags returned above.
    if unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } == -1 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn kill_group(child: &mut Child) {
    // SAFETY: child is the leader of the process group created by Command::process_group.
    unsafe {
        libc::kill(-(child.id() as libc::pid_t), libc::SIGKILL);
    }
    let _ = child.kill();
}

fn drain(
    reader: &mut impl Read,
    output: &mut Vec<u8>,
    remaining: &mut usize,
) -> Result<(), String> {
    let mut buffer = [0; 1024];
    loop {
        match reader.read(&mut buffer[..(*remaining + 1).min(1024)]) {
            Ok(0) => return Ok(()),
            Ok(n) => {
                let keep = n.min(*remaining);
                output.extend_from_slice(&buffer[..keep]);
                *remaining -= keep;
                if keep != n {
                    return Err("output exceeds 4096 bytes".into());
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Ok(()),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(format!("reading output: {e}")),
        }
    }
}

fn stderr_detail(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes)
        .chars()
        .filter(|c| !c.is_control() || *c == '\n' || *c == '\t')
        .collect()
}

pub fn run(extension: &Extension) -> Result<(String, Option<String>), String> {
    let mut child = Command::new(&extension.command[0])
        .args(&extension.command[1..])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .process_group(0)
        .spawn()
        .map_err(|e| format!("cannot start: {e}"))?;
    let mut stdout = child.stdout.take().ok_or("stdout unavailable")?;
    let mut stderr = child.stderr.take().ok_or("stderr unavailable")?;
    let mut out = Vec::new();
    let mut err = Vec::new();
    let mut remaining = LIMIT;
    let result = (|| {
        nonblocking(stdout.as_raw_fd()).map_err(|e| e.to_string())?;
        nonblocking(stderr.as_raw_fd()).map_err(|e| e.to_string())?;
        let deadline = Instant::now() + Duration::from_millis(extension.timeout_ms);
        loop {
            drain(&mut stdout, &mut out, &mut remaining)?;
            drain(&mut stderr, &mut err, &mut remaining)?;
            if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
                kill_group(&mut child);
                drain(&mut stdout, &mut out, &mut remaining)?;
                drain(&mut stderr, &mut err, &mut remaining)?;
                return if status.success() {
                    Ok(())
                } else {
                    Err(format!("exit {status}"))
                };
            }
            if Instant::now() >= deadline {
                return Err(format!("timed out after {} ms", extension.timeout_ms));
            }
            thread::sleep(Duration::from_millis(5));
        }
    })();
    kill_group(&mut child);
    let _ = child.wait();
    if let Err(reason) = result {
        let detail = stderr_detail(&err);
        return Err(if detail.is_empty() {
            reason
        } else {
            format!("{reason}: {detail}")
        });
    }
    let text = std::str::from_utf8(&out).map_err(|_| "stdout is not UTF-8")?;
    if !plain_text(text) {
        return Err("stdout contains control characters".into());
    }
    let stderr_text = std::str::from_utf8(&err).map_err(|_| "stderr is not UTF-8")?;
    if !plain_text(stderr_text) {
        return Err("stderr contains control characters".into());
    }
    let mut lines = text.lines();
    let value = lines
        .next()
        .filter(|s| !s.trim().is_empty())
        .ok_or("stdout value is empty")?;
    let detail = lines.next().filter(|s| !s.is_empty()).map(str::to_owned);
    Ok((value.to_owned(), detail))
}
