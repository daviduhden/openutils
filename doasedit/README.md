# doasedit

Edit files with root privileges using your own, unprivileged editor.

A security-hardened fork of doasedit 1.0.9, rewritten in C from the
original shell script.  The shell version validated paths, copied
content and wrote results back all through pathnames, which is a
TOCTOU/symlink race at every step, and its final `doas dd` write
followed symlinks (writing an attacker-chosen destination as root).

This implementation uses file-descriptor based validation (`open(2)`
with `O_NOFOLLOW`, `fstat(2)` identity snapshots), a private
`mkdtemp(3)` directory, and no shell invocation anywhere. Privileged
write-back of an existing file uses `doas install`, which renames a new
file into place (atomic) and cannot follow a swapped final symlink.
A file the user may write but not read is updated in place through the
existing inode; that direct-write path is not atomic. It stays
unpledged because it executes the editor and doas.

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

The editor is taken from `DOAS_EDITOR`, then `VISUAL`, then `EDITOR`;
if none is set, `vi` is used. The command may contain arguments; it is
executed directly with `execvp(3)` and no shell is involved. `TMPDIR`
selects the directory for the private temporary files (default
`/tmp`).

## Security limitations

The real trust boundary is `doas.conf`: `doas.d` matters only if the
local doas implementation reads it, and broad or blanket doas rules
widen what can be done. The `install` write-back reopens its temporary
source by pathname, so `TMPDIR` must be trusted; it is used as supplied
and is not validated by doasedit. The target identity check is made
immediately before the install, not atomically with it, and there is no
post-install identity check.

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
