// A repository-local check that ties the aurie-loader/ binaries this
// repository ships to a checked-in manifest, and that every bundle resource
// tauri.conf.json names actually exists on disk. Neither the Tracker nor its
// installer verifies the bytes of `aurie-loader/YYToolkit.dll` today: a wrong
// or partial binary would only surface as a fail-closed sensor or a late
// Tauri bundling error naming a resource path, not the file itself. This
// closes that gap without a network fetch or a build-time dependency — Node
// and node:crypto are already prerequisites (see
// docs/submodules/HS-Offline-Tracker/instructions.md, "Bundled Loader &
// Modified YYToolkit").
//
//   node scripts/verify-loader.mjs                      # check this checkout
//   node scripts/verify-loader.mjs --loader-dir <path>   # check the manifest's
//     binaries inside another aurie-loader/-shaped directory instead — used by
//     this script's own negative-control test, not by `npm run check`
//
// Wired into `npm run check` as `npm run loader:verify`.

import { readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

let loaderDir = join(root, 'aurie-loader');
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--loader-dir') {
    loaderDir = resolve(args[++i]);
  }
}

function sha256(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

const failures = [];
const checked = [];

// 1. Every binary the manifest lists must exist under loaderDir and match
// the checked-in hash.
const manifestPath = join(root, 'aurie-loader', 'loader-manifest.json');
const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
for (const entry of manifest.files) {
  const filePath = join(loaderDir, entry.path);
  if (!existsSync(filePath)) {
    failures.push(`${entry.path}: not found at ${filePath}`);
    continue;
  }
  const actual = sha256(filePath);
  if (actual !== entry.sha256.toLowerCase()) {
    failures.push(
      `${entry.path}: sha256 ${actual} does not match the manifest's ${entry.sha256} (${filePath})`,
    );
    continue;
  }
  checked.push(entry.path);
}

// 2. Every aurie-loader/ source path tauri.conf.json's bundle.resources
// lists must exist on disk. Scoped to aurie-loader/ rather than every
// resource: other entries (the producer DLL, most of all) are build output
// from a separate, native toolchain this script has no part in and that a
// Group A checkout — this one included — has no reason to have run; this
// check exists to catch the *loader* files tauri.conf.json bundles as
// literal resource paths (D2: a missing one fails the bundle late, in
// Tauri, naming a resource path rather than the loader file itself) going
// stale or missing, matching the manifest check above. Always resolved
// against this checkout's own src-tauri/ — the bundler reads the real
// config, so --loader-dir does not redirect this half.
const tauriConfPath = join(root, 'src-tauri', 'tauri.conf.json');
const tauriConf = JSON.parse(readFileSync(tauriConfPath, 'utf8'));
const resources = tauriConf.bundle?.resources ?? {};
const srcTauriDir = join(root, 'src-tauri');
let resourceCount = 0;
for (const source of Object.keys(resources)) {
  if (!source.includes('aurie-loader/')) continue;
  resourceCount++;
  const resourcePath = join(srcTauriDir, source);
  if (!existsSync(resourcePath)) {
    failures.push(`bundle resource "${source}": not found at ${resourcePath}`);
  }
}

if (failures.length) {
  console.error('verify-loader: FAILED');
  for (const failure of failures) console.error(`  ${failure}`);
  process.exit(1);
}

console.log(
  `verify-loader: OK — ${checked.length} loader binaries match the manifest ` +
    `(${checked.join(', ')}); ${resourceCount} bundle resource path(s) present on disk.`,
);
