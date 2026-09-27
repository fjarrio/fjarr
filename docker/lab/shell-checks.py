#!/usr/bin/env python3
"""The `fjarr-connect shell` gate (docs/17 "Next for the native client", docs/27#shell).

    shell-checks.py <fjarr-connect binary> <robot id> <signaling url>
    SHELL_GRANT          a grant carrying fjarr.terminal
    SHELL_GRANT_DENIED   a grant for the same robot without it

Each run gets a pty this script owns, so it can do what a person at a terminal does — type, resize
the window — and read what only the terminal knows: its termios, before, during and after. The
claims, one per check:

  1. an interactive round trip: a command the robot's shell evaluates, not just echoes
  2. the size the client opened with is what the robot's `stty size` reports
  3. a resize of the local window is seen by the robot's `stty size`
  4. `exit 7` on the robot is the client's exit status 7, and no log line of the client's own
     landed on the screen
  5. the local terminal is restored after the shell exits, after the session is killed under it
     (heartbeats withheld, so the agent's liveness budget ends it), and after a SIGTERM
  6. a grant without the terminal is refused as policy, with 255, and the terminal never touched

The binary should be a copy with no file capability: the shell needs no privilege (docs/27#shell).
"""
import errno
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

BIN, ROBOT, SERVER = sys.argv[1:4]
GRANT = os.environ["SHELL_GRANT"]
DENIED = os.environ["SHELL_GRANT_DENIED"]

failures = []


def check(ok, what, detail=""):
    print(f"shell-checks: {'PASS' if ok else 'FAIL'} {what}{(' — ' + detail) if detail and not ok else ''}", flush=True)
    if not ok:
        failures.append(what)


def cooked_default():
    """What a fresh terminal's settings are: the baseline every run must be returned to."""
    master, slave = pty.openpty()
    try:
        return termios.tcgetattr(slave)
    finally:
        os.close(master)
        os.close(slave)


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


class Run:
    """One `fjarr-connect shell` on a pty of its own."""

    def __init__(self, grant, *extra, rows=24, cols=80):
        pid, fd = pty.fork()
        if pid == 0:
            set_size(0, rows, cols)  # the size the client will open with
            os.environ["TERM"] = "xterm-256color"
            os.environ.pop("FJARR_LOG", None)  # the client's own default: nothing on the screen but the robot's
            os.execv(BIN, [BIN, "shell", ROBOT, "--server", SERVER, "--grant", grant, *extra])
        self.pid, self.fd, self.out, self.status = pid, fd, b"", None

    def attrs(self):
        # On Linux a termios request on the master is answered for the slave: this is the client's
        # terminal as the client left it.
        return termios.tcgetattr(self.fd)

    def raw(self):
        return self.attrs()[3] & (termios.ICANON | termios.ECHO | termios.ISIG) == 0

    def read_some(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        try:
            chunk = os.read(self.fd, 65536)
        except OSError as e:
            if e.errno == errno.EIO:  # the client exited and the slave closed
                return None
            raise
        if not chunk:
            return None
        self.out += chunk
        return True

    def expect(self, pattern, timeout=15.0, since=0):
        deadline = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < deadline:
            m = rx.search(self.out, since)
            if m:
                return m
            if self.read_some(min(0.2, max(0.0, deadline - time.monotonic()))) is None:
                break
        return rx.search(self.out, since)

    def type(self, text):
        os.write(self.fd, text.encode())

    def ready(self):
        """Wait until the robot's shell evaluates what it is given, and time one round trip."""
        if not self.expect(rb"\$ ", 30):
            return None
        mark = len(self.out)
        t0 = time.monotonic()
        self.type("echo round-$((6*7))-trip\r")
        m = self.expect(rb"round-42-trip", 15, mark)
        return (time.monotonic() - t0) * 1000 if m else None

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            done, status = os.waitpid(self.pid, os.WNOHANG)
            if done:
                self.status = os.waitstatus_to_exitcode(status)
                return self.status
            self.read_some(0.2)  # keep draining, or a full pty would stall the client
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        return None

    def text(self):
        return self.out.decode(errors="replace")

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


baseline = cooked_default()

# 1–5: the interactive session, a resize, and the exit status.
run = Run(GRANT, rows=24, cols=80)
rtt = run.ready()
check(rtt is not None, "an interactive round trip on the robot's shell", run.text()[-400:])
if rtt is not None:
    print(f"shell-checks: note — round trip {rtt:.0f} ms (typed command to evaluated output)")
check(run.raw(), "the local terminal is in raw mode while attached")

mark = len(run.out)
run.type("stty size\r")
check(bool(run.expect(rb"(?<![0-9])24 80\r\n", 10, mark)), "the robot's stty size is the size the client opened with (24 80)",
      run.text()[mark:][-300:])

set_size(run.fd, 40, 120)  # the kernel sends the client SIGWINCH, as a window manager's resize does
time.sleep(0.5)
mark = len(run.out)
run.type("stty size\r")
check(bool(run.expect(rb"(?<![0-9])40 120\r\n", 10, mark)), "a local resize is seen by the robot's stty size (40 120)",
      run.text()[mark:][-300:])

run.type("exit 7\r")
status = run.wait(20)
check(status == 7, "the shell's exit status is the client's (exit 7 → 7)", f"got {status}")
check(run.attrs() == baseline, "the local terminal is restored after the shell exits")
# The client's libraries log from their own threads; a log line in raw mode lands mid-screen with no
# carriage return. webrtc-rs reports a lab TURN retry as an ERROR, which is how this was found.
# The level may be coloured (a tty gets ANSI), so the word boundary is an escape as often as a space.
logged = [m.group(0) for m in re.finditer(rb"(?:\x1b\[[0-9;]*m|\b)(?:ERROR|WARN|INFO)(?:\x1b\[0m|\b)[^\r\n]*", run.out)]
check(not logged, "nothing on the screen but the robot's output", b"; ".join(logged[:3]).decode(errors="replace"))
run.close()

# 5: a link killed under a live shell. Heartbeats withheld, so the agent ends the session after three
# are missed (docs/08) — from the client's side, indistinguishable from the robot going away.
run = Run(GRANT, "--no-heartbeat")
ok = run.ready() is not None
check(ok and run.raw(), "a second shell is attached and raw before its link is killed", run.text()[-300:])
status = run.wait(60)
check(status == 255, "a killed link exits 255: the shell's status is unknown", f"got {status}")
check("was lost" in run.text(), "a killed link says so", run.text()[-300:])
check(run.attrs() == baseline, "the local terminal is restored after a killed link")
run.close()

# 5: a signal to the client.
run = Run(GRANT)
ok = run.ready() is not None
check(ok and run.raw(), "a third shell is attached and raw before the client is signalled", run.text()[-300:])
os.kill(run.pid, signal.SIGTERM)
status = run.wait(20)
check(status == 128 + signal.SIGTERM, "SIGTERM to the client exits 143", f"got {status}")
check(run.attrs() == baseline, "the local terminal is restored after SIGTERM")
run.close()

# 6: a grant without the terminal is the backend's policy, said as such.
run = Run(DENIED)
status = run.wait(60)
check(status == 255, "a grant without fjarr.terminal exits 255", f"got {status}")
check("not given a shell" in run.text(), "…and says the shell was not given", run.text()[-300:])
check(run.attrs() == baseline, "…and never touched the local terminal")
run.close()

if failures:
    print(f"shell-checks: FAIL — {len(failures)} check(s): " + "; ".join(failures))
    sys.exit(1)
print("shell-checks: PASS — interactive, resized, exit status carried, terminal restored every way out")
