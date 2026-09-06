//! Offline event source.
//!
//! The upstream project listened to the game's network traffic. That cannot
//! observe a true offline session, so this product deliberately has no packet
//! capture dependency. Compatible local producers send versioned JSON events
//! through `events.ndjson` or the per-process Windows named pipe; this module
//! normalises both transports into the same `GameEvent` contract used by the
//! statistics/filter engine.

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::Value;
use sysinfo::{ProcessRefreshKind, ProcessesToUpdate, System};
use tauri::Emitter;

use crate::parser::{Currency, GameEvent};
use crate::save_source::SaveWatcher;
use crate::stats::GameStats;

const POLL: Duration = Duration::from_millis(100);
// Every 3 s: enumerating every process each second was the loop's only real cost.
const PROCESS_POLL_TICKS: u8 = 30;
const SAVE_POLL_TICKS: u8 = 10;
const MAX_EVENT_BYTES: usize = 1 << 20;
const READ_CHUNK_BYTES: usize = 64 << 10;
const PIPE_QUEUE_DEPTH: usize = 64;

#[derive(Clone, Debug, PartialEq, Eq)]
enum FramedRecord {
    Line(Vec<u8>),
    TooLong,
}

/// Incrementally splits an arbitrary byte stream into bounded NDJSON records.
///
/// Once a record crosses `MAX_EVENT_BYTES`, its buffered prefix is discarded
/// and input is ignored up to the next newline. This keeps memory bounded and,
/// importantly, lets the first valid record after a malformed one through.
#[derive(Default)]
struct BoundedLineFramer {
    current: Vec<u8>,
    dropping_oversized: bool,
}

impl BoundedLineFramer {
    fn push(&mut self, bytes: &[u8]) -> Vec<FramedRecord> {
        let mut records = Vec::new();
        for &byte in bytes {
            if self.dropping_oversized {
                if byte == b'\n' {
                    self.dropping_oversized = false;
                }
                continue;
            }

            if byte == b'\n' {
                records.push(FramedRecord::Line(std::mem::take(&mut self.current)));
            } else if self.current.len() < MAX_EVENT_BYTES {
                self.current.push(byte);
            } else {
                self.current.clear();
                self.dropping_oversized = true;
                records.push(FramedRecord::TooLong);
            }
        }
        records
    }

    fn reset(&mut self) {
        self.current.clear();
        self.dropping_oversized = false;
    }
}

fn record_text(record: FramedRecord) -> Result<String, String> {
    match record {
        FramedRecord::Line(bytes) => {
            String::from_utf8(bytes).map_err(|_| "bridge event is not valid UTF-8".into())
        }
        FramedRecord::TooLong => Err("event exceeds the 1 MiB safety limit".into()),
    }
}

#[derive(Clone, PartialEq)]
pub enum Status {
    WaitingForGame,
    BridgeWaiting { events: u64 },
    SaveProgress { events: u64 },
    Live { events: u64 },
    Fault { reason: String },
}

impl Status {
    pub fn text(&self) -> String {
        match self {
            Status::WaitingForGame => "waiting-for-game".into(),
            Status::BridgeWaiting { events } => format!("offline-waiting|{events}"),
            Status::SaveProgress { events } => format!("offline-progress|{events}"),
            Status::Live { events } => format!("offline-live|{events}"),
            Status::Fault { reason } => {
                let clean = reason.replace(['\r', '\n', '|'], " ");
                format!(
                    "offline-error|{}",
                    clean.chars().take(180).collect::<String>()
                )
            }
        }
    }
}

pub struct Shared {
    pub stats: Arc<Mutex<GameStats>>,
    pub status: Arc<Mutex<Status>>,
}

impl Shared {
    pub fn stats(&self) -> std::sync::MutexGuard<'_, GameStats> {
        self.stats.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn status(&self) -> std::sync::MutexGuard<'_, Status> {
        self.status.lock().unwrap_or_else(|e| e.into_inner())
    }
}

impl Default for Shared {
    fn default() -> Self {
        Self {
            stats: Arc::new(Mutex::new(GameStats::default())),
            status: Arc::new(Mutex::new(Status::WaitingForGame)),
        }
    }
}

static GAME_UP: AtomicBool = AtomicBool::new(false);
static GAME_PID: AtomicU32 = AtomicU32::new(0);
/// Where the running game's executable is, so the settings page can install
/// the live sensor beside it without asking the player for a path.
static GAME_EXE: Mutex<Option<PathBuf>> = Mutex::new(None);

pub fn game_exe() -> Option<PathBuf> {
    GAME_EXE.lock().ok().and_then(|g| g.clone())
}

pub fn game_up() -> bool {
    GAME_UP.load(Ordering::Relaxed)
}

pub fn pipe_up() -> bool {
    PIPE_UP.load(Ordering::Relaxed)
}
static PIPE_UP: AtomicBool = AtomicBool::new(false);
static EVENT_COUNT: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Debug, PartialEq, Eq)]
enum BridgeSignal {
    Ready,
    Waiting,
    Fault(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum ConsumeOutcome {
    Event,
    Bridge(BridgeSignal),
    Ignored,
}

pub fn game_running() -> bool {
    GAME_UP.load(Ordering::Relaxed)
}

/// Upgrade compatibility while the legacy preference migrates out.
pub fn set_wide_capture(_on: bool) {}
pub fn prepare_capture() {}

pub fn event_path() -> PathBuf {
    if let Some(path) = std::env::var_os("HS_OFFLINE_TRACKER_EVENTS") {
        return PathBuf::from(path);
    }
    let root = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .or_else(|| std::env::current_dir().ok())
        .unwrap_or_else(|| PathBuf::from("."));
    root.join("HS Offline Tracker").join("events.ndjson")
}

fn set_status(status: &Arc<Mutex<Status>>, next: Status) {
    let mut guard = status.lock().unwrap_or_else(|e| e.into_inner());
    if *guard != next {
        *guard = next;
    }
}

fn is_game_executable_name(raw: &str) -> bool {
    let flat = raw
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .collect::<String>()
        .to_ascii_lowercase();
    matches!(flat.as_str(), "herosiege" | "herosiegeexe")
}

fn game_process(sys: &mut System) -> Option<(u32, Option<SystemTime>)> {
    sys.refresh_processes_specifics(
        ProcessesToUpdate::All,
        true,
        ProcessRefreshKind::nothing().with_exe(sysinfo::UpdateKind::OnlyIfNotSet),
    );
    // Match the game executable exactly. Prefix matching also accepted tools
    // such as HeroSiegeItemEditor.exe, which made the tracker wait forever on
    // a pipe belonging to the wrong PID. If two real game copies exist, prefer
    // the one launched most recently (normally the active offline session).
    sys.processes()
        .values()
        .filter(|p| {
            is_game_executable_name(&p.name().to_string_lossy())
                || p.exe()
                    .and_then(|path| path.file_name())
                    .is_some_and(|name| is_game_executable_name(&name.to_string_lossy()))
        })
        .max_by_key(|p| p.start_time())
        .map(|p| {
            let started_at =
                (p.start_time() != 0).then(|| UNIX_EPOCH + Duration::from_secs(p.start_time()));
            if let Ok(mut slot) = GAME_EXE.lock() {
                *slot = p.exe().map(|path| path.to_path_buf());
            }
            (p.pid().as_u32(), started_at)
        })
}

#[cfg(windows)]
fn spawn_pipe_reader(sender: mpsc::SyncSender<Result<String, String>>) {
    std::thread::spawn(move || loop {
        let pid = GAME_PID.load(Ordering::Relaxed);
        if pid == 0 {
            PIPE_UP.store(false, Ordering::Relaxed);
            std::thread::sleep(Duration::from_millis(250));
            continue;
        }
        let path = format!(r"\\.\pipe\HSOfflineTrackerBridge_{pid}");
        let file = match OpenOptions::new().read(true).open(&path) {
            Ok(file) => file,
            Err(_) => {
                PIPE_UP.store(false, Ordering::Relaxed);
                std::thread::sleep(Duration::from_millis(250));
                continue;
            }
        };
        PIPE_UP.store(true, Ordering::Relaxed);
        let mut reader = file;
        let mut framer = BoundedLineFramer::default();
        let mut chunk = [0u8; READ_CHUNK_BYTES];
        loop {
            match reader.read(&mut chunk) {
                Ok(0) | Err(_) => break,
                Ok(read) => {
                    for record in framer.push(&chunk[..read]) {
                        if sender.send(record_text(record)).is_err() {
                            return;
                        }
                    }
                }
            }
        }
        PIPE_UP.store(false, Ordering::Relaxed);
        std::thread::sleep(Duration::from_millis(100));
    });
}

#[cfg(not(windows))]
fn spawn_pipe_reader(_sender: mpsc::SyncSender<Result<String, String>>) {}

fn integer(v: &Value, key: &str) -> i64 {
    v.get(key)
        .and_then(|n| {
            n.as_i64()
                .or_else(|| n.as_u64().and_then(|n| i64::try_from(n).ok()))
                .or_else(|| n.as_f64().map(|n| n as i64))
                .or_else(|| n.as_str().and_then(|n| n.trim().parse().ok()))
        })
        .unwrap_or_default()
}

fn first<'a>(v: &'a Value, keys: &[&str]) -> Option<&'a Value> {
    keys.iter().find_map(|key| v.get(*key))
}

fn integer_any(v: &Value, keys: &[&str]) -> i64 {
    first(v, keys)
        .and_then(|n| {
            n.as_i64()
                .or_else(|| n.as_u64().and_then(|n| i64::try_from(n).ok()))
                .or_else(|| n.as_f64().map(|n| n as i64))
                .or_else(|| n.as_str().and_then(|n| n.trim().parse().ok()))
        })
        .unwrap_or_default()
}

fn boolean(v: &Value, key: &str) -> bool {
    v.get(key)
        .and_then(|b| {
            b.as_bool().or_else(|| {
                b.as_str()
                    .map(|s| matches!(s.trim().to_ascii_lowercase().as_str(), "1" | "true" | "yes"))
            })
        })
        .unwrap_or(false)
}

fn text(v: &Value, key: &str) -> String {
    v.get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .trim()
        .to_string()
}

/// "weapons_unique_gladius": a localization key the game had not resolved yet,
/// not a name anyone should read.
fn looks_like_key(name: &str) -> bool {
    name.contains('_')
        && name
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
}

fn text_any(v: &Value, keys: &[&str]) -> String {
    first(v, keys)
        .and_then(|value| match value {
            Value::String(s) => Some(s.clone()),
            Value::Number(n) => Some(n.to_string()),
            _ => None,
        })
        .unwrap_or_default()
        .trim()
        .to_string()
}

fn nested<'a>(v: &'a Value, object: &str, key: &str) -> Option<&'a Value> {
    v.get(object).and_then(|value| value.get(key))
}

fn integer_map(v: Option<&Value>) -> HashMap<String, i64> {
    v.and_then(Value::as_object)
        .map(|map| {
            map.iter()
                .filter_map(|(k, value)| {
                    value
                        .as_i64()
                        .or_else(|| value.as_f64().map(|n| n as i64))
                        .map(|n| (k.clone(), n))
                })
                .collect()
        })
        .unwrap_or_default()
}

fn bytes(v: Option<&Value>) -> Vec<u8> {
    v.and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_u64)
                .filter_map(|n| u8::try_from(n).ok())
                .collect()
        })
        .unwrap_or_default()
}

/// Versioned bridge line -> source-independent event(s).
fn decode_line(line: &str) -> Result<Vec<GameEvent>, String> {
    if line.len() > MAX_EVENT_BYTES {
        return Err("event exceeds the 1 MiB safety limit".into());
    }
    let value: Value =
        serde_json::from_str(line).map_err(|e| format!("invalid bridge JSON: {e}"))?;
    let version_present = value.get("v").is_some();
    let protocol_present = value.get("protocol").is_some();
    let version = integer(&value, "v");
    let protocol = text(&value, "protocol");

    // A producer may use either v1 marker for backwards compatibility. If it
    // sends both, however, both must agree. Accepting a valid `v` beside an
    // unknown protocol (or vice versa) would silently reinterpret a future or
    // malformed contract as v1.
    if !version_present && !protocol_present {
        return Err("bridge event has no protocol version".into());
    }
    if version_present && version != 1 {
        return Err(format!("unsupported bridge event version {version}"));
    }
    if protocol_present && protocol != "hs-offline-tracker/1" {
        return Err(format!("unsupported bridge protocol '{protocol}'"));
    }
    let kind = text(&value, "kind");
    let event = match kind.as_str() {
        "gold" | "currency" => GameEvent::Gold(Currency {
            gss: integer(&value, "gss"),
            gsh: integer(&value, "gsh"),
            gns: integer(&value, "gns"),
            gnh: integer(&value, "gnh"),
            gbp: integer(&value, "gbp"),
            delta: integer(&value, "delta"),
        }),
        "xp_gain" => GameEvent::XpGain(integer(&value, "amount")),
        "session_delta" => GameEvent::SessionDelta {
            gold: integer(&value, "gold").max(0),
            xp: integer(&value, "xp").max(0),
            kills: integer(&value, "kills").max(0),
        },
        "sensor_diagnostic" => {
            // Keep diagnostics out of the statistics stream, but retain one
            // compact line in the local log so a hook can be validated without
            // guessing from the dashboard.
            crate::log::say("sensor", &format!(
                "sensor diagnostic: gold calls={} accepted={} rejected={} delta={}; xp calls={} accepted={} rejected={} delta={}; kill candidates={} enabled={} route={}; drops calls={} accepted={} rejected={} queue_dropped={} emitted={} route={}",
                integer(&value, "gold_calls").max(0),
                integer(&value, "gold_accepted").max(0),
                integer(&value, "gold_rejected").max(0),
                integer(&value, "gold_delta_total").max(0),
                integer(&value, "xp_calls").max(0),
                integer(&value, "xp_accepted").max(0),
                integer(&value, "xp_rejected").max(0),
                integer(&value, "xp_delta_total").max(0),
                integer(&value, "kill_candidate_calls").max(0),
                boolean(&value, "kill_emission_enabled"),
                text(&value, "kill_route").chars().take(80).collect::<String>(),
                integer(&value, "drop_calls").max(0),
                integer(&value, "drop_accepted").max(0),
                integer(&value, "drop_rejected").max(0),
                integer(&value, "drop_queue_dropped").max(0),
                integer(&value, "drop_emitted").max(0),
                // Includes compact correctness and hot-path timing telemetry.
                // Keep enough of it to diagnose a burst without another build.
                text(&value, "drop_route").chars().take(240).collect::<String>(),
            ));
            return Ok(Vec::new());
        }
        "player_item_dropped" => GameEvent::ItemsLetGo(
            value
                .get("fingerprints")
                .and_then(Value::as_array)
                .map(|values| {
                    values
                        .iter()
                        .filter_map(Value::as_str)
                        .map(str::to_owned)
                        .collect()
                })
                .unwrap_or_default(),
        ),
        "account_id" => GameEvent::WhoseAccount(text(&value, "account_id")),
        "character" | "progress" => GameEvent::Account {
            experience: integer(&value, "experience"),
            has_experience: value.get("experience").is_some(),
            armed_baseline: false,
            has_mode: true,
            season: integer(&value, "season"),
            hardcore: integer(&value, "hardcore"),
            blood_pact: integer(&value, "blood_pact"),
            name: text(&value, "name"),
            level: integer(&value, "level"),
            herolevel: integer(&value, "hero_level"),
            difficulty: integer(&value, "difficulty"),
            hell_sub: integer(&value, "hell_sub"),
            act: integer(&value, "act"),
            kills: integer(&value, "kills"),
            tallies: integer_map(value.get("tallies")),
        },
        "mail" => GameEvent::Mail(boolean(&value, "has_mail")),
        "room" => GameEvent::Room(text(&value, "room")),
        "vitals" => GameEvent::Vitals {
            mf: value
                .get("magic_find")
                .map(|_| integer(&value, "magic_find")),
            level: integer(&value, "level"),
            hlevel: integer(&value, "hero_level"),
            satanic_here: value
                .get("satanic_here")
                .map(|_| boolean(&value, "satanic_here")),
        },
        "satanic_zone" => GameEvent::SatanicZone {
            zone: text(&value, "zone"),
            buffs: bytes(value.get("buffs")),
            debuffs: bytes(value.get("debuffs")),
        },
        "drop" | "ground_drop" | "drop_spawned" | "item_picked_up" => {
            // The bridge emits the stable public contract with an `item`
            // object. Flat fields remain accepted so recorded v1 fixtures and
            // early development builds stay replayable.
            let item = value.get("item").unwrap_or(&value);
            let source = text_any(&value, &["source", "drop_source"]).to_ascii_lowercase();
            if matches!(
                source.as_str(),
                "vendor" | "trade" | "quest" | "craft" | "player_drop"
            ) {
                return Ok(Vec::new());
            }
            let event_id = text_any(&value, &["event_id", "id"]);
            let hash = {
                let direct = text_any(item, &["hash", "itemDataHash", "item_data_hash"]);
                if direct.is_empty() {
                    text(&value, "hash")
                } else {
                    direct
                }
            };
            let fingerprint = {
                let direct = text_any(item, &["fingerprint", "inventory_fingerprint"]);
                if direct.is_empty() {
                    text(&value, "fingerprint")
                } else {
                    direct
                }
            };
            let rarity = first(item, &["rarity", "rarity_id"])
                .cloned()
                .or_else(|| nested(item, "itemInfoStruct", "27").cloned())
                .unwrap_or(Value::Null);
            let null = Value::Null;
            let definition = item.get("itemDefinitionStruct").unwrap_or(&null);
            let info = item.get("itemInfoStruct").unwrap_or(&null);
            let mut wire_name = text_any(item, &["name", "display_name"]);
            if wire_name.is_empty() {
                wire_name = text(info, "28");
            }
            let item_type = integer_any(item, &["item_type", "itemType"]);
            let item_id = integer_any(item, &["item_id", "itemId"]).max(integer(definition, "b"));
            let weapon_type =
                integer_any(item, &["weapon_type", "weaponType"]).max(integer(definition, "j"));
            // The lists are matched by name and carry the table's spelling,
            // so for an identity the table knows, the table's name is the
            // one that counts — a weapon the sensor named a little
            // differently made no card. The wire name is kept for an item
            // the table has never heard of (a newer game build), unless it
            // is a raw localization key, which is no name at all.
            // Only a packet that names an identity gets the table's answer: id 0
            // is a real item, so a name-only packet must not be renamed to
            // whatever sits at the table's origin.
            let has_identity = first(item, &["item_type", "itemType"]).is_some()
                || item.get("itemDefinitionStruct").is_some();
            let known_name = has_identity
                .then(|| crate::items::item_name(item_type, item_id, weapon_type))
                .flatten();
            let name = match known_name {
                Some(known_name) => known_name.to_owned(),
                None if looks_like_key(&wire_name) => String::new(),
                None => wire_name,
            };
            let phase = text_any(&value, &["phase", "state"]).to_ascii_lowercase();
            let ground = match kind.as_str() {
                "ground_drop" | "drop_spawned" => true,
                "item_picked_up" => false,
                _ => !matches!(phase.as_str(), "pickup" | "picked_up" | "inventory"),
            };
            GameEvent::ItemAdded {
                rarity,
                unscaled: boolean(item, "unscaled"),
                mf: boolean(item, "magic_find_drop"),
                tier: integer_any(item, &["tier", "grade"]),
                item_type,
                item_id,
                weapon_type,
                seed: integer_any(item, &["seed"]).max(integer(definition, "a")),
                name,
                announced: false,
                amount: integer_any(item, &["amount", "stack", "quantity"]).max(1),
                fingerprint: if fingerprint.is_empty() {
                    event_id.clone()
                } else {
                    fingerprint
                },
                hash: if hash.is_empty() { event_id } else { hash },
                ground,
            }
        }
        "game_started" | "game_stopped" | "heartbeat" | "bridge_status" => return Ok(Vec::new()),
        _ => return Err(format!("unknown bridge event kind '{kind}'")),
    };
    Ok(vec![event])
}

fn bridge_signal(line: &str) -> Result<Option<BridgeSignal>, String> {
    let value: Value =
        serde_json::from_str(line).map_err(|e| format!("invalid bridge JSON: {e}"))?;
    if text(&value, "kind") != "bridge_status" {
        return Ok(None);
    }

    let state = text(&value, "state");
    let code = text(&value, "code");
    let detail = text(&value, "detail");
    let reason = match (code.is_empty(), detail.is_empty()) {
        (false, false) => format!("{code}: {detail}"),
        (false, true) => code,
        (true, false) => detail,
        (true, true) => "local sensor reported an unspecified error".into(),
    };
    match state.as_str() {
        "transport_ready" | "ready" => Ok(Some(BridgeSignal::Ready)),
        "scaffold_ready" | "starting" => Ok(Some(BridgeSignal::Waiting)),
        "blocked" | "transport_error" | "error" => Ok(Some(BridgeSignal::Fault(reason))),
        _ => Err(format!("unknown bridge status state '{state}'")),
    }
}

fn apply_events(events: &[GameEvent], stats: &Arc<Mutex<GameStats>>, app: &tauri::AppHandle) {
    let fresh: Vec<_> = {
        let mut stats = stats.lock().unwrap_or_else(|e| e.into_inner());
        events
            .iter()
            .filter_map(|event| stats.apply(event))
            .collect()
    };
    for drop in fresh {
        if let Some(key) = &drop.sound {
            let _ = app.emit("item-drop", (key, &drop.rarity));
        }
        if drop.announce {
            let _ = app.emit("drop-entry", &drop);
        }
        if drop.flourish {
            crate::maybe_flourish(app, &drop);
        }
    }
}

fn consume_line(
    line: &str,
    stats: &Arc<Mutex<GameStats>>,
    app: &tauri::AppHandle,
) -> Result<ConsumeOutcome, String> {
    let line = line.trim();
    if line.is_empty() {
        return Ok(ConsumeOutcome::Ignored);
    }
    let events = decode_line(line)?;
    if events.is_empty() {
        return Ok(match bridge_signal(line)? {
            Some(signal) => ConsumeOutcome::Bridge(signal),
            None => ConsumeOutcome::Ignored,
        });
    }
    apply_events(&events, stats, app);
    EVENT_COUNT.fetch_add(1, Ordering::Relaxed);
    Ok(ConsumeOutcome::Event)
}

fn ensure_event_file(path: &PathBuf) -> std::io::Result<File> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    OpenOptions::new()
        .create(true)
        .append(true)
        .read(true)
        .open(path)
}

pub fn spawn(shared: &Shared, app: tauri::AppHandle) {
    let stats = Arc::clone(&shared.stats);
    let status = Arc::clone(&shared.status);
    // A bounded queue lets pipe backpressure cap memory even if a local
    // producer writes faster than the statistics engine can consume events.
    let (pipe_tx, pipe_rx) = mpsc::sync_channel(PIPE_QUEUE_DEPTH);
    spawn_pipe_reader(pipe_tx);
    std::thread::spawn(move || {
        let path = event_path();
        let mut file = match ensure_event_file(&path) {
            Ok(file) => file,
            Err(e) => {
                set_status(
                    &status,
                    Status::Fault {
                        reason: format!("cannot open local event bridge: {e}"),
                    },
                );
                crate::log::error(format!("cannot open {}: {e}", path.display()));
                return;
            }
        };
        let mut cursor = file.metadata().map(|m| m.len()).unwrap_or(0);
        let mut file_framer = BoundedLineFramer::default();
        let mut read_chunk = [0u8; READ_CHUNK_BYTES];
        let mut system = System::new();
        let mut process_tick = 0u8;
        let mut save_tick = 0u8;
        let mut save_watcher = SaveWatcher::new();
        let mut was_up = false;
        let mut saw_bridge_event = false;
        let mut saw_save_event = false;
        let mut bridge_ready = false;
        let mut bridge_fault: Option<String> = None;

        loop {
            if process_tick == 0 {
                let game = game_process(&mut system);
                let pid = game.as_ref().map(|(pid, _)| *pid).unwrap_or(0);
                let up = pid != 0;
                GAME_UP.store(up, Ordering::Relaxed);
                GAME_PID.store(pid, Ordering::Relaxed);
                if !was_up && up {
                    // Event counts and the live-state latch belong to one game
                    // process lifetime. A previous session must not make a new
                    // process appear live before its first event arrives.
                    EVENT_COUNT.store(0, Ordering::Relaxed);
                    saw_bridge_event = false;
                    saw_save_event = false;
                    bridge_ready = false;
                    bridge_fault = None;
                    // Seed every slot's signature first. The read-only source
                    // waits for the game itself to change a character save
                    // instead of guessing which old slot is currently active.
                    if let Some(started_at) = game.and_then(|(_, started_at)| started_at) {
                        save_watcher.game_started_since(started_at);
                    } else {
                        save_watcher.game_started();
                    }
                    save_tick = 0;
                }
                if was_up && !up {
                    save_watcher.game_stopped();
                    crate::end_run_after_blackout(&app);
                }
                was_up = up;
                process_tick = PROCESS_POLL_TICKS;
            }
            process_tick = process_tick.saturating_sub(1);

            while let Ok(message) = pipe_rx.try_recv() {
                match message {
                    Ok(line) => match consume_line(&line, &stats, &app) {
                        Ok(ConsumeOutcome::Event) => saw_bridge_event = true,
                        Ok(ConsumeOutcome::Bridge(BridgeSignal::Ready)) => {
                            bridge_ready = true;
                            bridge_fault = None;
                        }
                        Ok(ConsumeOutcome::Bridge(BridgeSignal::Waiting)) => {
                            bridge_ready = false;
                            bridge_fault = None;
                        }
                        Ok(ConsumeOutcome::Bridge(BridgeSignal::Fault(reason))) => {
                            bridge_ready = false;
                            bridge_fault = Some(reason);
                        }
                        Ok(ConsumeOutcome::Ignored) => {}
                        Err(e) => crate::log::warn(format!("local bridge pipe event ignored: {e}")),
                    },
                    Err(e) => crate::log::warn(format!("local bridge pipe event ignored: {e}")),
                }
            }

            let size = file.metadata().map(|m| m.len()).unwrap_or(0);
            if size < cursor {
                cursor = 0;
                file_framer.reset();
            }
            if size > cursor && file.seek(SeekFrom::Start(cursor)).is_ok() {
                // Read only the metadata snapshot. Fixed-size chunks prevent a
                // concurrently growing file from turning this pass into an
                // unbounded allocation/read.
                let mut remaining = size - cursor;
                while remaining > 0 {
                    let wanted = remaining.min(READ_CHUNK_BYTES as u64) as usize;
                    match file.read(&mut read_chunk[..wanted]) {
                        Ok(0) => break,
                        Ok(read) => {
                            cursor += read as u64;
                            remaining -= read as u64;
                            for record in file_framer.push(&read_chunk[..read]) {
                                match record_text(record) {
                                    Ok(line) => match consume_line(&line, &stats, &app) {
                                        Ok(ConsumeOutcome::Event) => saw_bridge_event = true,
                                        Ok(ConsumeOutcome::Bridge(BridgeSignal::Ready)) => {
                                            bridge_ready = true;
                                            bridge_fault = None;
                                        }
                                        Ok(ConsumeOutcome::Bridge(BridgeSignal::Waiting)) => {
                                            bridge_ready = false;
                                            bridge_fault = None;
                                        }
                                        Ok(ConsumeOutcome::Bridge(BridgeSignal::Fault(reason))) => {
                                            bridge_ready = false;
                                            bridge_fault = Some(reason);
                                        }
                                        Ok(ConsumeOutcome::Ignored) => {}
                                        Err(e) => crate::log::warn(format!(
                                            "local bridge event ignored: {e}"
                                        )),
                                    },
                                    Err(e) => {
                                        crate::log::warn(format!("local bridge event ignored: {e}"))
                                    }
                                }
                            }
                        }
                        Err(e) => {
                            set_status(
                                &status,
                                Status::Fault {
                                    reason: format!("cannot read local event bridge: {e}"),
                                },
                            );
                            break;
                        }
                    }
                }
            }

            if was_up && save_tick == 0 {
                match save_watcher.poll() {
                    Ok(events) if !events.is_empty() => {
                        // Save progress travels through the same ordered
                        // GameEvent path as the local bridge. It never emits
                        // drops, room, MF, mail, or currency-gain events.
                        apply_events(&events, &stats, &app);
                        EVENT_COUNT.fetch_add(events.len() as u64, Ordering::Relaxed);
                        saw_save_event = true;
                    }
                    Ok(_) => {}
                    Err(error) => crate::log::warn(format!("local save snapshot ignored: {error}")),
                }
                save_tick = SAVE_POLL_TICKS;
            }
            save_tick = save_tick.saturating_sub(1);

            let count = EVENT_COUNT.load(Ordering::Relaxed);
            let next = if !was_up {
                Status::WaitingForGame
            } else if let Some(reason) = &bridge_fault {
                Status::Fault {
                    reason: reason.clone(),
                }
            } else if saw_bridge_event || bridge_ready {
                Status::Live { events: count }
            } else if saw_save_event {
                Status::SaveProgress { events: count }
            } else {
                Status::BridgeWaiting { events: count }
            };
            set_status(&status, next);
            std::thread::sleep(POLL);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn game_process_name_match_is_exact() {
        assert!(is_game_executable_name("Hero_Siege.exe"));
        assert!(is_game_executable_name("hero_siege"));
        assert!(!is_game_executable_name("HeroSiegeItemEditor.exe"));
        assert!(!is_game_executable_name("Hero_Siege_Launcher.exe"));
        assert!(!is_game_executable_name("NotHeroSiege.exe"));
    }

    #[test]
    fn bounded_framer_accepts_a_record_at_the_exact_limit() {
        let mut framer = BoundedLineFramer::default();
        let payload = vec![b'a'; MAX_EVENT_BYTES];
        assert!(framer.push(&payload).is_empty());

        let records = framer.push(b"\n");
        assert_eq!(records.len(), 1);
        let FramedRecord::Line(line) = &records[0] else {
            panic!("an exact-limit record must be retained");
        };
        assert_eq!(line.len(), MAX_EVENT_BYTES);
    }

    #[test]
    fn bounded_framer_drops_oversized_record_and_resynchronises() {
        let mut framer = BoundedLineFramer::default();
        assert!(framer.push(&vec![b'x'; MAX_EVENT_BYTES]).is_empty());

        let records = framer.push(b"xdiscarded suffix\n{\"v\":1}\n");
        assert_eq!(records.len(), 2);
        assert_eq!(records[0], FramedRecord::TooLong);
        assert_eq!(records[1], FramedRecord::Line(br#"{"v":1}"#.to_vec()));
    }

    #[test]
    fn bounded_framer_never_buffers_past_the_limit_without_a_newline() {
        let mut framer = BoundedLineFramer::default();
        let records = framer.push(&vec![b'x'; MAX_EVENT_BYTES + READ_CHUNK_BYTES]);
        assert_eq!(records, vec![FramedRecord::TooLong]);
        assert!(framer.current.is_empty());
        assert!(framer.dropping_oversized);

        let records = framer.push(b"still discarded\nvalid\n");
        assert_eq!(records, vec![FramedRecord::Line(b"valid".to_vec())]);
    }

    #[test]
    fn invalid_utf8_record_does_not_poison_the_following_record() {
        let mut framer = BoundedLineFramer::default();
        let records = framer.push(&[0xff, b'\n', b'{', b'}', b'\n']);
        assert!(record_text(records[0].clone()).is_err());
        assert_eq!(record_text(records[1].clone()).unwrap(), "{}");
    }

    #[test]
    fn a_drop_line_becomes_one_ground_item() {
        let events = decode_line(
            r#"{"v":1,"kind":"drop","event_id":"demo-1","name":"Arcane Reliquary","rarity":"Angelic","tier":6,"source":"monster"}"#,
        )
        .unwrap();
        assert_eq!(events.len(), 1);
        let GameEvent::ItemAdded {
            name,
            ground,
            amount,
            ..
        } = &events[0]
        else {
            panic!("not an item event");
        };
        assert_eq!(name, "Arcane Reliquary");
        assert!(*ground);
        assert_eq!(*amount, 1);
    }

    #[test]
    fn native_unholy_drop_counts_and_alerts_on_the_ground() {
        let events = decode_line(
            r#"{"protocol":"hs-offline-tracker/1","v":1,"kind":"ground_drop","event_id":"s10-99-12","source":"game_rare_announcement","item":{"rarity":"Unholy","item_type":8,"item_id":321,"amount":1,"properties":[]}}"#,
        )
        .expect("the producer payload follows protocol v1");
        assert_eq!(events.len(), 1);

        let mut stats = crate::stats::GameStats::default();
        stats.set_prefer_ground(true);
        stats.set_filter(vec!["Unholy".into()], 0);
        let alert = stats
            .apply(&events[0])
            .expect("an enabled ground alert must produce one drop entry");

        assert_eq!(alert.rarity, "Unholy");
        assert!(alert.name.is_empty());
        assert_eq!(alert.sound.as_deref(), Some("unholy"));
        assert!(alert.ground);
        assert_eq!(stats.snapshot(String::new()).items["Unholy"].total, 1);
    }

    #[test]
    fn either_v1_marker_is_accepted_on_its_own() {
        assert!(decode_line(r#"{"v":1,"kind":"heartbeat"}"#).is_ok());
        assert!(decode_line(r#"{"protocol":"hs-offline-tracker/1","kind":"heartbeat"}"#).is_ok());
    }

    #[test]
    fn conflicting_or_missing_protocol_markers_are_rejected() {
        assert!(
            decode_line(r#"{"v":1,"protocol":"hs-offline-tracker/2","kind":"heartbeat"}"#).is_err()
        );
        assert!(
            decode_line(r#"{"v":2,"protocol":"hs-offline-tracker/1","kind":"heartbeat"}"#).is_err()
        );
        assert!(decode_line(r#"{"kind":"heartbeat"}"#).is_err());
    }

    #[test]
    fn non_combat_items_are_not_announced() {
        let events = decode_line(
            r#"{"v":1,"kind":"drop","event_id":"demo-2","name":"Vendor Item","rarity":"Satanic","source":"vendor"}"#,
        )
        .unwrap();
        assert!(events.is_empty());
    }

    #[test]
    fn stable_nested_bridge_contract_maps_known_s10_fields() {
        let events = decode_line(
            r#"{"protocol":"hs-offline-tracker/1","kind":"ground_drop","event_id":"evt-42","source":"boss","item":{"itemType":12,"itemDefinitionStruct":{"a":77,"b":321,"j":4},"itemInfoStruct":{"27":6,"28":"Solar Reliquary"},"itemDataHash":"hash-42","tier":6}}"#,
        )
        .unwrap();
        let GameEvent::ItemAdded {
            name,
            item_type,
            item_id,
            weapon_type,
            seed,
            hash,
            ground,
            ..
        } = &events[0]
        else {
            panic!("not an item event");
        };
        assert_eq!(name, "Solar Reliquary");
        assert_eq!(
            (*item_type, *item_id, *weapon_type, *seed),
            (12, 321, 4, 77)
        );
        assert_eq!(hash, "hash-42");
        assert!(*ground);
    }

    #[test]
    fn native_session_delta_keeps_character_xp_in_final_units() {
        let events = decode_line(
            r#"{"protocol":"hs-offline-tracker/1","kind":"session_delta","gold":75,"xp":200,"kills":1,"source":"s10-native"}"#,
        )
        .unwrap();
        assert!(matches!(
            events.as_slice(),
            [GameEvent::SessionDelta {
                gold: 75,
                xp: 200,
                kills: 1
            }]
        ));
    }

    #[test]
    fn native_session_delta_cannot_subtract_session_progress() {
        let events =
            decode_line(r#"{"v":1,"kind":"session_delta","gold":-5,"xp":-10,"kills":-1}"#).unwrap();
        assert!(matches!(
            events.as_slice(),
            [GameEvent::SessionDelta {
                gold: 0,
                xp: 0,
                kills: 0
            }]
        ));
    }

    #[test]
    fn sensor_diagnostic_is_supported_but_never_changes_stats() {
        let events = decode_line(
            r#"{"v":1,"kind":"sensor_diagnostic","gold_calls":3,"gold_accepted":2,"gold_rejected":1,"gold_delta_total":75,"xp_calls":1,"xp_accepted":1,"xp_rejected":0,"xp_delta_total":200,"kill_candidate_calls":4,"kill_emission_enabled":false,"kill_route":"EnemyAddStatistics"}"#,
        )
        .unwrap();
        assert!(events.is_empty());
    }

    #[test]
    fn pickup_contract_is_not_marked_as_ground() {
        let events = decode_line(
            r#"{"protocol":"hs-offline-tracker/1","kind":"item_picked_up","event_id":"evt-43","source":"monster","item":{"name":"Ashen Sigil"}}"#,
        )
        .unwrap();
        assert!(matches!(
            events[0],
            GameEvent::ItemAdded { ground: false, .. }
        ));
    }

    #[test]
    fn bridge_status_is_a_supported_control_record() {
        let line = r#"{"protocol":"hs-offline-tracker/1","kind":"bridge_status","state":"ready"}"#;
        let events = decode_line(line).expect("the bridge may describe its own fail-closed state");
        assert!(events.is_empty());
        assert_eq!(bridge_signal(line).unwrap(), Some(BridgeSignal::Ready));
    }

    #[test]
    fn bridge_errors_are_not_mistaken_for_a_live_transport() {
        let line = r#"{"v":1,"kind":"bridge_status","state":"blocked","code":"unsupported_build","detail":"fingerprint mismatch"}"#;
        assert_eq!(
            bridge_signal(line).unwrap(),
            Some(BridgeSignal::Fault(
                "unsupported_build: fingerprint mismatch".into()
            ))
        );
    }

    #[test]
    fn checked_in_demo_session_is_a_valid_replay() {
        let mut decoded = 0usize;
        let mut drops = 0usize;
        for line in include_str!("../../fixtures/demo-session.ndjson").lines() {
            let events = decode_line(line).expect("every checked-in event must follow protocol v1");
            decoded += events.len();
            drops += events
                .iter()
                .filter(|event| matches!(event, GameEvent::ItemAdded { .. }))
                .count();
        }
        assert_eq!(decoded, 9);
        assert_eq!(drops, 3);
    }
}

#[cfg(test)]
mod drop_name_tests {
    use super::*;

    /// A raw localization key on the wire yields to the table; a real name
    /// stays as the game sent it.
    #[test]
    fn a_localization_key_is_not_a_name() {
        assert!(looks_like_key("weapons_unique_gladius"));
        assert!(looks_like_key("belts_normal_heavy_belt"));
        assert!(!looks_like_key("Shattered Dimensions"));
        assert!(!looks_like_key("St. Draxis' Pigstick"));
        assert!(!looks_like_key("Judge, Jury & Executioner"));
        assert!(!looks_like_key("Diablo"));
        assert!(!looks_like_key(""));
    }

    fn drop_name(item: &str) -> String {
        let line = format!(
            r#"{{"protocol":"hs-offline-tracker/1","v":1,"kind":"ground_drop","event_id":"s10-1-1","source":"game_ground_item_create","item":{item}}}"#
        );
        let events = decode_line(&line).expect("a protocol v1 drop");
        let GameEvent::ItemAdded { name, .. } = &events[0] else {
            panic!("not an item event");
        };
        name.clone()
    }

    /// A weapon is named by its weapon type, and the table's spelling is the
    /// one the lists carry — so it wins over the sensor's for a known item.
    #[test]
    fn a_known_weapon_takes_the_tables_name_and_an_unknown_one_keeps_the_games() {
        let table = crate::items::item_name(3, 5, 2).expect("weapon 3:5:2 is in the table");
        assert_eq!(
            drop_name(r#"{"rarity":"Angelic","item_type":3,"item_id":5,"weapon_type":2,"name":"thunder dagger "}"#),
            table
        );
        assert_eq!(
            drop_name(r#"{"rarity":"Angelic","item_type":3,"item_id":5,"weapon_type":2}"#),
            table
        );
        assert_eq!(
            drop_name(r#"{"rarity":"Angelic","item_type":3,"item_id":64000,"weapon_type":2,"name":"Brand New Blade"}"#),
            "Brand New Blade"
        );
        assert_eq!(
            drop_name(r#"{"rarity":"Angelic","item_type":3,"item_id":64000,"weapon_type":2,"name":"weapons_unique_new"}"#),
            ""
        );
        // no identity at all: the name is all there is, and 0:0:0 is not it
        assert_eq!(drop_name(r#"{"rarity":"Angelic","name":"Arcane Reliquary"}"#), "Arcane Reliquary");
    }
}
