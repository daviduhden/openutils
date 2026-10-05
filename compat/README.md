# compat

Portability shims used when the code is built on a host that is not
OpenBSD (or another BSD).  On OpenBSD every file here compiles to
nothing: the interfaces are native.  The directory exists so that the
utilities can be built and tested on Linux without a large portability
layer, and so that a C23-capable compiler paired with an older libc
still works.

| File | Purpose |
| --- | --- |
| [bsdcompat.h](bsdcompat.h) | Included first by every translation unit. Defines the feature-test macros for non-OpenBSD hosts and declares or provides the BSD interfaces the sources rely on. |
| [stdckdint.h](stdckdint.h) | `<stdckdint.h>` shim. Uses the real header when one is reachable, otherwise provides `ckd_add`/`ckd_sub`/`ckd_mul` with the compiler's overflow builtins. |
| [strtonum.c](strtonum.c) | `strtonum(3)` for libcs that lack it. |
| [bsdstring.c](bsdstring.c) | `strlcpy(3)`, `strlcat(3)` and `reallocarray(3)` where the libc does not provide them. |
| [err.c](err.c) | The `err(3)` family for libcs that declare but do not provide it. |
| [tests/](tests/) | Self-test for the `<stdckdint.h>` fallback (see [Tests](#tests)). |

`bsdcompat.h` must be the first include in every C file so that its
feature-test macros are seen before any system header.  `_XOPEN_SOURCE`
is deliberately confined to this header; `make check` fails if it leaks
into a production source.

The per-utility Makefiles link these files in automatically:

```
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c
```

## Tests

The self-test in [tests/](tests/) forces the `<stdckdint.h>` fallback
with `OPENUTILS_STDCKDINT_FORCE_FALLBACK`, so it is covered even on a
host whose libc already ships a real `<stdckdint.h>`.

| File | Purpose |
| --- | --- |
| [tests/run.sh](tests/run.sh) | Builds and runs the fallback test. |
| [tests/stdckdint.c](tests/stdckdint.c) | Exercises `ckd_add`/`ckd_mul` against the C23 contract: representability is judged against the result type, operands are not converted first, and each argument is evaluated exactly once. |

The test binary is link-only against libc; it needs no terminal and no
extra packages.  Run it from the repository root:

```
sh compat/tests/run.sh
```

`make test` runs it first, before the utility suites.
