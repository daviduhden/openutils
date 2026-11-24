# doasedit

Edit files with root privileges using your own, unprivileged editor.

A security-hardened fork of doasedit 1.0.9, rewritten in C from the
original shell script.  The shell version validated paths, copied
content and wrote results back all through pathnames, which is a
TOCTOU/symlink race at every step, and its final `doas dd` write
followed symlinks (writing an attacker-chosen destination as root).

This implementation uses file-descriptor based validation (`open(2)`
with `O_NOFOLLOW`, `fstat(2)` identity snapshots), a private
`mkdtemp(3)` directory, and fixed arguments for privileged commands. Privileged
write-back of an existing file uses `doas install`, which renames a new
file into place for a stable regular destination. The path must remain
trusted across authentication and publication.
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
passed as arguments to `execvp(3)`. That interface may select a shell
for an executable script without an interpreter line. A successful
editor exit is required before write-back. `TMPDIR`
selects the directory for the private temporary files (default
`/tmp`).

## Security limitations

The authorization boundary is `doas.conf`. OpenBSD base reads
`/etc/doas.conf`. Configuration fragments are relevant only where the
local doas implementation supports them. Broad rules widen privileged access. The `install` write-back reopens its temporary
source by pathname, so `TMPDIR` must be trusted; it is used as supplied
and is not validated by doasedit. The target identity check is made
before each install attempt. The check and replacement are separate
operations and authentication can extend the interval between them.
Read-back checks verify the named target after a privileged copy.
They do not bind the privileged read to a previously opened descriptor.

With `DOASEDIT_KEEP_FAILED=1`, a write-back failure after a successful
edit stops further processing and reports the private recovery path. The
original comparison copy is discarded. Remove the recovery directory
after retrieving the edit, particularly if the contents are sensitive.
By default temporary copies are discarded.

For privileged validation, boot a disposable OpenBSD VM and use a
dedicated non-root account with a reviewed policy and disposable targets.

- Verify a regular edit publishes the intended bytes and preserves the
  documented owner, group and mode. Verify a write-only file retains its inode.
- Verify an editor failure, a detected destination change or an interruption
  before publication leaves the destination untouched.
- Verify a failed save reports the retained edit with
  `DOASEDIT_KEEP_FAILED=1`. Check that editor-created residual files are
  reported and that another operand receives a separate temporary directory.
- Check configuration syntax only against the VM's disposable configuration
  and separately verify its intended authorization rules.

See [the privilege-boundary design review](../docs/doasedit-privilege-boundary.md)
for the assumptions and the open design decision.

## Building

```
make -C doasedit            # build ./doasedit
make -C doasedit debug      # unoptimized build under the debugger
make -C doasedit install    # honours PREFIX/DESTDIR
```

Runtime requirements: `doas(1)`, the base `dd(1)`, `cat(1)` and
`install(1)` commands, and a `doas.conf` authorizing the invocations in
doasedit(1). Granting unrestricted access to these commands delegates
general privileged file access. Use this arrangement for trusted
administrators. Review exact-argument rules after changing path
canonicalization or write-back arguments.

See [../README.md](../README.md) for the pledge profile and the known
differences from the reference implementation.
