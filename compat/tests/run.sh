#!/bin/sh
# Run the checked arithmetic corpus with normal and forced header selection.

set -u

TESTS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
COMPAT_DIR=$(CDPATH='' cd -- "$TESTS_DIR/.." && pwd)

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -pipe}
WARNINGS=${WARNINGS:--Wall -Wextra -Wpedantic}
CSTD=${CSTD:--std=c23}

work=$(mktemp -d "${TMPDIR:-/tmp}/stdckdint-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

bin="$work/stdckdint-test"

for selection in normal fallback; do
	define=
	if [ "$selection" = fallback ]; then
		define=-DOPENUTILS_STDCKDINT_FORCE_FALLBACK
	fi
	# shellcheck disable=SC2086  # the options are intentionally split
	"$CC" $CFLAGS $CSTD $WARNINGS -I"$COMPAT_DIR" $define \
		-o "$bin" "$TESTS_DIR/stdckdint.c" || exit 1
	"$bin" || exit 1
done
