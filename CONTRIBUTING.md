# Contributing

wt-vertical-tabs is a Windhawk mod for Windows Terminal. You need **Windows 11 (Windows 10 is
untested), Windows Terminal 1.24 and Windhawk 1.7 or later** to build it meaningfully and test it.
There is no cross-platform build path: the mod runs inside Windows Terminal and is compiled with
Windhawk's Windows toolchain.

## Prerequisites

| Requirement | Detail |
|---|---|
| OS | Windows 11, x64 (Windows 10 untested; ARM64 is compiled in CI, not tested on a device) |
| Windows Terminal | 1.24; a portable copy from the [Terminal releases](https://github.com/microsoft/terminal/releases) is best for testing |
| Windhawk | 1.7 or later; a portable copy scoped to the test Terminal is best for testing |
| Python | 3.12 or later, for `tools/build.py` |

Test against a portable Terminal and a portable Windhawk whose engine is scoped to it (in the
Windhawk engine's `settings.ini`, `Exclude=*` and `Include=<path of the test WindowsTerminal.exe>`),
never against the Terminal you work in. A mod that throws inside a layout pass takes every tab in
that Terminal process down with it.

## Build

```
python tools/build.py --windhawk <path to Windhawk>
```

This compiles the mod for every architecture it declares, with the same clang invocation the
Windhawk editor uses, into a temporary folder. `--install --include <test Terminal path>` also
loads it into a portable Windhawk. `--define WTVT_TEST_HOOKS` adds the test hooks described in the
README.

## PR gates (all must pass)

All CI jobs run on `windows-latest`.

1. **Compile.** The mod compiles for x86-64 and ARM64, the test-hook build compiles, and the test
   harness compiles, all with Windhawk's own toolchain.
2. **Prose.** The `prose` job greps for U+2014 in `*.md` and `docs/**`. Source code and commit
   messages are reviewer-enforced, not CI-enforced.
3. **Verified against a real Terminal.** Not automated. Describe in the pull request what you ran
   and what you saw; see the checklist at the end of this file.

## Commit convention

This project uses [Conventional Commits](https://www.conventionalcommits.org/).

```
type(scope): lowercase imperative summary
```

- **Type** (required, lowercase): `feat`, `fix`, `docs`, `refactor`, `perf`, `test`,
  `build`, `ci`, `chore`, `security`.
- **Scope** (optional, rare): one subsystem from the fixed list: `layout`, `tabs`, `menu`,
  `diag`, `settings`, `harness`, `tools`, `ci`, `release`.
- **Summary**: lowercase, imperative, no trailing period, 72-character hard limit.
- **Body**: wrap at 72 columns. State what was tested and why the decision was made. Close
  with a `Verified:` line describing the evidence.
- **Breaking changes**: `type!: summary` plus a `BREAKING CHANGE:` footer paragraph.

Examples:

```
fix(layout): restore the title bar host before re-applying
feat(menu): add the toggle to torn-out windows
docs: note the elevated-process caveat
```

## Writing style for docs and Markdown

- Australian spelling in all prose: behaviour, colour, initialise, centre, licence (noun).
  Code identifiers and Windows or WinUI API names stay US-spelled (`Color`, `Initialize`).
- Sentence case headings. Second person, present tense.
- **The em dash character U+2014 is forbidden in all tracked files** (house rule).
  CI enforces this in `*.md` and `docs/**` via the `prose` job. Source code and commit
  messages are reviewer-enforced. If a sentence seems to need one, restructure it with
  a colon, commas, or parentheses.
- No en dash in prose. En dash is permitted only inside numeric ranges in tables.
- No marketing superlatives. Be frank about failure modes.

## Manual verification checklist (required before release, not automated)

Against a portable Terminal 1.24 with the mod loaded through a scoped portable Windhawk:

- [ ] Toggle on and off from the tab context menu; the horizontal strip afterwards is
      pixel-identical to a fresh start
- [ ] Collapse and expand; the new-tab dropdown arrow comes back after expanding
- [ ] Toggle state and collapsed state survive a restart
- [ ] "Show tabs in title bar" on and off, light and dark theme
- [ ] Focus mode and full screen hide the sidebar; with "Always show tabs" off, one tab hides it
- [ ] A second window, and a tab moved to a new window ("Move tab to new window"), both come up
      vertical
- [ ] With a real mouse: drag a tab to reorder it in the sidebar, and drag one out into a new
      window
- [ ] Settings change (width, position) applies live
- [ ] Disabling the mod in Windhawk restores every window and Terminal keeps running
- [ ] 100 tabs: live apply stays under 250 ms (see `logs` line "Vertical tabs applied to N tabs")
