import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';

import { buildZip, packageRelease, readZipEntries, sha256Hex } from '../scripts/package-release.mjs';

// Mirrors tools/build_catalog.py's _CHECKSUM_LINE / parse_checksums exactly
// (sha256, whitespace, optional `*`, filename) -- "parses under the hub's
// own rules" means this regex, not a JS reimplementation of a different one.
const CHECKSUM_LINE = /^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$/;
function parseChecksums(text) {
  const result = {};
  for (const raw of text.replace(/\r\n/g, '\n').split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const match = CHECKSUM_LINE.exec(line);
    if (match) result[match[2]] = match[1].toLowerCase();
  }
  return result;
}

const RESOURCES = {
  '../LICENSE': 'LICENSE',
  '../THIRD_PARTY_NOTICES.md': 'THIRD_PARTY_NOTICES.md',
  '../PROTOCOL.md': 'PROTOCOL.md',
  '../aurie-producer/build/bin/Release/HSOfflineTrackerProducer.dll': 'producer/HSOfflineTrackerProducer.dll',
  '../aurie-loader/AurieCore.dll': 'aurie-loader/AurieCore.dll',
  '../aurie-loader/YYToolkit.dll': 'aurie-loader/YYToolkit.dll',
  '../aurie-loader/AuriePatcher.exe': 'aurie-loader/AuriePatcher.exe',
  '../aurie-loader/yytoolkit-modified/NOTICE.md': 'aurie-loader/yytoolkit-modified/NOTICE.md',
  '../aurie-loader/yytoolkit-modified/YYToolkit-BUILD-INFO.json': 'aurie-loader/yytoolkit-modified/YYToolkit-BUILD-INFO.json',
};

function writeFile(path, content) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, content);
}

function buildFakeTree(version) {
  const root = mkdtempSync(join(tmpdir(), 'hsot-pkg-'));
  const srcTauriDir = join(root, 'src-tauri');

  writeFile(
    join(srcTauriDir, 'tauri.conf.json'),
    JSON.stringify(
      {
        productName: 'HS Offline Tracker',
        version,
        bundle: { targets: ['nsis'], resources: RESOURCES },
      },
      null,
      2,
    ),
  );

  writeFile(join(root, 'package.json'), JSON.stringify({ name: 'hs-offline-tracker', version }, null, 2));

  // The already-built tree: the exe, every resource source, and the NSIS setup.
  writeFile(join(srcTauriDir, 'target', 'release', 'hs-offline-tracker.exe'), 'FAKE EXE BYTES');
  for (const source of Object.keys(RESOURCES)) {
    writeFile(join(srcTauriDir, source), `content of ${source}`);
  }
  writeFile(
    join(srcTauriDir, 'target', 'release', 'bundle', 'nsis', `HS Offline Tracker_${version}_x64-setup.exe`),
    'FAKE SETUP BYTES',
  );

  return root;
}

test('buildZip/readZipEntries round-trip', () => {
  const entries = [
    { name: 'a.txt', data: Buffer.from('hello') },
    { name: 'dir/b.txt', data: Buffer.from('world') },
  ];
  const zip = buildZip(entries);
  const read = readZipEntries(zip);
  assert.deepEqual(
    read.map((e) => e.name),
    ['a.txt', 'dir/b.txt'],
  );
  assert.equal(read[0].size, 5);
  assert.equal(read[1].size, 5);
});

// Checked against the ZIP format itself (APPNOTE 4.3.16 / 4.3.12 / 4.3.7), not
// against readZipEntries, which shares the writer's idea of the layout: the
// v0.1.3 draft's zip round-tripped through readZipEntries and still failed
// in every real unzip tool, because the end record's comment-length write
// landed on the top half of the central-directory offset. The payload is
// past 64 KiB so the offset's top half is non-zero -- the positive control
// for that bug; a tree of small files cannot see it.
test('buildZip -- end record and every offset follow the ZIP format past 64 KiB', () => {
  const big = Buffer.alloc(200 * 1024, 0xab);
  const entries = [
    { name: 'big.bin', data: big },
    { name: 'small.txt', data: Buffer.from('after the big one') },
  ];
  const zip = buildZip(entries);

  const eocd = zip.length - 22;
  assert.equal(zip.readUInt32LE(eocd), 0x06054b50, 'end record at the very end (no comment)');
  assert.equal(zip.readUInt16LE(eocd + 8), 2, 'entries on this disk');
  assert.equal(zip.readUInt16LE(eocd + 10), 2, 'total entries');
  const cdSize = zip.readUInt32LE(eocd + 12);
  const cdOffset = zip.readUInt32LE(eocd + 16);
  assert.equal(zip.readUInt16LE(eocd + 20), 0, 'comment length');
  assert.ok(cdOffset > 0xffff, 'test must exercise an offset past 64 KiB');
  assert.equal(cdOffset + cdSize, eocd, 'central directory ends where the end record starts');
  assert.equal(zip.readUInt32LE(cdOffset), 0x02014b50, 'central directory starts at the recorded offset');

  let p = cdOffset;
  for (const { name, data } of entries) {
    assert.equal(zip.readUInt32LE(p), 0x02014b50);
    const nameLen = zip.readUInt16LE(p + 28);
    assert.equal(zip.toString('utf8', p + 46, p + 46 + nameLen), name);
    const local = zip.readUInt32LE(p + 42);
    assert.equal(zip.readUInt32LE(local), 0x04034b50, `${name}: local header at its recorded offset`);
    const localNameLen = zip.readUInt16LE(local + 26);
    const start = local + 30 + localNameLen + zip.readUInt16LE(local + 28);
    assert.ok(zip.subarray(start, start + data.length).equals(data), `${name}: stored bytes intact`);
    p += 46 + nameLen;
  }

  assert.deepEqual(readZipEntries(zip).map((e) => e.name), ['big.bin', 'small.txt']);
});

test('readZipEntries refuses an end record whose offset does not meet it (negative control)', () => {
  const zip = buildZip([{ name: 'big.bin', data: Buffer.alloc(100 * 1024) }]);
  // Reproduce the old bug: zero the top half of the central-directory offset.
  zip.writeUInt16LE(0, zip.length - 22 + 18);
  assert.throws(() => readZipEntries(zip), /end-of-central-directory record says/);
});

test('packageRelease -- zip name, no wrapping directory, every resource present', () => {
  const version = '0.1.3';
  const root = buildFakeTree(version);
  const out = join(root, 'release');
  try {
    const result = packageRelease({ root, version, out });

    // Matches the hub's asset_pattern.
    assert.match(result.zipName, /^hs-offline-tracker-[0-9][^/]*-portable\.zip$/);
    assert.equal(result.zipName, 'hs-offline-tracker-0.1.3-portable.zip');

    const zipBytes = readFileSync(result.zipPath);
    const entries = readZipEntries(zipBytes);
    const names = entries.map((e) => e.name).sort();

    // No wrapping directory: the exe sits at the zip root (empty strip prefix).
    assert.ok(names.includes('hs-offline-tracker.exe'));
    assert.ok(!names.some((n) => n.startsWith('hs-offline-tracker-0.1.3/')));

    // Every bundle.resources destination is present -- 9 resources + the exe.
    const expectedDests = Object.values(RESOURCES);
    assert.equal(expectedDests.length, 9);
    for (const dest of expectedDests) assert.ok(names.includes(dest), dest);
    assert.equal(entries.length, 10);

    // The checksum file parses under the hub's own rules, with an entry for
    // each artefact.
    const sumsText = readFileSync(result.sumsPath, 'utf8');
    const checksums = parseChecksums(sumsText);
    assert.equal(Object.keys(checksums).length, 2);
    assert.equal(checksums[result.zipName], sha256Hex(zipBytes));
    const originalSetupBytes = readFileSync(join(root, 'src-tauri', 'target', 'release', 'bundle', 'nsis', result.setupName));
    assert.equal(checksums[result.setupName], sha256Hex(originalSetupBytes));

    // The setup .exe is copied into --out too, not just hashed in place --
    // the workflow uploads out/'s contents, so a setup left at its original
    // build location would never reach the draft.
    assert.equal(result.setupPath, join(out, result.setupName));
    const copiedSetupBytes = readFileSync(result.setupPath);
    assert.deepEqual(copiedSetupBytes, originalSetupBytes);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('packageRelease refuses when a resource is missing, naming it', () => {
  const version = '0.1.3';
  const root = buildFakeTree(version);
  const out = join(root, 'release');
  // Delete one resource the manifest declares (resource sources are
  // relative to src-tauri/, and "../aurie-loader/..." lands one level up,
  // i.e. at the repository root -- matching the real layout).
  rmSync(join(root, 'aurie-loader', 'YYToolkit.dll'));
  try {
    assert.throws(() => packageRelease({ root, version, out }), /YYToolkit\.dll/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('packageRelease refuses when the exe is missing', () => {
  const version = '0.1.3';
  const root = buildFakeTree(version);
  const out = join(root, 'release');
  rmSync(join(root, 'src-tauri', 'target', 'release', 'hs-offline-tracker.exe'));
  try {
    assert.throws(() => packageRelease({ root, version, out }), /hs-offline-tracker\.exe/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
