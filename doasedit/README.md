# doasedit

Edit files with root privileges using your own, unprivileged editor.

A security-hardened fork of doasedit 1.0.9, rewritten in C from the
original shell script.  The shell version validated paths, copied
content and wrote results back all through pathnames, which is a
TOCTOU/symlink race at every step, and its final `doas dd` write
followed symlinks (writing an attacker-chosen destination as root).

This implementation uses file-descriptor based validation (`open(2)`
with `O_NOFOLLOW`, `fstat(2)` identity snapshots), an atomic,
rename-based write-back through `install(1)` that cannot follow a
swapped symlink, a private `mkdtemp(3)` directory, no shell invocation
anywhere, and a `pledge(2)` policy.

| File | Purpose |
| --- | --- |
| [doasedit.c](doasedit.c) | The program. |
| [doasedit.1](doasedit.1) | Manual page. |
| [Makefile](Makefile) | Build, debug and install targets. |

## Usage

```
doasedit [-h] [--] file ...
```

`-h`/`--help` prints usage; `--` ends option processing so that a file
name beginning with `-` can be edited.

The editor is taken from `DOAS_EDITOR`, then `VISUAL`, then `EDITOR`.
The command may contain arguments; it is executed directly with
`execvp(3)` and no shell is involved.  `TMPDIR` selects the directory
for the private temporary files (default `/tmp`).

## Building

```
make -C doasedit            # build ./doasedit
make -C doasedit debug      # unoptimized build under the debugger
make -C doasedit install    # honours PREFIX/DESTDIR
```

Runtime requirements: `doas(1)`, `install(1)` and a `doas.conf` that
permits them (for example `permit persist :wheel`).

See [../README.md](../README.md) for the pledge profile and the known
differences from the reference implementation.
