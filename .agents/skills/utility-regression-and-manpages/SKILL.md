---
name: utility-regression-and-manpages
description: "Use when adding a regression test, running a focused test suite, or editing an openutils manual page or README, including mdoc/man(7) conventions and test harness details."
---

# Utility regression tests and manual pages

## When to use this

Use this skill when a behavior changes and needs a regression test, when
you need to run one suite instead of `make test`, or when editing a
`.1` page or a utility README. Pair it with the tool-specific skill:
`openutils-per-tool-workflows` for locations and
`privileged-editing-security` for doasedit.

## Test harness conventions

- Shell suites are `#!/bin/sh`, use `set -u` only (not `set -e`), and
  locate the repository root from the script path. Keep that pattern.
- Every suite creates its own temporary directory with
  `mktemp -d "${TMPDIR:-/tmp}/...XXXXXX"` and removes it with a trap.
  Tests must not write outside that directory.
- Output is TAP-like: `ok N - description`, `not ok - description`, and
  a final `pass: N  fail: M` line. Add a skip counter only where a real
  prerequisite can be missing; do not count a skipped differential
  check as a pass.
- Binary selection uses environment variables with absolute defaults:
  `TREE` for tree, `TRUNC` for truncate, `EE` for ee. The tree driver
  changes directory, so a relative `TREE` override is resolved in the
  wrong place.
- `truncate/tests/run.sh` runs differential checks only when a GNU
  `truncate` is found in `PATH`; without it, those checks are skipped and
  the direct assertions carry the regression coverage.
- ee uses Perl 5 with `IO::Pty` and a PTY. `run.pl` covers editing,
  saving, UTF-8 and locale policy; `signals.pl` covers signals and
  terminal robustness; `modern.pl` covers resize, NUL/malformed UTF-8,
  wide characters, tiny terminals and a shell command; `features.pl`
  covers vi bindings, settings-menu navigation and formatter behavior.
- ee tests should isolate `HOME` to a temporary directory so a
  developer's `~/.init.ee` cannot change results.

## Procedure

1. Reproduce the behavior in the existing harness style, then read the
   parser/write path that implements it.
2. Add the smallest assertion to the suite that owns the component, or
   edit the manual page/README that documents it.
3. Keep tests inside the suite's temporary directory and keep manual
   examples limited to targets that actually exist.
4. Run the focused suite if prerequisites are present. If not, report
   the missing prerequisite instead of claiming a pass.
5. Re-read the final diff for stale options, wrong paths and
   contradictions between the README, the manpage and the source.

## Running focused tests

From the repository root:

```sh
TREE="$PWD/tree/tree" sh tree/tests/run.sh
TRUNC="$PWD/truncate/truncate" sh truncate/tests/run.sh
make -C ee test
EE="$PWD/ee/ee" perl ee/tests/run.pl
EE="$PWD/ee/ee" perl ee/tests/signals.pl
```

`make test` runs all of them. `make check` is not a substitute: it is a
strict build/source-policy check.

## Adding a regression test

1. Reproduce the behavior manually in the existing harness style, then
   add the smallest assertion to the suite that owns the component.
2. For tree, add cases to `tree/tests/run.sh` using `assert_out`,
   `assert_not_out`, `assert`, or `assert_eq`. Add fixtures under the
   existing temporary hierarchy.
3. For truncate, add direct size/status assertions to
   `truncate/tests/run.sh`; use `compare` only for GNU-differential
   behavior. `assert_eq` returns nonzero on failure so a fallback can
   run.
4. For ee, add a C unit test for spell/dictionary logic, or add a PTY
   case to the relevant `.pl` suite using `pty_run.pm`. Register a new
   Perl suite in `ee/Makefile`'s `test` target.
5. For compat, add a shell test under `compat/tests/` and register it in
   the top-level `test` target. There is no `compat/Makefile`.
6. For doasedit, do not add a mock-based suite. There is no automated
   suite by design; use the privileged-test policy in
   `privileged-editing-security`.
7. Run the focused suite. If you cannot run it, say so explicitly.

## Manual page conventions

- `doasedit/doasedit.1`, `tree/tree.1` and `truncate/truncate.1` use
  mdoc: `.Dd`, `.Dt`, `.Os`, `.Sh`, `.Ss`, `.Nm`, `.Nd`, `.Op`, `.Fl`,
  `.Ar`, `.Cm`, `.Pa`, `.Ev`, `.Xr`, `.Bl`/`.It`, `.Pp`, `.Sq`, `.Ql`.
- `ee/ee.1` is an imported legacy man(7) page: `.TH`, `.SH`, `.SS`,
  `.TP`, `.IP`, `.B`, `.I`, `.nf`/`.fi`. Keep that dialect rather than
  converting it to mdoc.
- Keep section order consistent with the file: NAME, SYNOPSIS,
  DESCRIPTION, options/environment/files, EXIT STATUS where present,
  custom sections such as LOCALE/COMPATIBILITY/SECURITY, SEE ALSO.
- Do not document an option that the parser rejects, and do not omit an
  option that is implemented. Check the actual parser and `--help`
  output.
- Use `.Xr` for cross-references in mdoc pages. Keep the date field
  current when behavior changes (use a real full date, e.g.
  `October 24, 2026`).
- Preserve upstream copyright/warranty text and per-component licence
  references. Do not add a new licence header to a fork source; the
  per-component file under `LICENSES/` is the authority.
- README examples must be runnable from the repository root and must
  match the Makefile targets that actually exist.

## Validation

- Static: inspect the diff, `git diff --check`, `git status`, and check
  every command/path/option against the source and Makefile.
- Test syntax only where it does not run the suite: `sh -n` for shell
  drivers or `perl -c` for Perl files if those interpreters are
  available. Do not claim this is a test run.
- Focused runtime suite for behavior changes; `make test` only when the
  change spans components and the prerequisites are present.
- If a prerequisite is missing (IO::Pty, GNU truncate, UTF-8 locale),
  report it; do not silently treat the suite as fully passing.

## Common problems

- Adding an assertion for behavior the code does not implement. Read
  the parser and the write path first.
- Using a relative `TREE`/`TRUNC` after the driver changes directory.
- Writing a test that depends on a developer's `$HOME/.init.ee`.
- Counting skipped GNU-differential checks as passes.
- Adding a new Perl suite without registering it in `ee/Makefile`.
- Documenting `--version`, `-A`, or another option the parser rejects.
- Converting ee's man(7) page to mdoc or mixing macro dialects.
- Claiming `make check` proves behavior; it only compiles and checks
  source policy.

## Maintenance & Continuous Learning

Whenever this skill is used to change the repository, review it after the
implementation. Update harness conventions, manual-page guidance, paths,
commands and validation procedures when they change; add reusable findings
and correct instructions that proved wrong. Keep examples current and omit
one-off details. Review the root `AGENTS.md` and applicable skills; run
`perl scripts/check-agent-maintenance.pl` and `git diff --check`. Include
the synchronization result in the final report. A task is incomplete if
relevant instructions or the gate are skipped; automated structure/reference
checks need a semantic human review as well.

## References

- `tree/tests/run.sh`, `truncate/tests/run.sh`,
  `compat/tests/run.sh`, `ee/tests/`
- `Makefile` and `ee/Makefile` test targets
- `tree/tree.1`, `truncate/truncate.1`, `doasedit/doasedit.1`,
  `ee/ee.1`
- `AGENTS.md`
