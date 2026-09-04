# HS Offline Tracker

HS Offline Tracker is a desktop loot journal, alert engine and compact overlay for Hero Siege offline play. It consumes a small, versioned local event stream and turns new events into session totals, run history, configurable drop sounds and a priority-aware notification stack.

This project does **not** capture network traffic and does not require Npcap, libpcap or a packet-capture driver. It is also not an anti-cheat bypass, injector or online-play tool. Use it only with offline/single-player sessions.

## How it works

```text
compatible local event producer             local character saves
            |                                         |
            | NDJSON protocol v1                      | read-only stable snapshots
            +------------------------------+          |
            |                              |          |
            v                              v          v
events.ndjson                 \\.\pipe\HSOfflineTrackerBridge_<pid>
            |                              |          |
            +---------------+--------------+----------+
                            v
                  normalised game events -> statistics/filter engine
            |
            +-> dashboard and compact overlay
            +-> run history and export cards
            +-> sounds and compact drop notifications
```

The repository contains the tracker/consumer and a separately gated native
drop-sensor prototype. A compatible producer may append newline-delimited events
described in [PROTOCOL.md](PROTOCOL.md), or on Windows serve the same records
through the per-process local named pipe. The event file can be moved by setting
`HS_OFFLINE_TRACKER_EVENTS` to an absolute path before starting the tracker.

The native prototype is deliberately not auto-loaded and is not part of the
end-user installer. Its fingerprint, route and ABI gates are documented in
[bridge-native/README.md](bridge-native/README.md). Until that component has a
reviewed offline host and a real in-game observation pass, the packaged tracker
should be described as a local-event consumer rather than a complete live sensor.

The reader starts at the file's current end when the application opens. Existing lines are not replayed; only newly appended, newline-terminated records enter the current session. Truncating or rotating the file is supported.

The packaged tracker also watches local character saves without modifying them.
It prefers `%LOCALAPPDATA%\Hero_Siege\hs2saves\herosiegeN.hss` and falls back to
the legacy parent folder only when the nested folder has no character saves. It
does not guess the active slot: after the game starts, only a character file the
game changes can become active. Every changed file must produce two identical
read-only snapshots and pass strict Base64, zlib, XOR and character-field
validation before it is accepted; an incomplete write is ignored while the last
good snapshot remains authoritative.

Save snapshots provide delayed character identity, level, hero level,
experience, difficulty, coarse act, total kills, boss counters and chest
counters. They do **not** contain season/Blood Pact mode, the exact room, Magic
Find, mail, current health/mana, an authoritative gold-gain event, or enough
provenance to classify an inventory change as a drop. A save snapshot therefore
does not select a currency purse and never fabricates room, MF, mail,
currency-gain, pickup or drop events. Instant loot alerts still require a
compatible local event producer.

## Features

- Offline session totals for gold, experience, kills and tracked resources
- Rarity journal and configurable drop alerts
- Always-on-top compact overlay plus full dashboard
- Run history, shopping list and shareable run cards
- Three-card, priority-aware drop notifications with burst grouping
- Optional custom sounds; bundled defaults use project-generated rarity alerts and documented CC0 utility alerts with auditable provenance
- Original obsidian-style visual layer with no copied game sprites
- Versioned and size-limited local event input
- Read-only, fail-closed local save progress snapshots with 4 MiB raw and decompressed limits

Season 10 item and room metadata checked into this repository is maintained from
verified tuples and translation data. The project intentionally has no automatic
game-data extraction command: ambiguous IDs stay unchanged until a real event or
save tuple proves the mapping.

## Live sensor

Instant gold, XP, kills, rare drops, the current room and the satanic zone with
its modifiers come from the Aurie producer module
(`aurie-producer`). Open **Settings > Game link** and press **Install live
sensor**: the tracker copies `HSOfflineTrackerProducer.dll` into the game's
`mods/aurie` folder (the Aurie/YYToolkit loader must already be there). The
sensor resolves the game's routines by name, so it keeps working after game
updates; the status line on that page says whether the game is seen, the
sensor is installed and events are flowing. Live magic find is not read yet:
the game's stat routines cannot be hooked safely on the current build, so the
panel leaves it out.

The dashboard's right column shows the zone, the satanic zone's pros and cons
(names and wording from the game's own translation table; buff magnitudes as
read from the game, debuffs without magnitudes), and the items the game ties to
the current area with their odds.

## Development

Requirements:

- Node.js 20.19+ or 22.12+ (matching Vite 8's supported release lines)
- Rust 1.88 or newer through rustup
- Tauri 2 desktop prerequisites
- On Windows: WebView2 and the Visual Studio C++ build tools

Cargo writes its build output outside the checkout (see `src-tauri/.cargo/config.toml`),
so a checkout inside OneDrive is not synced gigabyte by gigabyte. Build the live sensor with
`powershell -File aurie-producer/build.ps1` (Visual Studio C++ and CMake); the packaged
installer bundles the DLL it finds in `aurie-producer/build/bin/Release`.

Install dependencies and build the web interface:

```powershell
npm ci
npm run build
```

Run the desktop application in development:

```powershell
npm start
```

With the tracker already open, replay the deterministic local protocol fixture for UI verification:

```powershell
npm run demo
```

Set `HS_OFFLINE_TRACKER_REPLAY_DELAY` (milliseconds) to change the delay between fixture events. The demo writer uses the same `HS_OFFLINE_TRACKER_EVENTS` override as the application and performs no game, memory or network access.

Run the Rust test suite:

```powershell
npm test
```

Run the complete local check or create a Tauri package build:

```powershell
npm run check
npm run package
```

`npm run build` is the portable frontend validation step. `npm test` requires the Rust/Windows native toolchain described above.

## Privacy and safety boundaries

- Event ingestion uses a local file or loopback-only per-process named pipe; no packet capture is performed.
- Character saves are opened read-only. Failed, changing or malformed snapshots are discarded and never replaced by zero/default progress.
- The tracker does not automatically upload event, character or drop data.
- The About page makes a GitHub request only when the user presses its update-check button and only when a project release repository is configured.
- Input lines are treated as untrusted data. Malformed, unknown, oversized or unsupported-version records are ignored and logged.
- A producer should emit events only for offline play. Process detection controls the displayed run state; the documented local protocol remains the trust boundary.

## Licensing and attribution

The application is distributed under the MIT license in [LICENSE](LICENSE). It contains MIT-licensed work derived from HS Tracker at upstream commit `26b0fc5d4ec36fb8399d47c5a33aaba53cdd15c4`. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the exact scope and exclusions.

Hero Siege is a trademark of its respective owner. This project is independent, unofficial and not endorsed by Panic Art Studios.
