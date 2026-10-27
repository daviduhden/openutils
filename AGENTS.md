# AGENTS.md

## What this repository is

`openutils` is a small collection of independent Unix utilities for
OpenBSD, built from BSD-origin implementations and adapted to match the
commonly expected user-visible behavior. It is not a portability
framework and it is not a multicall binary.

| Component | Source | Purpose |
| --- | --- | --- |
| `doasedit/` | doasedit 1.0.9, rewritten in C | edit root-owned files with an unprivileged editor |
| `ee/` | FreeBSD ee 1.5.2 | UTF-8 screen editor, also installed as `ree` and `edit` |
| `tree/` | pyr's tree 0.62 | directory tree display, including JSON |
| `truncate/` | DragonFlyBSD truncate | file size manipulation |
| `compat/` | project shims | BSD interfaces and `<stdckdint.h>` on non-BSD or older libcs |
| `LICENSES/` | per-component licences | provenance and licence text |

Each utility has its own `Makefile`, source, manual page, README and
(except `doasedit`) a test suite. The top-level `Makefile` only drives
those independent builds.

## Architecture and conventions

- OpenBSD first: use native libc interfaces (`strlcpy`, `strlcat`,
  `strtonum`, `reallocarray`, `pledge`, `unveil`, `err`, `getprogname`)
  rather than portable reimplementations. Use `pledge(2)` where the
  program does not need to execute another program.
- C23, strict ISO mode (`-std=c23`). The project uses `static_assert`,
  `[[noreturn]]`, `[[nodiscard]]`, `nullptr`, `auto` for inferred
  locals, typed enums where representation matters, and
  `<stdckdint.h>` for checked arithmetic.
- Every production C file must include `"bsdcompat.h"` before any
  system header. Never define `_XOPEN_SOURCE` in a production source:
  it belongs only in `compat/bsdcompat.h`. `make check` enforces both
  rules for the production sources it knows about.
- Follow the existing KNF/BSD style: tabs, return type on its own line,
  declarations before statements where the surrounding file does that,
  parenthesized returns, `err(3)`/`warn(3)` for diagnostics.
- No GNU dependencies, no shell wrappers around C functionality, and no
  new multicall binary. `ee` installs `ree`/`edit` links and dispatches
  on its program name; that is not a general multicall design.
- Preserve copyright notices, per-component licences and upstream
  provenance. Do not replace a component's licence with a new one.
- Keep user-visible text U.S. English and text data UTF-8, following the
  policy documented in the root README.

## Build and validation commands

These commands exist. Do not invent new targets.

```sh
make                         # build all four utilities
make doasedit tree ee truncate
make rebuild                 # clean, then build all
make check                   # strict-warning build + source-policy checks
make test                    # runtime suites (compat, tree, truncate, ee)
make debug PROG=tree         # -O0 -g build under $(DEBUGGER)
make install                 # honors PREFIX, BINDIR, MANDIR, DICTDIR, DESTDIR
make uninstall
make clean
```

Per-tool commands:

```sh
make -C doasedit            # build
make -C doasedit debug      # unoptimized build under the debugger
make -C doasedit install
make -C tree test
make -C truncate test
make -C ee test             # C unit tests + Perl PTY suites
```

`make check` is not a runtime test. It checks the first-include and
feature-test-macro rules, rebuilds with the `CHECK_WARNINGS` /
`EE_WARNINGS` sets, then cleans and rebuilds normally. It does not use
`-Werror`, so a new compiler warning is reported but does not by itself
fail the target. It mutates build outputs and cleans them.

`make test` builds first, then runs:

```sh
sh compat/tests/run.sh
FORCE=OPENUTILS_STDCKDINT_FORCE_FALLBACK sh compat/tests/stdckdint-compile-fail.sh
sh tree/tests/run.sh
sh truncate/tests/run.sh
make -C ee test
```

The ee suites need `ncursesw`, Perl 5 and `IO::Pty`; a missing
`IO::Pty` is a hard failure, not a skip. The tree and truncate suites
need a POSIX shell, `mktemp`, and `stat` with either GNU `-c` or BSD
`-f`. GNU `truncate` is optional and only enables differential checks.
`doasedit` has no automated suite.

## Validation levels

Distinguish these explicitly when reporting work:

1. **Static analysis**: reading code, `git diff`, `git diff --check`,
   `git status`, link/path checks, shell/Perl syntax checks where
   appropriate. This is the minimum for documentation-only changes.
2. **Build**: `make` or a per-tool build. It proves the compiler and
   headers accept the code, not that behavior is correct.
3. **Tests**: `make test` or a focused suite. It proves the tested
   behaviors on the current host; it does not prove OpenBSD pledge
   enforcement or privileged doas behavior.
4. **Real runtime**: OpenBSD with a real `doas.conf`, a non-root user
   and a disposable target file for `doasedit`; a real terminal for ee
   beyond PTY emulation.

For documentation-only changes, do not run `make`, `make check` or the
test suites just to "validate" the documentation. Use static checks and
state that no build or test was run.

## Security and test policy

- Never weaken or bypass `doasedit`'s `O_NOFOLLOW`, identity-snapshot,
  `mkdtemp(3)` or `doas install` checks. Do not add a shell.
- Never run privileged or destructive `doasedit` experiments against
  real system files, real `/etc/doas.conf`, or a production account.
  Use a disposable VM or a controlled test account and files. The
  repository intentionally has no mock-based doasedit suite.
- Tests must write only inside `mktemp -d`/`File::Temp` directories.
  Do not add tests that modify files outside a temporary directory.
- Do not install packages, change system configuration, commit, push,
  or rewrite unrelated work unless the user explicitly asks.
- Before editing, inspect `git status` and the diff. Treat existing
  modifications as user work; do not revert or overwrite them.
- If a change requires a real OpenBSD or `doas` environment, implement
  the code/doc change and report the unverified runtime behavior rather
  than claiming it was tested.

## Where to look first

- Root `README.md`: behavior, locale policy, pledge profiles, known
  differences, build/install overview.
- `compat/README.md`: shim matrix and `<stdckdint.h>` tests.
- Each utility's `README.md` and `.1` page: exact options and caveats.
- `LICENSES/<component>`: the licence that applies to that directory.
- `git log --oneline -- <path>`: recent intent, especially the C23
  hardening, doasedit test removal and ee modernization commits.

## Skills

Load the skill that matches the work:

- `openutils-c23-standards` — C23, headers, KNF, warnings, checked
  arithmetic, and the project's portability rules.
- `openutils-per-tool-workflows` — where each utility lives, what its
  invariants are, and the exact build/test workflow.
- `privileged-editing-security` — `doasedit` symlink/TOCTOU, privileged
  write-back, `doas.conf` and safe manual validation.
- `bsd-compatibility-and-build` — `compat/bsdcompat.h`, feature-test
  macros, `<stdckdint.h>`, Makefile variables and install layout.
- `utility-regression-and-manpages` — adding regression tests, running
  focused suites, and maintaining mdoc/man(7) pages.

Skill files live under `.agents/skills/<name>/SKILL.md`.

## Continuous Agent & Skills Maintenance

Agent instructions and skills are living repository artifacts.

Every repository modification MUST include an agent/skills synchronization step.

Before completing any task, inspect the applicable AGENTS.md and SKILL.md files, update the affected instructions, and validate their consistency with the implementation.

A code change without the corresponding agent knowledge maintenance is considered incomplete.

Never claim completion when the synchronization step has been skipped or has failed.

This applies to every repository change, including source, tests, dependencies,
configuration, workflows, documentation, generated project files and changes to
the agent system itself. Perform one complete review at the end of the task; run
the validation again only after correcting a validation failure.

### Required procedure

1. Inspect `AGENTS.md` files and the applicable skills under
   `.agents/skills/**/SKILL.md`, `.github/skills/**/SKILL.md`,
   `.claude/skills/**/SKILL.md`, plus `.github/agents/*.agent.md` and any
   tool-specific instruction files present (for example Cursor or Copilot).
   Use `rg --files --hidden` so hidden instruction directories are included.
2. Map changed paths and behavior to the instructions' stated scope, triggers,
   procedures and validation commands. Review both the root guidance and every
   matching specialized skill/agent; do not edit unrelated instructions.
3. Update an existing instruction when its behavior, assumptions, paths, tools,
   commands, checks or reusable implementation knowledge changed. Create a
   skill only when a recurring, coherent workflow has no suitable existing
   skill; give it valid Agent Skills frontmatter and link it here when relevant.
4. Keep specialized agents synchronized with the root policy and their related
   skills. If no specialized agents exist, record that in the final status;
   do not create an agent just to satisfy the policy.
5. Remove stale or contradictory operational knowledge and add only reusable
   findings. Never make cosmetic, timestamp-only or empty edits to satisfy the
   rule. If no useful update can be made, explain the reason in the final report
   and treat the task as incomplete until a relevant instruction is maintained.
6. Validate changed instruction structure, local references and the full
   synchronization gate with `python3 scripts/validate-agent-knowledge.py --scope worktree`.
   In CI, compare the PR with its base using
   `python3 scripts/validate-agent-knowledge.py --scope range --base origin/<base-branch> --head HEAD`
   Also run `git diff --check`. Run build or runtime checks appropriate to the
   implementation; documentation-only changes do not need builds or tests.
   The automated gate checks structure and substantive textual changes, not
   whether an update is semantically useful; a human review must assess that.
7. If an instruction update or validation cannot be completed, state the exact
   limitation and report the task as incomplete. Do not silently omit it.

### Mandatory definition of done

- [ ] Repository changes implemented.
- [ ] Relevant agent instructions reviewed.
- [ ] Relevant skills reviewed.
- [ ] Affected AGENTS.md / SKILL.md files updated.
- [ ] Obsolete knowledge removed.
- [ ] New reusable knowledge incorporated.
- [ ] Agent and skill references validated.
- [ ] Automated maintenance checks passed.
- [ ] Final synchronization status reported.

Every final task report must include **Agent & Skills Synchronization** and
identify agents and skills reviewed, instruction files updated, reusable
knowledge added or corrected, validations actually run, and a `PASS` or `FAIL`
status. Do not say a check passed unless it was run.

### Shared Agent Knowledge Validation Contract

Use `python3 scripts/validate-agent-knowledge.py --scope worktree` for local
changes and `python3 -m unittest discover -s tests/agent_knowledge_validator -v`
for its contract tests. The validator requires Python 3.11+ and Git, uses only
the Python standard library, and publishes contract version `1.0.0`. All
adopting repositories share the same CLI, AK001-AK006 identifiers, output
schema, and exit codes (`0` pass, `1` policy failure, `2` execution/configuration
error). For a pull request, pass both revisions with
`--scope range --base BASE_REF --head HEAD_REF`; compare from their merge base.

Read [Agent Knowledge Validation Contract](docs/agent-knowledge-validation.md)
for the complete rules, JSON schema, optional repository configuration,
warning semantics, and extension process. AK004 and AK006 can require human
review; their warnings do not prove or disprove semantic correctness. Keep the
canonical script and black-box unittest suite byte-identical across adopting
repositories. Repository-specific requirements belong in the documented TOML
configuration or a stable `AKX...` extension, never in a private fork of the
shared rules. The GitHub PR workflow runs the same tests and range command;
it does not edit files or require secrets.
