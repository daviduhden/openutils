#!/usr/bin/env python3
# Behavioural tests for ee(1).  Interactive behaviour is exercised
# through a pseudo-terminal; editing, menu navigation, saving, UTF-8
# input and terminal restoration are checked.
#
# Requires Python 3.

import os
import subprocess
import sys
import tempfile

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


def assert_eq(desc, expected, actual):
    if expected == actual:
        ok(desc)
    else:
        notok(desc)
        print("# expected: %s" % expected)
        print("# actual:   %s" % actual)


def read(path):
    """Return the file content with trailing newlines stripped, as a
    shell command substitution would."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().rstrip("\n")
    except OSError:
        return ""


def main():
    if not os.access(EE, os.X_OK):
        notok("ee binary exists")
        print()
        print("pass: %d  fail: %d" % (PASS, FAIL))
        return 1
    ok("ee binary exists")

    tmpdir = os.environ.get("TMPDIR", "/tmp")
    with tempfile.TemporaryDirectory(prefix="ee-test.", dir=tmpdir) as work:
        # refuses to run without a terminal
        rc = subprocess.run([EE, "-?"], input=b"x",
                            stderr=subprocess.DEVNULL).returncode
        assert_eq("requires a terminal", 1, rc)

        # usage output through a pty
        usage = pty_run.run([EE, "-?"], [], env={"TERM": "xterm"}).decode(
            "utf-8", "replace")
        if "usage:" in usage and "-i" in usage:
            ok("usage shows options")
        else:
            notok("usage shows options")

        # edit, save and exit via menu, with UTF-8 input
        path = os.path.join(work, "testfile.txt")
        pty_run.run([EE, "-i", path],
                    ["hello world\nsecond line caf\xc3\xa9", "\x1b", "a",
                     "a"], env={"TERM": "xterm"})
        assert_eq("edited file saved", "hello world\nsecond line café",
                  read(path))

        # mode preserved for existing files
        with open(path, "w", encoding="utf-8") as f:
            f.write("original\n")
        os.chmod(path, 0o640)
        pty_run.run([EE, "-i", path], ["xx", "\x1b", "a", "a"],
                    env={"TERM": "xterm"})
        assert_eq("content updated", "xxoriginal", read(path))
        assert_eq("mode preserved", "640",
                  oct(os.stat(path).st_mode & 0o777)[2:])

        # new file created with normal umask
        path = os.path.join(work, "newfile.txt")
        pty_run.run([EE, "-i", path], ["data", "\x1b", "a", "a"],
                    env={"TERM": "xterm"})
        assert_eq("new file content", "data", read(path))

        # long lines (longer than the internal 512-byte read chunks)
        # survive a load/save round trip
        with open(path, "w", encoding="utf-8") as f:
            f.write("x" * 3000 + "\n" + "short\n")
        pty_run.run([EE, "-i", path], ["\x1b", "a", "a"],
                    env={"TERM": "xterm"})
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            first = f.readline().rstrip("\n")
        assert_eq("long line preserved", "x" * 3000, first)

        # file without a final newline
        with open(path, "w", encoding="utf-8") as f:
            f.write("no trailing newline")
        pty_run.run([EE, "-i", path], ["\x1b", "a", "a"],
                    env={"TERM": "xterm"})
        assert_eq("file without final newline kept", "no trailing newline",
                  read(path))

        # "no save" path leaves the file untouched
        with open(path, "w", encoding="utf-8") as f:
            f.write("keep\n")
        pty_run.run([EE, "-i", path], ["junk", "\x1b", "a", "b"],
                    env={"TERM": "xterm"})
        assert_eq("no-save leaves file untouched", "keep", read(path))

        # locale: the interface stays English and UTF-8 editing works
        # under foreign and unusual locale environments
        for lc in ("C", "de_DE.UTF-8", "es_ES.UTF-8", "fr_FR.UTF-8",
                   "zz_ZZ.NOPE"):
            out = pty_run.run([EE, "-?"], [],
                              env={"LC_ALL": lc, "LANG": lc,
                                   "LC_MESSAGES": lc,
                                   "TERM": "xterm"}).decode("utf-8",
                                                            "replace")
            if "usage:" in out and "turn off info window" in out:
                ok("usage is English under %s" % lc)
            else:
                notok("usage is English under %s" % lc)

        path = os.path.join(work, "locfile.txt")
        pty_run.run([EE, "-i", path], ["caf\xc3\xa9", "\x1b", "a", "a"],
                    env={"LC_ALL": "de_DE.UTF-8",
                         "LANG": "de_DE.UTF-8", "TERM": "xterm"})
        assert_eq("UTF-8 edit under de_DE.UTF-8", "café", read(path))

    print()
    print("pass: %d  fail: %d" % (PASS, FAIL))
    if FAIL > 0:
        print("failed tests:%s" % ("".join(" " + t for t in FAILED)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
