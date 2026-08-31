// Deterministic offline sensor replay for UI and release verification.
// It writes the same public protocol the native bridge uses; no game process,
// memory access or network connection is involved.

import { appendFile, mkdir, readFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fixture = join(root, 'fixtures', 'demo-session.ndjson');
const target =
  process.env.HS_OFFLINE_TRACKER_EVENTS ||
  join(process.env.LOCALAPPDATA || process.env.XDG_DATA_HOME || root, 'HS Offline Tracker', 'events.ndjson');
const delay = Math.max(0, Number.parseInt(process.env.HS_OFFLINE_TRACKER_REPLAY_DELAY || '180', 10));

await mkdir(dirname(target), { recursive: true });
const lines = (await readFile(fixture, 'utf8')).split(/\r?\n/).filter(Boolean);
console.log(`Replaying ${lines.length} local events -> ${target}`);
for (const line of lines) {
  await appendFile(target, `${line}\n`, 'utf8');
  if (delay) await new Promise((resolveDelay) => setTimeout(resolveDelay, delay));
}
console.log('Replay complete.');
