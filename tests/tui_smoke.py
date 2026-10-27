#!/usr/bin/env python3
"""Exercise terminal startup, tab navigation, resizing, and graceful exit."""
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

with tempfile.TemporaryDirectory(prefix="jmcomic-tui-") as directory:
    executable = os.path.join(directory, "jmcomic")
    shutil.copy2(sys.argv[1], executable)  # Isolate settings beside the executable.
    pid, terminal = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["LC_ALL"] = "C.UTF-8"
        os.execv(executable, [executable])
    transcript = bytearray()
    def read_until(needle, timeout=5):
        start = len(transcript)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if select.select([terminal], [], [], 0.05)[0]:
                try:
                    chunk = os.read(terminal, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                transcript.extend(chunk)
                if b"\x1b[6n" in chunk:
                    os.write(terminal, b"\x1b[1;1R")
                if needle in transcript[start:]:
                    return
        raise AssertionError("Terminal did not render %r; tail: %r" % (needle, transcript[-800:]))
    try:
        fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        read_until(b"Search by keyword")
        for marker in [b"Open a comic from", b"Guest / session", b"chapters complete", b"API origin"]:
            os.write(terminal, b"\x1b[C")
            read_until(marker)
        fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        read_until(b"API origin")
        os.write(terminal, b"\x11")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result, status = os.waitpid(pid, os.WNOHANG)
            if result:
                assert os.waitstatus_to_exitcode(status) == 0, status
                pid = None
                print("TUI startup, five tabs, resize, and graceful quit passed")
                break
            if select.select([terminal], [], [], 0.05)[0]:
                try:
                    os.read(terminal, 65536)
                except OSError:
                    pass
        else:
            raise AssertionError("Terminal failed to quit promptly")
    finally:
        if pid:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(terminal)
