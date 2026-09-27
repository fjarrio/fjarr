//! `fjarr-connect login`: obtain an operator credential from the customer's dashboard
//! (docs/27#logging-in, docs/21#cli-login).
//!
//! Two shapes. **Loopback**: listen on 127.0.0.1 at a random port, open the dashboard's login route
//! with the port and a random `state`, and accept exactly one POST that echoes that state. **Code**:
//! ask the backend for a code, print the URL and the code, and poll with the separate poll token
//! until a human approves it wherever a browser exists. The code path is what a workstation
//! reached over ssh gets, which in robotics is the common case; it is also what the lab drives.
//!
//! What this never does: hold the tenant's control-plane token, or trust a callback from anywhere
//! but loopback (it never listens anywhere else).
use std::io::Write as _;

use anyhow::{anyhow, bail, Context, Result};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

use crate::operator_api;

/// Where the dashboard's login route is, given where the operator API is mounted: the API is
/// usually `<site>/api`, and the route is a page on the site, so `/api` is dropped when present.
pub fn default_login_url(api_base: &str) -> String {
    let base = api_base.trim_end_matches('/');
    let site = base.strip_suffix("/api").unwrap_or(base);
    format!("{site}/cli-login")
}

fn random_state() -> String {
    // 128 bits from the OS, hex: enough that a stray POST cannot guess it. Through `getrandom`
    // rather than the Linux syscall, because this binary also builds where `shell` runs (docs/04).
    let mut buf = [0u8; 16];
    if getrandom::fill(&mut buf).is_err() {
        // Fall back to the clock rather than fail: `state` guards against a stray post on
        // loopback, not against an attacker who already runs code as this user.
        let t = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        buf.copy_from_slice(&t.to_le_bytes());
    }
    buf.iter().map(|b| format!("{b:02x}")).collect()
}

/// Try the platform's opener. Returns false rather than failing: a machine with no browser is a
/// case to handle, not an error (docs/27#logging-in).
pub fn open_browser(url: &str) -> bool {
    let candidates: &[&str] = if cfg!(target_os = "macos") {
        &["open"]
    } else {
        &["xdg-open", "sensible-browser", "x-www-browser"]
    };
    if std::env::var_os("DISPLAY").is_none()
        && std::env::var_os("WAYLAND_DISPLAY").is_none()
        && !cfg!(target_os = "macos")
    {
        return false; // nothing to open a window on
    }
    for program in candidates {
        if let Ok(status) = std::process::Command::new(program)
            .arg(url)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
        {
            if status.success() {
                return true;
            }
        }
    }
    false
}

/// The loopback flow. Returns the credential the dashboard posted.
pub async fn loopback(login_url: &str, timeout: std::time::Duration) -> Result<String> {
    let listener = TcpListener::bind(("127.0.0.1", 0))
        .await
        .context("listening on loopback")?;
    let port = listener.local_addr()?.port();
    let state = random_state();
    let url = format!("{login_url}?port={port}&state={state}");

    if open_browser(&url) {
        println!("opening browser… approve the sign-in there (listening on 127.0.0.1:{port})");
    } else {
        println!("no browser could be opened here. Open this on any machine with one:\n  {url}\n(or run `fjarr-connect login --code` for a code you can approve elsewhere)");
    }

    serve_one(listener, &state, timeout).await
}

/// The code flow: prints what the human needs, polls until approved, expired, or `timeout`.
pub async fn code(
    api: &operator_api::Client,
    login_url: &str,
    timeout: std::time::Duration,
) -> Result<String> {
    let issued = api.create_code().await?;
    let url = format!("{login_url}?code={}", issued.code);
    println!(
        "open this on any machine with a browser and approve the code:\n  {url}\n\n  code: {}\n",
        issued.code
    );
    println!("waiting for approval (expires in {} s)…", issued.expires_in);
    let deadline = tokio::time::Instant::now() + timeout;
    let mut dots = 0;
    loop {
        match api.poll_code(&issued.poll_token).await? {
            operator_api::Poll::Approved { credential } => {
                println!();
                return Ok(credential);
            }
            operator_api::Poll::Expired => {
                println!();
                bail!("the code expired before it was approved — run login again");
            }
            operator_api::Poll::Pending => {}
        }
        if tokio::time::Instant::now() >= deadline {
            println!();
            bail!("nobody approved the code within {}s", timeout.as_secs());
        }
        dots += 1;
        if dots % 10 == 0 {
            print!(".");
            let _ = std::io::stdout().flush();
        }
        tokio::time::sleep(std::time::Duration::from_millis(1500)).await;
    }
}

/// A grant from the `grant_command` escape hatch: the command's stdout, trimmed.
pub fn grant_from_command(template: &str, robot_id: &str) -> Result<String> {
    let cmd = template.replace("{robot}", robot_id);
    let out = std::process::Command::new("sh")
        .arg("-c")
        .arg(&cmd)
        .output()
        .with_context(|| format!("running `{cmd}`"))?;
    if !out.status.success() {
        bail!(
            "`{cmd}` exited {}: {}",
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    let grant = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if grant.is_empty() {
        return Err(anyhow!("`{cmd}` printed nothing"));
    }
    Ok(grant)
}

async fn serve_one(
    listener: TcpListener,
    state: &str,
    timeout: std::time::Duration,
) -> Result<String> {
    let accept = async {
        loop {
            let (mut socket, _) = listener.accept().await?;
            let mut buf = vec![0u8; 16 * 1024];
            let mut read = 0;
            let request = loop {
                let n = socket.read(&mut buf[read..]).await?;
                if n == 0 {
                    break None;
                }
                read += n;
                let text = String::from_utf8_lossy(&buf[..read]).to_string();
                if let Some(idx) = text.find("\r\n\r\n") {
                    let head = &text[..idx];
                    let content_length = head
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length:")
                                .map(|v| v.trim().parse::<usize>().unwrap_or(0))
                        })
                        .unwrap_or(0);
                    if read >= idx + 4 + content_length {
                        break Some(text);
                    }
                }
                if read == buf.len() {
                    break None;
                }
            };
            let Some(request) = request else {
                let _ = socket
                    .write_all(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                    .await;
                continue;
            };
            let first = request.lines().next().unwrap_or("");
            if first.starts_with("OPTIONS ") {
                let _ = socket
                    .write_all(b"HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: content-type\r\nContent-Length: 0\r\n\r\n")
                    .await;
                continue;
            }
            if !first.starts_with("POST /callback") {
                let _ = socket
                    .write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
                    .await;
                continue;
            }
            let body = request.split_once("\r\n\r\n").map(|(_, b)| b).unwrap_or("");
            let posted: serde_json::Value =
                serde_json::from_str(body).unwrap_or(serde_json::Value::Null);
            let posted_state = posted.get("state").and_then(|v| v.as_str()).unwrap_or("");
            let credential = posted
                .get("credential")
                .and_then(|v| v.as_str())
                .unwrap_or("");
            if posted_state != state || credential.is_empty() {
                let _ = socket
                    .write_all(b"HTTP/1.1 403 Forbidden\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n")
                    .await;
                eprintln!("fjarr-connect: ignored a post to the callback that did not carry this login's state");
                continue;
            }
            let _ = socket
                .write_all(b"HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n")
                .await;
            return Ok::<String, anyhow::Error>(credential.to_string());
        }
    };
    match tokio::time::timeout(timeout, accept).await {
        Ok(r) => r,
        Err(_) => bail!("nobody approved the sign-in within {}s", timeout.as_secs()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_login_route_is_the_site_not_the_api() {
        assert_eq!(
            default_login_url("https://fleet.acme.com/api"),
            "https://fleet.acme.com/cli-login"
        );
        assert_eq!(
            default_login_url("https://fleet.acme.com/api/"),
            "https://fleet.acme.com/cli-login"
        );
        assert_eq!(
            default_login_url("http://demo-backend:9090"),
            "http://demo-backend:9090/cli-login"
        );
    }

    #[test]
    fn state_is_long_and_never_the_same_twice() {
        let a = random_state();
        let b = random_state();
        assert_eq!(a.len(), 32);
        assert_ne!(a, b);
    }

    /// The listener accepts exactly one post, and only one carrying its state. Driven with plain
    /// TCP, the way the component's fetch arrives, preflight included.
    #[tokio::test]
    async fn loopback_takes_one_post_with_the_right_state_and_rejects_the_rest() {
        // `loopback` is bind + browser + serve_one; the browser half has nothing to open in a
        // test, so this drives the listener half with the same listener it would bind.
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let flow = tokio::spawn(async move {
            serve_one(
                listener,
                "expected-state",
                std::time::Duration::from_secs(5),
            )
            .await
        });

        async fn post(port: u16, body: &str) -> String {
            let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port))
                .await
                .unwrap();
            let req = format!("POST /callback HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}", body.len(), body);
            s.write_all(req.as_bytes()).await.unwrap();
            let mut reply = String::new();
            let mut buf = [0u8; 256];
            if let Ok(n) = s.read(&mut buf).await {
                reply.push_str(&String::from_utf8_lossy(&buf[..n]));
            }
            reply
        }
        async fn preflight(port: u16) -> String {
            let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port))
                .await
                .unwrap();
            s.write_all(
                b"OPTIONS /callback HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://dash\r\n\r\n",
            )
            .await
            .unwrap();
            let mut buf = [0u8; 256];
            let n = s.read(&mut buf).await.unwrap();
            String::from_utf8_lossy(&buf[..n]).to_string()
        }

        assert!(
            preflight(port).await.starts_with("HTTP/1.1 204"),
            "a CORS preflight must be answered"
        );
        assert!(post(port, r#"{"credential":"stolen","state":"wrong"}"#)
            .await
            .starts_with("HTTP/1.1 403"));
        assert!(post(port, r#"{"credential":"","state":"expected-state"}"#)
            .await
            .starts_with("HTTP/1.1 403"));
        assert!(
            post(port, r#"{"credential":"cred-ok","state":"expected-state"}"#)
                .await
                .starts_with("HTTP/1.1 200")
        );
        assert_eq!(flow.await.unwrap().unwrap(), "cred-ok");
    }
}
