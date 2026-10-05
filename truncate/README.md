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
truncate [-c] [-o] -r rfile file ...
```

- `-c`/`--no-create`: do not create files that do not exist;
- `-o`/`--io-blocks`: treat `size` as a number of I/O blocks;
- `-r rfile`/`--reference=rfile`: use the size of `rfile`;
- `-s size`/`--size=size`: set or adjust the size;
- `--help`.

`size` accepts the `+ - < > / %` modifiers (extend, reduce, at least, at
most, round down, round up) and the `K`/`KB`/`KiB` ... suffix set.  See
[truncate.1](truncate.1) for the full grammar.

## Building and testing

```
make -C truncate             # build ./truncate
make -C truncate debug       # unoptimized build under the debugger
make -C truncate test        # POSIX shell behavioural tests
make -C truncate install     # honours PREFIX/DESTDIR
```

Error messages are similar to, but not identical with, the reference
utility, and the usage text differs.  See [../README.md](../README.md)
for the pledge profile.

## Tests

The suite in [tests/](tests/) compares behaviour with a GNU truncate when
one is available in `PATH`, so it acts as a compatibility check as well
as a regression test.  It is POSIX shell and needs no extra packages.

| File | Purpose |
| --- | --- |
| [tests/run.sh](tests/run.sh) | Test driver (`/bin/sh`). |

Run it from the repository root:

```
sh truncate/tests/run.sh
```

The `TRUNC` environment variable selects the binary under test (default
`truncate/truncate`).
