import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { run, sha256Hex, MANIFEST_PATH } from '../scripts/fetch-producer-sdk.mjs';

function tempDir() {
  return mkdtempSync(join(tmpdir(), 'hsot-fetch-'));
}

function manifestFor(entries) {
  return { files: entries.map(([dest, data]) => ({ dest, url: `mem://${dest}`, sha256: sha256Hex(data) })) };
}

test('positive control -- all seven land, hashes match', async () => {
  const entries = [
    ['include/YYToolkit/a.hpp', Buffer.from('alpha')],
    ['include/YYToolkit/b.hpp', Buffer.from('beta')],
    ['include/YYToolkit/c.hpp', Buffer.from('gamma')],
    ['include/YYToolkit/d.cpp', Buffer.from('delta')],
    ['include/YYToolkit/e.hpp', Buffer.from('epsilon')],
    ['include/FunctionWrapper/f.hpp', Buffer.from('zeta')],
    ['include/Aurie/shared.hpp', Buffer.from('eta')],
  ];
  const manifest = manifestFor(entries);
  const bytes = new Map(entries.map(([dest, data]) => [`mem://${dest}`, data]));
  const fetcher = async (url) => {
    if (!bytes.has(url)) throw new Error(`no fixture for ${url}`);
    return bytes.get(url);
  };

  const root = tempDir();
  try {
    const result = await run(manifest, { root, fetcher, verifyOnly: false });
    assert.equal(result.ok, true, result.lines.join('\n'));
    for (const [dest, data] of entries) {
      assert.deepEqual(readFileSync(join(root, dest)), data);
    }

    // --verify-only confirms what was just written, without a fetcher.
    const verify = await run(manifest, { root, fetcher: async () => { throw new Error('must not fetch'); }, verifyOnly: true });
    assert.equal(verify.ok, true, verify.lines.join('\n'));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('negative control -- one byte flipped writes nothing and exits non-zero', async () => {
  const good = Buffer.from('correct bytes');
  const manifest = { files: [{ dest: 'include/Aurie/shared.hpp', url: 'mem://shared.hpp', sha256: sha256Hex(good) }] };
  // The fetcher returns bytes that do NOT match the pinned hash.
  const fetcher = async () => Buffer.from('WRONG bytes');

  const root = tempDir();
  try {
    const result = await run(manifest, { root, fetcher, verifyOnly: false });
    assert.equal(result.ok, false);
    assert.ok(result.lines.some((l) => l.includes('MISMATCH')));

    // Nothing was written: the root is still empty.
    const verify = await run(manifest, { root, fetcher: async () => { throw new Error('must not fetch'); }, verifyOnly: true });
    assert.equal(verify.ok, false);
    assert.ok(verify.lines.some((l) => l.includes('MISSING')));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('--verify-only against an empty root is always a failure', async () => {
  const manifest = manifestFor([['include/Aurie/shared.hpp', Buffer.from('x')]]);
  const root = tempDir();
  try {
    const result = await run(manifest, { root, fetcher: async () => { throw new Error('must not fetch'); }, verifyOnly: true });
    assert.equal(result.ok, false);
    assert.ok(result.lines.some((l) => l.includes('MISSING')));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('a manifest dest cannot escape the root', async () => {
  const manifest = { files: [{ dest: '../escape.hpp', url: 'mem://escape', sha256: sha256Hex(Buffer.from('x')) }] };
  const root = tempDir();
  try {
    const result = await run(manifest, { root, fetcher: async () => Buffer.from('x'), verifyOnly: false });
    assert.equal(result.ok, false);
    assert.ok(result.lines.some((l) => l.includes('REFUSED')));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('the seven pinned hashes match the values this workorder recorded', () => {
  const manifest = JSON.parse(readFileSync(MANIFEST_PATH, 'utf8'));
  const expected = {
    'include/YYToolkit/YYTK_Shared.hpp': '6d6666f1db2553ea0c17658bdc53e65c396bc402053d73257038e66bd713d22a',
    'include/YYToolkit/YYTK_Shared_Base.hpp': 'ab64a23eed8a2358e3fb37a2789bce42cf557b9dbcc4051dc61029859ffcb887',
    'include/YYToolkit/YYTK_Shared_Interface.hpp': 'bec19a3f956568d85d717acc906de24bed2b5ed965940a82af393808d213a102',
    'include/YYToolkit/YYTK_Shared_Types.cpp': '93531e2d1827ec411376b0e4baa7663a8a111c9e7ede49a708cf75ce6c58011e',
    'include/YYToolkit/YYTK_Shared_Types.hpp': '7d3ad5426a7014e290beba84ed4660dbe7f7c97081efbb3c4a62a7b0cdfbfc1a',
    'include/FunctionWrapper/FunctionWrapper.hpp': 'e72e263dcf3403efb196552fa08f2beea8f46f3ff67ae16c7de2b1599c0bccff',
    'include/Aurie/shared.hpp': 'c830652fc89854723520b26ee3fdc7b468abed94131109c032d3b32ed2e1dc5d',
  };
  assert.equal(manifest.files.length, 7);
  for (const entry of manifest.files) {
    assert.equal(entry.sha256, expected[entry.dest], entry.dest);
  }
  assert.deepEqual(new Set(manifest.files.map((f) => f.dest)), new Set(Object.keys(expected)));
});
