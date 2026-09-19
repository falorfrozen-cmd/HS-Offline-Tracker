// Fetch the pinned third-party headers aurie-producer/ needs to compile, and
// verify every hash before anything is written.
//
//   node scripts/fetch-producer-sdk.mjs                  # fetch/verify into aurie-producer/.sdk/
//   node scripts/fetch-producer-sdk.mjs --root <dir>      # fetch/verify into another directory
//   node scripts/fetch-producer-sdk.mjs --verify-only     # hash what's on disk, download nothing
//
// What it fetches, from aurie-producer/toolchain-pins.json: the seven
// YYToolkit v4.0.1 / Aurie v2.0.2 headers aurie-producer/CMakeLists.txt's
// YYTK_SDK_ROOT needs (see that file's own comment; both are AGPL-3.0 and
// never ours to commit -- see .gitignore). These are exactly the
// plugin_build/include/ entries of ForgePact/tools/toolchain-pins.json, dest
// rewritten relative to this repository's own fetch root -- the two pin
// files must never disagree about what these headers are.
//
// **All-or-nothing.** Every hash is verified before any file is written; a
// single bad hash writes nothing at all and exits 1, naming the dest and
// both the expected and the actual hash. A dest that already exists with the
// pinned hash is left alone (printed "ok (already present)"); one that
// exists with a different hash is refused -- exit 1, nothing written.
//
// `--verify-only` hashes whatever is under --root against the manifest,
// downloads nothing, and exits 1 on the first missing or mismatched file --
// an empty root is therefore always a failure, the negative control that
// proves this flag actually checks something.
//
// `run()` takes a `fetcher: (url) => Promise<Buffer>` in place of the
// network, so tests/fetch-producer-sdk.test.mjs needs no network.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
export const MANIFEST_PATH = join(root, 'aurie-producer', 'toolchain-pins.json');
export const DEFAULT_ROOT = join(root, 'aurie-producer', '.sdk');

export function sha256Hex(data) {
  return createHash('sha256').update(data).digest('hex');
}

/** No absolute path, no `..` component -- a manifest `dest` writes only under
 * the caller's root, never outside it. */
function destIsSafe(dest) {
  if (!dest) return false;
  const normalized = dest.replace(/\\/g, '/');
  if (normalized.startsWith('/')) return false;
  if (normalized.length >= 2 && normalized[1] === ':') return false; // a drive letter, e.g. C:
  const parts = normalized.split('/');
  return !parts.includes('') && !parts.includes('..') && !parts.includes('.');
}

export async function nodeFetcher(url) {
  const response = await fetch(url, { headers: { 'User-Agent': 'HS-Offline-Tracker-fetch-producer-sdk' } });
  if (!response.ok) {
    throw new Error(`${url}: HTTP ${response.status}`);
  }
  return Buffer.from(await response.arrayBuffer());
}

function verifyOnly(manifest, rootDir) {
  let ok = true;
  const lines = [];
  for (const entry of manifest.files) {
    const dest = join(rootDir, entry.dest);
    if (!existsSync(dest)) {
      lines.push(`${entry.dest}  -  MISSING (not present under ${rootDir})`);
      ok = false;
      continue;
    }
    const actual = sha256Hex(readFileSync(dest));
    if (actual !== entry.sha256) {
      lines.push(`${entry.dest}  ${actual}  MISMATCH (expected ${entry.sha256})`);
      ok = false;
      continue;
    }
    lines.push(`${entry.dest}  ${actual}  ok`);
  }
  return { ok, lines };
}

/**
 * @param {object} manifest - parsed toolchain-pins.json
 * @param {object} options
 * @param {string} options.root - directory to fetch/verify into
 * @param {(url: string) => Promise<Buffer>} options.fetcher
 * @param {boolean} [options.verifyOnly]
 * @returns {Promise<{ok: boolean, lines: string[]}>}
 */
export async function run(manifest, { root: rootDir, fetcher, verifyOnly: verifyOnlyMode = false }) {
  for (const entry of manifest.files) {
    if (!destIsSafe(entry.dest)) {
      return { ok: false, lines: [`${entry.dest}  -  REFUSED (unsafe destination)`] };
    }
  }

  if (verifyOnlyMode) {
    return verifyOnly(manifest, rootDir);
  }

  const toWrite = [];
  const statuses = [];
  let failed = false;

  for (const entry of manifest.files) {
    const dest = join(rootDir, entry.dest);

    if (existsSync(dest)) {
      const existingHash = sha256Hex(readFileSync(dest));
      if (existingHash === entry.sha256) {
        statuses.push(`${entry.dest}  ${existingHash}  ok (already present)`);
        continue;
      }
      statuses.push(
        `${entry.dest}  ${existingHash}  REFUSED (pin wants ${entry.sha256}; remove the file to re-fetch it)`,
      );
      failed = true;
      continue;
    }

    let data;
    try {
      data = await fetcher(entry.url);
    } catch (err) {
      statuses.push(`${entry.dest}  -  FETCH FAILED (${err.message})`);
      failed = true;
      continue;
    }
    const actual = sha256Hex(data);
    if (actual !== entry.sha256) {
      statuses.push(`${entry.dest}  ${actual}  MISMATCH (expected ${entry.sha256})`);
      failed = true;
      continue;
    }
    toWrite.push([dest, data]);
    statuses.push(`${entry.dest}  ${actual}  ok`);
  }

  if (failed) {
    return { ok: false, lines: [...statuses, 'nothing written -- fix the mismatches above and re-run'] };
  }

  for (const [dest, data] of toWrite) {
    mkdirSync(dirname(dest), { recursive: true });
    writeFileSync(dest, data);
  }

  return { ok: true, lines: statuses };
}

async function main(argv) {
  let rootDir = DEFAULT_ROOT;
  let verifyOnlyMode = false;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--root') {
      rootDir = argv[++i];
    } else if (argv[i] === '--verify-only') {
      verifyOnlyMode = true;
    } else {
      console.error(`unrecognised argument: ${argv[i]}`);
      return 1;
    }
  }

  const manifest = JSON.parse(readFileSync(MANIFEST_PATH, 'utf8'));
  const { ok, lines } = await run(manifest, { root: rootDir, fetcher: nodeFetcher, verifyOnly: verifyOnlyMode });
  for (const line of lines) console.log(line);
  return ok ? 0 : 1;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  main(process.argv.slice(2)).then((code) => process.exit(code));
}
