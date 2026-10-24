---
name: openutils-per-tool-workflows
description: "Use when locating or changing one of the openutils utilities (doasedit, tree, ee, truncate), choosing the right build/test command, or checking a tool's invariants before a change."
---

# openutils per-tool workflows

## When to use this

Use this skill when the task names a specific utility, when you need to
find where an option or behavior lives, or when you need the exact
build/test command. Load `privileged-editing-security` before changing
`doasedit`, and `utility-regression-and-manpages` before adding tests
or editing a manual page.

## Tool map

| Tool | Main source | Tests | Key invariants |
| --- | --- | --- | --- |
| `doasedit` | `doasedit/doasedit.c` | none | no shell; fd-based validation; `doas` boundary; unprivileged editor |
| `tree` | `tree/tree.c` | `tree/tests/run.sh` | bounded traversal; symlink loop detection; JSON escaping; `-o` output |
| `ee` | `ee/ee.c`, `ee/utf8.c`, `ee/help.c`, `ee/spell/spell.c` | `ee/tests/*.pl`, C unit tests | UTF-8-only buffer; explicit save; restricted `ree`; ncursesw |
| `truncate` | `truncate/truncate.c` | `truncate/tests/run.sh` | checked size grammar; `ftruncate(2)`; per-file errors |

## Common commands

From the repository root:

```sh
make                         # all four
make doasedit                # one tool
make -C doasedit             # direct per-tool build
make test                    # compat, tree, truncate, ee
make check                   # strict build + source policy, not runtime tests
make debug PROG=tree         # build tree_debug and launch $(DEBUGGER)
make install                 # PREFIX/DESTDIR aware
make clean
```

Focused tests:

```sh
TREE="$PWD/tree/tree" sh tree/tests/run.sh
TRUNC="$PWD/truncate/truncate" sh truncate/tests/run.sh
make -C ee test
EE="$PWD/ee/ee" perl ee/tests/run.pl
```

Use absolute paths for `TREE` and `TRUNC` when invoking the drivers
directly: the tree driver changes into a temporary directory, and both
drivers default to absolute paths derived from their own location.

## doasedit

- Read `doasedit/README.md` and `doasedit/doasedit.1` before changing
  behavior. The security model is described there, including the split
  between the atomic `doas install` path and the non-atomic direct-write
  path.
- There is no automated suite. The old mock-based tests were removed
  deliberately. Do not reintroduce test-only hooks or a fake `doas`
  harness unless the user explicitly asks.
- Build for syntax/warning coverage with `make -C doasedit`. Real
  validation needs OpenBSD, a non-root user, a real `doas.conf`, and
  disposable files; never test against a production `/etc/doas.conf` or
  a real system file.
- Keep the editor unprivileged and the privileged operations limited to
  fixed `doas dd`, `doas cat`, `doas install` and `doas -C` calls.

## tree

- Traversal is split between the text walk (`dirwalk`) and the JSON
  walk (`jsonentries`/`jsonwalk`). If you change a traversal rule,
  update both paths.
- The visited `(dev, ino)` set is global to the walk; a symlink to any
  directory already visited is reported as recursive. It is not an
  ancestor-only detector.
- `-x` applies to ordinary subdirectories and followed symlinks; a
  mount point is printed but not descended.
- `-o` opens with `O_NOFOLLOW` and output write/close errors set exit
  status 2.
- `-N` deliberately prints raw names; do not use it when output is
  consumed by a terminal or a strict JSON parser.
- Tests: `sh tree/tests/run.sh`. Use `TREE=/absolute/path`.

## ee

- The buffer is a doubly linked list of lines. `line_length` includes
  the terminating NUL; `write_file` writes `line_length - 1` bytes plus
  a newline.
- Input is validated before it enters the buffer: NUL and malformed
  UTF-8 are rejected. Do not add a byte-oriented mode.
- The editor is always UTF-8; `LC_CTYPE` must be UTF-8 or ee refuses to
  start. The interface is U.S. English.
- `ree` is selected by `argv[0]`; it may save the named file but refuses
  save-as, shell escapes, read/write commands and configuration saving.
- Configuration is read from `/usr/share/misc/init.ee`, then
  `$HOME/.init.ee`, then `./.init.ee`, with later files overriding.
- `make -C ee test` runs the C spell tests, then `run.pl`, `signals.pl`,
  `modern.pl` and `features.pl`. The Perl suites need `IO::Pty`; a
  missing module is a hard failure.
- Tests isolate `HOME` to a temporary directory. Keep that isolation if
  you add a suite.

## truncate

- `parse_size()` is hand-written and uses checked arithmetic. It
  accepts absolute sizes, the `+ - < > / %` modifiers, and the
  K/M/G/T/P/E/Z/Y/R/Q suffix set with `B`/`iB` variants. The `D` suffix
  is not accepted.
- A leading `-` means "reduce by", not a negative absolute size.
- With `-r`, the final `-s` token must itself be relative.
- Options must precede operands; the command line is not permuted.
- Each operand is handled independently; per-file errors set exit 1 but
  do not stop later operands.
- Tests: `sh truncate/tests/run.sh`. Without GNU `truncate`, differential
  checks are reported as skipped; direct assertions still run.

## Procedure

1. Identify the utility and read its README, manual page and Makefile
   before touching the source.
2. Locate the parser, the write path and the tests that already cover
   the behavior. For tree, check both text and JSON walks; for ee, check
   both the editor and the spell unit; for truncate, check the size
   grammar and per-file loop.
3. Make the smallest change that preserves the tool invariants listed
   above. Do not add a new binary, shell wrapper or GNU dependency.
4. Add or adjust a focused regression test in the existing suite.
5. Run the focused build/test if the environment allows it; otherwise
   report the change as statically reviewed and unverified.

## Validation

- Static: `git diff --check`, `git status`, and a path/option check
  against the tool's parser and manual page.
- Build: `make -C <tool>` (and `make check` only when the strict
  warning/source-policy gate is wanted; it cleans and rebuilds).
- Focused tests: `sh tree/tests/run.sh`, `sh truncate/tests/run.sh`, or
  `make -C ee test`; use absolute `TREE`/`TRUNC` overrides.
- Real runtime: OpenBSD with a real terminal for ee interactions beyond
  PTY emulation, and OpenBSD plus `doas` for doasedit. Do not claim
  these were tested without the environment.

## Common problems

- Running `make check` to "test" behavior. It is a strict build and
  source-policy check that cleans and rebuilds; use `make test` or a
  focused suite for behavior.
- Running `make test` on a host without `IO::Pty`; the ee suites fail
  rather than skip.
- Using a relative `TREE` or `TRUNC` override after the driver changes
  directory.
- Testing `doasedit` with a fake `doas` or against real system files.
- Editing only one of tree's text/JSON traversal paths.
- Assuming ee can save an unnamed buffer in `ree`; save-as is refused.

## References

- `AGENTS.md`
- `doasedit/README.md`, `tree/README.md`, `ee/README.md`,
  `truncate/README.md`
- `Makefile` and each tool's `Makefile`
- `LICENSES/<tool>`
