// One way in for everything the windows ask of the backend.
//
// There used to be two sides to this: the app's own windows, where Tauri is
// there and every command works, and a page served to OBS as a Browser Source,
// where none of it exists. The served page is gone — it went with the little
// HTTP server that fed it — so what is left is the one door, and the guards
// that used to choose between two.
//
// The guards stay. `native` is what tells a Tauri window from anything else,
// and the panels are drawn by a webview either way: a component that calls a
// command while it is being rendered somewhere without one should get nothing
// back, not an exception in a transparent window nobody can see fail.

import { invoke as tauriInvoke } from '@tauri-apps/api/core';
import { listen as tauriListen } from '@tauri-apps/api/event';
import { getCurrentWindow } from '@tauri-apps/api/window';

/// Tauri puts this on the window before any of our code runs.
export const native = typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;

const line = (total, earned, per_hour) => ({ total, earned, per_hour });
const item = (total, mf, per_hour) => ({ total, mf, per_hour });
let DEMO_SETTINGS = {
  satanic: { enabled: true, volume: .72 }, set: { enabled: true, volume: .66 },
  heroic: { enabled: true, volume: .62 }, angelic: { enabled: true, volume: .76 },
  unholy: { enabled: true, volume: .7 }, mail: { enabled: true, volume: .5 },
  zone: { enabled: true, volume: .55 }, zone_buffs: [],
  alerts: ['Satanic', 'Set', 'Heroic', 'Angelic', 'Unholy'], min_tier: 1,
  notable: [], filters: [], filter: '', use_filter: true, lists: [], locked: false,
  opacity: 1, scale: 1, auto_show: true, autostart: false, ticker: false,
  debug_log: false, sound_on_ground: true, theme: 'obsidian',
  flourish: true, flourish_scale: 1, flourish_shade: .55, flourish_secs: 4,
  flourish_rarities: ['Satanic', 'Set', 'Heroic', 'Angelic', 'Unholy'],
  flourish_tier: 1, flourish_listed: true, flourish_zone: true,
  flourish_always: false, discord: false, compact: false, ghost: false,
  x11_backend: false, hidden: [],
};

let DEMO_SNAPSHOT = {
  status: 'offline-live|184', session_secs: 3672, paused: false,
  save_age_secs: 2, bank_age_secs: 3, carried_bank: false, carried_totals: false,
  has_mail: true,
  gold: line(183_942_810, 14_862_340, 14_571_230),
  xp: line(728_844_020, 42_184_600, 41_356_941),
  kills: line(91_420, 4_862, 4_766),
  resources: { keys: 42, materials: 318, socketables: 74, collectibles: 16 },
  notable: [{ label: 'Angelic Key', total: 5 }, { label: 'SS runes', total: 3 }],
  items: {
    Satanic: item(91, 38, 89), Set: item(17, 9, 17), Heroic: item(8, 3, 8),
    Angelic: item(5, 2, 5), Unholy: item(2, 1, 2),
  },
  satanic_zone: { zone: 'Satanic_9_3', buffs: [6, 14, 21], debuffs: [3, 12] },
  satanic_at: Date.now() - 11 * 60_000, room: 'Act_09_03', act: 9,
  mf: 12840, satanic_here: true,
  character: { name: 'Shaman', level: 100, herolevel: 300, difficulty: 6, hell_sub: 5, hardcore: false, season: 10 },
  tallies: [
    { label: 'Uber Phantom Leviathan', group: 'boss', total: 4 },
    { label: 'Chaos Tower floors', group: 'boss', total: 18 },
    { label: 'Crystal', group: 'chest', total: 23 },
  ],
  ss: 7,
};

const now = Date.now();
const DEMO_EXTRA = {
  character: DEMO_SNAPSHOT.character,
  series: [
    { t: 0, gold: 0, xp: 0 }, { t: 900, gold: 2_600_000, xp: 8_400_000 },
    { t: 1800, gold: 6_900_000, xp: 20_600_000 }, { t: 2700, gold: 10_800_000, xp: 31_700_000 },
    { t: 3600, gold: 14_862_340, xp: 42_184_600 },
  ],
  drops: [
    { ts_ms: now - 42_000, rarity: 'Angelic', mf: true, tier: 6, item_type: 3, item_id: 1, weapon_type: 0, seed: 91, name: 'Astral Covenant', announced: false, ground: true, zone: null, room: 'Act_09_03', sound: 'angelic', announce: true, flourish: true },
    { ts_ms: now - 188_000, rarity: 'Unholy', mf: false, tier: 5, item_type: 3, item_id: 2, weapon_type: 0, seed: 77, name: 'Oath of the Hollow Star', announced: false, ground: true, zone: null, room: 'Act_09_03', sound: 'unholy', announce: true, flourish: true },
    { ts_ms: now - 420_000, rarity: 'Heroic', mf: true, tier: 5, item_type: 3, item_id: 3, weapon_type: 0, seed: 48, name: 'Soulforged Ring', announced: false, ground: true, zone: null, room: 'Act_09_03', sound: 'heroic', announce: true, flourish: false },
  ],
};

async function demoInvoke(command, args = {}) {
  switch (command) {
    case 'snapshot': return structuredClone(DEMO_SNAPSHOT);
    case 'get_extra': return structuredClone(DEMO_EXTRA);
    case 'get_settings': return structuredClone(DEMO_SETTINGS);
    case 'save_settings': DEMO_SETTINGS = structuredClone(args.settings); return null;
    case 'session_info': return { overlay: true, wayland: false, through_x11: false, can_switch: false, tray: true, discord: false };
    case 'about': return { version: '0.1.0', platform: 'windows', repo: '', overlay_w: 444, overlay_h: 147, binary: '', appimage: false };
    case 'get_runs': return [];
    case 'get_shopping': return [];
    case 'sound_status': return null;
    case 'set_paused': DEMO_SNAPSHOT.paused = !!args.paused; return null;
    case 'reset_stats': DEMO_SNAPSHOT.session_secs = 0; return null;
    default: return null;
  }
}

export async function invoke(command, args) {
  if (!native) return demoInvoke(command, args);
  return tauriInvoke(command, args);
}

export async function listen(name, handler) {
  if (!native) return () => {};
  return tauriListen(name, handler);
}

/// What the windows remember between sessions, and what to do when they cannot.
///
/// `localStorage` is not guaranteed to work. A WebView2 profile that is damaged,
/// read-only or out of quota throws on the first touch of it, and this app read
/// it at module scope, before the interface was mounted: the throw took the
/// whole module with it. Every window here is transparent and has no frame, so
/// what was left on screen was an invisible rectangle with no close button and
/// no drag region — a window that answers no click and can only be ended in the
/// task manager. A remembered tab is not worth that.
export function recall(key) {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

export function remember(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // A preference that cannot be written is a preference that lasts one
    // session. The window stays up, which is the part that matters.
  }
}

/// The window itself — minimise, drag, resize. Without Tauri under it there is
/// no window to speak of, so it gets one that politely does nothing.
const NOTHING = {
  minimize() {},
  hide() {},
  setFocus() {},
  startDragging() {},
  startResizeDragging() {},
  label: 'none',
};

export function appWindow() {
  // the import is harmless anywhere; it is the call that needs Tauri under it
  return native ? getCurrentWindow() : NOTHING;
}
