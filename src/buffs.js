const glyph = (mark = '✦', hue = '#59d6c0') =>
  `data:image/svg+xml,${encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><radialGradient id="g"><stop stop-color="${hue}" stop-opacity=".45"/><stop offset="1" stop-color="#07101a"/></radialGradient></defs><rect x="3" y="3" width="58" height="58" rx="14" fill="url(#g)" stroke="${hue}" stroke-width="2"/><text x="32" y="41" text-anchor="middle" font-family="Segoe UI Symbol, sans-serif" font-size="22" font-weight="700" fill="#edf7ff">${mark}</text></svg>`)}`;

// Compact-mode rarity marks use paths instead of font glyphs.  That keeps the
// silhouettes identical on every WebView/font installation and lets the two
// adjacent counters remain recognisable without relying on colour alone.
const sigil = (body, hue) =>
  `data:image/svg+xml,${encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><radialGradient id="g"><stop stop-color="${hue}" stop-opacity=".42"/><stop offset="1" stop-color="#07101a"/></radialGradient></defs><rect x="3" y="3" width="58" height="58" rx="14" fill="url(#g)" stroke="${hue}" stroke-width="2"/>${body}</svg>`)}`;

const ICONS = {
  xp: glyph('XP', '#72b8ff'),
  time: glyph('◷', '#59d6c0'),
  mf: glyph('MF', '#d6a64c'),
  mail_0: glyph('✉', '#70849a'),
  mail_1: glyph('✉', '#f0c66e'),
  chest: glyph('◇', '#d6a64c'),
  satanic: sigil(
    '<path d="M16 17c6 1 10 5 12 10l4-9 4 9c2-5 6-9 12-10-1 8-4 13-8 16 0 9-3 15-8 18-5-3-8-9-8-18-4-3-7-8-8-16Z" fill="#ff5d72" stroke="#ffd1d8" stroke-width="1.5" stroke-linejoin="round"/><path d="m25 36 7 7 7-7" fill="none" stroke="#410b15" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>',
    '#ff5d72',
  ),
  heroic: sigil(
    '<path d="m32 13 17 7v12c0 11-7 18-17 22-10-4-17-11-17-22V20l17-7Z" fill="#54e0ce" stroke="#c9fff8" stroke-width="1.5" stroke-linejoin="round"/><path d="m22 33 7 7 13-15" fill="none" stroke="#073b39" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>',
    '#54e0ce',
  ),
  set: sigil(
    '<path d="m20 18 12 12-12 12L8 30l12-12Zm24 4 10 10-10 10-10-10 10-10Z" fill="#5ee35e" stroke="#dcffdc" stroke-width="1.5" stroke-linejoin="round"/><path d="M26 30h12" fill="none" stroke="#123d17" stroke-width="4" stroke-linecap="round"/>',
    '#5ee35e',
  ),
};
const icon = (name) => ICONS[name] ?? glyph('✦');
const defaultBuffIcon = glyph('✦', '#ad72ff');

// Magnitudes: the game keeps the numeric part of a satanic buff in protected
// values (ReturnSatanicZoneBuffs reads gDataProtected[0xcd..0xe3]); the ones
// below were read live on 2026-09-04 (Hero Siege 7.0.6.0): Rune Master 15+5,
// Gold Hunger 40+8.75, Heroic 3+3, Angelic 4+6, Goblin's Greed 0.5, Artifact
// Digger 155+5, Seeker 210+10, Excavator 370+20, Recruit 10+2.5, Combat
// Training 15+3.75, Battle Scarred 20+5. Re-read them after a balance patch
// with NetProbe: `gpv 205` .. `gpv 227`.
// id → [name, description]; order matches the game's buff ids
const BUFFS = {
  1: ['Loot Goblin I', '+1 Maximum Loot from Enemy Killed', 'sz_buff_loot'],
  2: ['Loot Goblin II', '+2 Maximum Loot from Enemy Killed', 'sz_buff_loot'],
  3: ['Rune Master', 'Rune Drop Chance increased by 15% + (5% per sub difficulty level)', 'sz_buff_runes'],
  4: ['Gold Hunger', 'Gold from monster kills increased by 40% + (8.75% per sub difficulty level)', 'sz_buff_gold'],
  5: ['Heroic Windfall', 'Heroic Item drop chances increased by 3% + (3% per sub difficulty level)', 'sz_buff_heroic'],
  6: ['Angelic Fortune', 'Angelic Item drop chances increased by 4% + (6% per sub difficulty level)', 'sz_buff_angelic'],
  7: ['Zephy’s Grace', 'Movement Speed increased by 50%', 'sz_buff_zephy'],
  8: ['Fury of Tempest', 'Attack Speed increased by 60%', 'sz_buff_fury_of_tempest'],
  9: ['Rapid Casting', 'Faster Cast Rate increased by 60%', 'sz_buff_cast_rate'],
  10: ['Onslaught', 'Attack Damage increased by 100%', 'sz_buff_onslaught'],
  11: ['Nether Surge', 'Magic Skill Damage increased by 40%', 'sz_buff_nether_surge'],
  12: ['Relic Keepers', 'Ancient monsters have a chance to drop a relic on death', 'sz_buff_relics'],
  13: ['Goblin’s Greed', 'Champion+ monsters have a 0.5% chance to summon a Treasure Goblin on death', 'sz_buff_goblin'],
  14: ['Artifact Digger', 'Magic Find increased by 155% + (5% per sub difficulty level)', 'sz_buff_mf'],
  15: ['Artifact Seeker', 'Magic Find increased by 210% + (10% per sub difficulty level)', 'sz_buff_mf'],
  16: ['Artifact Excavator', 'Magic Find increased by 370% + (20% per sub difficulty level)', 'sz_buff_mf'],
  17: ['Recruit', '+10% Experience Gain + (2.5% per sub difficulty level)', 'sz_buff_combat_training'],
  18: ['Combat Training', '+15% Experience Gain + (3.75% per sub difficulty level)', 'sz_buff_combat_training'],
  19: ['Battle Scarred', '+20% Experience Gain + (5% per sub difficulty level)', 'sz_buff_combat_training'],
  20: ['Clairvoyance', 'All recovery increased by 100% (Includes: Mana per hit, Life per hit, Mana and Life Replenish etc)', 'sz_buff_clairvoyance'],
  21: ['Aftermath', 'Monsters have a 3% chance to summon a Legion version of them on death', 'sz_buff_aftermath'],
  22: ['Deep Cuts', 'Critical Strike damage increased by 200%', 'sz_buff_deep_cuts'],
  23: ['Old Town', '+15% chance for Ancient Packs', 'sz_buff_ancient_pack'],
  24: ['Terror Zone', '+25% chance for Ancient Packs', 'sz_buff_ancient_pack_2'],
  25: ['Fields of Carnage', '+30% chance for Ancient Packs', 'sz_buff_ancient_pack_2'],
};

// id → [name, description], in the game's own order.
//
// This was a list of 25 read at `id - 2`, with an override for id 1 — an offset
// fitted to the ids that had been seen rather than taken from the game. The
// 2026-08-21 patch inserted Lifeflow Starvation in the middle of the id space,
// and Sundered Armor had never been in the right place to begin with. Five ids
// named a different, entirely plausible debuff, and nothing said so: a fitted
// offset never reports that it is out of range, it just answers wrongly.
//
// The order is the game's `satanicDebuff*` rows, which is also the order of the
// case bodies in its own map screen: id N is the Nth entry.
// Names and wording straight from the game's translation table
// (translationsAttributes.csv: satanicDebuff* for the name, debuff_* for the
// effect), in the game's own key order: id N is the Nth entry. No magnitudes:
// the game hands those to BuffAdd from a table of its own, and the numbers
// once written here by hand had drifted from it (Absolute Limbo said 50 while
// the game said 25).
const DEBUFFS_LIST = [
  ['Dusk\u2019s Shroud', 'Light Radius decreased'],
  ['Elemental Erosion', 'All Resistances decreased'],
  ['Sundered Armor', 'Damage Taken increased'],
  ['Vitality Drain', 'Life decreased'],
  ['Essence Drain', 'Mana decreased'],
  ['Abyssal Gloom', 'Darkness increased'],
  ['Skill Debilitation', 'All Skills decreased'],
  ['Weakening Essence', 'All Attributes decreased'],
  ['Lifeflow Starvation', 'Life and Mana replenish decreased'],
  ['Sanguine Impairment', 'Life Steal decreased'],
  ['Arcane Impairment', 'Mana Steal decreased'],
  ['Consumed Time', 'Cooldown Recovery reduced'],
  ['Absolute Limbo', 'Cooldown Recovery reduced (the stronger tier)'],
  ['Boulder Fall', 'Monsters have a chance to drop a boulder from the sky when killed'],
  ['Lingering Evil', 'Movement Speed decreased'],
  ['Fatal Wounds', 'Monster Critical Strike Damage increased'],
  ['Bloated Veins', 'Monster life increased'],
  ['Abnormal Dwelling', 'Monster life increased (the stronger tier)'],
  ['Colossal Bloating', 'Monster life increased (the strongest tier)'],
  ['Necrosis', 'Life drained every second'],
  ['Venomous Presence', 'Poison Length increased'],
  ['Flaming Agony', 'Monsters unleash a Fire Nova when killed'],
  ['Unholy Agility', 'Monster Movement Speed and Attack Speed increased'],
  ['Broken Armor', 'Block Rating reduced'],
  ['Hemorrhage', 'Monster attacks inflict a bleed that stacks 20 times'],
  ['Crippling Slow', 'Monster attacks inflict a slow for 2 seconds'],
];

export function debuffInfo(id) {
  const d = DEBUFFS_LIST[id - 1];
  if (!d) return { name: `Unknown Debuff ${id}`, desc: '' };
  return { name: d[0], desc: d[1] };
}

const ZONES = {
  1: ['Outskirts of Inoya', 'Fields of Battle', 'The Pumpkin Patch', 'Woodhill Plains', "King's Garden", 'Witching River'],
  2: ['Crystal Village', 'Chilling Lake', 'Arctic Tundra', 'Snowy Mountains', 'The Glacial Trail'],
  3: ['Corrupted Oasis', 'Dry Hills', "Mos'Arathim Desert", 'Pyramid Level 1', 'Pyramid Level 2', 'Curacan Hollow'],
  4: ['Old Mining Village', 'The Highland Mines', 'Corrupted Cave', 'The Nightmare', "The Devil's Breach"],
  5: ['Mt. Fuji', 'Misty Swamp', 'Fuji Coast', 'Sea of Karponia', 'Temple of Zamjo'],
  6: ['Highland Graveyard', 'The Cathedral', 'Prison Dungeon', 'Steam Train', 'The Depths of Hell'],
  7: ['Deep Space', 'Event Horizon', 'The Black Hole', 'Parallel Dimension', 'Subconscious Mind', 'Shattered Realm'],
  8: ['Forest of the Slain', 'Flooded Plains', 'Forgotten Caves', 'Camp of Souls', 'Helheim'],
  9: ['Abyss Jungle', 'Shipwreck Cove', 'Tormented Reef', 'Boreal Island', 'Volcanic Island', 'Abyss Realm'],
};

export function buffInfo(id) {
  const b = BUFFS[id];
  const ic = glyph(String(id), id % 3 === 0 ? '#d6a64c' : id % 2 === 0 ? '#59d6c0' : '#ad72ff');
  if (!b) return { name: `Unknown Buff ${id}`, desc: '', icon: ic };
  return { name: b[0], desc: b[1], icon: ic };
}

/// Every buff in the table, in id order, ready to draw — the picker on the
/// Alerts page and nothing else. Derived rather than written out a second time:
/// a hand-kept copy is a thing to forget when the game adds a buff, and what it
/// would cost is an alert that never fires for a buff nobody deselected.
export const ALL_BUFFS = Object.keys(BUFFS)
  .map(Number)
  .sort((a, b) => a - b)
  .map((id) => ({ id, ...buffInfo(id) }));

export { defaultBuffIcon };

// "Satanic_5_5" → "Act 5 : Temple of Zamjo"
export function zoneName(raw) {
  const parts = String(raw).split('_');
  if (parts.length >= 3 && /^\d+$/.test(parts[1]) && /^\d+$/.test(parts[2])) {
    const act = +parts[1];
    const idx = +parts[2];
    const names = ZONES[act];
    if (names && idx >= 1 && idx <= names.length) return `Act ${act} : ${names[idx - 1]}`;
  }
  return String(raw);
}

// Which act a satanic zone belongs to: "SZ_8_2" is act 8. Zero for anything
// that does not name one — the announcement is the only place this string comes
// from, but it has been spelled three ways across patches.
export function zoneAct(raw) {
  const act = Number(String(raw ?? '').split('_')[1]);
  return Number.isInteger(act) && act > 0 ? act : 0;
}

export { icon };
