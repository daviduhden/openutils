#!/usr/bin/env python3
# pty_run.py - run a program on a pseudo-terminal, feed it key
# sequences, and return (or print) its output.
#
# Requires Python 3.  This helper is used by the ee behavioural tests
# only; the other OpenUtils test suites are POSIX shell.
#
# usage: pty_run.py <program> <args...> -- <input-sequence>...
#
# Each input sequence may contain \xHH escapes (or plain text) and is
# sent to the program with a small delay between sequences, ending
# with a final drain period.

import os
import pty
import select
import sys
import time


def decode(s):
    out = b""
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == "x" and i + 3 < len(s):
                out += bytes([int(s[i + 2:i + 4], 16)])
                i += 4
                continue
            esc = {"n": 10, "r": 13, "t": 9, "e": 27, "\\": 92}
            if nxt in esc:
                out += bytes([esc[nxt]])
                i += 2
                continue
        out += s[i].encode()
        i += 1
    return out


class Session:
    """A program running on a pseudo-terminal."""

    def __init__(self, argv, env=None):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            if env:
                os.environ.update(env)
            os.environ.setdefault("TERM", "xterm")
            try:
                os.execvp(argv[0], argv)
            except OSError:
                os._exit(127)
        self.buf = b""

    def pump(self, sec):
        deadline = time.time() + sec
        while time.time() < deadline:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    d = os.read(self.fd, 4096)
                except OSError:
                    return False
                if not d:
                    return False
                self.buf += d
        return True

    def write(self, data):
        os.write(self.fd, data)

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


def run(argv, inputs=(), env=None, startup=2.0, delay=1.0, drain=2.0):
    """Run argv on a pty, send inputs, and return the captured output."""
    session = Session(argv, env)
    session.pump(startup)
    for seq in inputs:
        session.write(decode(seq))
        session.pump(delay)
    session.pump(drain)
    session.close()
    return session.buf


def main():
    args = sys.argv[1:]
    sep = args.index("--") if "--" in args else len(args)
    argv = args[:sep]
    inputs = args[sep + 1:]
    sys.stdout.write(run(argv, inputs).decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
