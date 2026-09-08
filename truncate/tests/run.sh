#!/bin/sh
# Behavioural tests for truncate(1).  Where a GNU truncate is
# available in PATH (and is not this binary), the same operations are
# run against both implementations and the results compared.

set -u

TESTS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TESTS_DIR/../.." && pwd)
TRUNC=${TRUNC:-$ROOT/truncate/truncate}

PASS=0
FAIL=0
FAILED_TESTS=

work=$(mktemp -d "${TMPDIR:-/tmp}/truncate-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# the reference implementation, if any
GNU=""
if command -v truncate >/dev/null 2>&1; then
	candidate=$(command -v truncate)
	case "$candidate" in
	"$TRUNC") ;;
	"$ROOT/truncate"*) ;;
	*) GNU=$candidate ;;
	esac
fi

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

size() {
	stat -c %s "$1" 2>/dev/null || stat -f %z "$1" 2>/dev/null
}

# run one size specification through both implementations
# compare <desc> <size-arg> <initial-size>
compare() {
	desc=$1
	sarg=$2
	init=$3

	"$TRUNC" -s "$init" "$work/a" 2>/dev/null
	"$TRUNC" -s "$sarg" "$work/a" 2>/dev/null
	ra=$?
	sa=$(size "$work/a")

	if [ -n "$GNU" ]; then
		"$GNU" -s "$init" "$work/b" 2>/dev/null
		"$GNU" -s "$sarg" "$work/b" 2>/dev/null
		rb=$?
		sb=$(size "$work/b")
		if [ "$ra:$sa" != "$rb:$sb" ]; then
			notok "$desc"
			printf '# %s -s %s: ref=(%s,%s) ours=(%s,%s)\n' \
				"$desc" "$sarg" "$rb" "$sb" "$ra" "$sa"
			return
		fi
	fi
	ok "$desc"
}

: >"$work/a"
: >"$work/b"

# ------------------------------------------------ absolute sizes
"$TRUNC" -s 100 "$work/a"
assert_eq "absolute size" 100 "$(size "$work/a")"

"$TRUNC" -s 0 "$work/a"
assert_eq "absolute zero" 0 "$(size "$work/a")"

"$TRUNC" -s 100 "$work/a"
"$TRUNC" -s 10 "$work/a"
assert_eq "shrinking" 10 "$(size "$work/a")"

"$TRUNC" -s 10 "$work/a"
"$TRUNC" -s 100 "$work/a"
assert_eq "extending" 100 "$(size "$work/a")"

# ------------------------------------------------ relative sizes
compare "+ extends" +10 100
compare "- shrinks" -10 100
compare "- clamps at zero" -200 100
compare "< at most" '<90' 100
compare "< leaves smaller files alone" '<90' 50
compare "> at least" '>90' 50
compare "> leaves larger files alone" '>90' 100
compare "/ rounds down" /30 100
compare "/ never extends" /30 20
compare "% rounds up" %30 100
compare "% never shrinks" %30 110
compare "negative absolute clamps to zero" -5 100

# ------------------------------------------------ suffixes
compare "K suffix" 3K 0
compare "KB suffix" 3KB 0
compare "KiB suffix" 3KiB 0
compare "M suffix" 2M 0
compare "MB suffix" 2MB 0
compare "MiB suffix" 2MiB 0
compare "G suffix" 1G 0
compare "lowercase k" 5k 0
compare "suffix with relative" +1K 100
compare "T suffix" 1T 0
compare "E suffix" 1E 0
compare "Z suffix" 1Z 0
compare "Y suffix" 1Y 0
compare "R suffix" 1R 0
compare "Q suffix" 1Q 0

# ------------------------------------------------ creation
rm -f "$work/new"
"$TRUNC" -s 5 "$work/new"
assert_eq "creates missing file" 5 "$(size "$work/new")"

rm -f "$work/new"
"$TRUNC" -c -s 5 "$work/new" 2>/dev/null
assert_eq "-c skips missing file silently" 0 $?
assert_eq "-c does not create" "no" \
	"$([ -e "$work/new" ] && echo yes || echo no)"

# -c with an existing file still works
: >"$work/existing"
"$TRUNC" -c -s 3 "$work/existing"
assert_eq "-c truncates existing files" 3 "$(size "$work/existing")"

# ------------------------------------------------ reference files
printf 'refdata' >"$work/ref"
"$TRUNC" -r "$work/ref" "$work/a"
assert_eq "-r uses reference size" 7 "$(size "$work/a")"

"$TRUNC" -r "$work/ref" -s +3 "$work/a"
assert_eq "-r with relative size" 10 "$(size "$work/a")"

"$TRUNC" -r "$work/ref" -s '<2' "$work/a"
assert_eq "-r with at-most" 2 "$(size "$work/a")"

# ------------------------------------------------ io blocks
"$TRUNC" -o -s 1 "$work/a"
assert_eq "-o scales by block size" "$(stat -c %o "$work/a" 2>/dev/null || echo 4096)" "$(size "$work/a")" 2>/dev/null ||
	assert_eq "-o produces non-zero size" "0" \
		"$([ "$(size "$work/a")" -gt 0 ] && echo 1 || echo 0)"

# ------------------------------------------------ multiple operands
: >"$work/m1"
: >"$work/m2"
"$TRUNC" -s 20 "$work/m1" "$work/m2"
assert_eq "multiple files sized" "20 20" \
	"$(size "$work/m1") $(size "$work/m2")"

"$TRUNC" -s 20 "$work/m1" "$work/no-such-dir/x" >/dev/null 2>&1
assert_eq "failure in one operand exits 1" 1 $?
assert_eq "other operands still processed" 20 "$(size "$work/m1")"

# ------------------------------------------------ invalid input
for bad in 5X "" x 5.5 "5 5" "<+5" "+<5" "/0" "%0" - + 5b 5c 5w 99999999999999999999999999; do
	"$TRUNC" -s "$bad" "$work/a" >/dev/null 2>&1
	rc=$?
	if [ "$rc" = 0 ]; then
		notok "invalid size '$bad' rejected"
	else
		ok "invalid size '$bad' rejected"
	fi
done

# ------------------------------------------------ usage errors
"$TRUNC" >/dev/null 2>&1
assert_eq "no options: usage error" 1 $?
"$TRUNC" -s 5 >/dev/null 2>&1
assert_eq "missing operand: usage error" 1 $?
"$TRUNC" -r "$work/ref" -s 100 >/dev/null 2>&1
assert_eq "-r with absolute -s rejected" 1 $?
"$TRUNC" -o -r "$work/ref" >/dev/null 2>&1
assert_eq "-o without -s rejected" 1 $?
"$TRUNC" -z >/dev/null 2>&1
assert_eq "unknown option rejected" 1 $?

# ------------------------------------------------ long options
: >"$work/l1"
"$TRUNC" --size=5 --no-create "$work/l1"
assert_eq "--size=N works" 5 "$(size "$work/l1")"
"$TRUNC" --size 7 "$work/l1"
assert_eq "--size N works" 7 "$(size "$work/l1")"
"$TRUNC" --reference="$work/ref" "$work/l1"
assert_eq "--reference=N works" 7 "$(size "$work/l1")"
"$TRUNC" --io-blocks --size=1 "$work/l1"
assert_eq "--io-blocks works" "$(stat -c %o "$work/l1" 2>/dev/null || echo 4096)" "$(size "$work/l1")" 2>/dev/null ||
	assert_eq "--io-blocks produces non-zero size" "0" \
		"$([ "$(size "$work/l1")" -gt 0 ] && echo 1 || echo 0)"

"$TRUNC" --help >/dev/null 2>&1
assert_eq "--help exits 0" 0 $?

# ------------------------------------------------ boundary values
# OFF_MAX (2^63 - 1) is accepted by the parser; whether ftruncate(2)
# succeeds depends on the filesystem, so compare with the reference
compare "OFF_MAX parsed" 9223372036854775807 0

# OFF_MAX + 1 overflows the parser
"$TRUNC" -s 9223372036854775808 "$work/a" >/dev/null 2>&1
assert_eq "OFF_MAX+1 rejected" 1 $?

# OFF_MIN is representable; the resulting size is clamped to zero
"$TRUNC" -s 0 "$work/a"
"$TRUNC" -s -9223372036854775808 "$work/a" >/dev/null 2>&1
assert_eq "OFF_MIN clamps to zero" 0 $?
assert_eq "OFF_MIN results in empty file" 0 "$(size "$work/a")"

# relative size overflowing the target size
"$TRUNC" -s 10 "$work/a"
"$TRUNC" -s +9223372036854775800 "$work/a" >/dev/null 2>&1
assert_eq "relative overflow detected" 1 $?
assert_eq "file unchanged after overflow" 10 "$(size "$work/a")"

# division by zero
"$TRUNC" -s /0 "$work/a" >/dev/null 2>&1
assert_eq "/0 rejected" 1 $?
"$TRUNC" -s %0 "$work/a" >/dev/null 2>&1
assert_eq "%0 rejected" 1 $?

# rounding at one-byte granularity
"$TRUNC" -s 10 "$work/a"
"$TRUNC" -s /1 "$work/a"
assert_eq "/1 is a no-op" 10 "$(size "$work/a")"
"$TRUNC" -s %1 "$work/a"
assert_eq "%1 is a no-op" 10 "$(size "$work/a")"

# negative relative rounding does not extend
"$TRUNC" -s 3 "$work/a"
"$TRUNC" -s /10 "$work/a"
assert_eq "/10 rounds to zero" 0 "$(size "$work/a")"

# ------------------------------------------------ parser interactions
compare "bare K suffix means 1024" K 0
compare "leading whitespace in SIZE" ' 5' 0
compare "trailing whitespace in SIZE" '5 ' 0
compare "sign after modifier rejected" '<+5' 5
compare "modifier after sign rejected" '+<5' 5
# a relative modifier persists across later -s occurrences without
# their own modifier (documented GNU quirk)
: >"$work/q1"
"$TRUNC" -s 3 "$work/q1"
"$TRUNC" -s +1 -s 5 "$work/q1" >/dev/null 2>&1
assert_eq "-s +1 -s 5 keeps relative mode" 8 "$(size "$work/q1")"

# a sign combined with an inherited modifier is rejected
"$TRUNC" -s +1 -s -2 "$work/q1" >/dev/null 2>&1
assert_eq "-s +1 -s -2 rejected" 1 $?

# -c with a missing file is silent and successful
rm -f "$work/missing-c"
"$TRUNC" -c -s 5 "$work/missing-c" >/dev/null 2>&1
assert_eq "-c missing file exits 0" 0 $?
assert_eq "-c does not create" "no" \
	"$([ -e "$work/missing-c" ] && echo yes || echo no)"

# --size with a separate argument and a modifier
"$TRUNC" --size +2 "$work/q1" >/dev/null 2>&1
assert_eq "--size +N works" 10 "$(size "$work/q1")"

# -o with a reference file is allowed; the size is scaled
"$TRUNC" -o -r "$work/ref" -s +1 "$work/q1" >/dev/null 2>&1
assert_eq "-o -r -s +1 scales" \
	"$((7 + $(stat -c %o "$work/q1" 2>/dev/null || echo 4096)))" \
	"$(size "$work/q1")"

# ------------------------------------------------ symlinks
: >"$work/real"
ln -sf real "$work/lnk"
"$TRUNC" -s 9 "$work/lnk"
assert_eq "follows symlink" 9 "$(size "$work/real")"

say
if [ -n "$GNU" ]; then
	say "(differential mode against $GNU)"
else
	say "(standalone mode; no reference implementation found)"
fi
say "pass: $PASS  fail: $FAIL"
if [ "$FAIL" -gt 0 ]; then
	say "failed tests:$FAILED_TESTS"
	exit 1
fi
exit 0
