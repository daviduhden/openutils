#!/bin/sh
# pty-run.sh - run a program on a pseudo-terminal, feed it key
# sequences, and print its output.
#
# usage: pty-run.sh <program> <args...> -- <input-sequence>...
#
# Each input sequence may contain \xHH escapes (or plain text) and is
# sent to the program with a small delay between sequences, ending
# with a final drain period.
exec python3 - "$@" <<'PYEOF'
import os, pty, select, sys, time

args = sys.argv[1:]
sep = args.index('--') if '--' in args else len(args)
argv = args[:sep]
inputs = args[sep + 1:]

def decode(s):
    out = b''
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            nxt = s[i+1]
            if nxt == 'x' and i + 3 < len(s):
                out += bytes([int(s[i+2:i+4], 16)])
                i += 4
                continue
            esc = {'n': 10, 'r': 13, 't': 9, 'e': 27, '\\': 92}
            if nxt in esc:
                out += bytes([esc[nxt]])
                i += 2
                continue
        out += s[i].encode()
        i += 1
    return out

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = os.environ.get('TERM', 'xterm')
    try:
        os.execvp(argv[0], argv)
    except OSError:
        os._exit(127)

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
                return False
            if not d:
                return False
            buf += d
    return True

pump(2.0)
for seq in inputs:
    os.write(fd, decode(seq))
    pump(1.0)
pump(2.0)

try:
    os.close(fd)
except OSError:
    pass

sys.stdout.write(buf.decode('utf-8', 'replace'))
PYEOF
