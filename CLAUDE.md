# wt-vertical-tabs: agent guidance

Repository: `latticelabs-au/wt-vertical-tabs` (public, MIT, Copyright (c) 2026 Lattice Labs)

wt-vertical-tabs is a Windhawk mod that gives Windows Terminal browser-style vertical tabs. It runs
inside `WindowsTerminal.exe`, finds each window's tab strip through XAML Diagnostics, and
rearranges Terminal's WinUI 2 `TabView` into a sidebar in memory. It is toggled from each tab's
context menu and can collapse to an icon rail. Nothing on disk is patched.

The design of record is `docs/specs/2026-09-28-wt-vertical-tabs-design.md`. Where that spec and this
file disagree, the spec wins on behaviour and this file wins on process.

---

## Repository layout

```
windows-terminal-vertical-tabs.wh.cpp   the mod: metadata, readme, settings, all code
tools/build.py                          compile with Windhawk's toolchain; install into a portable Windhawk
tools/test-harness.wh.cpp               development-only mod that keeps a test Terminal off the desktop
docs/specs/                             design record
docs/images/                            README screenshots
.github/                                CI, release, issue forms, PR template, Dependabot
```

The mod is one file on purpose: that is the unit Windhawk distributes, and the unit a user pastes
into the Windhawk editor. Do not split it.

### Map of the mod file, top to bottom

1. `==WindhawkMod==`, `==WindhawkModReadme==`, `==WindhawkModSettings==` blocks.
2. Settings and global state (`g_vertical`, `g_collapsed` are the persisted toggles).
3. XAML strings: the vertical `ItemsPanelTemplate` and the tab (`TabViewItem`) template.
4. Visual tree helpers.
5. `WindowState` and `ThreadContext`, `SetValueSaved` / `RestoreSavedValues`.
6. Templates: loading, validation, the collapsed variant.
7. The context menu toggle.
8. The vertical strip: `ApplyVerticalStrip` / `RemoveVerticalStrip`, the new-tab button, the
   collapse button, `ReassertVerticalStrip`.
9. Layout: `ApplySidebarLayout`, `TrackWindow`, `ApplyLayout`, `RemoveLayout`, `UntrackWindow`,
   `RefreshCurrentThread`.
10. Deferred work (`RunPendingWork`, `QueueWork`, `RequestToggle`) and the `WTVT_TEST_HOOKS` seam.
11. XAML Diagnostics: TAP, `VisualTreeWatcher`, burst attach and detach, handle release queue.
12. Running code on a window's UI thread, per-thread teardown.
13. `CreateWindowExW` hook and the Windhawk entry points.

---

## Invariants (do not break these)

- **Nothing runs after unload.** Every event handler, property-changed callback and timer the mod
  registers is revoked on the UI thread that owns it, in `UninitializeCurrentThread`. Deferred work
  uses `DispatcherQueueTimer` (stoppable), never `DispatcherQueue.TryEnqueue` (not stoppable).
  Advise and unadvise workers are counted and awaited in `Wh_ModUninit`.
- **Everything is restorable.** Every property the mod sets goes through `SetValueSaved`, except
  tab templates, which go back to the stock template (`SetTabViewItemTemplate(tab, nullptr)`). Tree
  elements are held weakly; non-UIElement objects (row and column definitions, flyouts, menu items)
  strongly, because a weak reference to them comes back empty once XAML drops the wrapper. A value
  that is a `TemplateBinding` is changed at its source (for example `TabView.Padding`, not the
  list's `Padding`), so restoring it brings the binding back.
- **Diagnostics stay attached only in bursts.** While advised, the XAML runtime leaks about 100 KB
  per tab opened. The mod detaches five seconds after the last tab row sighting and re-attaches only
  when a new Terminal window is created. New tabs are followed through `TabView.TabItemsChanged`.
  Initialising XAML Diagnostics at all costs about 35 KB per tab opened, advised or not, so compare
  a per-tab memory change against a build that never calls `InjectTap` before calling it the mod's.
- **Measure memory settled.** Terminal's private bytes keep climbing for about 20 seconds after
  many shells start. Take the minimum of three samples after 12 seconds, never a single reading.
- **Unknown layouts are left alone.** If the `TabView` template parts are not where WinUI 2.8 puts
  them, the mod logs it and puts the tab row back where Terminal had it. If a tab template fails
  to instantiate, the tabs keep their stock look in the sidebar.
- **The template is rearranged in place, never replaced.** Swapping the `TabView` template moves live
  tab items between lists, which XAML rejects with `E_UNEXPECTED`.

---

## Testing

Never load a development build into the Terminal someone is working in. Test against a portable
Windows Terminal 1.24 and a portable Windhawk whose engine is scoped to that Terminal only
(`Exclude=*`, `Include=<path of the test WindowsTerminal.exe>` in the engine's `settings.ini`).

Install `tools/test-harness.wh.cpp` into the same portable Windhawk, scoped the same way. It creates
the test Terminal's windows on a virtual desktop named "WT test" and blocks every way the Terminal
could take focus. Without it, Windows Terminal brings itself to the foreground on start and on every
`wt.exe` command, and once the user has been idle past the foreground lock timeout, Windows lets it:
keystrokes land in the test Terminal, or Windows switches the user to the test desktop.

Drive the test Terminal with UI Automation (Invoke on buttons and menu items) and capture it with
`PrintWindow(PW_RENDERFULLCONTENT)`; both work on another virtual desktop. Posted mouse messages do
not drive XAML's right-tap reliably. In a `WTVT_TEST_HOOKS` build, post the registered message
`WTVT_TEST_OPEN_TAB_MENU` (wParam: tab index) to a Terminal window to open that tab's context menu.

Before a release, walk the manual checklist in `CONTRIBUTING.md`.

---

## House rules

- **No em dash (U+2014) in any tracked file.** Prose, comments, UI strings, commit messages. CI
  enforces it in `*.md` and `docs/**`; the rest is reviewer-enforced. Restructure the sentence.
- **Australian spelling in prose** (behaviour, colour, licence as a noun). Code identifiers and
  Windows or WinUI API names stay US-spelled.
- **Sentence case headings, second person, present tense.** Explain the why next to the what. Be
  frank about failure modes. No marketing superlatives.
- **Commits:** `type(scope): lowercase imperative summary`, max 72 characters, no trailing period.
  Body wrapped at 72 columns, closing with a `Verified:` line of concrete evidence. Scopes: `layout`,
  `tabs`, `menu`, `diag`, `settings`, `harness`, `tools`, `ci`, `release`.
- **No AI attribution** in commits: no `Co-Authored-By` trailer, no generated-with line.
- Bump `@version` in the mod's metadata and add a `CHANGELOG.md` section before tagging; the release
  workflow refuses a tag that disagrees with `@version`.
