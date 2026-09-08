#!/bin/sh
#
# Behavioural tests for doasedit.
#
# These tests use a test build of doasedit (compiled with
# -DDOASEDIT_TEST) that can fake the user identity seen by the
# ownership checks, plus fake doas(1) and editor programs, so that the
# privileged code paths can be exercised without real root privileges.
# The production binary is built without any of the test hooks.
#
# The fake-doas based flows verify doasedit's own logic (which
# commands are invoked, how their results are handled, race
# detection, cleanup); they do not test doas(1) itself, which only
# exists on OpenBSD.

set -u

TESTS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH='' cd -- "$TESTS_DIR/../.." && pwd)
DOASEDIT=${DOASEDIT:-$ROOT/doasedit/doasedit_test}

PASS=0
FAIL=0
FAILED_TESTS=

tmpbase=$(mktemp -d "${TMPDIR:-/tmp}/doasedit-test.XXXXXX") || exit 1
trap 'chmod -R u+w "$tmpbase" 2>/dev/null; rm -rf "$tmpbase"' EXIT HUP INT TERM

# the fake doas must be on PATH under the name "doas"
mkdir -p "$tmpbase/bin"
ln -s "$TESTS_DIR/fake-doas" "$tmpbase/bin/doas"
ln -s "$TESTS_DIR/fake-editor" "$tmpbase/bin/fake-editor"
export PATH="$tmpbase/bin:$PATH"

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

# assert <description> <command...>
assert() {
	desc=$1
	shift
	if "$@"; then
		ok "$desc"
	else
		notok "$desc"
	fi
}

# assert_eq <description> <expected> <actual>
assert_eq() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		notok "$1"
		printf '# expected: %s\n# actual:   %s\n' "$2" "$3"
	fi
}

# assert_out <description> <expected-substring> <file>
assert_out() {
	if grep -q "$2" "$3" 2>/dev/null; then
		ok "$1"
	else
		notok "$1"
		printf '# pattern %s not found in %s\n' "$2" "$3"
	fi
}

base=$tmpbase/work
mkdir -p "$base"
mkdir -p "$base/privdir"
chmod 555 "$base/privdir"
cd "$base" || exit 1

# ---------------------------------------------------------------- 1
# option handling
"$DOASEDIT" --help >/dev/null 2>&1
assert_eq "--help exits 0" 0 $?
"$DOASEDIT" -h >/dev/null 2>&1
assert_eq "-h exits 0" 0 $?
"$DOASEDIT" >/dev/null 2>&1
assert_eq "no arguments exits 1" 1 $?
"$DOASEDIT" --bogus 2>/dev/null
assert_eq "invalid option exits 1" 1 $?

# running as root is not permitted: covered by the uid shim instead,
# since the real test user is not root.
DOASEDIT_TEST_UID=0 "$DOASEDIT" foo 2>/dev/null
assert_eq "root usage rejected" 1 $?

# ---------------------------------------------------------------- 2
# path validation
"$DOASEDIT" /tmp/ 2>/dev/null
assert_eq "trailing slash rejected" 1 $?
"$DOASEDIT" "$base/nonexistent-dir/file" 2>/dev/null
assert_eq "nonexistent directory rejected" 1 $?

touch "$base/myfile"
"$DOASEDIT" "$base/myfile" 2>/dev/null
assert_eq "own file rejected" 1 $?

mkdir -p "$base/mydir"
"$DOASEDIT" "$base/mydir/newfile" 2>/dev/null
assert_eq "own directory rejected" 1 $?

# user-writable directory (owned by someone else in the faked view)
DOASEDIT_TEST_UID=9999 "$DOASEDIT" "$base/mydir/newfile" 2>/dev/null
assert_eq "user-writable directory rejected" 1 $?

# user-readable and -writable file (faked view: not own file)
chmod 666 "$base/myfile"
DOASEDIT_TEST_UID=9999 "$DOASEDIT" "$base/myfile" 2>/dev/null
assert_eq "readable and writable file rejected" 1 $?

# ---------------------------------------------------------------- 3
# environment / editor selection

# unknown editor
DOAS_EDITOR=no-such-editor-xyz "$DOASEDIT" "$base/x" 2>/dev/null
assert_eq "invalid editor rejected" 1 $?
unset DOAS_EDITOR VISUAL EDITOR

# editor with arguments, DOAS_EDITOR wins over VISUAL/EDITOR
FAKE_EDITOR_LOG=$base/elog1 DOAS_EDITOR="fake-editor --x" VISUAL=vi \
	EDITOR=emacs DOASEDIT_TEST_UID=9999 \
	"$DOASEDIT" "$base/privdir/plain1" 2>/dev/null
rc=$?
assert_eq "DOAS_EDITOR used" 0 $rc
assert_out "editor got args" "args: --x .*plain1" "$base/elog1"

FAKE_EDITOR_LOG=$base/elog2 VISUAL="fake-editor" DOASEDIT_TEST_UID=9999 \
	"$DOASEDIT" "$base/privdir/plain2" 2>/dev/null
assert_eq "VISUAL used when DOAS_EDITOR unset" 0 $?
assert_out "VISUAL invoked" "args: .*plain2" "$base/elog2"

FAKE_EDITOR_LOG=$base/elog3 EDITOR="fake-editor -z" DOASEDIT_TEST_UID=9999 \
	"$DOASEDIT" "$base/privdir/plain3" 2>/dev/null
assert_eq "EDITOR used" 0 $?
assert_out "EDITOR invoked with arg" "args: -z .*plain3" "$base/elog3"
unset FAKE_EDITOR_LOG

# ---------------------------------------------------------------- 4
# new-file creation via privileged install
rm -f "$base/privdir/newfile"
DOAS_FAKE_LOG=$base/dlog1 DOASEDIT_TEST_UID=9999 DOAS_EDITOR=fake-editor \
	"$DOASEDIT" "$base/privdir/newfile" 2>/dev/null
assert_eq "new file created" 0 $?
assert_eq "new file has content" "edited by fake editor" \
	"$(cat "$base/privdir/newfile" 2>/dev/null)"
assert_out "install -m 0644 used for new file" \
	"install -o default -g default -m 0644" "$base/dlog1"

# ---------------------------------------------------------------- 5
# existing root-owned file: edit + privileged write-back with
# preserved metadata
printf 'original\n' >"$base/rootfile"
chmod 440 "$base/rootfile"
rm -f "$base/dlog2"
DOAS_FAKE_LOG=$base/dlog2 DOASEDIT_TEST_UID=9999 DOAS_EDITOR=fake-editor \
	"$DOASEDIT" "$base/rootfile" 2>/dev/null
assert_eq "root-owned file edited" 0 $?
assert_eq "content updated" "original
edited by fake editor" "$(cat "$base/rootfile")"
assert_out "install preserves owner" "install -o 1000 -g 1000 -m 440" \
	"$base/dlog2"

# unchanged file: no write-back
rm -f "$base/dlog3"
DOAS_FAKE_LOG=$base/dlog3 DOASEDIT_TEST_UID=9999 \
	DOAS_EDITOR=fake-editor FAKE_EDITOR_NOCHANGE=1 \
	"$DOASEDIT" "$base/rootfile" >"$base/out3" 2>/dev/null
assert_eq "unchanged file exits 0" 0 $?
assert_out "unchanged reported" "unchanged" "$base/out3"
assert "no install for unchanged file" \
	[ ! -s "$base/dlog3" ]

# ---------------------------------------------------------------- 6
# unreadable file: content fetched via doas cat
printf 'secret\n' >"$base/secret"
chmod 440 "$base/secret"
rm -f "$base/dlog4" "$base/elog4"
FAKE_EDITOR_LOG=$base/elog4 DOAS_FAKE_LOG=$base/dlog4 \
	DOASEDIT_TEST_UID=9999 DOASEDIT_TEST_UNREADABLE=1 \
	DOAS_EDITOR=fake-editor \
	"$DOASEDIT" "$base/secret" 2>/dev/null
assert_eq "unreadable file edited" 0 $?
assert_out "doas cat used" "cat .*secret" "$base/dlog4"
assert_out "editor saw contents" "secret" "$base/elog4"
assert_out "install used for write-back" "install" "$base/dlog4"

# ---------------------------------------------------------------- 7
# race detection: file replaced while the editor runs
printf 'target\n' >"$base/race"
chmod 440 "$base/race"
(
	DOAS_FAKE_LOG=$base/dlog5 DOASEDIT_TEST_UID=9999 \
		DOAS_EDITOR=fake-editor FAKE_EDITOR_SLEEP=2 \
		"$DOASEDIT" "$base/race" >"$base/out5" 2>&1 &
	editor_started=$!
	sleep 1
	rm -f "$base/race"
	printf 'replaced\n' >"$base/race"
	chmod 440 "$base/race"
	wait "$editor_started"
)
rc=$?
assert_eq "replaced file: edit loop exits" 1 $rc
assert_out "replacement detected" "file changed during editing" "$base/out5"
assert_eq "replacement content untouched" "replaced" \
	"$(cat "$base/race")"

# ---------------------------------------------------------------- 8
# doas.conf syntax check loop
printf 'a\n' | DOAS_FAKE_CONF_FAIL=1 DOASEDIT_TEST_UID=9999 \
	DOAS_EDITOR=fake-editor \
	"$DOASEDIT" /etc/doas.conf >"$base/out6" 2>/dev/null
assert_eq "doas.conf abort exits 1 after skip" 1 $?
assert_out "doas.conf warning shown" "break doas" "$base/out6"

# ---------------------------------------------------------------- 9
# temp file hygiene
FAKE_EDITOR_LOG=$base/elog5 DOASEDIT_TEST_UID=9999 DOAS_EDITOR=fake-editor \
	FAKE_EDITOR_SLEEP=0 \
	"$DOASEDIT" "$base/privdir/plain9" 2>/dev/null
# the log contains "drwx------" for the tmpdir and "-rw-------" for
# the file
assert_out "tmpdir is 0700" "drwx------" "$base/elog5"
assert_out "tmpfile is 0600" "rw-------" "$base/elog5"
# no doasedit.* leftovers in the tmp directory after success
leftovers=$(find "${TMPDIR:-/tmp}" -maxdepth 1 -name 'doasedit.??????' \
	-user "$(id -u)" 2>/dev/null | wc -l)
assert_eq "no leftover tmpdirs" "0" "$leftovers"

# ---------------------------------------------------------------- 10
# signal cleanup
rm -rf "${TMPDIR:-/tmp}"/doasedit.*
DOASEDIT_TEST_UID=9999 DOAS_EDITOR=fake-editor FAKE_EDITOR_SLEEP=5 \
	"$DOASEDIT" "$base/privdir/sigfile" >/dev/null 2>&1 &
pid=$!
sleep 1
kill -TERM "$pid" 2>/dev/null
sleep 1
leftovers=$(find "${TMPDIR:-/tmp}" -maxdepth 1 -name 'doasedit.??????' \
	-user "$(id -u)" 2>/dev/null | wc -l)
assert_eq "tmpdir removed on signal" "0" "$leftovers"
wait "$pid" 2>/dev/null

# ---------------------------------------------------------------- 11
# multiple files: error in one does not abort the others
printf 'a\n' >"$base/multi1"
chmod 440 "$base/multi1"
DOAS_FAKE_LOG=$base/dlog6 DOASEDIT_TEST_UID=9999 DOAS_EDITOR=fake-editor \
	"$DOASEDIT" "$base/no-such-dir/x" "$base/multi1" >/dev/null 2>&1
assert_eq "multi-file: overall success" 0 $?
assert_out "second file still processed" "install" "$base/dlog6"

# ----------------------------------------------------------------
say
say "pass: $PASS  fail: $FAIL"
if [ "$FAIL" -gt 0 ]; then
	say "failed tests:$FAILED_TESTS"
	exit 1
fi
exit 0
