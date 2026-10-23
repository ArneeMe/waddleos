# Working in the WaddleOS fork

Read this before changing anything. It is written for AI agents and for me.

This repo (`ArneeMe/waddleos`) is a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
Upstream is very active: about 400 commits from late July to late October 2026,
several per day. Every change we make must survive regular merges from upstream.

## Branches and remotes

| Name | What it is |
|---|---|
| `origin` | `github.com/ArneeMe/waddleos` (ours) |
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
| `src/components/UITheme.cpp` | `#if WADDLE` include and a block at the top of `setTheme()` | Waddle theme replaces Lyra |
| `platformio.ini` | `[env:x4pro-waddle]` appended at the end | Our build target |
| `.github/workflows/*.yml` (all upstream ones) | Deleted | We only build the X4 Pro with `waddle-build.yml`. On a merge conflict, keep them deleted (`git rm`) |
| `.github/FUNDING.yml`, `ISSUE_TEMPLATE/`, `PULL_REQUEST_TEMPLATE.md` | Deleted | Upstream project files. Keep them deleted on a conflict |
| `src/network/CrossPointWebServer.cpp` | `#if WADDLE` include and `registerRoutes()` call after the Wi-Fi routes | `/api/firmware/apply` |
| `src/activities/network/CrossPointWebServerActivity.cpp` | `#if WADDLE` includes and a block after the `handleClient()` loop | Opens the SD update for a queued `.bin` |
| `src/activities/settings/SdFirmwareUpdateActivity.*` | `#if WADDLE` constructor with a preset path, checks in `onEnter()` and `loop()` | Skip the file picker when started from the API |
| `src/activities/boot_sleep/BootActivity.cpp`, `SleepActivity.cpp` | `#if WADDLE` swaps the logo block for `waddle::drawSplash()` | Penguin boot and sleep screens. The default sleep screen is not inverted |
| `src/activities/home/HomeActivity.cpp` | `#if WADDLE` include, one more menu row ("Node") in count, render and selection | Bluesound remote (`src/waddle/NodeActivity.*`) |
| `src/activities/network/WifiSelectionActivity.cpp` | `#if WADDLE_PRIVATE` neutral hostname, no NTP on join | Privacy |
| `src/util/PluginEvents.cpp` | `#if WADDLE_PRIVATE` early return in `emit()` and `drain()` | No reading activity sent to plugins |
| `src/activities/reader/EpubReaderActivity.cpp`, `EpubReaderMenuActivity.cpp` | `#if WADDLE_PRIVATE` no KOReader sync, menu item hidden | No progress upload |
| `src/main.cpp` | `#if WADDLE_PRIVATE` early return before the sleep-time Wi-Fi join | Privacy |
| `src/network/HttpDownloader.cpp`, `src/util/PluginHttp.cpp` | `#if WADDLE_PRIVATE` neutral user agent | Privacy |
| `src/activities/settings/SettingsActivity.cpp` | `#if !WADDLE_PRIVATE` around KOReader sync and Check for updates | Privacy |
| `src/network/CrossPointWebServer.cpp` | also `#if !WADDLE_PRIVATE` around `enableCORS` | Privacy |
| `src/activities/network/CrossPointWebServerActivity.cpp` | also `#if WADDLE_PRIVATE` hotspot name, password, QR code | Privacy |

## Upstream AI rules

Upstream's `CLAUDE.md` and `.agents/` still apply for code quality (memory,
allocation, input, rendering, comment style). These parts are replaced in this fork:

| Upstream rule | In this fork |
|---|---|
| "Integration work targets `develop`" | Branch from `waddle`, open a PR into `waddle` |
| "Never open or close a PR" | Agents may open PRs, but **only in `ArneeMe/waddleos`**. Never open anything in `crosspoint-reader/crosspoint-reader` |
| "The human must write PR descriptions" | Agents may write short, factual PR descriptions |
| Ask before every commit and push | Commit and push to feature branches without asking. Never push to `develop`, never force-push `waddle` |
| Four-review `firmware-handoff` | Optional. The PR build is the required check. Hardware testing is still on me |

The local `upstream` remote has its push URL disabled on purpose.

## Building

This cloud session cannot download the ESP32 toolchain, so builds run in
GitHub Actions (`.github/workflows/waddle-build.yml`) or on Arch with
`pio run -e x4pro-waddle`. Each push to `waddle` publishes
`waddleos-x4pro.bin` to the `waddle-latest` pre-release.

## Hardware testing

Only I can flash and look at the reader. Every firmware change needs a
"how to verify on the device" note.
