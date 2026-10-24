# tree

Recursive directory listing in tree form.

A fork of Pierre-Yves Ritschard's tree 0.62 (the historical OpenBSD ports
implementation), extended with the option set of the tree utility in
common use today.  UTF-8 line drawing is used when the selected locale
data is available; otherwise the program warns and falls back to ASCII
line drawing and byte-oriented escaping.  Symlinks can be followed with
loop detection, and `-J` produces JSON.  Patterns use `fnmatch(3)`; the
`|`/`^` extensions of the original matcher are deliberately not
reproduced.

| File | Purpose |
| --- | --- |
| [tree.c](tree.c) | The program. |
| [tree.1](tree.1) | Manual page with the full option reference. |
| [Makefile](Makefile) | Build, test, debug and install targets. |
| [tests/](tests/) | Behavioural test suite (POSIX shell). See [Tests](#tests). |

## Usage

```
tree [-acdfFghilnpqrstuxCDJQNU] [-L level] [-o file]
     [-P pattern] [-I pattern] [--si] [--dirsfirst]
     [--filelimit n] [--inodes] [--noreport] [--prune]
     [--sort key] [--timefmt fmt] [--help] [directory ...]
```

Notable options include `-a` (hidden entries), `-d` (directories only),
`-L` (depth), `-l` (follow directory symlinks), `-s`/`-h`/`--si`
(sizes), `-p`/`-u`/`-g` (permissions, user, group), `-D` (dates, `-c`
for ctime), `-J` (JSON), `-P`/`-I` (include/exclude patterns),
`--filelimit`, `--dirsfirst`, `--sort` and `--noreport`.  `--timefmt`
implies `-D`.

Not implemented: `-X` (XML), `-H` (HTML), `-R`, `-A`, `-S`, `-v`,
`--du`, `--charset`, `--matchdirs` and `--fromfile`.  See
[tree.1](tree.1) for the exact behaviour and the differences from the
reference utility.

## Building and testing

```
make -C tree             # build ./tree
make -C tree debug       # unoptimized build under the debugger
make -C tree test        # POSIX shell behavioural tests
make -C tree install     # honours PREFIX/DESTDIR
```

The program selects `en_US.UTF-8` itself and ignores the locale
environment; on a host without that locale data it warns and degrades
to byte-oriented output with ASCII line drawing.

## Tests

The suite in [tests/](tests/) builds a temporary hierarchy with regular
files, hidden files, symlinks, FIFOs, unusual file names and nested
directories, then checks option parsing, output, JSON escaping and exit
status.  It is POSIX shell and needs no extra packages; only the
optional UTF-8 box-drawing check is skipped when the locale is absent.

| File | Purpose |
| --- | --- |
| [tests/run.sh](tests/run.sh) | Test driver (`/bin/sh`). |

Run it from the repository root:

```
sh tree/tests/run.sh
```

The `TREE` environment variable selects the binary under test.  It
defaults to an absolute path derived from the script location (the
repository's `tree/tree`).  The driver changes into a temporary
directory before running the tests, so a relative override would be
resolved in the wrong place; use an absolute path:

```
TREE=/absolute/path/to/tree sh tree/tests/run.sh
```
