# Phase 2: privacy (draft)

Goal: the reader only talks to my own network and to hosts I choose.
Everything else is removed from the UI and blocked in code.

Status: draft. No code yet.

## Approach

Two layers:

1. **Hide features** that talk to outside servers. Done with our own build flag,
   `WADDLE_PRIVATE`, so the upstream code stays and CrossPoint updates still merge.
2. **Block at the network choke point.** All outbound HTTP goes through two
   places: `HttpDownloader` (`src/network/HttpDownloader.cpp`) and
   `freeink::SecureHttpClient` (FreeInk SDK). A host allowlist there catches
   anything the UI change misses, including SD card plugins that fetch URLs.

Layer 2 is the real guarantee. Layer 1 keeps the UI clean.

## New build target

`[env:x4pro-waddle]` in `platformio.ini`: a copy of `x4pro` plus
`-DWADDLE_PRIVATE=1` and a `waddle` version string. A matching
`x4pro-waddle-gh_release` for GitHub release builds.

## Feature decisions

| Feature | Talks to | Code | Decision |
|---|---|---|---|
| Check for updates (OTA) | `api.github.com` (CrossPoint) | `src/network/OtaUpdater.cpp:22` | **Keep**, point at `ArneeMe/ebook-os` releases |
| KOReader sync | `sync.crosspointreader.com`, `sync.koreader.rocks` | `lib/KOReaderSync/` | Hide |
| Clipping sync | Same sync server | `src/clippings/ClippingSync.cpp` | Hide |
| Plugin Hub install | `pluginhub.crosspointreader.com` | `src/activities/settings/PluginHubInstallActivity.cpp` | Hide |
| Font download | `github.com` | `src/activities/settings/FontDownloadActivity.cpp:178` | Hide. SD card fonts still work |
| LCP enroll and unlock (DRM books) | `lcp.freeink.org` | `src/network/LcpDevice.cpp:141`, `src/activities/browser/OpdsBookBrowserActivity.cpp:658` | Hide. DRM books will not open |
| Background Wi-Fi join at sleep | Any plugin event target | `src/main.cpp:403-410` | **Remove.** It joins Wi-Fi without me opening anything |
| Web server (book upload) | Local network only | `src/activities/network/CrossPointWebServerActivity.cpp` | Keep |
| Hotspot mode | Local only | same file, `:243` | Keep |
| Calibre wireless | Local network only | `src/activities/network/CalibreConnectActivity.cpp` | Keep |
| mDNS (`crosspoint.local`) | Local network only | `CrossPointWebServerActivity.cpp:48` | Keep, rename to `waddle.local` later |
| OPDS browser | Servers I add | `src/activities/browser/OpdsBookBrowserActivity.cpp` | Keep, limited by the allowlist |
| SD card plugins | Whatever the plugin asks | `src/util/PluginHttp.cpp`, `/api/relay`, `/api/fetch` | Keep, limited by the allowlist |
| Bluetooth page turner | Local radio only | `lib/BlePageTurner` | **Open question** |

## Allowlist

Compile-time list in phase 1. Allowed:

- Private network addresses (`192.168.x.x`, `10.x.x.x`, `172.16-31.x.x`) and `*.local`
- `api.github.com` and the GitHub release download hosts, only for the OTA update
- Later: `arnee.me`

Everything else fails with a clear error on screen.

## Wi-Fi only on demand

Mostly true already: each network screen turns Wi-Fi on itself, and Wi-Fi is
turned off before deep sleep (`src/main.cpp:494-498`). With the sleep-time join
removed, the remaining work is checking that every network screen turns Wi-Fi
off when you leave it.

## How to verify

1. **Code check:** search for every outbound call site and confirm each goes
   through the allowlist.
2. **Unit test:** a host test for the allowlist matcher (the `test/` folder has
   the pattern).
3. **On device:** open each hidden feature's old menu path and confirm it is gone.
   Try a blocked URL from an OPDS server entry and confirm it fails.
4. **Traffic test:** put the reader on a separate network for a day. The router
   log or Pi-hole should show only allowed hosts.

## Known gaps

- The FreeInk SDK (`freeink-sdk/`) is a git submodule and is not checked out
  here yet, so its network code is not audited. `SecureHttpClient` lives there.
  Adding the allowlist inside it means patching the SDK. The other option is to
  check before each call to it (about 5 call sites).
- Espressif's Wi-Fi and Bluetooth drivers are closed binaries. They cannot be
  audited. The traffic test covers this.
- HTTP requests send `User-Agent: CrossPoint-ESP32-<version>`
  (`src/network/HttpDownloader.cpp:33`). Change it to `WaddleOS` or something neutral.

## Open questions

1. Bluetooth page turner: keep or remove?
2. Allowlist in the SDK (one place, but a patched submodule) or before each call
   (no SDK patch, more places to keep in sync)?
3. OPDS: should any public catalogs (Standard Ebooks, Gutenberg) be on the allowlist?
