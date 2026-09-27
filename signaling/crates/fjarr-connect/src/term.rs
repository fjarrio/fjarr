//! The operator's own terminal, for `shell`: raw mode as a guard that restores on drop, the window
//! size, a notification when it changes, and the signals that end the client (docs/27#shell).
//!
//! Termios on Unix, the console API on Windows. Nothing here knows about the robot, so it is the
//! only part of `shell` that differs by platform, and `make connect-platform-check` type-checks it
//! for macOS and Windows (docs/04).
//!
//! spec: docs/27-network-tunnel.md#shell
use std::io;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Size {
    pub cols: u16,
    pub rows: u16,
}

impl Size {
    /// What a pty gets when nothing says otherwise: the VT100's.
    pub const DEFAULT: Size = Size { cols: 80, rows: 24 };
}

/// Stdin is a terminal, so there is a raw mode to set.
pub fn stdin_is_tty() -> bool {
    std::io::IsTerminal::is_terminal(&std::io::stdin())
}

/// `$TERM`, when there is one worth passing on: the robot's pty describes itself with it, and a
/// shell that guessed wrong draws wrong.
pub fn name() -> Option<String> {
    usable(std::env::var("TERM").ok())
}

fn usable(term: Option<String>) -> Option<String> {
    term.filter(|t| !t.is_empty() && t != "dumb")
}

pub use imp::{size, RawMode, Resizes, Signals};

#[cfg(unix)]
mod imp {
    use super::Size;
    use std::io;
    use std::os::fd::RawFd;

    /// The window size of whichever standard stream is a terminal: output first, since that is what
    /// the robot draws on.
    pub fn size() -> Option<Size> {
        [libc::STDOUT_FILENO, libc::STDIN_FILENO, libc::STDERR_FILENO]
            .into_iter()
            .find_map(size_of)
    }

    pub(super) fn size_of(fd: RawFd) -> Option<Size> {
        // SAFETY: TIOCGWINSZ writes one `winsize` into a value of that type.
        let mut ws: libc::winsize = unsafe { std::mem::zeroed() };
        let rc = unsafe { libc::ioctl(fd, libc::TIOCGWINSZ as _, &mut ws) };
        (rc == 0 && ws.ws_col > 0 && ws.ws_row > 0).then_some(Size {
            cols: ws.ws_col,
            rows: ws.ws_row,
        })
    }

    /// Raw mode on one terminal until dropped. Drop is the only way out of it, so every return path
    /// and an unwinding panic restore the operator's terminal (docs/27#shell).
    pub struct RawMode {
        fd: RawFd,
        saved: libc::termios,
    }

    impl RawMode {
        pub fn enter() -> io::Result<RawMode> {
            Self::enter_fd(libc::STDIN_FILENO)
        }

        pub fn enter_fd(fd: RawFd) -> io::Result<RawMode> {
            // SAFETY: tcgetattr/tcsetattr read and write one `termios` for an fd we were handed.
            unsafe {
                let mut saved: libc::termios = std::mem::zeroed();
                if libc::tcgetattr(fd, &mut saved) != 0 {
                    return Err(io::Error::last_os_error());
                }
                let mut raw = saved;
                // Everything off, as `ssh -t` does: no echo, no line editing, no signal keys — Ctrl-C
                // is a byte for the robot's shell — and no output processing, because the robot's
                // pty already turned its newlines into CRLF.
                libc::cfmakeraw(&mut raw);
                if libc::tcsetattr(fd, libc::TCSADRAIN, &raw) != 0 {
                    return Err(io::Error::last_os_error());
                }
                Ok(RawMode { fd, saved })
            }
        }
    }

    impl Drop for RawMode {
        fn drop(&mut self) {
            // SAFETY: as above; the saved settings are the ones read from this fd.
            unsafe {
                libc::tcsetattr(self.fd, libc::TCSADRAIN, &self.saved);
            }
        }
    }

    /// `SIGWINCH`: the terminal was resized.
    pub struct Resizes(tokio::signal::unix::Signal);

    impl Resizes {
        pub fn new() -> io::Result<Resizes> {
            use tokio::signal::unix::{signal, SignalKind};
            Ok(Resizes(signal(SignalKind::window_change())?))
        }

        pub async fn changed(&mut self) {
            self.0.recv().await;
        }
    }

    /// The signals a client can be sent that it must survive long enough to restore the terminal
    /// and close the pty: terminate, hang-up (the window closed), and interrupt and quit from `kill`
    /// — raw mode means the keyboard sends those as bytes, not signals.
    pub struct Signals {
        term: tokio::signal::unix::Signal,
        hup: tokio::signal::unix::Signal,
        int: tokio::signal::unix::Signal,
        quit: tokio::signal::unix::Signal,
    }

    impl Signals {
        pub fn new() -> io::Result<Signals> {
            use tokio::signal::unix::{signal, SignalKind};
            Ok(Signals {
                term: signal(SignalKind::terminate())?,
                hup: signal(SignalKind::hangup())?,
                int: signal(SignalKind::interrupt())?,
                quit: signal(SignalKind::quit())?,
            })
        }

        /// The next one, as its number, for an exit status of 128 + it.
        pub async fn next(&mut self) -> i32 {
            tokio::select! {
                _ = self.term.recv() => libc::SIGTERM,
                _ = self.hup.recv() => libc::SIGHUP,
                _ = self.int.recv() => libc::SIGINT,
                _ = self.quit.recv() => libc::SIGQUIT,
            }
        }
    }
}

#[cfg(windows)]
mod imp {
    use super::Size;
    use std::io;
    use windows_sys::Win32::Foundation::{HANDLE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::System::Console::{
        GetConsoleMode, GetConsoleScreenBufferInfo, GetStdHandle, SetConsoleMode, CONSOLE_MODE,
        CONSOLE_SCREEN_BUFFER_INFO, DISABLE_NEWLINE_AUTO_RETURN, ENABLE_ECHO_INPUT,
        ENABLE_LINE_INPUT, ENABLE_PROCESSED_INPUT, ENABLE_PROCESSED_OUTPUT,
        ENABLE_VIRTUAL_TERMINAL_INPUT, ENABLE_VIRTUAL_TERMINAL_PROCESSING, STD_HANDLE,
        STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
    };

    fn handle(which: STD_HANDLE) -> Option<HANDLE> {
        // SAFETY: GetStdHandle has no preconditions.
        let h = unsafe { GetStdHandle(which) };
        (!h.is_null() && h != INVALID_HANDLE_VALUE).then_some(h)
    }

    fn mode(h: HANDLE) -> Option<CONSOLE_MODE> {
        let mut m: CONSOLE_MODE = 0;
        // SAFETY: writes one mode into `m` for a console handle; fails harmlessly otherwise.
        (unsafe { GetConsoleMode(h, &mut m) } != 0).then_some(m)
    }

    /// The visible window, not the scrollback buffer: that is what the robot's shell draws on.
    pub fn size() -> Option<Size> {
        let h = handle(STD_OUTPUT_HANDLE)?;
        // SAFETY: fills one CONSOLE_SCREEN_BUFFER_INFO; zeroed is a valid value of it.
        let mut info: CONSOLE_SCREEN_BUFFER_INFO = unsafe { std::mem::zeroed() };
        if unsafe { GetConsoleScreenBufferInfo(h, &mut info) } == 0 {
            return None;
        }
        let w = info.srWindow;
        let cols = (w.Right - w.Left + 1).max(0) as u16;
        let rows = (w.Bottom - w.Top + 1).max(0) as u16;
        (cols > 0 && rows > 0).then_some(Size { cols, rows })
    }

    /// Raw mode on the console until dropped, both handles: input with no line editing, echo or
    /// Ctrl-C processing and with virtual-terminal input, so arrows and function keys arrive as the
    /// escape sequences the robot's pty expects; output that interprets the robot's escape
    /// sequences.
    pub struct RawMode {
        input: HANDLE,
        output: Option<HANDLE>,
        saved_in: CONSOLE_MODE,
        saved_out: CONSOLE_MODE,
    }

    impl RawMode {
        pub fn enter() -> io::Result<RawMode> {
            let input = handle(STD_INPUT_HANDLE).ok_or_else(io::Error::last_os_error)?;
            let saved_in = mode(input).ok_or_else(io::Error::last_os_error)?;
            let output = handle(STD_OUTPUT_HANDLE);
            let saved_out = output.and_then(mode).unwrap_or(0);
            let raw_in = (saved_in
                & !(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
                | ENABLE_VIRTUAL_TERMINAL_INPUT;
            // SAFETY: SetConsoleMode on handles GetStdHandle returned.
            if unsafe { SetConsoleMode(input, raw_in) } == 0 {
                return Err(io::Error::last_os_error());
            }
            if let Some(out) = output.filter(|_| saved_out != 0) {
                let raw_out = saved_out
                    | ENABLE_PROCESSED_OUTPUT
                    | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                    | DISABLE_NEWLINE_AUTO_RETURN;
                unsafe { SetConsoleMode(out, raw_out) };
            }
            Ok(RawMode {
                input,
                output,
                saved_in,
                saved_out,
            })
        }
    }

    impl Drop for RawMode {
        fn drop(&mut self) {
            // SAFETY: restoring the modes read from these same handles.
            unsafe {
                SetConsoleMode(self.input, self.saved_in);
                if let Some(out) = self.output.filter(|_| self.saved_out != 0) {
                    SetConsoleMode(out, self.saved_out);
                }
            }
        }
    }

    /// Windows has no `SIGWINCH`, and the console's resize record would have to be read from the
    /// same input the keystrokes come from, so the size is polled (docs/27#shell).
    pub struct Resizes {
        tick: tokio::time::Interval,
        last: Option<Size>,
    }

    impl Resizes {
        pub fn new() -> io::Result<Resizes> {
            Ok(Resizes {
                tick: tokio::time::interval(std::time::Duration::from_millis(250)),
                last: size(),
            })
        }

        pub async fn changed(&mut self) {
            loop {
                self.tick.tick().await;
                let now = size();
                if now != self.last {
                    self.last = now;
                    return;
                }
            }
        }
    }

    /// The console's own ways to end a process: closing the window, Ctrl-Break, and Ctrl-C when
    /// something else sent it (raw mode makes the key a byte). Numbered as the Unix signals they
    /// stand for, so the exit status reads the same.
    pub struct Signals {
        close: tokio::signal::windows::CtrlClose,
        brk: tokio::signal::windows::CtrlBreak,
        c: tokio::signal::windows::CtrlC,
    }

    impl Signals {
        pub fn new() -> io::Result<Signals> {
            use tokio::signal::windows::{ctrl_break, ctrl_c, ctrl_close};
            Ok(Signals {
                close: ctrl_close()?,
                brk: ctrl_break()?,
                c: ctrl_c()?,
            })
        }

        pub async fn next(&mut self) -> i32 {
            tokio::select! {
                _ = self.close.recv() => 1,  // SIGHUP: the window went away
                _ = self.brk.recv() => 3,    // SIGQUIT
                _ = self.c.recv() => 2,      // SIGINT
            }
        }
    }
}

/// Read stdin on a thread of its own and hand each read to `tx` until end of input. A thread,
/// not the runtime's blocking pool: a read on a terminal never returns until a key is pressed, and
/// the runtime waits for its pool at shutdown, where this thread is simply abandoned.
pub fn read_stdin(tx: tokio::sync::mpsc::Sender<Vec<u8>>) -> io::Result<()> {
    std::thread::Builder::new()
        .name("fjarr-stdin".into())
        .spawn(move || {
            use std::io::Read;
            let mut stdin = std::io::stdin().lock();
            let mut buf = [0u8; 4096];
            loop {
                match stdin.read(&mut buf) {
                    Ok(0) => break,
                    Ok(n) => {
                        if tx.blocking_send(buf[..n].to_vec()).is_err() {
                            break;
                        }
                    }
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(_) => break,
                }
            }
        })?;
    Ok(())
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    /// A pty pair, so raw mode can be entered and left on a real terminal without the test's own.
    fn pty() -> (std::os::fd::RawFd, std::os::fd::RawFd) {
        let (mut master, mut slave) = (0, 0);
        let ws = libc::winsize {
            ws_row: 33,
            ws_col: 101,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        // SAFETY: openpty writes two fds; the null name and termios are allowed.
        let rc = unsafe {
            libc::openpty(
                &mut master,
                &mut slave,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                &ws,
            )
        };
        assert_eq!(rc, 0, "openpty: {}", io::Error::last_os_error());
        (master, slave)
    }

    fn attrs(fd: std::os::fd::RawFd) -> libc::termios {
        // SAFETY: reads one termios.
        unsafe {
            let mut t: libc::termios = std::mem::zeroed();
            assert_eq!(libc::tcgetattr(fd, &mut t), 0);
            t
        }
    }

    fn same(a: &libc::termios, b: &libc::termios) -> bool {
        a.c_iflag == b.c_iflag
            && a.c_oflag == b.c_oflag
            && a.c_cflag == b.c_cflag
            && a.c_lflag == b.c_lflag
            && a.c_cc == b.c_cc
    }

    /// docs/27#shell: the operator's terminal is exactly as it was once the guard is gone — on a
    /// normal return and on a panic unwinding through it.
    #[test]
    fn raw_mode_is_entered_and_restored_exactly() {
        let (master, slave) = pty();
        let before = attrs(slave);
        assert_ne!(before.c_lflag & libc::ICANON, 0, "a fresh pty is canonical");
        {
            let _raw = RawMode::enter_fd(slave).unwrap();
            let during = attrs(slave);
            assert_eq!(during.c_lflag & (libc::ICANON | libc::ECHO | libc::ISIG), 0);
            assert_eq!(during.c_oflag & libc::OPOST, 0);
        }
        assert!(same(&attrs(slave), &before), "drop restored the terminal");

        let unwound = std::panic::catch_unwind(|| {
            let _raw = RawMode::enter_fd(slave).unwrap();
            panic!("the shell loop panicked");
        });
        assert!(unwound.is_err());
        assert!(
            same(&attrs(slave), &before),
            "a panic restored the terminal"
        );
        unsafe {
            libc::close(slave);
            libc::close(master);
        }
    }

    #[test]
    fn the_size_is_the_terminals_own() {
        let (master, slave) = pty();
        assert_eq!(
            imp::size_of(slave),
            Some(Size {
                cols: 101,
                rows: 33
            })
        );
        // Not a terminal: no size, rather than a zero one the robot would take literally.
        let file = std::fs::File::open("/dev/null").unwrap();
        assert_eq!(imp::size_of(std::os::fd::AsRawFd::as_raw_fd(&file)), None);
        unsafe {
            libc::close(slave);
            libc::close(master);
        }
    }

    /// Unset, empty or `dumb` leaves `term` out of `open`, so the robot's own default applies.
    #[test]
    fn a_dumb_or_missing_term_is_not_passed_on() {
        assert_eq!(
            usable(Some("xterm-256color".into())).as_deref(),
            Some("xterm-256color")
        );
        assert_eq!(usable(Some("dumb".into())), None);
        assert_eq!(usable(Some(String::new())), None);
        assert_eq!(usable(None), None);
    }
}
