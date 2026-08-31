//! Read-only progress snapshots from Hero Siege's local character saves.
//!
//! This source deliberately emits only facts persisted by the character save:
//! identity, levels, experience, selected difficulty, coarse act, kill totals,
//! and the game's `statistic...` counters. A save does not state the exact
//! room, Magic Find, mail, current resources, or whether an inventory change
//! was a drop; none of those are inferred here.

use std::collections::HashMap;
use std::fs::{self, File, Metadata};
use std::io::{Read, Take};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use base64::Engine as _;
use flate2::read::ZlibDecoder;

use crate::parser::GameEvent;

const MAX_SAVE_BYTES: usize = 4 * 1024 * 1024;

// Hero Siege's second save layer is a repeating XOR over UTF-16LE-like bytes.
// After XOR, every odd byte is zero and the even bytes are UTF-8 text.
const HSS_XOR_KEY: [u8; 32] = [
    0xE3, 0x95, 0x3D, 0xB1, 0x01, 0x6B, 0xB6, 0x58, 0x54, 0x38, 0x3F, 0x46, 0xA1, 0x74, 0x29, 0xCC,
    0x45, 0x45, 0x51, 0xF2, 0xA7, 0xF7, 0xAB, 0xB7, 0x26, 0xF1, 0x37, 0xA8, 0x81, 0x91, 0xE6, 0x7E,
];

#[derive(Clone, Debug, PartialEq, Eq)]
struct FileStamp {
    len: u64,
    modified: Option<SystemTime>,
}

impl FileStamp {
    fn from_metadata(metadata: &Metadata) -> Self {
        Self {
            len: metadata.len(),
            modified: metadata.modified().ok(),
        }
    }
}

#[derive(Debug)]
struct RawSnapshot {
    path: PathBuf,
    stamp: FileStamp,
    bytes: Vec<u8>,
}

/// Non-blocking double-snapshot reader.
///
/// `poll` is called about once a second. The first changed-file observation is
/// held in `pending`; only an identical second observation on a later call is
/// decoded. This avoids sleeping in the local-event ingestion thread and avoids
/// ever treating a truncate/write window as a real progress reset.
pub struct SaveWatcher {
    root: PathBuf,
    running: bool,
    observed: HashMap<PathBuf, FileStamp>,
    /// Stable bytes seen when the game process appeared. We do not know which
    /// slot is active yet, so one baseline is kept per slot. The first slot the
    /// game actually writes identifies itself; its old snapshot can then be
    /// applied immediately before the new one, preserving progress earned
    /// before the game's first delayed save.
    armed_baselines: HashMap<PathBuf, RawSnapshot>,
    pending: Option<RawSnapshot>,
    last_accepted: Option<(PathBuf, Vec<u8>)>,
}

impl SaveWatcher {
    pub fn new() -> Self {
        let root = std::env::var_os("LOCALAPPDATA")
            .map(PathBuf::from)
            .or_else(|| std::env::current_dir().ok())
            .unwrap_or_else(|| PathBuf::from("."))
            .join("Hero_Siege");
        Self::at(root)
    }

    fn at(root: PathBuf) -> Self {
        Self {
            root,
            running: false,
            observed: HashMap::new(),
            armed_baselines: HashMap::new(),
            pending: None,
            last_accepted: None,
        }
    }

    /// Arm the watcher without guessing which of many character slots is live.
    /// Only a file that changes after this baseline can become the active slot.
    pub fn game_started(&mut self) {
        self.arm(None);
    }

    /// Arm the watcher when the tracker joins a game process that was already
    /// running. A character save written after that process began is evidence
    /// from the game itself, not a newest-slot guess. It still has to pass the
    /// same second, identical read in `poll` before anything is emitted.
    pub fn game_started_since(&mut self, started_at: SystemTime) {
        self.arm(Some(started_at));
    }

    fn arm(&mut self, started_at: Option<SystemTime>) {
        self.running = true;
        self.observed = self.current_stamps();
        self.armed_baselines = self
            .observed
            .iter()
            .filter_map(|(path, stamp)| {
                let first = read_once(path).ok()?;
                let second = read_once(path).ok()?;
                (first.stamp == *stamp
                    && first.stamp == second.stamp
                    && first.bytes == second.bytes)
                    .then_some((path.clone(), second))
            })
            .collect();
        self.pending = None;
        self.last_accepted = None;

        let Some(started_at) = started_at else {
            return;
        };
        let latest_reasonable = SystemTime::now()
            .checked_add(Duration::from_secs(60))
            .unwrap_or(SystemTime::now());
        let mut eligible: Vec<_> = self
            .observed
            .iter()
            .filter(|(_, stamp)| {
                stamp
                    .modified
                    .is_some_and(|modified| modified >= started_at && modified <= latest_reasonable)
            })
            .map(|(path, stamp)| (path.clone(), stamp.clone()))
            .collect();
        eligible.sort_by(|(path_a, stamp_a), (path_b, stamp_b)| {
            stamp_a
                .modified
                .cmp(&stamp_b.modified)
                .then_with(|| path_a.cmp(path_b))
        });
        if let Some((selected, _)) = eligible.pop() {
            self.pending = read_once(&selected).ok();
        }
    }

    pub fn game_stopped(&mut self) {
        self.running = false;
        self.observed.clear();
        self.armed_baselines.clear();
        self.pending = None;
        self.last_accepted = None;
    }

    /// Return authoritative progress snapshots after two identical reads.
    ///
    /// The first accepted write may return two ordered events: the stable slot
    /// value captured when the game started, then the value the game just
    /// wrote. This is the only safe time to choose one of many character-slot
    /// baselines, and it prevents the first delayed save from swallowing the
    /// beginning of the session.
    pub fn poll(&mut self) -> Result<Vec<GameEvent>, String> {
        if !self.running {
            return Ok(Vec::new());
        }

        if let Some(first) = self.pending.take() {
            match read_once(&first.path) {
                Ok(second) if first.stamp == second.stamp && first.bytes == second.bytes => {
                    self.observed
                        .insert(second.path.clone(), second.stamp.clone());
                    let same_as_last = self.last_accepted.as_ref().is_some_and(|(path, bytes)| {
                        path == &second.path && bytes == &second.bytes
                    });
                    if same_as_last {
                        return Ok(Vec::new());
                    }
                    let text = decode_hss(&second.bytes)
                        .map_err(|error| format!("{} rejected: {error}", second.path.display()))?;
                    let event = progress_event(&text)
                        .map_err(|error| format!("{} rejected: {error}", second.path.display()))?;

                    let mut events = Vec::with_capacity(2);
                    if self.last_accepted.is_none() {
                        if let Some(baseline) = self.armed_baselines.remove(&second.path) {
                            if baseline.bytes != second.bytes {
                                if let Ok(baseline_text) = decode_hss(&baseline.bytes) {
                                    if let Ok(baseline_event) = progress_event(&baseline_text) {
                                        if same_character(&baseline_event, &event) {
                                            events.push(as_armed_baseline(baseline_event));
                                        }
                                    }
                                }
                            }
                        }
                        // The changed slot has now identified the active
                        // character. Baselines for every other slot are stale
                        // guesses and must never be replayed later.
                        self.armed_baselines.clear();
                    }
                    events.push(event);
                    self.last_accepted = Some((second.path, second.bytes));
                    return Ok(events);
                }
                Ok(second) => {
                    // The game completed another write between the two reads.
                    // Make that newer complete observation the first half of a
                    // new pair instead of accepting either version.
                    self.pending = Some(second);
                    return Ok(Vec::new());
                }
                Err(_) => {
                    // Leave the last known stamp in place. A later successful
                    // read of the changed file will be attempted again.
                    return Ok(Vec::new());
                }
            }
        }

        let now = self.current_stamps();
        self.observed.retain(|path, _| now.contains_key(path));
        let mut changed: Vec<_> = now
            .iter()
            .filter(|(path, stamp)| self.observed.get(*path) != Some(*stamp))
            .map(|(path, stamp)| (path.clone(), stamp.clone()))
            .collect();
        if changed.is_empty() {
            return Ok(Vec::new());
        }

        // If more than one slot changed during a poll interval, the newest
        // filesystem timestamp is the only defensible active-slot signal. Two
        // candidates with that same timestamp are ambiguous; picking by slot
        // number would be deterministic but would not make it true.
        changed.sort_by(|(path_a, stamp_a), (path_b, stamp_b)| {
            stamp_a
                .modified
                .cmp(&stamp_b.modified)
                .then_with(|| path_a.cmp(path_b))
        });
        let latest_modified = changed.last().and_then(|(_, stamp)| stamp.modified);
        let newest_count = changed
            .iter()
            .filter(|(_, stamp)| stamp.modified == latest_modified)
            .count();
        if newest_count != 1 {
            for (path, stamp) in changed {
                self.observed.insert(path, stamp);
            }
            return Ok(Vec::new());
        }
        let (selected, _) = changed.pop().expect("changed is not empty");

        // Do not queue stale changes from other slots on later ticks. They were
        // observed, but only the newest change is eligible to identify the live
        // character.
        for (path, stamp) in changed {
            self.observed.insert(path, stamp);
        }

        match read_once(&selected) {
            Ok(snapshot) => self.pending = Some(snapshot),
            Err(error) => {
                return Err(format!("{} could not be read: {error}", selected.display()));
            }
        }
        Ok(Vec::new())
    }

    fn current_stamps(&self) -> HashMap<PathBuf, FileStamp> {
        candidate_files(&self.root)
            .into_iter()
            .filter_map(|path| {
                fs::metadata(&path)
                    .ok()
                    .filter(|metadata| metadata.is_file())
                    .map(|metadata| (path, FileStamp::from_metadata(&metadata)))
            })
            .collect()
    }
}

fn same_character(left: &GameEvent, right: &GameEvent) -> bool {
    matches!(
        (left, right),
        (
            GameEvent::Account { name: left, .. },
            GameEvent::Account { name: right, .. }
        ) if !left.is_empty() && left.eq_ignore_ascii_case(right)
    )
}

fn as_armed_baseline(mut event: GameEvent) -> GameEvent {
    if let GameEvent::Account { armed_baseline, .. } = &mut event {
        *armed_baseline = true;
    }
    event
}

fn is_character_save(path: &Path) -> bool {
    let Some(name) = path.file_name().and_then(|name| name.to_str()) else {
        return false;
    };
    let lower = name.to_ascii_lowercase();
    lower
        .strip_prefix("herosiege")
        .and_then(|rest| rest.strip_suffix(".hss"))
        .is_some_and(|slot| !slot.is_empty() && slot.bytes().all(|byte| byte.is_ascii_digit()))
}

fn files_in(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut files: Vec<_> = entries
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| is_character_save(path))
        .collect();
    files.sort();
    files
}

fn candidate_files(root: &Path) -> Vec<PathBuf> {
    let nested = files_in(&root.join("hs2saves"));
    if nested.is_empty() {
        files_in(root)
    } else {
        nested
    }
}

fn read_once(path: &Path) -> Result<RawSnapshot, String> {
    let before = fs::metadata(path).map_err(|error| error.to_string())?;
    if !before.is_file() {
        return Err("not a regular file".into());
    }
    if before.len() == 0 {
        return Err("empty save".into());
    }
    if before.len() > MAX_SAVE_BYTES as u64 {
        return Err("raw save exceeds the 4 MiB limit".into());
    }

    // File::open requests read access only. Take one extra byte so a file that
    // grows after metadata was read still cannot bypass the raw-size limit.
    let file = File::open(path).map_err(|error| error.to_string())?;
    let mut reader = file.take((MAX_SAVE_BYTES + 1) as u64);
    let mut bytes = Vec::with_capacity(before.len() as usize);
    reader
        .read_to_end(&mut bytes)
        .map_err(|error| error.to_string())?;
    if bytes.len() > MAX_SAVE_BYTES {
        return Err("raw save exceeds the 4 MiB limit".into());
    }

    let after = fs::metadata(path).map_err(|error| error.to_string())?;
    let before_stamp = FileStamp::from_metadata(&before);
    let after_stamp = FileStamp::from_metadata(&after);
    if before_stamp != after_stamp || bytes.len() as u64 != before.len() {
        return Err("save changed during the read".into());
    }

    Ok(RawSnapshot {
        path: path.to_path_buf(),
        stamp: after_stamp,
        bytes,
    })
}

fn decode_hss(raw: &[u8]) -> Result<String, String> {
    if raw.is_empty() {
        return Err("empty save".into());
    }
    if raw.len() > MAX_SAVE_BYTES {
        return Err("raw save exceeds the 4 MiB limit".into());
    }

    let mut cleaned = Vec::with_capacity(raw.len());
    let mut saw_padding = false;
    for &byte in raw {
        if byte == 0 {
            saw_padding = true;
            continue;
        }
        if byte.is_ascii_whitespace() {
            continue;
        }
        if saw_padding {
            return Err("non-padding data follows a NUL terminator".into());
        }
        cleaned.push(byte);
    }
    if cleaned.is_empty() {
        return Err("empty encoded payload".into());
    }

    let compressed = base64::engine::general_purpose::STANDARD
        .decode(&cleaned)
        .map_err(|error| format!("invalid Base64: {error}"))?;
    let mut decoder: Take<ZlibDecoder<&[u8]>> =
        ZlibDecoder::new(compressed.as_slice()).take((MAX_SAVE_BYTES + 1) as u64);
    let mut obfuscated = Vec::new();
    decoder
        .read_to_end(&mut obfuscated)
        .map_err(|error| format!("invalid zlib stream: {error}"))?;
    if obfuscated.len() > MAX_SAVE_BYTES {
        return Err("decompressed save exceeds the 4 MiB limit".into());
    }
    if obfuscated.is_empty() || !obfuscated.len().is_multiple_of(2) {
        return Err("decoded save is not an even UTF-16-like byte stream".into());
    }

    let decoded: Vec<u8> = obfuscated
        .iter()
        .enumerate()
        .map(|(index, byte)| byte ^ HSS_XOR_KEY[index % HSS_XOR_KEY.len()])
        .collect();
    if decoded[1..].iter().step_by(2).any(|byte| *byte != 0) {
        return Err("decoded save has non-zero high bytes".into());
    }
    let payload: Vec<u8> = decoded.iter().step_by(2).copied().collect();
    let text =
        String::from_utf8(payload).map_err(|error| format!("save text is not UTF-8: {error}"))?;
    Ok(text.replace("\r\n", "\n").replace('\r', "\n"))
}

fn strip_quotes(value: &str) -> &str {
    let value = value.trim();
    value
        .strip_prefix('"')
        .and_then(|inner| inner.strip_suffix('"'))
        .unwrap_or(value)
}

fn parsed_sections(text: &str) -> (HashMap<String, String>, HashMap<String, String>) {
    let mut section = "";
    let mut character = HashMap::new();
    let mut statistics = HashMap::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(name) = line
            .strip_prefix('[')
            .and_then(|line| line.strip_suffix(']'))
        {
            section = name;
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let target = match section {
            "0" => &mut character,
            "stat" => &mut statistics,
            _ => continue,
        };
        target.insert(key.trim().to_string(), strip_quotes(value).to_string());
    }
    (character, statistics)
}

fn number(value: Option<&String>, key: &str) -> Result<i64, String> {
    let Some(value) = value else {
        return Err(format!("missing {key}"));
    };
    let number: f64 = value
        .trim()
        .replace(',', ".")
        .parse()
        .map_err(|_| format!("invalid {key}"))?;
    if !number.is_finite() || number < i64::MIN as f64 || number > i64::MAX as f64 {
        return Err(format!("out-of-range {key}"));
    }
    Ok(number as i64)
}

fn optional_number(fields: &HashMap<String, String>, key: &str) -> Result<i64, String> {
    match fields.get(key) {
        Some(value) => number(Some(value), key),
        None => Ok(0),
    }
}

fn flat_key(key: &str) -> String {
    key.chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .collect::<String>()
        .to_ascii_lowercase()
}

fn progress_event(text: &str) -> Result<GameEvent, String> {
    let (character, statistics) = parsed_sections(text);
    let name = character
        .get("name")
        .map(|value| value.trim())
        .unwrap_or_default();
    if name.is_empty() || matches!(name.to_ascii_lowercase().as_str(), "new char" | "unnamed") {
        return Err("placeholder or missing character name".into());
    }
    let class = number(character.get("class"), "class")?;
    if !(1..=24).contains(&class) {
        return Err("unsupported character class".into());
    }
    let level = number(character.get("level"), "level")?;
    if level <= 0 {
        return Err("invalid character level".into());
    }
    let experience = number(character.get("experience"), "experience")?;

    let mut tallies = HashMap::new();
    for (key, value) in &statistics {
        let flat = flat_key(key);
        if flat.len() > "statistic".len() && flat.starts_with("statistic") {
            tallies.insert(flat, number(Some(value), key)?);
        }
    }
    let kills = statistics
        .get("statisticTotalMonsterKills")
        .map(|value| number(Some(value), "statisticTotalMonsterKills"))
        .transpose()?
        .unwrap_or(0);

    Ok(GameEvent::Account {
        experience,
        has_experience: true,
        armed_baseline: false,
        // Current local saves carry no season or Blood Pact field. `has_mode`
        // prevents these placeholders from selecting or mutating a purse.
        has_mode: false,
        season: 0,
        hardcore: optional_number(&character, "hardcore")?,
        blood_pact: 0,
        name: name.to_string(),
        level,
        herolevel: optional_number(&character, "herolevel")?,
        difficulty: optional_number(&character, "difficulty")?,
        hell_sub: optional_number(&character, "hell_subdifficulty")?,
        // The flattened second element of `act_previous` is the coarse act.
        // `act_*` and `zone*,*` are progression markers, not location.
        act: optional_number(&character, "act_previous_1")?,
        kills,
        tallies,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEMP_ID: AtomicU64 = AtomicU64::new(0);

    fn encode_hss(text: &str) -> Vec<u8> {
        let payload = text.as_bytes();
        let mut utf16_like = vec![0u8; payload.len() * 2];
        for (index, byte) in payload.iter().enumerate() {
            utf16_like[index * 2] = *byte;
        }
        let obfuscated: Vec<u8> = utf16_like
            .iter()
            .enumerate()
            .map(|(index, byte)| byte ^ HSS_XOR_KEY[index % HSS_XOR_KEY.len()])
            .collect();
        let mut encoder = ZlibEncoder::new(Vec::new(), Compression::best());
        encoder.write_all(&obfuscated).unwrap();
        let compressed = encoder.finish().unwrap();
        let mut encoded = base64::engine::general_purpose::STANDARD
            .encode(compressed)
            .into_bytes();
        encoded.push(0);
        encoded
    }

    fn save(name: &str, xp: i64, kills: i64) -> Vec<u8> {
        encode_hss(&format!(
            "[0]\nname=\"{name}\"\nclass=\"13.000000\"\nlevel=\"100.000000\"\nherolevel=\"9.000000\"\nexperience=\"{xp}.000000\"\ndifficulty=\"3.000000\"\nhardcore=\"0.000000\"\nact_previous_0=\"1.000000\"\nact_previous_1=\"9.000000\"\n[stat]\nstatisticTotalMonsterKills=\"{kills}.000000\"\nstatisticUberPhantomLeviathanKills=\"4.000000\"\nstatisticCrystalChestOpened=\"23.000000\"\n"
        ))
    }

    fn account_values(event: &GameEvent) -> (&str, i64, i64) {
        let GameEvent::Account {
            name,
            experience,
            kills,
            ..
        } = event
        else {
            panic!("save watcher emitted a non-account event");
        };
        (name, *experience, *kills)
    }

    fn temp_root() -> PathBuf {
        let id = TEMP_ID.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!("hsot-save-source-{}-{id}", std::process::id()))
    }

    #[test]
    fn synthetic_s10_save_becomes_only_an_account_snapshot() {
        let text = decode_hss(&save("Test Hero", 728_844_020, 91_420)).unwrap();
        let event = progress_event(&text).unwrap();
        let GameEvent::Account {
            name,
            experience,
            has_mode,
            level,
            herolevel,
            difficulty,
            act,
            kills,
            tallies,
            ..
        } = event
        else {
            panic!("save source emitted a non-progress event");
        };
        assert_eq!(name, "Test Hero");
        assert!(
            !has_mode,
            "local saves must not assert a season/currency mode"
        );
        assert_eq!(
            (experience, level, herolevel, difficulty, act, kills),
            (728_844_020, 100, 9, 3, 9, 91_420)
        );
        assert_eq!(tallies["statisticuberphantomleviathankills"], 4);
        assert_eq!(tallies["statisticcrystalchestopened"], 23);
    }

    #[test]
    fn raw_and_decompressed_limits_are_both_enforced() {
        assert!(decode_hss(&vec![b'A'; MAX_SAVE_BYTES + 1])
            .unwrap_err()
            .contains("raw save"));

        let text = "x".repeat(MAX_SAVE_BYTES / 2 + 1);
        let encoded = encode_hss(&text);
        assert!(
            encoded.len() < MAX_SAVE_BYTES,
            "fixture must exercise only the decompressed limit"
        );
        assert!(decode_hss(&encoded)
            .unwrap_err()
            .contains("decompressed save"));
    }

    #[test]
    fn truncated_and_corrupted_layers_fail_closed() {
        let encoded = save("Test Hero", 10, 2);
        for cut in [1, 2, 7, encoded.len() / 2, encoded.len() - 2] {
            assert!(
                decode_hss(&encoded[..cut]).is_err(),
                "cut {cut} unexpectedly decoded"
            );
        }
        let mut internal_nul = encoded.clone();
        internal_nul[8] = 0;
        assert!(decode_hss(&internal_nul).unwrap_err().contains("NUL"));
    }

    #[test]
    fn nested_saves_take_priority_and_require_two_identical_reads() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        fs::write(root.join("herosiege1.hss"), save("Legacy", 10, 1)).unwrap();
        let nested_path = nested.join("herosiege2.hss");
        fs::write(&nested_path, save("Nested", 20, 2)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started();
        assert!(
            watcher.poll().unwrap().is_empty(),
            "baseline must not guess an active slot"
        );

        fs::write(root.join("herosiege1.hss"), save("Legacy", 30, 3)).unwrap();
        assert!(
            watcher.poll().unwrap().is_empty(),
            "legacy root is fallback only"
        );

        fs::write(&nested_path, save("Nested", 40, 4)).unwrap();
        assert!(
            watcher.poll().unwrap().is_empty(),
            "first stable observation must remain pending"
        );
        let events = watcher.poll().unwrap();
        assert_eq!(events.len(), 2, "the armed baseline precedes the new save");
        assert!(matches!(
            events[1],
            GameEvent::Account {
                experience: 40,
                kills: 4,
                ..
            }
        ));
        assert!(
            watcher.poll().unwrap().is_empty(),
            "unchanged saves must be deduplicated"
        );

        watcher.game_stopped();
        assert!(watcher.poll().unwrap().is_empty());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn legacy_root_is_used_only_when_nested_has_no_character_slots() {
        let root = temp_root();
        fs::create_dir_all(&root).unwrap();
        let legacy_path = root.join("herosiege3.hss");
        fs::write(&legacy_path, save("Legacy", 10, 1)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started();
        fs::write(&legacy_path, save("Legacy", 300, 3)).unwrap();
        assert!(watcher.poll().unwrap().is_empty());
        assert!(matches!(
            watcher.poll().unwrap().as_slice(),
            [
                GameEvent::Account { .. },
                GameEvent::Account {
                    experience: 300,
                    kills: 3,
                    ..
                }
            ]
        ));

        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn first_delayed_write_returns_the_armed_baseline_then_current_progress() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        let path = nested.join("herosiege1.hss");
        fs::write(&path, save("Sgham", 270_653, 44_023)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started();
        fs::write(&path, save("Sgham", 0, 44_138)).unwrap();
        assert!(watcher.poll().unwrap().is_empty());
        let events = watcher.poll().unwrap();

        assert_eq!(events.len(), 2);
        assert_eq!(account_values(&events[0]), ("Sgham", 270_653, 44_023));
        assert_eq!(account_values(&events[1]), ("Sgham", 0, 44_138));
        assert!(matches!(
            events.as_slice(),
            [
                GameEvent::Account {
                    armed_baseline: true,
                    ..
                },
                GameEvent::Account {
                    armed_baseline: false,
                    ..
                }
            ]
        ));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn changing_characters_does_not_replay_another_armed_slot_baseline() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        let main = nested.join("herosiege1.hss");
        let alt = nested.join("herosiege2.hss");
        fs::write(&main, save("Main", 1_000, 100)).unwrap();
        fs::write(&alt, save("Alt", 2_000, 200)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started();
        fs::write(&main, save("Main", 1_050, 105)).unwrap();
        assert!(watcher.poll().unwrap().is_empty());
        assert_eq!(watcher.poll().unwrap().len(), 2);

        fs::write(&alt, save("Alt", 2_040, 204)).unwrap();
        assert!(watcher.poll().unwrap().is_empty());
        let events = watcher.poll().unwrap();
        assert_eq!(
            events.len(),
            1,
            "the alt's pre-session save is not replayed"
        );
        assert_eq!(account_values(&events[0]), ("Alt", 2_040, 204));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn equally_new_slot_writes_are_rejected_as_ambiguous() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        let one = nested.join("herosiege1.hss");
        let two = nested.join("herosiege2.hss");
        fs::write(&one, save("One", 10, 1)).unwrap();
        fs::write(&two, save("Two", 20, 2)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started();
        fs::write(&one, save("One", 30, 3)).unwrap();
        fs::write(&two, save("Two", 40, 4)).unwrap();
        let tied = SystemTime::now()
            .checked_add(Duration::from_secs(30))
            .unwrap();
        for path in [&one, &two] {
            File::options()
                .write(true)
                .open(path)
                .unwrap()
                .set_times(std::fs::FileTimes::new().set_modified(tied))
                .unwrap();
        }

        assert!(watcher.poll().unwrap().is_empty());
        assert!(
            watcher.poll().unwrap().is_empty(),
            "a filename tie-break would guess which character is active"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn truncated_write_is_never_emitted_and_a_later_stable_save_recovers() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        let path = nested.join("herosiege1.hss");
        fs::write(&path, save("Safe", 100, 10)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started();
        fs::write(&path, b"truncated base64 payload").unwrap();
        assert!(watcher.poll().unwrap().is_empty());

        fs::write(&path, save("Safe", 150, 15)).unwrap();
        assert!(
            watcher.poll().unwrap().is_empty(),
            "the changed second read only replaces the pending half"
        );
        let events = watcher.poll().unwrap();
        assert_eq!(events.len(), 2);
        assert_eq!(account_values(&events[0]), ("Safe", 100, 10));
        assert_eq!(account_values(&events[1]), ("Safe", 150, 15));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn a_save_written_since_the_game_started_is_bootstrapped_safely() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        fs::write(
            nested.join("herosiege7.hss"),
            save("Already Running", 777, 12),
        )
        .unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started_since(std::time::UNIX_EPOCH);
        assert!(matches!(
            watcher.poll().unwrap().as_slice(),
            [GameEvent::Account {
                experience: 777,
                kills: 12,
                ..
            }]
        ));

        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn a_save_older_than_the_running_game_is_not_guessed_as_active() {
        let root = temp_root();
        let nested = root.join("hs2saves");
        fs::create_dir_all(&nested).unwrap();
        fs::write(nested.join("herosiege8.hss"), save("Old Slot", 88, 8)).unwrap();

        let mut watcher = SaveWatcher::at(root.clone());
        watcher.game_started_since(
            SystemTime::now()
                .checked_add(Duration::from_secs(60))
                .unwrap(),
        );
        assert!(watcher.poll().unwrap().is_empty());

        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn placeholder_inventory_and_unpersisted_runtime_values_are_not_events() {
        let placeholder = encode_hss(
            "[0]\nname=\"New Char\"\nclass=\"1.000000\"\nlevel=\"1.000000\"\nexperience=\"0.000000\"\ninventory=\"not-telemetry\"\nmf=\"999999.000000\"\nroom=\"Act_09_09\"\n",
        );
        let text = decode_hss(&placeholder).unwrap();
        assert!(progress_event(&text).is_err());
    }
}
