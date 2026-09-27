# HS Offline Tracker Aurie producer

This directory contains a standalone tracker producer module. It is separate
from ForgePact/BloodPactPlugin and shares only HS Offline Tracker's local event
protocol and bounded named-pipe transport.

The module does not patch the executable on disk. Aurie loads it into the game
process; YYToolkit resolves named GML routines and Aurie's owned hooks observe
their calls. Every route is resolved by its GML routine name through YYToolkit, so the
producer keeps working across game updates. When the host executable matches
one of the reviewed exact profiles (PE machine, timestamp, `.text` size and
`.text` SHA-256) that profile's reviewed argument layout and caller allowlist
are used. Any other x64 Hero Siege build gets the **adaptive profile**: the
same routines by name, the earned-gold argument found at call time (the
positive integral argument, the second one on a tie), and the ground-item route
guarded by hooking the player-discard and API reconstruction routines plus a
1.5 s quiet window after every room change instead of caller RVAs. The build
fingerprint is still reported in `bridge_status` (`exact_build_ready` or
`adaptive_build_ready`) as a diagnostic. A route whose name no longer resolves
is dropped on its own; only the three counter routes are mandatory.

## Current counter contract

- `gml_Script_GoldLogAdd`: exact `argc == 2`; the earned-amount argument must
  be finite, integral and positive. That amount is argument 0 in the legacy
  reviewed S10 code profile and argument 1 in the current 2026-08-29 profile.
  The selection is bound to the exact `.text` fingerprint; a validated value
  becomes `session_delta.gold` only after the original routine returns.
- `gml_Script_ExperienceUpdate`: the same checks; argument 0 becomes
  `session_delta.xp` only after the original routine returns. Its reviewed
  rejection exit returns explicit `VALUE_BOOL false`; rejected calls are
  never added to XP.
- `gml_Script_EnemyAddStatistics`: exact `argc == 1`; argument 0 is a
  `killStatistic` ID/index and must be a safe non-negative integer (`0` is
  valid). Accepted calls add one to `session_delta.kills` after the original
  routine returns. The route was
  promoted only after a packaged live run showed candidate calls `96 -> 152`
  (`+56`) while the same character's persisted
  `statisticTotalMonsterKills` changed `45516 -> 45572` (`+56`) over the same
  save interval.
- `gml_Script_LootGroundInit`: the final ground-placement route. The current
  code profile accepts only its five reviewed local generation callers
  (`LootExplosion`, `LootGroundDrop`, `LootGroundCreate`,
  `LootGroundCreateFromItem` and `CreateItemDrop`). It explicitly excludes
  `CA_playerItemDrop`, both API/market reconstruction callers and
  `Zone_State_Buffer` restore. After the original routine completes, the
  `arg0` is the `Loot_Ground` instance; its finalized `itemInstance` supplies
  `itemInfoStruct[27]` (rarity), `itemType` and `itemDefinitionStruct.b`
  (item ID). A `ground_drop` is queued only for
  rarity IDs `4`, `6`, `7`, `9` and `10`. This route is enabled only for the
  exact current `S10-code-c4dc91d9` code profile; the older profile remains
  fail-closed for drops until separately reviewed.

Counter hooks only validate and update atomics. The ground-item hook copies three
validated integers into a fixed 2048-entry queue. A below-normal-priority private
producer worker wakes on a 16 ms cadence and handles at most eight drops per cycle;
it owns coalescing, JSON serialization and named-pipe queueing. The module does
not register a YYToolkit frame callback. The transport performs connection and
write I/O on its own bounded worker thread. No serialization, allocation, pipe
I/O or blocking lock runs inside a game hook. Release builds do not install a
Magic Find observer or collect per-event diagnostic/timing counters. A full
queue fails open to protect gameplay; the skipped count is written to
`%LOCALAPPDATA%\HS Offline Tracker\producer.log` when Aurie unloads the module.

- `gml_Script_RoomGoto`: after the original returns, argument 0 (the target
  room) becomes a `room` event via `room_get_name`. The same hook reads
  `global.satanicZoneBuff` / `global.satanicZoneDebuff` and asks the game's own
  `LoadSatanicZone(room)` for every candidate in `global.satanicZone`; the one
  room it answers yes for is emitted as `satanic_zone` (`Satanic_<act>_<zone>`)
  together with `vitals.satanic_here`. This is the one hook that allocates,
  because a room change is rare and the routine it wraps already does.
- Magic find: switched off (`kMagicFindRouteNamed.enabled = false`). StatMagicFind
  takes nine arguments and returns a carry array; which element the character
  sheet prints is unsettled, and the sheet's own dispatcher, ReturnSpecificStat,
  could not be hooked without crashing 7.0.6.0 at startup. The hook code stays in
  place for a future build.

## Shutdown

The module owns two threads, the publisher and the transport's pipe worker, and
they end in one of two ways:

- **The game exits.** `ExitProcess` terminates every other thread first and
  only then runs `DLL_PROCESS_DETACH`, where the CRT destroys the module's static
  objects; Aurie calls no unload routine at that point. A `std::thread` global
  is still joinable there, and destroying it calls `std::terminate`, so a game
  exit used to end in `ucrtbase!abort` (`0xc0000409`, fast-fail 7) and a Windows
  Error Reporting dump. The publisher is therefore an `ExitSafeThread`
  (`include/hsot_aurie/exit_safe_thread.h`) and the transport a plain pointer.
  Neither has a static destructor: nothing runs at process exit, and the OS
  reclaims both.
- **Aurie unloads the module.** `ModuleUnload` stops the publisher and waits up
  to 2 s to join it, then stops the transport, which joins the pipe worker, and
  frees both. Aurie also calls `ModuleUnload` from its own `DllMain` when the
  framework itself is unloaded (the Aurie console's *Unload framework*). The
  loader lock is held there, a thread cannot finish exiting until it is
  released, and the wait runs out; the module then pins itself, staying loaded
  with its threads until the game exits, instead of letting `FreeLibrary` unmap
  code that is still running. It has to be the `ModuleUnload` export: Aurie
  dispatches its unload callbacks only for a module that exports it.

`tests/exit_teardown_smoke.cpp` covers both, each case in a child process: a
control with the old `std::thread` global aborts at `ExitProcess`; the current
shape exits cleanly with both threads running; `ModuleUnload` on an ordinary
thread joins both threads and `FreeLibrary` unmaps the module; a publisher that
does not stop in time gets the module pinned; and `ModuleUnload` called from
another DLL's `DLL_PROCESS_DETACH`, standing in for AurieCore, returns after the
bounded wait. No case can leave a crash report or dump behind.

## Build

Requirements:

- Visual Studio C++ x64 toolchain
- CMake 3.24+
- an Aurie/YYToolkit SDK tree containing `include/YYToolkit`, `include/Aurie`
  and `include/YYToolkit/YYTK_Shared_Types.cpp`

From PowerShell:

```powershell
.\build.ps1 -Configuration Release -YytkSdkRoot C:\path\to\plugin_build
```

The local development tree is auto-detected when `-YytkSdkRoot` is omitted.
Output is `build\bin\Release\HSOfflineTrackerProducer.dll`; the build script
also runs the native test suite.

This directory does not install or copy the DLL into a game. Packaging and a
controlled offline smoke test are separate release steps. Do not load this
module together with `HSOfflineTrackerBridge.dll`: both intentionally own the
same per-process named-pipe address.

## Build gate and limitations

The embedded builds are documented in `profiles/s10-code-1dde65e4.json` and
`profiles/s10-code-c4dc91d9.json`. Any other game-code update is unsupported
until a new reviewed profile is added. Named routine lookup makes route addresses
resilient inside a reviewed code build; it does not weaken the exact `.text`
gate.

Gold and XP ABI semantics (including the current Gold argument-order change)
and EnemyAddStatistics kill cardinality have been confirmed. Gold, XP and kills
each use an independent atomic pending total. Pipe delivery is best-effort and
bounded; when the queue cannot accept a delta, the producer worker requeues every
field into its matching pending total.

YYToolkit is a build/runtime dependency and is not copied into this directory.
Review and comply with the licenses of the exact Aurie/YYToolkit versions used
when distributing a combined package.
