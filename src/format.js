// The numbers, said the same way everywhere.
//
// `fmt` was byte-identical in three panels and a fourth copy on the run card
// had quietly drifted into another dialect — `2.4M` and `4.5k` on the picture
// that leaves the app, against `2.40kk` and `4,500` in the panel it was copied
// from, and it turned to `k` at a thousand rather than ten. The card is the
// artefact people paste into chat, so it was the one place the app spoke a
// language of its own.
//
// Hero Siege itself says kk and kkk rather than M and B, which is why these
// are not the SI suffixes a general-purpose helper would reach for.

/** 1234 -> "1,234"; 12345 -> "12.3k"; 1234567 -> "1.23kk". */
export function fmt(n) {
  const v = n ?? 0;
  const abs = Math.abs(v);
  if (abs >= 1e9) return `${(v / 1e9).toFixed(2)}kkk`;
  if (abs >= 1e6) return `${(v / 1e6).toFixed(2)}kk`;
  // below ten thousand the digits still fit, and reading them exactly is
  // worth more than the two characters saved
  if (abs >= 10_000) return `${(v / 1e3).toFixed(1)}k`;
  return v.toLocaleString('en-US');
}

/** Seconds as a running clock: 3661 -> "1:01:01". Hours never wrap. */
export function clock(secs) {
  const s = Math.max(0, Math.floor(secs ?? 0));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `${h}:${String(m).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

/** Seconds as a length of time, for reading rather than watching: "1h 24m". */
export function span(secs) {
  const s = Math.max(0, Math.floor(secs ?? 0));
  // Rounding the remainder gives "60m" for anything from 59m30s: round the
  // whole thing into minutes first, then split, and an hour stays an hour.
  const mins = Math.round(s / 60);
  const h = Math.floor(mins / 60);
  const m = mins % 60;
  if (h && m) return `${h}h ${m}m`;
  if (h) return `${h}h`;
  return `${m}m`;
}

/// The colour each rarity is drawn in. The same five classes are declared in
/// four components' styles; this is the one place that says which is which.
export const RARITY_CLASS = {
  Satanic: 'c-sat',
  Set: 'c-set',
  Heroic: 'c-her',
  Angelic: 'c-ang',
  Unholy: 'c-unh',
};

/// The five the app counts, in the order every panel lists them.
export const RARITIES = ['Satanic', 'Set', 'Heroic', 'Angelic', 'Unholy'];

/// What the game calls the difficulty a character is on.
///
/// The current Season 10 client still ships these four labels in this order in
/// translationsMain.csv, and its save gate accepts difficulty values 0..3.
/// Values outside that verified range stay explicit (`D4`, `D5`, …) instead of
/// being assigned a plausible but unproven name.
export function difficulty(n, hellSub = 0) {
  if (n == null) return null;
  const name = ['Normal', 'Nightmare', 'Hell', 'Inferno'][n] ?? `D${n}`;
  // Hell is five difficulties wearing one name, and the game says which in a
  // field of its own. Only shown on Hell: it carries a value on characters who
  // are not there, and reading it out then would be inventing a fact.
  return name === 'Hell' && hellSub >= 1 && hellSub <= 5 ? `Hell ${hellSub}` : name;
}
