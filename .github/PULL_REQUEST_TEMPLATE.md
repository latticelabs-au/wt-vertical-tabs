## Type of change

- [ ] `feat`: new feature
- [ ] `fix`: bug fix
- [ ] `refactor`: code change with no behaviour difference
- [ ] `perf`: performance improvement
- [ ] `docs`: documentation only
- [ ] `test`: test harness or test scripts
- [ ] `build`: build tooling change
- [ ] `ci`: CI pipeline change
- [ ] `chore`: maintenance (version bump, cleanup)
- [ ] `security`: security fix

## Description

<!-- What does this PR do and why? -->

## How it was verified

<!-- Terminal version, Windhawk version, what you did, what you saw. Screenshots for layout changes. -->

## Pre-merge checklist

- [ ] **Compiles.** `python tools/build.py --windhawk <path>` succeeds for x86-64 and ARM64.
- [ ] **Verified against a real Terminal**, through a portable Windhawk scoped to a test Terminal,
      covering the relevant items of the checklist in CONTRIBUTING.md.
- [ ] **Restores cleanly.** Anything the change sets on Terminal's XAML is recorded and put back
      when the layout is removed and when the mod unloads.
- [ ] **No em dash in Markdown or docs.** The character U+2014 does not appear in any `*.md` or
      `docs/**` file touched by this PR. CI ("prose" job) enforces this automatically. Source code
      and commit messages are reviewer-enforced, not CI-enforced.
      Check with `git grep -n -P '\x{2014}'` if unsure.
- [ ] **Mod version bumped** in the `==WindhawkMod==` block, if the mod itself changed.
- [ ] **README updated** (if this change affects user-visible behaviour or setup steps).
- [ ] **CHANGELOG updated** under `[Unreleased]` with the appropriate subsection
      (`Added`, `Changed`, `Fixed`, `Removed`, `Security`).
