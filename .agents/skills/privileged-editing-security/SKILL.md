---
name: privileged-editing-security
description: "Use when changing or reviewing doasedit, privileged file write-back, symlink/TOCTOU handling, doas.conf interaction, or any code that edits files owned by another user."
---

# Privileged editing security

## When to use this

Use this skill for `doasedit/doasedit.c`, its manual page or README, and
for any review of privileged write-back logic. It is also relevant when
a change touches `doas`, `install`, temporary-directory handling, or
ownership/mode preservation. Pair it with `openutils-per-tool-workflows`
for commands and `utility-regression-and-manpages` for documentation.

## Threat model

`doasedit` lets an unprivileged user edit a file that the user cannot
write directly. The danger is not the editor itself; it is the
privileged operations used to copy the file in and write it back. A
path-based privileged copy or write can be redirected by:

- replacing the final path component with a symlink;
- replacing a parent directory or an inode between validation and use;
- modifying the temporary file after it has been checked;
- relying on a user-controlled `TMPDIR` or editor environment;
- granting broad `doas` rules that permit more than the intended
  operation.

`doas.conf` is the real security boundary. `doasedit` cannot make a
blanket `permit` rule safe; it can only avoid adding new path races on
top of the permissions the administrator granted.

## Implementation invariants

Preserve all of these unless the user explicitly requests a redesign:

- The editor runs as the invoking user, never as root. `execvp(3)` is
  used directly; there is no shell.
- Source and target opens use `O_NOFOLLOW` where a final symlink would
  be dangerous.
- The target's identity is captured in a `struct snapshot` (device,
  inode, mode, owner, group, size, modification time) and compared
  before write-back.
- After opening a descriptor, compare its `fstat(2)` identity against
  the snapshot, not only device/inode.
- Temporary files live in a private `mkdtemp(3)` directory with `0600`
  files. Do not share the directory or predict its name.
- The privileged write for existing files goes through `doas install`
  (temporary file in the destination directory plus `rename(2)`), which
  replaces a swapped final symlink rather than following it.
- The direct-write path is only for a target the user may write but not
  read. It updates the existing inode in place and is not atomic; do not
  describe it as an atomic rename.
- New files are installed with mode `0644`; existing files preserve
  owner/group/mode on the install path, including setuid/setgid bits.
- The program refuses to run as root (`getuid` and `geteuid`) and
  refuses to edit the caller's own files, readable+writable files, and
  new files in a caller-owned or caller-writable parent.
- `doas -C` is a syntax check of an edited configuration, not a proof
  that the resulting rules are safe or still permit the user.

## Procedure for a change

1. Read `doasedit/README.md`, `doasedit/doasedit.1` and the whole
   affected function. Do not edit a single branch without understanding
   the copy-in, edit, compare and write-back sequence.
2. Keep path validation and descriptor validation separate. If you add a
   new path-based privileged operation, explain why it cannot be
   replaced by an already-open descriptor.
3. Never add a shell, `system(3)`, or a path that lets the editor choose
   a privileged command.
4. If you change the write path, update both the manual page and the
   README. State whether it is atomic, whether it preserves the inode
   and hard links, and which metadata may be lost.
5. Keep failure behavior safe: if identity checks fail, refuse to write;
   do not "retry anyway" except for the explicit password/copy retry
   paths.
6. Do not add test-only environment hooks. The project intentionally
   removed the mock-based suite.

## Validation

- Static: inspect the diff, `git diff --check`, verify every privileged
  call is a fixed argv, and trace each `open`/`fstat`/`install` path.
- Build: `make -C doasedit` for compiler/warning coverage.
- Runtime: only on OpenBSD, as a non-root user, with a controlled
  `doas.conf` and disposable target files in a dedicated directory. Test
  symlink replacement, write-only targets, unreadable targets, unchanged
  files, and signal cleanup. Never test against a real `/etc/doas.conf`,
  a production account, or system binaries.
- If the environment is not available, report the runtime behavior as
  unverified. Do not claim a privileged path was tested.

## Common problems

- Treating `TMPDIR` as trusted. It is used as supplied; the temporary
  directory must not be writable by an attacker.
- Forgetting that `doas install` reopens the temporary source by
  pathname. The temporary directory must be private.
- Calling the direct-write path "atomic". It is an in-place update and
  can leave a partial file on error or interruption.
- Assuming `doas -C` prevents lockout. It only parses syntax.
- Testing with a fake `doas` on `PATH`. The real program has privilege
  and setuid behavior that a fake cannot model.
- Preserving setuid/setgid bits without noting that the install path
  creates a new inode with user-controlled content.
- Adding a new privileged operation to the manual page without adding
  it to the exact `doas` permission list.

## References

- `doasedit/doasedit.c`
- `doasedit/doasedit.1`
- `doasedit/README.md`
- Root `README.md`, sections "doasedit" and "pledge profiles"
- `LICENSES/doasedit`
- `git log --oneline -- doasedit`

## Maintenance & Continuous Learning

Whenever this skill is used to change the repository, review it after the
implementation. Update threat assumptions, security invariants, procedures,
paths and validation guidance when the project changes; add reusable findings
and correct instructions that proved wrong. Keep examples current and omit
one-off execution details. Review the root `AGENTS.md` and applicable skills;
run `perl scripts/check-agent-maintenance.pl` and `git diff --check`. Report
the synchronization result. A task is incomplete if relevant instructions or
the gate are skipped; automation checks structure and references, while
semantic relevance requires review.
