# Working in the WaddleOS fork

Read this before changing anything. It is written for AI agents and for me.

This repo (`ArneeMe/ebook-os`) is a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
Upstream is very active: about 400 commits from late July to late October 2026,
several per day. Every change we make must survive regular merges from upstream.

## Branches and remotes

| Name | What it is |
|---|---|
| `origin` | `github.com/ArneeMe/ebook-os` (ours) |
| `upstream` | `github.com/crosspoint-reader/crosspoint-reader` |
| `develop` | Mirror of `upstream/develop`. **Never commit here.** |
| `waddle` | Our main branch. All our work goes here. |

Add the upstream remote in a fresh clone:

```bash
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git
```

Sync with upstream:

```bash
git fetch upstream develop
git checkout develop && git merge --ff-only upstream/develop && git push origin develop
git checkout waddle && git merge develop
```

Merge, do not rebase: `waddle` is pushed and shared.
After a merge, build `x4pro-waddle` and test on the device before pushing.

## Rules for changes

1. **New files over edits.** Put our code in new files: `src/waddle/`,
   `src/images/Waddle*.h`, `tools/`, `assets/waddle/`, `docs/waddle/`.
   Upstream never touches these, so they never conflict.
2. **Small hooks in upstream files.** When an upstream file must change, keep
   the edit to a few lines that call into our files. Mark it with a
   `// WADDLE:` comment so it is easy to find after a merge conflict.
3. **Use our build flags.** Gate behavior with `WADDLE_*` defines set only in
   our build target. Upstream builds stay unchanged, and our edits are clearly
   separated (`#if WADDLE_PRIVATE ... #endif`).
4. **Our build targets go at the end of `platformio.ini`**, as new
   `[env:x4pro-waddle...]` sections. Do not edit upstream sections.
5. **Branding through one define**, not translations. Do not edit
   `lib/I18n/translations/*.yaml` to rename CrossPoint. Those files change all
   the time and are generated into code.
6. **Hide features, do not delete them.** Deleting upstream code causes
   conflicts on every upstream change to that code.
7. **Record every upstream file we touch** in the table below.

## Hard to merge (avoid or keep edits tiny)

Most changed upstream files in the last 400 commits:

| File | Upstream commits | Why it is risky |
|---|---|---|
| `src/activities/reader/EpubReaderActivity.cpp` | 56 | Changes almost daily |
| `freeink-sdk` (submodule) | 52 | Bumped often. Never patch it in place |
| `lib/I18n/translations/english.yaml` (and the other languages) | 44 | Generated strings |
| `src/main.cpp` | 42 | Boot, sleep, Wi-Fi at sleep |
| `src/SettingsList.h`, `src/CrossPointSettings.*` | 34, 23 | Settings layout is saved to the SD card. Adding settings can break saved settings and conflict often |
| `platformio.ini` | 28 | Only append our own sections |
| `lib/Epub/**`, `lib/GfxRenderer/**` | 20 to 34 | Core rendering |
| `src/activities/settings/SettingsActivity.cpp` | 26 | Menus |
| `src/network/CrossPointWebServer.cpp` | 20 | Where `/api/firmware/apply` would hook in. Register our route from a new file |
| `src/components/themes/BaseTheme.*` | 19 | Prefer a new theme file over editing this |
| `.github/workflows/*` | 12 for `release.yml` | Prefer new workflow files named `waddle-*.yml` |
| `README.md`, `USER_GUIDE.md` | 12, 24 | Put our docs in `docs/waddle/` |

Calm files (safe to hook into): `src/activities/boot_sleep/BootActivity.cpp` (1),
`src/network/FirmwareFlasher.cpp` (3), `src/network/OtaUpdater.cpp` (5).
`src/activities/boot_sleep/SleepActivity.cpp` (17) is in between.

Also be careful with **saved data formats** (settings, caches, see
`docs/file-formats.md`). A change there can break the device after a merge, not
only the build.

## Upstream files we changed

Keep this table up to date. One row per upstream file.

| File | Change | Why |
|---|---|---|
| `AGENTS.md` (`CLAUDE.md` is a link to it) | One line at the top pointing to this file | So AI agents read this first |

## Upstream AI rules

Upstream's `CLAUDE.md` and `.agents/` still apply (build, memory, testing,
handoff checks). Where they talk about PRs to upstream, read them as commits to
`waddle`. Commits and pushes need my OK, but do not keep asking. I will say when.

## Hardware testing

Only I can flash and look at the reader. Every firmware change needs a
"how to verify on the device" note.
