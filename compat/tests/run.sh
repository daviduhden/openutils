#!/bin/sh
# Self-test for the <stdckdint.h> fallback in compat/stdckdint.h.
#
# The fallback is forced with OPENUTILS_STDCKDINT_FORCE_FALLBACK so
# that it is compiled and executed even when the host already provides
# a real <stdckdint.h>; otherwise the fallback would never be covered
# on a development machine.  The test binary is link-only against libc.

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

# shellcheck disable=SC2086  # the options are intentionally split
"$CC" $CFLAGS $CSTD $WARNINGS -I"$COMPAT_DIR" \
	-DOPENUTILS_STDCKDINT_FORCE_FALLBACK \
	-o "$bin" "$TESTS_DIR/stdckdint.c" || exit 1

"$bin"
