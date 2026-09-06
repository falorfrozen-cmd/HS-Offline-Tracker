import { convertFileSrc } from '@tauri-apps/api/core';
import { invoke, native } from './bridge.js';

export const RARITIES = ['satanic', 'set', 'heroic', 'angelic', 'unholy', 'mail', 'zone'];

// These files are prepared reproducibly by scripts/generate-alerts.mjs from
// documented project-generated and CC0 sources. See assets/sounds/SOURCES.md.
export const DEFAULTS = {
  satanic: new URL('./assets/sounds/satanic.wav', import.meta.url).href,
  set: new URL('./assets/sounds/set.wav', import.meta.url).href,
  heroic: new URL('./assets/sounds/heroic.wav', import.meta.url).href,
  angelic: new URL('./assets/sounds/angelic.wav', import.meta.url).href,
  unholy: new URL('./assets/sounds/unholy.wav', import.meta.url).href,
  mail: new URL('./assets/sounds/mail.wav', import.meta.url).href,
  zone: new URL('./assets/sounds/zone.wav', import.meta.url).href,
};

export async function soundUrl(key) {
  if (native) {
    try {
      const path = await invoke('sound_path', { rarity: key });
      if (path) {
        // The file is always filed under the same name, so the URL alone never
        // changed when the player picked another sound — and the webview's
        // cache kept handing back the first pick. The stamp makes each file
        // its own URL; a cleared sound has no path and falls to the default.
        const stamp = await invoke('sound_stamp', { rarity: key }).catch(() => null);
        const url = `${convertFileSrc(path)}${stamp ? `?v=${stamp}` : ''}`;
        if (await loadable(url)) return url;
        const inlined = await invoke('load_sound', { rarity: key });
        if (inlined && (await loadable(inlined))) return inlined;
      }
    } catch {}
  }
  // A list without its own file must be resolved by the actual item's rarity.
  // Returning Satanic here made the caller's rarity fallback unreachable.
  return DEFAULTS[key] ?? null;
}

function loadable(url) {
  return new Promise((resolve) => {
    const probe = new Audio();
    let settled = false;
    const done = (ok) => {
      if (settled) return;
      settled = true;
      probe.oncanplay = probe.onerror = null;
      probe.removeAttribute('src');
      resolve(ok);
    };
    probe.preload = 'metadata';
    probe.oncanplay = () => done(true);
    probe.onerror = () => done(false);
    probe.src = url;
    setTimeout(() => done(false), 2000);
  });
}

let sharedContext;
let master;
const buffers = new Map();
const voices = [];
const PRIORITY = { mail: 0, set: 1, satanic: 2, heroic: 3, angelic: 4, unholy: 5, zone: 6 };
const MAX_VOICES = 2;

function context() {
  const AudioContext = window.AudioContext || window.webkitAudioContext;
  if (!AudioContext) return null;
  if (!sharedContext || sharedContext.state === 'closed') {
    sharedContext = new AudioContext({ latencyHint: 'interactive' });
    const compressor = sharedContext.createDynamicsCompressor();
    compressor.threshold.value = -18;
    compressor.knee.value = 18;
    compressor.ratio.value = 8;
    compressor.attack.value = 0.003;
    compressor.release.value = 0.18;
    master = sharedContext.createGain();
    master.gain.value = 0.86;
    master.connect(compressor);
    compressor.connect(sharedContext.destination);
  }
  return sharedContext;
}

async function activeContext() {
  const ctx = context();
  if (!ctx) return null;
  if (ctx.state === 'suspended') await ctx.resume();
  return ctx.state === 'running' ? ctx : null;
}

function bufferFor(url) {
  if (!url) return Promise.resolve(null);
  if (!buffers.has(url)) {
    buffers.set(url, (async () => {
      try {
        const ctx = context();
        if (!ctx) return null;
        // Bundled defaults never change; a custom file is read fresh, its
        // URL says which version is wanted.
        const response = await fetch(url, { cache: url.includes('?v=') ? 'no-store' : 'force-cache' });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await ctx.decodeAudioData(await response.arrayBuffer());
      } catch (error) {
        console.warn('Alert sound could not be decoded:', error);
        return null;
      }
    })());
  }
  return buffers.get(url);
}

export async function preload(urls = Object.values(DEFAULTS)) {
  await Promise.allSettled([...new Set(urls.filter(Boolean))].map(bufferFor));
}

export function invalidate(url) {
  if (url) buffers.delete(url);
}

function removeVoice(voice) {
  const index = voices.indexOf(voice);
  if (index >= 0) voices.splice(index, 1);
  try { voice.source.disconnect(); } catch {}
  try { voice.gain.disconnect(); } catch {}
}

function fadeAndStop(voice, ctx) {
  try {
    const now = ctx.currentTime;
    voice.gain.gain.cancelScheduledValues(now);
    voice.gain.gain.setValueAtTime(Math.max(0.0001, voice.gain.gain.value), now);
    voice.gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.025);
    voice.source.stop(now + 0.03);
  } catch {}
}

function start(buffer, volume, key) {
  const ctx = context();
  if (!ctx || !master || !buffer) return false;
  const priority = PRIORITY[key] ?? 7; // custom lists outrank the generic rarity
  while (voices.length >= MAX_VOICES) {
    const victim = voices.reduce((lowest, voice) =>
      voice.priority < lowest.priority ? voice : lowest, voices[0]);
    if (victim.priority >= priority) return false;
    fadeAndStop(victim, ctx);
    removeVoice(victim);
  }

  const source = ctx.createBufferSource();
  const gain = ctx.createGain();
  gain.gain.value = Math.min(1, Math.max(0, volume));
  source.buffer = buffer;
  source.connect(gain);
  gain.connect(master);
  const voice = { source, gain, priority };
  voices.push(voice);
  source.onended = () => removeVoice(voice);
  source.start();
  return true;
}

async function playDecoded(url, volume, fallbackKey, priorityKey) {
  const ctx = await activeContext();
  if (!ctx) return false;
  const fallback = DEFAULTS[fallbackKey] ?? DEFAULTS.satanic;
  let decoded = await bufferFor(url);
  if (!decoded && url !== fallback) decoded = await bufferFor(fallback);
  return start(decoded, volume, priorityKey);
}

// Tests and settings previews call this directly. Live drops are additionally
// coalesced in App.svelte before reaching this two-voice, limited mixer.
export function play(url, volume = 0.7, fallbackKey = 'satanic', priorityKey = fallbackKey) {
  return playDecoded(url, volume, fallbackKey, priorityKey).catch((error) => {
    console.warn('Alert sound could not play:', error);
    return false;
  });
}
