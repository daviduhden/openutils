#!/bin/sh
# Behavioural tests for tree(1).  Builds a temporary hierarchy with
# regular files, hidden files, symlinks, fifos, unusual filenames and
# nested directories, then checks option parsing and output.

set -u

TESTS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TESTS_DIR/../.." && pwd)
TREE=${TREE:-$ROOT/tree/tree}

PASS=0
FAIL=0
FAILED_TESTS=

say()
{
	printf '%s\n' "$*"
}

ok()
{
	PASS=$((PASS + 1))
	printf 'ok %s - %s\n' "$PASS" "$1"
}

notok()
{
	FAIL=$((FAIL + 1))
	FAILED_TESTS="$FAILED_TESTS $1"
	printf 'not ok - %s\n' "$1"
}

assert()
{
	desc=$1
	shift
	if "$@" >/dev/null 2>&1; then
		ok "$desc"
	else
		notok "$desc"
	fi
}

assert_out()
{
	desc=$1
	pattern=$2
	shift 2
	if "$@" | grep -q "$pattern"; then
		ok "$desc"
	else
		notok "$desc"
		printf '# pattern %s not found in output of %s\n' "$pattern" "$1"
	fi
}

assert_not_out()
{
	desc=$1
	pattern=$2
	shift 2
	if "$@" | grep -q "$pattern"; then
		notok "$desc"
	else
		ok "$desc"
	fi
}

assert_eq()
{
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		notok "$1"
		printf '# expected: %s\n# actual:   %s\n' "$2" "$3"
	fi
}

# build the hierarchy
work=$(mktemp -d "${TMPDIR:-/tmp}/tree-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

cd "$work" || exit 1
mkdir -p sub/deep empty
printf 'one\n' > sub/afile
printf 'big\n' > sub/big
touch sub/.hidden
ln -s ../sub/deep sub/lnk
ln -s nowhere sub/broken
mkfifo sub/pipe 2>/dev/null
printf 'a\nb\n' > 'sub/with
newline'
touch 'sub/with space'
dd if=/dev/zero of=sub/tenk bs=1024 count=10 2>/dev/null
touch sub/zzz

# ------------------------------------------------ basic output
assert_out "lists directory" "afile" "$TREE" .
assert_out "lists nested dirs" "deep" "$TREE" .
assert_eq "report counts" "1 directory, 0 files" \
    "$("$TREE" empty | tail -1)"

assert_out "default hides dotfiles" "afile" "$TREE" sub
assert_not_out "default hides dotfiles (negative)" ".hidden" "$TREE" sub
assert_out "-a shows dotfiles" ".hidden" "$TREE" -a sub

# ------------------------------------------------ options
assert_out "-d shows directories only" "deep" "$TREE" -d sub
assert_not_out "-d hides files" "afile" "$TREE" -d sub
assert_out "-d shows dir symlink" "lnk" "$TREE" -d sub

assert_out "-f prints full path" "sub/afile" "$TREE" -f sub

assert_out "-F classifies dirs" "deep/" "$TREE" -F sub
assert_out "-F classifies symlink target" "lnk -> ../sub/deep/" \
    "$TREE" -F sub
assert_out "-F classifies fifo" "pipe|" "$TREE" -F sub
assert_out "-F classifies broken link" "broken -> nowhere" "$TREE" -F sub

assert_out "-L 1 limits depth" "sub" "$TREE" -L 1 .
assert_not_out "-L 1 hides nested files" "afile" "$TREE" -L 1 .
assert_out "-L 2 shows nested dirs" "deep" "$TREE" -L 2 sub

assert_out "-t sorts by time" "afile" "$TREE" -t sub

# size output
assert_out "-s shows size" "10240" "$TREE" -s sub
assert_out "-s -h human size" "10K" "$TREE" -s -h sub
assert_out "--si decimal size" "10k" "$TREE" -s --si sub

# permissions / users / groups / dates
assert_out "-p shows permissions" "drwx" "$TREE" -p sub
assert_out "-u shows user" "$(id -un)" "$TREE" -u sub
assert_out "-g shows group" "$(id -gn)" "$TREE" -g sub
assert_out "-D shows date" "\[" "$TREE" -D --timefmt '%Y' sub
assert_out "--timefmt respected" "$(date +%Y)" \
    "$TREE" -D --timefmt '%Y' sub

# sorting
assert_out "-r reverses" "zzz" "$TREE" -r sub
assert_out "--sort=size largest first" "tenk" \
    "$TREE" --sort=size -s sub
assert_out "--dirsfirst puts dirs first" "deep" \
    "$TREE" --dirsfirst sub

# patterns
assert_out "-P filters files" "afile" "$TREE" -P 'afile' sub
assert_not_out "-P hides non-matching" "big" "$TREE" -P 'afile' sub
assert_out "-I excludes entries" "afile" "$TREE" -I 'big' sub
assert_not_out "-I hides matching" "big" "$TREE" -I 'big' sub
assert_not_out "-I matches symlink targets" "broken" \
    "$TREE" -I 'nowhere' sub
assert_not_out "-I drops symlink by target" "lnk" \
    "$TREE" -I 'deep' sub

# pruning / limits
assert_out "--prune removes empty dirs" "afile" "$TREE" --prune sub
assert_not_out "--prune drops empty" "empty" "$TREE" --prune .
assert_out "--filelimit blocks descent" "exceeds filelimit" \
    "$TREE" --filelimit 2 sub
assert_out "--noreport suppresses report" "afile" \
    "$TREE" --noreport sub
assert_not_out "--noreport no report" "director" \
    "$TREE" --noreport sub

# -l follows symlinks
assert_out "-l follows dir symlink" "lnk" "$TREE" -l sub
ln -s . sub/loop 2>/dev/null
assert_out "-l detects loops" "recursive, not followed" "$TREE" -l sub

# filenames with special characters
assert_out "spaces preserved" "with space" "$TREE" sub
assert_out "newlines escaped" "with" "$TREE" sub
assert_out "-q replaces non-printables" "with?newline" "$TREE" -q sub
assert_out "-N prints raw" "with" "$TREE" -N sub
assert_out "-Q quotes names" '"with space"' "$TREE" -Q sub

# JSON
assert_out "-J emits json" '"type":"directory"' "$TREE" -J sub
assert_out "-J report object" '"type":"report"' "$TREE" -J sub
assert_out "-J link target" '"target":"../sub/deep"' "$TREE" -J sub
assert_out "-J escapes newlines" 'with\\nnewline' "$TREE" -J sub

# -o output file
"$TREE" -o "$work/out.txt" sub 2>/dev/null
assert "output file written" [ -s "$work/out.txt" ]
assert_out "output file has content" "afile" grep . "$work/out.txt"

# error handling
"$TREE" /nonexistent-tree-test >/dev/null 2>&1
assert_eq "nonexistent root exits 2" 2 $?
assert_out "nonexistent root reported" "error opening dir" \
    "$TREE" /nonexistent-tree-test
"$TREE" -Z >/dev/null 2>&1
assert_eq "invalid option exits 1" 1 $?
"$TREE" --help >/dev/null 2>&1
assert_eq "--help exits 0" 0 $?

# option combinations
assert_out "-d -L 1 shows nested dirs" "sub" "$TREE" -d -L 1 .
assert_not_out "-d -L 1 hides deep files" "afile" "$TREE" -d -L 1 .
assert_out "-a -I shows dotfiles not ignored" ".hidden" "$TREE" -a -I 'nomatch*' sub
assert_out "-s -h -D combine" "10K" "$TREE" -s -h -D --timefmt '%Y' sub
assert_out "--dirsfirst -r reverses within groups" "zzz" \
    "$TREE" --dirsfirst -r sub
assert_out "-f -L 1 shows full paths at depth" "sub/afile" "$TREE" -f -L 1 sub
assert_out "-J -d reports dirs only" '"directories":' "$TREE" -J -d sub
assert_out "-a -P matches hidden files" ".hidden" "$TREE" -a -P '.hidden' sub
assert_out "--prune -P keeps matching files" "afile" \
    "$TREE" --prune -P 'afile' .
assert_not_out "--prune -P drops non-matching" "big" \
    "$TREE" --prune -P 'afile' .

# multiple roots
assert_out "multiple roots listed" "afile" "$TREE" sub empty

say
say "pass: $PASS  fail: $FAIL"
if [ "$FAIL" -gt 0 ]; then
	say "failed tests:$FAILED_TESTS"
	exit 1
fi
exit 0
