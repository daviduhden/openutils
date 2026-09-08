#!/bin/sh
# PTY-based regression tests for ee: signal handling and malformed
# terminfo robustness.  Generous per-key delays are used because menu
# redraws are timing-sensitive.

set -u

TESTS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH='' cd -- "$TESTS_DIR/../.." && pwd)
EE=${EE:-$ROOT/ee/ee}

PASS=0
FAIL=0
FAILED_TESTS=

work=$(mktemp -d "${TMPDIR:-/tmp}/ee-sig-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

ok() {
	PASS=$((PASS + 1))
	printf 'ok %s - %s\n' "$PASS" "$1"
}

notok() {
	FAIL=$((FAIL + 1))
	FAILED_TESTS="$FAILED_TESTS $1"
	printf 'not ok - %s\n' "$1"
}

check() {
	desc=$1
	shift
	if [ "$1" = "1" ]; then
		ok "$desc"
	else
		notok "$desc"
	fi
}

# ----------------------------------------------------------------
# SIGINT while idle: the editor must exit through the normal,
# terminal-restoring path (no save, clean exit, reset sequence).
python3 - "$EE" "$work" sigint >"$work/sigint.result" <<'PYEOF'
import os, pty, select, signal, sys, time

ee, work, case = sys.argv[1], sys.argv[2], sys.argv[3]
pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm'
    os.execv(ee, [ee, '-i', work + '/sigint.txt'])
buf = b''
def pump(sec):
    global buf
    deadline = time.time() + sec
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            buf += d
pump(2.5)
os.write(fd, b'unsaved')
pump(1)
os.kill(pid, signal.SIGINT)
pump(2.5)
exited, status = os.waitpid(pid, os.WNOHANG)
print('exited=%d' % (status != 0))
print('restored=%d' % (b'\x1b[?1l\x1b>' in buf))
print('saved=%d' % os.path.exists(work + '/sigint.txt'))
try:
    os.close(fd)
except OSError:
    pass
PYEOF
check "SIGINT exits the editor" "$(grep -c '^exited=1$' "$work/sigint.result")"
check "SIGINT restores the terminal" \
	"$(grep -c '^restored=1$' "$work/sigint.result")"
check "SIGINT does not save" "$(grep -c '^saved=0$' "$work/sigint.result")"

# ----------------------------------------------------------------
# SIGWINCH (repeated) while editing, then save normally.
python3 - "$EE" "$work" sigwinch >"$work/sigwinch.result" <<'PYEOF'
import fcntl, os, pty, select, struct, sys, termios, time

ee, work, case = sys.argv[1], sys.argv[2], sys.argv[3]
pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm'
    os.execv(ee, [ee, '-i', work + '/resize.txt'])
buf = b''
def pump(sec):
    global buf
    deadline = time.time() + sec
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            buf += d
pump(2.5)
os.write(fd, b'kept text')
pump(1)
for rows, cols in ((40, 100), (10, 30), (30, 60)):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    os.kill(pid, 28)	# SIGWINCH
    pump(1.5)
os.write(fd, b'\x1b'); pump(1)
os.write(fd, b'a'); pump(1)
os.write(fd, b'a'); pump(2)
data = open(work + '/resize.txt', 'rb').read() if os.path.exists(work + '/resize.txt') else b''
print('saved=%d' % (b'kept text' in data))
try:
    os.close(fd)
except OSError:
    pass
PYEOF
check "SIGWINCH keeps the editor working" \
	"$(grep -c '^saved=1$' "$work/sigwinch.result")"

# ----------------------------------------------------------------
# Malformed terminfo: the parser must not crash on hostile input.
for bad in '%{0}%{0}/%d' '%{0}%{0}%%d' '%{123' '%p9' '%P9' '%g9' \
	'%{99999999999999999999}' '$<12x'; do
	python3 - "$EE" "$work" "$bad" >"$work/tinf.result" <<'PYEOF'
import os, pty, select, struct, sys, time

ee, work, clear = sys.argv[1], sys.argv[2], sys.argv[3]
tdir = work + '/terminfo'
os.makedirs(tdir + '/o', exist_ok=True)
NS = 190
numbers = [0xFFFF] * 40
numbers[0] = 80
numbers[2] = 24
soff = [0xFFFF] * NS
table = bytearray()
def addstr(idx, s):
    global table
    soff[idx] = len(table)
    table += s.encode() + b"\0"
addstr(5, clear)
addstr(10, "\033[%i%d;%dH")
addstr(104, "")
names = b"openutils-test|t|malformed terminfo test\0"
hdr = struct.pack("<HHHHHH", 282, len(names), 0, len(numbers), NS,
                  len(table))
with open(tdir + '/o/openutils-test', "wb") as f:
    f.write(hdr + names)
    f.write(struct.pack("<%dH" % len(numbers), *numbers))
    f.write(struct.pack("<%dH" % NS, *soff))
    f.write(table)

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'openutils-test'
    os.environ['TERMINFO'] = tdir
    os.execv(ee, [ee, '-i', work + '/tinf.txt'])
buf = b''
def pump(sec):
    global buf
    deadline = time.time() + sec
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            buf += d
pump(2.5)
os.write(fd, b'x')
pump(1.5)
exited, status = os.waitpid(pid, os.WNOHANG)
print('crashed=%d' % (status != 0 and os.WIFSIGNALED(status) and 1 or 0))
try:
    os.close(fd)
except OSError:
    pass
PYEOF
	check "terminfo '$bad' does not crash" \
		"$(grep -c '^crashed=0$' "$work/tinf.result")"
done

# a valid minimal entry must work end to end
python3 - "$EE" "$work" valid >"$work/tinfv.result" <<'PYEOF'
import os, pty, select, struct, sys, time

ee, work, clear = sys.argv[1], sys.argv[2], sys.argv[3]
tdir = work + '/terminfo'
NS = 190
numbers = [0xFFFF] * 40
numbers[0] = 80
numbers[2] = 24
soff = [0xFFFF] * NS
table = bytearray()
def addstr(idx, s):
    global table
    soff[idx] = len(table)
    table += s.encode() + b"\0"
addstr(5, "\033[H\033[2J")
addstr(10, "\033[%i%d;%dH")
addstr(104, "")
names = b"openutils-test|t|minimal terminfo\0"
hdr = struct.pack("<HHHHHH", 282, len(names), 0, len(numbers), NS,
                  len(table))
with open(tdir + '/o/openutils-test', "wb") as f:
    f.write(hdr + names)
    f.write(struct.pack("<%dH" % len(numbers), *numbers))
    f.write(struct.pack("<%dH" % NS, *soff))
    f.write(table)

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'openutils-test'
    os.environ['TERMINFO'] = tdir
    os.execv(ee, [ee, '-i', work + '/tinfv.txt'])
buf = b''
def pump(sec):
    global buf
    deadline = time.time() + sec
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            buf += d
pump(2.5)
os.write(fd, b'hello')
pump(1)
os.write(fd, b'\x1b'); pump(1)
os.write(fd, b'a'); pump(1)
os.write(fd, b'a'); pump(2)
data = open(work + '/tinfv.txt', 'rb').read() if os.path.exists(work + '/tinfv.txt') else b''
print('saved=%d' % (b'hello' in data))
try:
    os.close(fd)
except OSError:
    pass
PYEOF
check "minimal valid terminfo saves" "$(grep -c '^saved=1$' "$work/tinfv.result")"

printf 'pass: %s  fail: %s\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
	printf 'failed tests:%s\n' "$FAILED_TESTS"
	exit 1
fi
exit 0
