# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.1] - 2026-09-29

### Fixed

- Collapsing or expanding the sidebar now affects only the window whose button was clicked, not
  every Terminal window. New windows, and every window after a restart, start collapsed or
  expanded the way the last one was left.

## [1.0.0] - 2026-09-28

### Added

- Vertical tab sidebar for Windows Terminal, as a Windhawk mod. The tab strip moves into a sidebar,
  each tab becomes a full-width row with its icon, title and close button, and the new-tab dropdown
  opens upwards from the foot of the sidebar.
- "Turn on vertical tabs" and "Turn off vertical tabs" in every tab's context menu. Horizontal by
  default; the choice persists across restarts.
- Collapse and expand button at the top of the sidebar: a 48 px icon rail with titles as tooltips.
  Persists across restarts.
- Settings for the sidebar width (120 to 600 px) and position (left or right).
- Live apply and exact restore on toggle, collapse, settings change and mod disable.
- `tools/build.py`, which compiles the mod with Windhawk's own toolchain and can install it into a
  portable Windhawk, and `tools/test-harness.wh.cpp`, which keeps a test Terminal on its own virtual
  desktop and unable to take focus.

---

[Unreleased]: https://github.com/latticelabs-au/wt-vertical-tabs/compare/v1.0.1...HEAD
[1.0.1]: https://github.com/latticelabs-au/wt-vertical-tabs/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/latticelabs-au/wt-vertical-tabs/releases/tag/v1.0.0
