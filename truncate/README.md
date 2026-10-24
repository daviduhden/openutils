# truncate

Shrink or extend the size of files.

Imported from the DragonFlyBSD source tree (originally by Sheldon Hearn)
and extended for command line compatibility with the truncate utility in
common use today.  The extended behaviour was reimplemented from the
documented semantics of that utility; no GNU code was copied.

| File | Purpose |
| --- | --- |
| [truncate.c](truncate.c) | The program. |
| [truncate.1](truncate.1) | Manual page. |
| [Makefile](Makefile) | Build, test, debug and install targets. |
| [tests/](tests/) | Behavioural test suite (POSIX shell). See [Tests](#tests). |

## Usage

```
truncate [-c] [-o] -s size file ...
truncate [-c] -r rfile file ...
```

- `-c`/`--no-create`: do not create files that do not exist;
- `-o`/`--io-blocks`: treat `size` as a number of I/O blocks; requires
  `-s` and is not valid with `-r` alone;
- `-r rfile`/`--reference=rfile`: use the size of `rfile` (the separate
  form `--reference rfile` is also accepted);
- `-s size`/`--size=size`: set or adjust the size (the separate form
  `--size size` is also accepted);
- `--help`.

`size` is an integer and an optional unit.  The integer may be omitted
when a unit is given, in which case it is taken as 1.  Units are `K`,
`M`, `G`, `T`, `P`, `E`, `Z`, `Y`, `R`, and `Q` (powers of 1024),
`KB`, `MB`, ... (powers of 1000), and `KiB`, `MiB`, ... (binary
prefixes, equivalent to `K`, `M`, ...); lowercase `k`, `m`, `g`, and
`t` are also accepted.  The `D` suffix is not accepted, and `Z`, `Y`,
`R`, and `Q` overflow for nonzero values on a 64-bit `off_t`.

An absolute size cannot be negative.  A size may be prefixed by one of
`+` (extend by), `-` (reduce by), `<` (at most), `>` (at least), `/`
(round down to a multiple of), or `%` (round up to a multiple of).  A
leading `-` is the reduce-by modifier, not a negative absolute size.
When both `-r` and `-s` are given, the final `-s` argument must begin
with one of those modifiers.

## Building and testing

```
make -C truncate             # build ./truncate
make -C truncate debug       # unoptimized build under the debugger
make -C truncate test        # POSIX shell behavioural tests
make -C truncate install     # honours PREFIX/DESTDIR
```

Error messages and usage text are similar to, but not identical with,
the reference utility.  See [../README.md](../README.md) for the
pledge profile.

## Known differences

- Options must precede the file operands; unlike GNU truncate, the
  command line is not permuted, so an argument that looks like an
  option after the first operand is treated as a file operand.
- `--version` and `-V` are not supported.
- Error and usage messages differ from the reference utility, and the
  `--help` wording is not identical.

## Tests

The suite in [tests/](tests/) compares behaviour with a GNU truncate when
one is available in `PATH`, so it acts as a compatibility check as well
as a regression test.  It is POSIX shell and also needs `mktemp` and a
`stat` that supports GNU `-c` or BSD `-f`; GNU truncate is optional and
is used only for the differential checks.  When no GNU truncate is
found, those comparisons are reported as skipped rather than as
passes.

| File | Purpose |
| --- | --- |
| [tests/run.sh](tests/run.sh) | Test driver (`/bin/sh`). |

Run it from the repository root:

```
sh truncate/tests/run.sh
```

The `TRUNC` environment variable selects the binary under test (default
`truncate/truncate`).
