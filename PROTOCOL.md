# HS Offline Tracker local event protocol

This document defines protocol version 1 consumed by HS Offline Tracker. Every
transport carries UTF-8 NDJSON: one complete JSON object per record, terminated
by `\n`.

## Location and lifecycle

The default Windows path is:

```text
%LOCALAPPDATA%\HS Offline Tracker\events.ndjson
```

Set `HS_OFFLINE_TRACKER_EVENTS` to use another file. The tracker creates the parent directory and file when necessary.

On Windows, the tracker also attempts to read the local named pipe associated
with the detected game process:

```text
\\.\pipe\HSOfflineTrackerBridge_<pid>
```

The producer owns the pipe server; the tracker is the read-only client. The
native prototype creates it with `PIPE_REJECT_REMOTE_CLIENTS`. File and pipe
records use the identical JSON schema and may be consumed concurrently.

At startup the file reader seeks to the existing end. Old records are not
replayed. It polls for appended bytes, preserves an incomplete final line until
its newline arrives, and resets safely if the file is truncated. The consumer
rejects a record larger than 1 MiB; producers should keep records much smaller.

Every event must identify version 1 with either of these fields; producers should send both:

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"heartbeat"}
```

Unknown event kinds, malformed JSON and unsupported versions are ignored and
written to the application log. Lifecycle events `game_started`, `game_stopped`
and `heartbeat` do not directly change counters. The producer-control record
`bridge_status` also does not change counters, but it does drive sensor health:
`transport_ready` marks the source ready, `scaffold_ready` keeps it waiting, and
`blocked` / `transport_error` surface a fail-closed error in the UI.

The built-in local-save watcher does not write this protocol and does not add a
new producer surface. Its read-only snapshots enter the same internal progress
path, limited to persisted character identity, levels, experience, difficulty,
coarse act, kills and `statistic...` counters. A character save does not prove
season/Blood Pact mode, an exact room, Magic Find, mail, currency gain or item
provenance, so the watcher neither selects a currency purse nor fabricates
those event kinds.

## Canonical events

All numeric fields accept JSON numbers. The current reader also accepts numeric strings for compatibility, but producers should send numbers.

### Currency

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"currency","gss":0,"gsh":0,"gns":0,"gnh":0,"gbp":0,"delta":1250}
```

`gold` is accepted as an alias for `currency`. The six fields map to the existing statistics engine's currency snapshot/delta model; omitted fields are zero.

### Experience gain

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"xp_gain","amount":42000}
```

### Native session delta

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"session_delta","gold":1250,"xp":42000,"kills":3,"source":"aurie_named_gml"}
```

`session_delta` is a producer-observed, positive incremental batch. Its three
fields are independent: an unverified sensor must remain zero instead of
inventing a value. Producers must emit a batch only after the original game
routine completes. Consumers may reconcile a later absolute save snapshot
against these live deltas; they must not count the same persisted progress a
second time.

### Sensor diagnostic

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"sensor_diagnostic","gold_calls":2,"gold_accepted":2,"gold_rejected":0,"gold_delta_total":2500,"xp_calls":1,"xp_accepted":1,"xp_rejected":0,"xp_delta_total":42000,"kill_candidate_calls":3,"kill_emission_enabled":true,"kill_route":"gml_Script_EnemyAddStatistics"}
```

Diagnostics contain monotonic producer counters and never change session
totals. They exist to compare a candidate route with persisted save deltas. A
candidate must keep `kill_emission_enabled:false` until its one-call-per-kill
cardinality has been independently validated. The reviewed
`gml_Script_EnemyAddStatistics` route is enabled because a packaged live run
showed `+56` accepted calls and `+56` persisted
`statisticTotalMonsterKills` in the same save interval. Consumers still
reconcile these live deltas with later absolute save snapshots so that the two
sources count each kill once.

### Character progress

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"progress","name":"Shaman","experience":728844020,"season":10,"hardcore":0,"blood_pact":0,"level":100,"hero_level":300,"difficulty":6,"hell_sub":5,"act":9,"kills":91420,"tallies":{"Uber Phantom Leviathan":4,"Crystal":23}}
```

`character` is accepted as an alias for `progress`. `tallies` is an object whose values are integer counters. Omitting `experience` means the event updates identity/progress fields without asserting an experience snapshot.

### Vitals

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"vitals","magic_find":12840,"level":100,"hero_level":300,"satanic_here":true}
```

`magic_find` and `satanic_here` are optional updates. Level fields default to zero when omitted. The Aurie producer sends `magic_find` on its own line whenever the game recomputes it and `satanic_here` with every room change.

### Account, room and mail

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"account_id","account_id":"offline-profile-1"}
{"protocol":"hs-offline-tracker/1","v":1,"kind":"room","room":"Act_09_03"}
{"protocol":"hs-offline-tracker/1","v":1,"kind":"mail","has_mail":true}
```

Account identifiers should be local pseudonymous identifiers; do not place credentials or online-service tokens in this stream.

### Satanic zone

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"satanic_zone","zone":"Satanic_9_3","buffs":[6,14,21],"debuffs":[3,12]}
```

Buff and debuff identifiers must fit in an unsigned byte (`0` through `255`). Out-of-range and non-integer entries are discarded.

The Aurie producer spells `zone` as `Satanic_<act>_<zone>` derived from the game's own room name (`Act_09_03` becomes `Satanic_9_3`) and fills `buffs`/`debuffs` from the game's `satanicZoneBuff` / `satanicZoneDebuff` arrays at every room change.

### Ground drop

The preferred drop envelope keeps item data under `item`:

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"ground_drop","event_id":"run-42-drop-7","source":"monster","item":{"name":"Astral Covenant","rarity":"Angelic","tier":6,"item_type":3,"item_id":1,"weapon_type":0,"seed":91,"amount":1,"magic_find_drop":true,"unscaled":false,"hash":"stable-item-hash","fingerprint":"stable-instance-id"}}
```

Supported kinds are:

- `ground_drop` and `drop_spawned`: the item is on the ground.
- `item_picked_up`: the item left the ground and entered inventory.
- `drop`: compatibility form; `phase`/`state` values `pickup`, `picked_up` or `inventory` mark it as no longer on the ground.

Recognised fields:

| Field | Purpose |
| --- | --- |
| `event_id` | Stable producer event identity and fallback identity for missing hash/fingerprint |
| `source` | Origin classification; use `monster`, `boss`, `chest` or another honest local origin |
| `name` | Display name |
| `rarity` | Rarity name or numeric game rarity value |
| `tier` | Item grade/tier |
| `item_type`, `item_id`, `weapon_type`, `seed` | Stable game identity fields used by the item resolver |
| `amount` | Stack quantity; clamped to at least 1 |
| `magic_find_drop` | Whether the game marked this drop as Magic Find related |
| `unscaled` | Marks a rarity value that uses a different scale |
| `hash`, `fingerprint` | Stable identities used to associate ground and inventory transitions |

Drops with `source` equal to `vendor`, `trade`, `quest`, `craft` or `player_drop` are intentionally ignored. Use the same stable hash/fingerprint for the ground event and its pickup/removal event so the tracker does not treat one item as two independent drops.

For compatibility, item fields may be flat on the event. The reader also understands captured-structure aliases such as `itemType`, `itemId`, `weaponType`, `itemDataHash`, `itemDefinitionStruct.{a,b,j}` and `itemInfoStruct.{27,28}`. New producers should use the canonical names above.

### Player-dropped inventory fingerprints

```json
{"protocol":"hs-offline-tracker/1","v":1,"kind":"player_item_dropped","fingerprints":["stable-instance-id"]}
```

This lets the statistics engine distinguish an item discarded by the player from a newly generated loot event.

## Producer rules

1. Write only complete UTF-8 JSON objects followed by a newline.
2. Use a unique, stable `event_id` for every emitted occurrence.
3. Preserve item hash/fingerprint across ground and pickup transitions.
4. Emit absolute progress snapshots and incremental `xp_gain`/drop events according to the examples; do not mix meanings for one field.
5. Do not write secrets, account credentials or arbitrary user content.
6. Emit only events observed in an offline/single-player session.
7. Keep producer and tracker clocks independent; protocol v1 does not require a timestamp.

The application intentionally keeps this boundary narrow. Protocol additions require a new documented kind or a version change rather than silently reinterpreting existing fields.
