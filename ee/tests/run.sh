#!/bin/sh
# Behavioural tests for ee(1).  Interactive behaviour is exercised
# through a pseudo-terminal; editing, menu navigation, saving, UTF-8
# input and terminal restoration are checked.

set -u

TESTS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TESTS_DIR/../.." && pwd)
EE=${EE:-$ROOT/ee/ee}

PASS=0
FAIL=0
FAILED_TESTS=

work=$(mktemp -d "${TMPDIR:-/tmp}/ee-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

say() {
	printf '%s\n' "$*"
}

ok() {
	PASS=$((PASS + 1))
	printf 'ok %s - %s\n' "$PASS" "$1"
}

notok() {
	FAIL=$((FAIL + 1))
	FAILED_TESTS="$FAILED_TESTS $1"
	printf 'not ok - %s\n' "$1"
}

assert_eq() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		notok "$1"
		printf '# expected: %s\n# actual:   %s\n' "$2" "$3"
	fi
}

# the binary must exist
[ -x "$EE" ] || {
	notok "ee binary exists"
	say "pass: $PASS  fail: $FAIL"
	exit 1
}
ok "ee binary exists"

# refuses to run without a terminal
printf x | "$EE" -? 2>/dev/null
assert_eq "requires a terminal" 1 $?

# usage output through a pty
usage=$(TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -? 2>/dev/null)
case "$usage" in
*"usage:"*"-i"*)
	ok "usage shows options"
	;;
*)
	notok "usage shows options"
	;;
esac

# ----------------------------------------------------------------
# edit, save and exit via menu, with UTF-8 input
file=$work/testfile.txt
TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -i "$file" -- \
	'hello world\nsecond line caf\xc3\xa9' '\x1b' 'a' 'a' >/dev/null 2>&1
assert_eq "edited file saved" "$(printf 'hello world\nsecond line café')" \
	"$(cat "$file" 2>/dev/null)"

# mode preserved for existing files
printf 'original\n' >"$file"
chmod 640 "$file"
TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -i "$file" -- \
	'xx' '\x1b' 'a' 'a' >/dev/null 2>&1
assert_eq "content updated" "$(printf 'xxoriginal\n')" "$(cat "$file")"
assert_eq "mode preserved" 640 \
	"$(stat -c %a "$file" 2>/dev/null || stat -f %Lp "$file")"

# new file created with normal umask
file=$work/newfile.txt
TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -i "$file" -- \
	'data' '\x1b' 'a' 'a' >/dev/null 2>&1
assert_eq "new file content" "$(printf 'data\n')" \
	"$(cat "$file" 2>/dev/null)"

# long lines (longer than the internal 512-byte read chunks) survive
# a load/save round trip
python3 - "$file" <<'PYEOF2'
import sys
with open(sys.argv[1], 'w') as f:
    f.write('x' * 3000 + '\n' + 'short\n')
PYEOF2
TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -i "$file" -- \
	'\x1b' 'a' 'a' >/dev/null 2>&1
assert_eq "long line preserved" "$(python3 -c "
print('x' * 3000)")" "$(head -1 "$file" 2>/dev/null)"

# file without a final newline
printf 'no trailing newline' >"$file"
TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -i "$file" -- \
	'\x1b' 'a' 'a' >/dev/null 2>&1
assert_eq "file without final newline kept" "no trailing newline" \
	"$(cat "$file" 2>/dev/null)"

# "no save" path leaves the file untouched
printf 'keep\n' >"$file"
TERM=xterm "$ROOT/ee/tests/pty-run.sh" "$EE" -i "$file" -- \
	'junk' '\x1b' 'a' 'b' >/dev/null 2>&1
assert_eq "no-save leaves file untouched" "$(printf 'keep\n')" \
	"$(cat "$file")"

say
say "pass: $PASS  fail: $FAIL"
if [ "$FAIL" -gt 0 ]; then
	say "failed tests:$FAILED_TESTS"
	exit 1
fi
exit 0
