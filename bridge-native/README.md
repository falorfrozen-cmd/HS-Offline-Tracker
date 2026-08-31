# HS Offline Tracker native bridge

This directory contains an original Windows x64, read-only drop sensor for HS
Offline Tracker. It does not copy or link Aurie, YYToolkit, ForgePact, or another
patcher's source code. MinHook's BSD notice is preserved in
`THIRD_PARTY_NOTICES.md`.

## Reviewed-build boundary

The prototype is deliberately restricted to one independently reviewed test
build of `Hero_Siege.exe`:

- file size: `303584768`
- SHA-256: `5F8085456A27109681403D8C57533E6999FBD0664752FF5F2856985B5FBBDE71`
- PE timestamp: `0x6A8C4540`
- machine: AMD64 (`0x8664`)

An exact fingerprint match is only the first gate. All names, spans, the direct
caller relationship, and the reviewed argument ABI must also validate. There are
no fixed GML routine RVAs. A missing, duplicated, changed, or unsafe value means
**zero hooks**.

## Non-negotiable safety policy

This component is for offline/single-player use only. Do not use it with online play, EAC-protected sessions, leaderboards, trading, or another player's process.

Unknown or partially validated builds must install **zero hooks**. Capture can
proceed only when all of the following are true:

1. every fingerprint field exactly matches a reviewed build profile;
2. every required routine name resolves uniquely from the live registration table;
3. `Loot_Ground_obj_Create` references its named anonymous caller exactly once;
4. that caller is the only direct caller of `GetRareDropAnnouncement`;
5. the reviewed argument-count, pointer-array, result-slot, rarity-key, and
   function-prologue checks pass;
6. local IPC has initialized successfully.

## Read-only capture behavior

The sole hook target is the uniquely resolved
`gml_Script_GetRareDropAnnouncement`. It calls the original first and returns the
original value unchanged. Only after a successful native announcement does it
copy three profile-validated numeric arguments: rarity, item type, and item ID.
It never dereferences item-name/string pointers, invents metadata, changes a
drop/rate, or writes game state.

The hook copies a POD snapshot into a preallocated bounded queue. JSON formatting
and pipe I/O happen on worker threads. Invalid types, unreadable pointers,
out-of-range identities, contention, and full queues drop the event instead of
blocking the game thread.

## Local event channel

The transport uses `\\.\pipe\HSOfflineTrackerBridge_<pid>` and `PIPE_REJECT_REMOTE_CLIENTS`. Each message is one UTF-8 JSON object followed by `\n`. Producers only attempt to enqueue into a bounded queue; connection and writes happen on a worker thread. Oversized messages, lock contention, and full queues are dropped and counted instead of blocking the game thread.

Live output uses the parent tracker's canonical schema only:

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"ground_drop","event_id":"s10-1234-2","source":"game_rare_announcement","item":{"rarity":6,"item_type":3,"item_id":901,"amount":1,"properties":[]}}
```

Status messages use the same envelope with `"kind":"bridge_status"`. The
tracker treats `transport_ready` as ready, `scaffold_ready` as still starting,
and `blocked` / `transport_error` as visible fail-closed errors.

## Resolved route

The exact-build profile resolves and validates these names:

- `gml_Object_Loot_Ground_obj_Create_0`
- `gml_Script_anon@1084@gml_Object_Loot_Ground_obj_Create_0`
- `gml_Script_GetRareDropAnnouncement`

`CreateItemDrop` is **not** treated as the authoritative offline ground-drop
source. No address or RVA from the analysis is stored in the profile.

## Activation boundary

`DllMain` remains passive. This project contains no launcher or injector and does
not auto-start a hook. A host that has already loaded the DLL must explicitly
call `HSOT_BridgeValidateHost`, then `HSOT_BridgeStartSensor`, and must call
`HSOT_BridgeStopSensor` before unloading it. Do not invoke these exports from a
loader-lock context.

## Build

From a Visual Studio x64 developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tools/verify_profile.py "C:\path\to\reviewed\Hero_Siege.exe"
```

CMake fetches official MinHook at the pinned commit documented in
`THIRD_PARTY_NOTICES.md`. The audit reads the executable only; it does not launch
the game, load the DLL, inject code, or change any file.
