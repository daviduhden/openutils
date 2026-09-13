#!/usr/bin/env python3
# PTY-based regression tests for ee: signal handling and malformed
# terminfo robustness.  Generous per-key delays are used because menu
# redraws are timing-sensitive.
#
# Requires Python 3.

import fcntl
import os
import signal
import struct
import sys
import tempfile
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pty_run

ROOT = os.path.dirname(os.path.dirname(HERE))
EE = os.environ.get("EE", os.path.join(ROOT, "ee", "ee"))

PASS = 0
FAIL = 0
FAILED = []


def ok(desc):
    global PASS
    PASS += 1
    print("ok %d - %s" % (PASS, desc))


def notok(desc):
    global FAIL
    FAIL += 1
    FAILED.append(desc)
    print("not ok - %s" % desc)


def check(desc, value):
    if value:
        ok(desc)
    else:
        notok(desc)


def build_terminfo(path, clear, names):
    """Write a minimal compiled terminfo entry (magic 282)."""
    NS = 190
    numbers = [0xFFFF] * 40
    numbers[0] = 80
    numbers[2] = 24
    soff = [0xFFFF] * NS
    table = bytearray()

    def addstr(idx, s):
        soff[idx] = len(table)
        table.extend(s.encode() + b"\0")

    addstr(5, clear)
    addstr(10, "\033[%i%d;%dH")
    addstr(104, "")
    hdr = struct.pack("<HHHHHH", 282, len(names), 0, len(numbers), NS,
                      len(table))
    # the number section starts on a two-byte boundary
    pad = b"\0" if (len(names) % 2) else b""
    with open(path, "wb") as f:
        f.write(hdr + names + pad)
        f.write(struct.pack("<%dH" % len(numbers), *numbers))
        f.write(struct.pack("<%dH" % NS, *soff))
        f.write(table)


def sigint_test(work):
    session = pty_run.Session([EE, "-i", os.path.join(work, "sigint.txt")],
                              env={"TERM": "xterm"})
    session.pump(2.5)
    session.write(b"unsaved")
    session.pump(1)
    os.kill(session.pid, signal.SIGINT)
    session.pump(2.5)
    exited = 0
    for _ in range(25):
        wpid, _status = os.waitpid(session.pid, os.WNOHANG)
        if wpid != 0:
            exited = 1
            break
        time.sleep(0.1)
    restored = 1 if b"\x1b[?1l\x1b>" in session.buf else 0
    saved = 1 if os.path.exists(os.path.join(work, "sigint.txt")) else 0
    session.close()
    return exited, restored, saved


def sigwinch_test(work):
    session = pty_run.Session([EE, "-i", os.path.join(work, "resize.txt")],
                              env={"TERM": "xterm"})
    session.pump(2.5)
    session.write(b"kept text")
    session.pump(1)
    for rows, cols in ((40, 100), (10, 30), (30, 60)):
        fcntl.ioctl(session.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(session.pid, signal.SIGWINCH)
        session.pump(1.5)
    session.write(b"\x1b")
    session.pump(1)
    session.write(b"a")
    session.pump(1)
    session.write(b"a")
    session.pump(2)
    path = os.path.join(work, "resize.txt")
    data = open(path, "rb").read() if os.path.exists(path) else b""
    session.close()
    return 1 if b"kept text" in data else 0


def tinfo_test(work, clear):
    """Run ee against a synthetic entry with a hostile clear string."""
    tdir = os.path.join(work, "terminfo")
    os.makedirs(os.path.join(tdir, "o"), exist_ok=True)
    build_terminfo(os.path.join(tdir, "o", "openutils-test"), clear,
                   b"openutils-test|t|malformed terminfo test\0")
    session = pty_run.Session([EE, "-i", os.path.join(work, "tinf.txt")],
                              env={"TERM": "openutils-test",
                                   "TERMINFO": tdir})
    session.pump(2.5)
    session.write(b"x")
    session.pump(1.5)
    _wpid, status = os.waitpid(session.pid, os.WNOHANG)
    crashed = 1 if (status != 0 and os.WIFSIGNALED(status)) else 0
    session.close()
    return crashed


def tinfo_valid_test(work):
    tdir = os.path.join(work, "terminfo")
    os.makedirs(os.path.join(tdir, "o"), exist_ok=True)
    build_terminfo(os.path.join(tdir, "o", "openutils-test"),
                   "\033[H\033[2J",
                   b"openutils-test|t|minimal terminfo\0")
    session = pty_run.Session([EE, "-i", os.path.join(work, "tinfv.txt")],
                              env={"TERM": "openutils-test",
                                   "TERMINFO": tdir})
    session.pump(2.5)
    session.write(b"hello")
    session.pump(1)
    session.write(b"\x1b")
    session.pump(1)
    session.write(b"a")
    session.pump(1)
    session.write(b"a")
    session.pump(2)
    path = os.path.join(work, "tinfv.txt")
    data = open(path, "rb").read() if os.path.exists(path) else b""
    session.close()
    return 1 if b"hello" in data else 0


def main():
    tmpdir = os.environ.get("TMPDIR", "/tmp")
    with tempfile.TemporaryDirectory(prefix="ee-sig-test.",
                                     dir=tmpdir) as work:
        exited, restored, saved = sigint_test(work)
        check("SIGINT exits the editor", exited)
        check("SIGINT restores the terminal", restored)
        check("SIGINT does not save", not saved)

        check("SIGWINCH keeps the editor working", sigwinch_test(work))

        for bad in ("%{0}%{0}/%d", "%{0}%{0}%%d", "%{123", "%p9", "%P9",
                    "%g9", "%{99999999999999999999}", "$<12x"):
            check("terminfo '%s' does not crash" % bad,
                  not tinfo_test(work, bad))

        check("minimal valid terminfo saves", tinfo_valid_test(work))

    print("pass: %d  fail: %d" % (PASS, FAIL))
    if FAIL > 0:
        print("failed tests:%s" % ("".join(" " + t for t in FAILED)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
