# wt-vertical-tabs design

Date: 2026-09-28. Status: shipped as 1.0.0; per-window collapse in 1.0.1.

## Problem

Windows Terminal has one tab layout: a horizontal strip across the top. With more than a handful
of tabs the titles shrink to nothing, and the request for vertical tabs
([microsoft/terminal#835](https://github.com/microsoft/terminal/issues/835)) has been open since
2019. Microsoft's experimental [Intelligent Terminal](https://github.com/microsoft/intelligent-terminal)
fork has been adding vertical tabs, but it is a separate application, not the Terminal people
already use.

## Goals

- Browser-style vertical tabs in stock Windows Terminal 1.24, turned on and off from the tab
  context menu the way Edge does it, horizontal by default, the choice remembered.
- A collapsed icon rail, toggled from a button at the top of the sidebar, for that window only.
  New windows, and every window after a restart, start the way the last one was left.
- Everything Terminal already does with tabs keeps working.
- Fully reversible, live: toggling, settings changes and disabling the mod never need a restart,
  and turning it off restores the stock layout exactly.
- Never crash the Terminal it runs in.

## Non-goals

- A resizable sidebar (the width is a setting).
- Tab groups, search, pinning, or anything else browsers layer on top of vertical tabs.
- Supporting Terminal builds whose tab strip is not WinUI 2.8's `TabView`: those keep their tabs.

## Approaches considered

| Approach | Why not |
|---|---|
| Fork and rebuild Terminal | Not a mod: users would have to replace their Terminal, and every Terminal release would need a rebase. |
| A separate sidebar window docked beside Terminal, driven by UI Automation | Two windows pretending to be one: focus, z-order, snapping and DPI all fight it, and Terminal's own tab strip stays on screen. |
| Replace the `TabView` template with a vertical one | The tab list inside the template owns the tab items; after a template swap, handing the live items to the new list fails with `E_UNEXPECTED` and the tabs vanish. |
| **Rearrange the stock template in place** (chosen) | No items move. Rows and columns, panel orientation and scroll settings change on the existing instances, and every change is recorded for restore (tab templates are reset to stock instead). |

The tab template (one `TabViewItem`) is replaced, because a tab has no live children to move: its
header and icon are detached from the old template's presenters first, so the new template can
adopt them.

## How the mod reaches Terminal's tabs

Windhawk loads the mod into `WindowsTerminal.exe`. The mod calls `InitializeXamlDiagnosticsEx` with
its own DLL as the tool attach point, gets `IXamlDiagnostics`, and advises a visual tree watcher,
which reports every element entering the tree, including the ones already there. It acts on
`TerminalApp.TabRowControl`.

Findings that shaped this part:

- **Advised diagnostics leak.** With the watcher advised, Terminal's private memory grows by about
  100 KB per tab opened and closed, round after round, even with every reported handle released
  through `IXamlDiagnosticsTestHooks::UnregisterInstance`. So the watcher is advised in bursts: at
  start-up and when a Terminal window is created, detached five seconds after the last tab row
  sighting.
- **Initialised diagnostics cost something too.** Detached, most of that growth goes away, but not
  all: once `InitializeXamlDiagnosticsEx` has run, the process keeps about 35 KB more per tab opened
  than stock Terminal. A build that initialises diagnostics but never advises measures the same as
  the real mod, and so does one without the per-tab menu entry; a build that never initialises them
  measures the same as stock. There is no call to undo the initialisation. Finding the tab row
  without XAML Diagnostics (symbol hooks on `Windows.UI.Xaml.dll`, or on the island's
  `DesktopWindowXamlSource`) would avoid it.
- **New tabs without diagnostics.** `TabView.TabItemsChanged` fires on insertion, before the tab's
  first layout, which is early enough to give it the vertical template and the menu entry.
- **When to attach.** A zero-delay thread timer set when `CASCADIA_HOSTING_WINDOW_CLASS` is created
  fires after that window's XAML island exists, because Terminal builds the island on the same
  thread before it next pumps messages. XAML's own content bridge window is not created through the
  `CreateWindowExW` export, so it cannot be the trigger.
- **Advise off the UI thread.** Advising from the thread that runs `SetSite` can deadlock, so
  advise and unadvise run on short worker threads, serialised, and uninit waits for them.

## Making it vertical

1. The tab row leaves the title bar (when "Show tabs in title bar" is on) and joins the page's root
   grid, spanning all rows, left or right aligned at the sidebar width. The terminal area and the
   info bars get a matching margin, which follows the `TabView`'s visibility so focus mode, full
   screen and "one tab, always show tabs off" hide the sidebar and give the width back.
2. The `TabView` template's outer rows are swapped so the strip fills the height. `TabContainerGrid`
   loses its four column definitions and gains five rows: collapse button, header (the elevation
   shield), the tab list, the footer (Terminal's new-tab split button), and the rest of the height.
   The list's row sizes to its content, so the footer follows the last tab; `FitTabListHeight`
   caps the list's `MaxHeight` at what the other rows leave, recomputed when the strip resizes, so a
   long list scrolls with the footer still in view.
3. The list's `ItemsStackPanel` turns vertical and its scroll viewer switches horizontal scrolling
   off and vertical scrolling on.
4. Each tab gets the row template, first tried synchronously on one tab.

### Peeking

A collapsed rail opens to full width while the mouse rests on it and closes when the mouse leaves,
as in Edge. Only the look changes (`SetPeek`): the tab row widens, the terminal and the info bars get
`Canvas.ZIndex` -1 so the sidebar draws over them while overlays such as the command palette stay on
top, and an opaque backdrop the mod inserts at the back of `TabContainerGrid` hides the terminal
underneath. The terminal keeps the rail's margin (`DockedWidth`), so it never reflows. Nothing is
removed from the tree, so the pointer events that drive it don't restart.

Pointer events alone can't be trusted to say where the mouse is: `PointerEntered` and
`PointerExited` bubble up from every child the pointer crosses, and a fast exit off the window's edge
can arrive with the last position still inside, or not at all. So events only schedule a check, and
the check reads the cursor (`GetCursorPos`, the window under it, that window's XAML island, the tab
row's bounds). While open, it polls every 150 ms and closes 300 ms after the cursor is first seen
outside, but not while a popup is open or a tab is being dragged. Measured: opening or closing with
100 tabs takes about 50 ms. Touch and pen are ignored: a tap would open a rail nothing would close.

### The width fight

`TabView::UpdateTabWidths` gives every tab a fixed width and turns horizontal scrolling on whenever
tabs "do not fit" the width left after the header, add button and footer. With horizontal scrolling
on, the list measures tabs at infinite width: long titles push the close button out of view.

`UpdateTabWidths` skips all of that when no width is left over, so the footer gets a minimum width of
the whole sidebar. But `TabView::MeasureOverride` calls `UpdateTabWidths` before re-measuring its
children, so on the first pass it still sees the footer's old horizontal size. The fix is to call
`Measure` on the footer as soon as its minimum width is set. As a safety net, a `SizeChanged` handler
on the items presenter puts horizontal scrolling back off (rate limited, giving up after 20 attempts
in two seconds). A `RegisterPropertyChangedCallback` on the scroll viewer's attached property was
tried first and never fired.

### Theme resources

`TabViewCloseButtonStyle` lives in WinUI's control dictionary, which templates loaded at runtime
cannot see (`802B000A`, "Cannot find a Resource"). The close button's look is inlined from the
theme brushes it uses, which are in the public theme dictionaries.

## Restoring

`SetValueSaved` records each property's previous local value the first time the mod sets it;
`RestoreSavedValues` puts them back in reverse order (or clears the property if it had no local
value). Three lessons:

- Row and column definitions and flyouts must be held strongly. A weak reference to them comes back
  empty once XAML drops the object behind it, and the restore is silently skipped (the new-tab
  dropdown arrow stayed hidden after collapsing and expanding, before this).
- A property that holds a `TemplateBinding` loses the binding when restored by clearing. Change it at
  the source instead (the list's padding is template-bound to `TabView.Padding`, so the mod sets
  `TabView.Padding`).
- After the layout is removed, the tab row is the title bar presenter's content but has no visual
  parent until the next layout pass, so re-applying in the same pass has to recognise the known
  presenter by its content.

After toggling off, the tab strip is pixel-identical to a fresh start (0 of 50,805 pixels differ in
the strip region).

## Measurements

On Windows 11 24H2 (26100), Terminal 1.24.11911.0, portable:

| What | Result |
|---|---|
| Apply at start-up or on toggle, 1 to 5 tabs | 10 to 33 ms |
| Live apply (mod enabled under an open window), 69 to 100 tabs | 42 to 181 ms over three runs (budget 250 ms) |
| Live apply to 100 open tabs, memory, settled | +10.5 MB |
| Memory kept per tab opened and closed, fresh Terminal, rounds 2 to 4 of 120 tabs, mod loaded (vertical or horizontal) | 37 to 55 KB |
| Same, stock Terminal (mod disabled) | 2 to 18 KB |
| Same, a build that never initialises XAML Diagnostics | 6 to 12 KB |
| Same, a build that initialises them but never advises | 39 to 48 KB |
| Same, a build without the per-tab menu entry | 40 to 49 KB |
| Same, diagnostics kept advised | about 100 KB, every round |

Memory is read settled: the minimum of three samples taken after 12 seconds. Terminal's private
bytes keep climbing for about 20 seconds after many shells start (911 MB to 1752 MB for 99
PowerShell tabs), and an early stress run that read memory during that climb reported a 288 MB
"leak" that was only shells starting.

## Testing without disturbing the user

Terminal calls `SetForegroundWindow` on start and on every command line it receives. Once the user
has been idle past the foreground lock timeout, Windows grants it, so a test Terminal steals
keystrokes, and a test Terminal on another virtual desktop pulls the user onto that desktop.
Launching through Task Scheduler (no foreground rights) is not enough for the same reason. The fix
is in-process: `tools/test-harness.wh.cpp` hooks `SetForegroundWindow`, `ShowWindow`,
`SetWindowPos`, `BringWindowToTop` and `SwitchToThisWindow`, adds `WS_EX_NOACTIVATE` to Terminal's
windows, and moves them to the "WT test" virtual desktop with `IVirtualDesktopManager` before they
are first shown. The test Terminal is started suspended and resumed once Windhawk has queued its
injection, so the harness is loaded before any window exists.

XAML's right-tap cannot be driven by posted mouse messages (real clicks arrive as `WM_POINTER`), and
UI Automation's `ShowContextMenu` does not open `TabViewItem.ContextFlyout`. Test builds therefore
expose `WTVT_TEST_OPEN_TAB_MENU`, which opens a tab's context menu with `ShowAt`. The real right-click
path was verified by hand with an input watcher in the harness logging the pointer messages and the
flyout's `Opening` and `Opened` events.

## Future work

- A resizable sidebar (drag handle), with the width persisted like the toggles.
- Ellipsised titles, which needs Terminal's header to stop using a horizontal stack.
- Up and Down arrow navigation between tabs, which `TabView` cancels in `OnListViewGettingFocus`.
- Submission to [ramensoftware/windhawk-mods](https://github.com/ramensoftware/windhawk-mods).
