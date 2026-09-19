// Assemble the release artefacts from an already-built tree: the portable
// zip, a copy of the NSIS setup, and one SHA256SUMS-<version>.txt -- all
// three land in --out so the workflow can upload the directory's contents
// without re-deriving any path. Nothing here builds anything --
// tracker-release.yml runs `npm run build` and the Tauri bundle first.
//
//   node scripts/package-release.mjs [--root <dir>] [--version <v>] [--out <dir>]
//
// --root defaults to the repository root; --out defaults to release/
// (already gitignored -- "artefacts gathered by npm run all", reused here as
// the packager's own output directory). --version defaults to whatever
// package.json under --root says, and is checked against it either way (a
// mismatch is a refusal, the same shape as ForgePact's release_ci.py package).
//
// The portable zip has **no wrapping directory**: hs-offline-tracker.exe at
// the root, plus every destination src-tauri/tauri.conf.json's
// bundle.resources map declares -- read from that file, never hardcoded, so a
// newly bundled resource cannot be forgotten (nine of them at 0.1.3; ten zip
// entries once the exe is added). That is exactly what v0.1.2 shipped by
// hand (see the workorder context, "What v0.1.2 actually shipped"), and it is
// what catalog/sources.toml's derive_strip_prefix ("") already expects.
//
// SHA256SUMS-<version>.txt lists the portable zip and the NSIS setup by the
// filenames Tauri writes them under **locally** (the setup's has a space --
// "HS Offline Tracker_0.1.3_x64-setup.exe"), matching what v0.1.2 actually
// shipped. GitHub rewrites the space to a dot on upload
// ("HS.Offline.Tracker_0.1.3_x64-setup.exe"); tools/build_catalog.py's
// find_checksum already tries both spellings (_checksum_name_keys) for
// exactly this repository. One sidecar, not two -- D5.
//
// No npm dependency: the zip is built and read with a small hand-rolled
// writer/reader (store method only -- these are already-compressed binaries,
// so nothing is lost by skipping deflate) using only node:zlib's crc32
// table technique and node:fs/node:crypto, the same "no dependency" property
// every other new script here has.

import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

// -- CRC-32 (standard IEEE 802.3 polynomial, reflected) ----------------------

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(buf) {
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc = CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// -- A minimal ZIP writer/reader, store method only --------------------------
//
// Fixed 1980-01-01 00:00:00 DOS timestamp on every entry (dosDate=0x21,
// dosTime=0), so the same input tree always produces the same bytes -- there
// is no reason a release zip's checksum should depend on the wall clock.

const DOS_DATE = 0x21;
const DOS_TIME = 0x00;

/** @param {{name: string, data: Buffer}[]} entries
 * @returns {Buffer} */
export function buildZip(entries) {
  const localParts = [];
  const centralParts = [];
  let offset = 0;

  for (const { name, data } of entries) {
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);
    const size = data.length;

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4); // version needed
    local.writeUInt16LE(0, 6); // flags
    local.writeUInt16LE(0, 8); // method: store
    local.writeUInt16LE(DOS_TIME, 10);
    local.writeUInt16LE(DOS_DATE, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(size, 18); // compressed size
    local.writeUInt32LE(size, 22); // uncompressed size
    local.writeUInt16LE(nameBuf.length, 26);
    local.writeUInt16LE(0, 28); // extra length
    localParts.push(local, nameBuf, data);

    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4); // version made by
    central.writeUInt16LE(20, 6); // version needed
    central.writeUInt16LE(0, 8); // flags
    central.writeUInt16LE(0, 10); // method: store
    central.writeUInt16LE(DOS_TIME, 12);
    central.writeUInt16LE(DOS_DATE, 14);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(size, 20);
    central.writeUInt32LE(size, 24);
    central.writeUInt16LE(nameBuf.length, 28);
    central.writeUInt16LE(0, 30); // extra length
    central.writeUInt16LE(0, 32); // comment length
    central.writeUInt16LE(0, 34); // disk number start
    central.writeUInt16LE(0, 36); // internal attrs
    central.writeUInt32LE(0, 38); // external attrs
    central.writeUInt32LE(offset, 42);
    centralParts.push(central, nameBuf);

    offset += local.length + nameBuf.length + data.length;
  }

  const centralStart = offset;
  const central = Buffer.concat(centralParts);

  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(central.length, 12);
  eocd.writeUInt32LE(centralStart, 16);
  eocd.writeUInt16LE(0, 18);

  return Buffer.concat([...localParts, central, eocd]);
}

/** Read a zip this writer produced (or any store/deflate zip's central
 * directory) back into `{name, size}` entries, via the end-of-central-directory
 * record -- used by this script's own test, not by the packager itself. */
export function readZipEntries(buf) {
  const eocdSig = 0x06054b50;
  let eocdOffset = -1;
  for (let i = buf.length - 22; i >= 0; i--) {
    if (buf.readUInt32LE(i) === eocdSig) {
      eocdOffset = i;
      break;
    }
  }
  if (eocdOffset === -1) throw new Error('not a zip: no end-of-central-directory record');

  const count = buf.readUInt16LE(eocdOffset + 10);
  const centralStart = buf.readUInt32LE(eocdOffset + 16);

  const entries = [];
  let p = centralStart;
  for (let i = 0; i < count; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) throw new Error(`central directory entry ${i}: bad signature`);
    const compSize = buf.readUInt32LE(p + 20);
    const size = buf.readUInt32LE(p + 24);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    const name = buf.toString('utf8', p + 46, p + 46 + nameLen);
    entries.push({ name, size, compressedSize: compSize });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

// -- Packaging ----------------------------------------------------------------

export function sha256Hex(data) {
  return createHash('sha256').update(data).digest('hex');
}

/** Resource sources in tauri.conf.json are relative to src-tauri/. */
function readResources(srcTauriDir) {
  const conf = JSON.parse(readFileSync(join(srcTauriDir, 'tauri.conf.json'), 'utf8'));
  const resources = conf.bundle?.resources ?? {};
  return Object.entries(resources).map(([source, dest]) => ({
    source: resolve(srcTauriDir, source),
    dest: dest.replace(/\\/g, '/'),
  }));
}

function nsisSetupName(productName, version) {
  return `${productName}_${version}_x64-setup.exe`;
}

/**
 * @param {object} options
 * @param {string} options.root - repository root of the already-built tree
 * @param {string} options.version
 * @param {string} options.out - output directory
 * @returns {{zipPath: string, sumsPath: string, setupPath: string, zipName: string, setupName: string, sumsName: string}}
 */
export function packageRelease({ root, version, out }) {
  const srcTauriDir = join(root, 'src-tauri');
  const conf = JSON.parse(readFileSync(join(srcTauriDir, 'tauri.conf.json'), 'utf8'));
  const productName = conf.productName;

  const exePath = join(srcTauriDir, 'target', 'release', 'hs-offline-tracker.exe');
  const setupPath = join(srcTauriDir, 'target', 'release', 'bundle', 'nsis', nsisSetupName(productName, version));

  const missing = [];
  if (!existsSync(exePath)) missing.push(exePath);
  const resources = readResources(srcTauriDir);
  for (const { source } of resources) {
    if (!existsSync(source)) missing.push(source);
  }
  if (!existsSync(setupPath)) missing.push(setupPath);
  if (missing.length) {
    const lines = missing.map((p) => `  ${p}`).join('\n');
    throw new Error(`the built tree is incomplete, missing:\n${lines}`);
  }

  const zipEntries = [{ name: 'hs-offline-tracker.exe', data: readFileSync(exePath) }];
  for (const { source, dest } of resources) {
    zipEntries.push({ name: dest, data: readFileSync(source) });
  }
  // Deterministic entry order, independent of tauri.conf.json's own key order.
  zipEntries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));

  mkdirSync(out, { recursive: true });

  const zipName = `hs-offline-tracker-${version}-portable.zip`;
  const zipPath = join(out, zipName);
  writeFileSync(zipPath, buildZip(zipEntries));

  const setupName = nsisSetupName(productName, version);
  const setupBytes = readFileSync(setupPath);
  const setupOutPath = join(out, setupName);
  copyFileSync(setupPath, setupOutPath);
  const zipBytes = readFileSync(zipPath);

  const sumsLines = [`${sha256Hex(zipBytes)}  ${zipName}`, `${sha256Hex(setupBytes)}  ${setupName}`];
  const sumsName = `SHA256SUMS-${version}.txt`;
  const sumsPath = join(out, sumsName);
  writeFileSync(sumsPath, `${sumsLines.join('\n')}\n`);

  return { zipPath, sumsPath, setupPath: setupOutPath, zipName, setupName, sumsName };
}

function main(argv) {
  let root = REPO_ROOT;
  let version = null;
  let out = null;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--root') root = resolve(argv[++i]);
    else if (argv[i] === '--version') version = argv[++i];
    else if (argv[i] === '--out') out = resolve(argv[++i]);
    else {
      console.error(`unrecognised argument: ${argv[i]}`);
      process.exit(2);
    }
  }
  out = out ?? join(root, 'release');

  const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
  if (version === null) {
    version = pkg.version;
  } else if (version !== pkg.version) {
    console.error(`ERROR: package.json says ${pkg.version}, --version was ${version}`);
    process.exit(1);
  }

  let result;
  try {
    result = packageRelease({ root, version, out });
  } catch (err) {
    console.error(`ERROR: ${err.message}`);
    process.exit(1);
  }

  console.log(result.zipPath);
  console.log(result.setupPath);
  console.log(result.sumsPath);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  main(process.argv.slice(2));
}
