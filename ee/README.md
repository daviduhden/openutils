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
ee [-e] [-i] [-h] [-?] [+#] [file ...]
ree [-e] [-i] [-h] [-?] [+#] file ...
edit [-e] [-i] [-h] [-?] [+#] [file ...]
```

`-i` hides the shortcut bar (the status bar is always shown), `-e`
keeps tabs as tabs, `-h` disables reverse video, `+#` starts at a line,
and `-?` prints usage and exits 1.  Option processing stops at the
first operand, so options must precede file names; a later `-i` or
`-h` is treated as a file name.

`ree` is the restricted mode.  It requires a file operand and can save
that file, although not under another name; shell escapes (including
printing, which runs a shell command), the read and write commands, and
saving the configuration are refused.  `edit` is installed as a symbolic
link to `ee`, as upstream does.

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
`./.init.ee`, in that order.  When the current directory is the home
directory the file is not read twice.  Recognized lines include `case`,
`nocase`, `expand`, `noexpand`, `info`, `noinfo`, `margins`,
`nomargins`, `autoformat`, `noautoformat`, `printcommand`,
`rightmargin`, `highlight`, `nohighlight`, `emacs`, `noemacs`, `vi`,
`spell` and `nospell`.  `ECHO text` writes text to standard output with
backslash escapes interpreted; it is not a setting.  The settings menu
can save the current configuration with **save editor configuration**,
except in restricted mode.

The current directory is a configuration source. Open files from a
trusted working directory when local settings such as `printcommand`
matter. Configuration sources must resolve to regular files. Configuration saves finish the new file before replacing the
current one and retain the previous version as `.init.ee.old`.

Document saves stage a file in the destination directory and publish it
after a successful write and close. Saving an existing file requires a
writable directory and an owned, writable regular file with one link and
ordinary permissions. Symbolic links, hard-linked files, and files owned
by another user are refused. Existing owner, group and permission bits
are preserved. File flags, ACLs and extended attributes are outside
this preservation contract. Saving over the opened file checks for changes
since it was read, then checks the destination again before publication.
If the file changed on disk, use the write command to save the buffer under
another name. Crash durability is outside this contract. Saving normalizes
the file to end with a newline.

At runtime `EE_DICTIONARY` takes precedence over the build-time `DICTDIR`.
Changing `DICTDIR` triggers a rebuild. Existing MySpell and Hunspell
directories remain available as runtime fallbacks. Cursor movement uses Unicode code points
and terminal column widths. Some combining sequences can require more
than one editing operation.


## Formatting and spelling

Paragraph formatting targets 72 columns for ordinary prose (the
traditional width for technical mail) and leaves code, tables and
CVS/Git/Got patches untouched.  The miscellaneous menu can reflow the
current paragraph or the whole document; the whole-document command
reformats each paragraph independently and preserves blank lines and
paragraph boundaries.

Spelling is checked by ee's own small affix checker in
[spell/](spell/README.md); no external program is run.  A U.S. English
dictionary is bundled in [spell/](spell/README.md) as
[en_US.aff](spell/en_US.aff) and [en_US.dic](spell/en_US.dic), dedicated
to the public domain under CC0-1.0, and installed to `$(DICTDIR)`
(`/usr/local/share/openutils/ee` by default).  The miscellaneous menu can
check the word at the cursor, offer suggestions and toggle spell
checking, but only ASCII words are checked; non-ASCII text is not
examined.  The spell engine accepts and ignores a `SET` directive in
the affix file; `SET` is not an ee setting.

## Saving files

Saves stage and sync a sibling file before publication. The editor
takes a nonblocking advisory lock on the parent directory for the
final identity check and publication. A busy lock leaves the buffer
available for retry. The directory must be readable and support
advisory locking. The lock coordinates ee processes saving in the
same directory. Other programs can still modify files independently.

After publication, ee syncs the directory and reports a warning if
that sync fails. The file has already been published when this
warning appears.

## Building and testing

```
make -C ee             # build ./ee (needs ncursesw)
make -C ee debug       # unoptimized build under the debugger
make -C ee test        # unit, dictionary and PTY behavioural tests
make -C ee install     # installs ee, ree, edit, ee.1 and the en_US dictionary
```

`make -C ee test` builds and runs the spell unit test and the dictionary
integration test, then the Perl suites.  The Perl suites need Perl 5
with the IO::Pty module; on OpenBSD install it with
`doas pkg_add p5-IO-Tty` (devel/p5-IO-TTY).

## Tests

Most tests in [tests/](tests/) drive the editor through a
pseudo-terminal; the spell unit and dictionary tests are plain C
programs and need nothing extra.

| File | Purpose |
| --- | --- |
| [tests/test-spell.c](tests/test-spell.c) | Unit test for the [spell engine](spell/README.md). |
| [tests/test-dictionary.c](tests/test-dictionary.c) | Integration test that loads the bundled `en_US` dictionary. |
| [tests/spell-test.aff](tests/spell-test.aff), [tests/spell-test.dic](tests/spell-test.dic) | Tiny dictionary written for that test (project licence). |
| [tests/pty_run.pm](tests/pty_run.pm) | Shared PTY helper: runs a program, feeds key sequences and returns its output. |
| [tests/run.pl](tests/run.pl) | Core behaviour: editing, menus, saving, UTF-8 input, locale policy. |
| [tests/signals.pl](tests/signals.pl) | Signal handling and terminal robustness (SIGINT, SIGWINCH, hostile `TERM`). |
| [tests/modern.pl](tests/modern.pl) | Resize in every modal state, UTF-8/NUL policy, wide and combining characters, tiny terminals, resize inside help, and running a shell command. |
| [tests/features.pl](tests/features.pl) | vi key bindings, a settings-menu round trip, and the diff/code protection and paragraph boundaries of the formatter. |

Each suite prints TAP-style `ok`/`not ok` lines and a final `pass`/`fail`
summary, and exits non-zero if anything failed.
[tests/mkterminfo.pl](tests/mkterminfo.pl) is a standalone helper that
builds a minimal terminfo entry; the current suites do not invoke it.

Environment variables:

| Variable | Meaning |
| --- | --- |
| `EE` | Binary under test (default: an absolute path `$ROOT/ee/ee`, where `$ROOT` is the repository root derived from the test script's location). |
| `TERM` | Terminal type used for the PTY sessions (default `xterm`). |

The generated `test-spell` and `test-dictionary` binaries are removed by
`make -C ee clean`.

See [../README.md](../README.md) for the pledge profile, the locale
policy and the known differences from upstream.
