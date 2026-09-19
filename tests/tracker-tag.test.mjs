import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { composeBody, notesPlan, noteVersions, plan, publishedNotes } from '../scripts/tracker-tag.mjs';

const SCRIPT = new URL('../scripts/tracker-tag.mjs', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');

function runCli(args) {
  try {
    const stdout = execFileSync(process.execPath, [SCRIPT, ...args], { encoding: 'utf8' });
    return { status: 0, stdout };
  } catch (err) {
    return { status: err.status, stdout: err.stdout ?? '' };
  }
}

// -- Refusal 1: wrong shape ------------------------------------------------

test('refusal: wrong shape (leading zero, extra prefix, suffix)', () => {
  for (const bad of ['v01.2.3', 'vv0.1.4', 'V0.1.4', '0.1.4-rc1', 'hub-v0.1.4']) {
    assert.throws(() => plan(bad, [], '0.1.3'));
  }
});

// -- Refusal 2: the tag already exists -------------------------------------

test('refusal: the tag already exists', () => {
  assert.throws(() => plan('v0.1.2', ['refs/tags/v0.1.1', 'refs/tags/v0.1.2'], '0.1.3'));
});

// -- Refusal 3: below the highest existing tag -----------------------------

test('refusal: below the highest existing v* tag', () => {
  assert.throws(() => plan('v0.1.1', ['refs/tags/v0.1.1', 'refs/tags/v0.1.2'], '0.1.3'));
});

// -- Refusal 4: below what main already holds ------------------------------

test('refusal: below the tree', () => {
  assert.throws(() => plan('v0.1.2', ['refs/tags/v0.1.1'], '0.1.3'));
});

test('a refusal prints nothing on stdout and exits non-zero (CLI)', () => {
  const result = runCli(['--tag', 'v0.1.2', '--existing', 'v0.1.1', 'v0.1.2', '--tree', '0.1.3']);
  assert.notEqual(result.status, 0);
  assert.equal(result.stdout, '');
});

// -- Plan mode: the four lines, in order -----------------------------------

test('plan mode prints the four lines in order (CLI)', () => {
  const result = runCli(['--tag', 'v0.1.4', '--existing', 'v0.1.1', 'v0.1.2', '--tree', '0.1.3']);
  assert.equal(result.status, 0);
  assert.equal(result.stdout, 'version=0.1.4\ntag=v0.1.4\nbump=true\nprevious=v0.1.2\n');
});

test('bump=false when the tree already matches the tag', () => {
  const result = plan('v0.1.3', ['refs/tags/v0.1.1', 'refs/tags/v0.1.2'], '0.1.3');
  assert.equal(result.bump, false);
});

// -- Numeric, not lexical, comparison ----------------------------------

test('numeric comparison: v0.1.9 is below v0.1.16, not above it', () => {
  const result = plan('v0.1.20', ['refs/tags/v0.1.9', 'refs/tags/v0.1.16'], '0.1.3');
  assert.equal(result.previous, 'v0.1.16');
});

test('tags from another scheme are ignored', () => {
  const result = plan('v0.1.4', ['refs/tags/v0.1.2', 'refs/tags/hub-v9.0.0', 'refs/tags/catalog'], '0.1.3');
  assert.equal(result.previous, 'v0.1.2');
});

// -- notesPlan: newest-first skipped ordering ------------------------------

test('notesPlan skips newest-first, numerically', () => {
  const { skipped, topFromFile } = notesPlan(['0.1.9', '0.1.16', '0.1.10'], '0.1.20', 'v0.1.3');
  assert.equal(topFromFile, false);
  assert.deepEqual(skipped, ['0.1.16', '0.1.10', '0.1.9']);
});

test('notesPlan has no skipped versions with no previous tag', () => {
  const { skipped } = notesPlan(['0.1.1', '0.1.2'], '0.1.3', '');
  assert.deepEqual(skipped, []);
});

// -- noteVersions filename matching -----------------------------------------

test('noteVersions matches only the accepted shape', () => {
  const names = ['release-notes-v0.1.3.md', 'release-notes-v0.1.4-rc1.md', 'release-notes.md', 'README.md', 'release-notes-v01.2.0.md'];
  assert.deepEqual(noteVersions(names), ['0.1.3']);
});

// -- publishedNotes: oldest first, ceiling respected -----------------------

test('publishedNotes lists oldest first, at or below the ceiling only', () => {
  const result = publishedNotes(['0.1.1', '0.1.2', '0.1.4'], '0.1.3');
  assert.deepEqual(result, ['0.1.1', '0.1.2']);
});

// -- compose-notes: the generated-notes banner ------------------------------

test('composeBody falls back to a banner when the top file is missing', () => {
  const [body, source] = composeBody('0.1.3', null, 'Generated stuff.', []);
  assert.equal(source, 'generated');
  assert.match(body, /Draft notes, generated from pull request titles/);
  assert.match(body, /# HS Offline Tracker 0\.1\.3/);
});

test('composeBody uses the file directly, with no banner, when it exists', () => {
  const [body, source] = composeBody('0.1.3', '# HS Offline Tracker 0.1.3\n\nSome notes.\n\n## How to update\n\nDo the thing.\n', 'ignored', []);
  assert.equal(source, 'file');
  assert.doesNotMatch(body, /Draft notes, generated/);
});

// -- compose-notes: "## How to update" kept only once ------------------------

test('## How to update is kept only in the first file-sourced section', () => {
  const top = '# HS Offline Tracker 0.1.3\n\nTop notes.\n\n## How to update\n\nDo the thing.\n';
  const skippedText = '# HS Offline Tracker 0.1.2\n\nSkipped notes.\n\n## How to update\n\nDo the thing again.\n';
  const [body] = composeBody('0.1.3', top, 'ignored', [['0.1.2', skippedText]]);
  const occurrences = body.split('## How to update').length - 1;
  assert.equal(occurrences, 1);
});

// -- compose-notes CLI, end to end ------------------------------------------

test('compose-notes CLI composes from generated notes plus a skipped file (mixed)', () => {
  const root = mkdtempSync(join(tmpdir(), 'hsot-tag-'));
  try {
    writeFileSync(join(root, 'release-notes-v0.1.2.md'), '# HS Offline Tracker 0.1.2\n\nOlder notes.\n');
    const generated = join(root, 'generated.md');
    writeFileSync(generated, 'Generated body.');
    const out = join(root, 'out.md');

    const result = execFileSync(
      process.execPath,
      [SCRIPT, '--compose-notes', '--version', '0.1.3', '--previous', 'v0.1.1', '--root', root, '--generated', generated, '--out', out],
      { encoding: 'utf8' },
    );
    assert.match(result, /source=mixed/);
    // 0.1.2 is above previous (0.1.1) and below version (0.1.3), so it is skipped-in.
    assert.match(result, /versions=0\.1\.3 0\.1\.2/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

// -- --normalize: what tracker-release.yml uses for its own tag input -------

test('--normalize CLI accepts a bare or v-prefixed version and refuses a bad shape', () => {
  const ok1 = runCli(['--normalize', '--tag', '0.1.3']);
  assert.equal(ok1.status, 0);
  assert.equal(ok1.stdout, 'tag=v0.1.3\nversion=0.1.3\n');

  const ok2 = runCli(['--normalize', '--tag', 'v0.1.3']);
  assert.equal(ok2.stdout, 'tag=v0.1.3\nversion=0.1.3\n');

  const bad = runCli(['--normalize', '--tag', '01.1.3']);
  assert.notEqual(bad.status, 0);
  assert.equal(bad.stdout, '');
});

test('published-notes CLI', () => {
  const root = mkdtempSync(join(tmpdir(), 'hsot-tag-'));
  try {
    writeFileSync(join(root, 'release-notes-v0.1.1.md'), 'x');
    writeFileSync(join(root, 'release-notes-v0.1.2.md'), 'x');
    writeFileSync(join(root, 'release-notes-v0.1.4.md'), 'x');
    const out = execFileSync(process.execPath, [SCRIPT, '--published-notes', '--version', '0.1.3', '--root', root], { encoding: 'utf8' });
    assert.equal(out, 'release-notes-v0.1.1.md\nrelease-notes-v0.1.2.md\n');
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
