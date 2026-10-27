---
name: openutils-c23-standards
description: "Use when editing C sources, headers, or compiler flags in openutils, especially changes involving C23 features, bsdcompat.h, checked arithmetic, KNF style, or warning-clean builds."
---

# openutils C23 standards

## When to use this

Use this skill for any change to `doasedit/*.c`, `tree/*.c`,
`truncate/*.c`, `ee/*.c`, `ee/spell/*.c`, `compat/*.c`, or a project
header. Also use it when changing `CSTD`, warning flags or the
first-include policy. For build-system and shim details, pair it with
`bsd-compatibility-and-build`; for tests and manpages, pair it with
`utility-regression-and-manpages`.

## Non-negotiable rules

- The language is strict ISO C23: `-std=c23`. Do not fall back to GNU C
  or add compiler-specific syntax beyond the deliberate exceptions
  already in the tree.
- Every production C file includes `"bsdcompat.h"` first, before any
  system header. The include block is wrapped in
  `// clang-format off` / `// clang-format on` in the existing files;
  keep that pattern.
- `_XOPEN_SOURCE` and `_DEFAULT_SOURCE` are defined only in
  `compat/bsdcompat.h` for non-OpenBSD hosts. Never add them to a
  production source. On OpenBSD they would hide BSD-visible interfaces.
- Use OpenBSD/libc interfaces deliberately: `strlcpy`, `strlcat`,
  `strtonum`, `reallocarray`, `err`/`warn`, `getprogname`,
  `pledge`, `unveil`. Do not replace them with ad-hoc portable code.
- Use `<stdckdint.h>` (`ckd_add`, `ckd_sub`, `ckd_mul`) for arithmetic
  that can overflow. `compat/stdckdint.h` supplies it when the libc
  lacks the header.
- Keep the existing KNF/BSD style: tabs for indentation, return type on
  its own line, a space after keywords, parenthesized returns such as
  `return (0);`, aligned declarations, and braces on the next line.
- Use C23 contracts where the surrounding code does: `static_assert`
  for invariants, `[[noreturn]]`, `[[nodiscard]]`, `nullptr`, and
  `auto` only for a type-preserving local whose type is obvious.
- Do not add a new GNU dependency, a shell wrapper around C logic, or a
  multicall dispatcher. The only deliberate GNU-ish constructs are the
  `#include_next` fallback in `compat/stdckdint.h` and the printf
  format attribute on tree's bounded formatter.
- Keep user-visible text U.S. English. Keep text data UTF-8; do not add
  locale-dependent messages or alternative human languages.
- Preserve copyright/licence headers and per-component provenance.

## Procedure

1. Read the whole function and the nearby declarations before editing.
   Many files use file-static state and strict ordering.
2. Check the include block first. If you add a header, keep
   `bsdcompat.h` first and keep system/project headers in the existing
   order.
3. Prefer a small, local fix over a refactor. Match the file's existing
   error style (`err`, `warn`, `warnx`, or the editor's message bar).
4. For size/length arithmetic, use `size_t` and `ckd_*`; check the
   result before allocating or indexing.
5. For resource handling, keep the existing cleanup labels/patterns and
   close every descriptor on both success and failure paths.
6. For public/internal functions, add `[[nodiscard]]` only when the
   surrounding code uses it and ignoring the result is a bug.
7. Run the relevant static checks and, for code changes, the narrowest
   build/test that covers the file. Do not run privileged tests.

## Validation

- Documentation-only changes: `git diff --check`, `git status`, and
  path/link checks. Do not run `make`.
- C changes: at minimum a per-tool build (`make -C <tool>`), then the
  focused suite. `make check` is the strict-warning and source-policy
  gate; it cleans and rebuilds, so do not run it casually.
- Compiler warning policy: the default build uses `-Wall -Wextra
  -Wpedantic`; `make check` adds `-Wshadow -Wformat=2 -Wundef
  -Wstrict-prototypes -Wmissing-prototypes` and, for the non-ee tools,
  `-Wconversion -Wsign-conversion`. `-Werror` is not used.
- If you cannot build (for example, no C23 compiler or no ncursesw),
  report that the change is statically reviewed but unbuilt. Do not
  claim a successful build or test.

## Common problems

- Adding `_XOPEN_SOURCE` to a source to expose a prototype. Fix the
  include or the compat header instead; `make check` rejects the leak.
- Forgetting `bsdcompat.h` as the first include, especially after
  adding a project header.
- Using `int` for buffer sizes or `n * size` without checked
  arithmetic; use `size_t` and `ckd_mul`.
- Assuming `strlcpy`/`strlcat` always fit; check truncation where the
  result matters.
- Replacing `err(3)` with `perror`/`fprintf` and losing the program-name
  prefix or exit behavior.
- Casting away `const` or using `strdup` without checking the result.
- Adding a C23 feature in a file that still has old-style declarations;
  match the file's existing style rather than mixing conventions.

## References

- `AGENTS.md`
- `compat/bsdcompat.h`
- `compat/stdckdint.h`
- Root `README.md`, section "Language standard"
- `Makefile` (`CSTD`, `CHECK_WARNINGS`, `EE_WARNINGS`, `check` target)

## Maintenance & Continuous Learning

Whenever this skill is used to change the repository, review it after the
implementation. Update it if source conventions, compiler behavior, flags,
paths or validation procedures changed; incorporate reusable findings and
correct instructions that proved wrong. Keep examples current and omit
execution-specific details. Review the root `AGENTS.md` and other applicable
skills; run `python3 scripts/validate-agent-knowledge.py --scope worktree` and
`git diff --check`. Report synchronization in the final result. A task is
incomplete if the relevant instructions or gate were skipped; automation
checks structure and references, and a person must assess semantic relevance.
