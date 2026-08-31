# HS Offline Tracker Aurie producer

This directory contains a standalone tracker producer module. It is separate
from ForgePact/BloodPactPlugin and shares only HS Offline Tracker's local event
protocol and bounded named-pipe transport.

The module does not patch the executable on disk. Aurie loads it into the game
process; YYToolkit resolves named GML routines and Aurie's owned hooks observe
their calls. The producer refuses to create any hook unless the host executable
matches the embedded PE machine, timestamp, `.text` raw size and complete
`.text` SHA-256 exactly. Aurie's own non-code section can legitimately change,
so full-file size/SHA-256 are retained as status diagnostics, not gate inputs.

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
queue fails open to protect gameplay and reports its skipped count once at unload.

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
