# ee (Easy Editor)

The small, friendly screen editor.

Imported from the FreeBSD source tree (Hugh Mahon's ee 1.5.2) and
modernized: terminal handling now uses `ncursesw`, and the historical
bundled mini-curses library (`new_curse`) has been removed entirely.
The editing model, commands, buffer semantics and configuration file
format are unchanged.  The editor is UTF-8 only and its interface is
U.S. English only; text is validated before it is loaded, so a file
containing a NUL byte or malformed UTF-8 is rejected instead of being
silently accepted.

| File | Purpose |
| --- | --- |
| [ee.c](ee.c) | The editor: main loop, buffer, commands, menus and formatter. |
| [help.c](help.c), [help.h](help.h) | Built-in help text and the per-binding-set help screen. |
| [utf8.c](utf8.c), [utf8.h](utf8.h) | Strict UTF-8 validation, width and cursor helpers. |
| [spell/](spell/README.md) | ee's own affix-based spell checker. |
| [tests/](tests/) | PTY-based behavioural and regression tests. See [Tests](#tests). |
| [ee.1](ee.1) | Manual page. |
| [Makefile](Makefile) | Build, test, debug and install targets. |

## Usage

```
ee [-e] [-i] [-h] [+#] [file ...]
ree [-e] [-i] [-h] [+#] [file ...]
```

`-i` hides the shortcut bar (the status bar is always shown), `-e`
keeps tabs as tabs, `-h` disables reverse video, and `+#` starts at a
line.  `ree` is the restricted mode and `edit` is installed as another
name for `ee`, as upstream does.

## Key bindings

Three binding sets can be selected from the settings menu or from the
initialization file:

- the traditional Easy Editor control keys (default);
- Emacs-style control keys;
- a small vi normal/insert pair (`i`/`Esc`, `hjkl`, `0 $`, `w b`, `x`,
  `dd`, `D`, `o O`, `u`, and `:w`/`:q`/`:wq`/`:q!`).

All three dispatch to the same editing operations, so the editing logic
exists only once.  See [ee.1](ee.1) for the full key list.

## Configuration

`ee` reads `/usr/share/misc/init.ee`, then `$HOME/.init.ee`, then
`./.init.ee`, in that order.  Recognized lines include `case`,
`nocase`, `expand`, `noexpand`, `info`, `noinfo`, `margins`,
`nomargins`, `autoformat`, `noautoformat`, `printcommand`,
`rightmargin`, `highlight`, `nohighlight`, `emacs`, `noemacs`, `vi`,
`spell` and `nospell`.  The settings menu can save the current
configuration with **save editor configuration**.

## Formatting and spelling

Paragraph formatting targets 72 columns for ordinary prose (the
traditional width for technical mail) and leaves code, tables and
CVS/Git/Got patches untouched.

Spelling is checked by ee's own small affix checker in
[spell/](spell/README.md); no external program is run and no dictionary
is bundled.  The miscellaneous menu can check the word at the cursor,
offer suggestions and toggle spell checking.

## Building and testing

```
make -C ee             # build ./ee (needs ncursesw)
make -C ee debug       # unoptimized build under the debugger
make -C ee test        # unit test + PTY behavioural tests
make -C ee install     # installs ee, ree, edit and ee.1
```

`make -C ee test` builds and runs the spell unit test, then the Perl
suites.  The Perl suites need Perl 5 with the IO::Pty module; on
OpenBSD install it with `doas pkg_add p5-IO-Tty` (devel/p5-IO-TTY).

## Tests

Most tests in [tests/](tests/) drive the editor through a
pseudo-terminal; the spell test is a plain C program and needs nothing
extra.

| File | Purpose |
| --- | --- |
| [tests/test-spell.c](tests/test-spell.c) | Unit test for the [spell engine](spell/README.md). |
| [tests/spell-test.aff](tests/spell-test.aff), [tests/spell-test.dic](tests/spell-test.dic) | Tiny dictionary written for that test (project licence). |
| [tests/pty_run.pl](tests/pty_run.pl) | Shared PTY helper: runs a program, feeds key sequences and returns its output. |
| [tests/run.pl](tests/run.pl) | Core behaviour: editing, menus, saving, UTF-8 input, locale policy. |
| [tests/signals.pl](tests/signals.pl) | Signal handling and terminal robustness (SIGINT, SIGWINCH, hostile `TERM`). |
| [tests/modern.pl](tests/modern.pl) | Resize in every modal state, UTF-8/NUL policy, wide and combining characters, tiny terminals, help paging, shell round trips. |
| [tests/features.pl](tests/features.pl) | vi key bindings, configuration persistence and the diff/code protection of the formatter. |
| [tests/mkterminfo.pl](tests/mkterminfo.pl) | Builds a minimal terminfo entry for the tests. |

Each suite prints TAP-style `ok`/`not ok` lines and a final `pass`/`fail`
summary, and exits non-zero if anything failed.

Environment variables:

| Variable | Meaning |
| --- | --- |
| `EE` | Binary under test (default `ee/ee`). |
| `TERM` | Terminal type used for the PTY sessions (default `xterm`). |

The generated `test-spell` binary is removed by `make -C ee clean`.

See [../README.md](../README.md) for the pledge profile, the locale
policy and the known differences from upstream.
