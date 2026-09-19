// Pins tracker-tag.yml, tracker-release.yml and tracker-notes-cleanup.yml the
// way ForgePact/tests/test_forgepact_tag_workflow.py and
// test_forgepact_release_workflow.py pin ForgePact's own: a reader that
// extracts only `run:` lines with comments dropped, a positive control
// proving that reader finds a line known to be there, then assertions on
// trigger shape, that no `inputs.` interpolation reaches a `run:` line, step
// order (the guardrail), and that nothing here publishes.
//
// A comment mentioning a command contains the word the command does, so a
// check against the raw file text would pass whether or not the command is
// actually there -- the positive control is what proves this reader is
// reading code, not comments.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { PREFIX } from '../scripts/tracker-tag.mjs';

const ROOT = join(new URL('..', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const WORKFLOWS_DIR = join(ROOT, '.github', 'workflows');

function text(name) {
  return readFileSync(join(WORKFLOWS_DIR, name), 'utf8');
}

/** Every line inside a `run:` block, which is what a shell will execute. */
function runLines(t) {
  const lines = [];
  let inside = false;
  let indent = 0;
  for (const line of t.split(/\r?\n/)) {
    if (/^\s*run: \|/.test(line)) {
      inside = true;
      indent = line.length - line.trimStart().length;
      continue;
    }
    if (/^\s*run: /.test(line)) {
      lines.push(line.split('run: ')[1]);
      continue;
    }
    if (inside) {
      if (line.trim() && line.length - line.trimStart().length <= indent) {
        inside = false;
      } else {
        lines.push(line);
      }
    }
  }
  return lines;
}

/** `run:` lines a shell will act on, with comments dropped. */
function codeLines(t) {
  return runLines(t)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'));
}

// ============================================================================
// tracker-tag.yml
// ============================================================================

test('tracker-tag.yml exists', () => {
  assert.doesNotThrow(() => text('tracker-tag.yml'));
});

test('tracker-tag.yml: only workflow_dispatch triggers it', () => {
  const t = text('tracker-tag.yml');
  assert.match(t, /on:\s*\n\s*workflow_dispatch:/);
  for (const trigger of ['push:', 'pull_request:', 'schedule:', 'repository_dispatch:', 'release:']) {
    assert.doesNotMatch(t, new RegExp(`^\\s{2}${trigger}`, 'm'), `${trigger} would cut releases nobody asked for`);
  }
});

test('tracker-tag.yml: takes a required tag input', () => {
  const t = text('tracker-tag.yml');
  assert.match(t, /^\s{6}tag:\s*$/m);
  assert.match(t, /^\s*required: true\s*$/m);
});

test('tracker-tag.yml: refuses to run off main', () => {
  assert.match(text('tracker-tag.yml'), /\[ "\$BRANCH" != "main" \]/);
});

test('tracker-tag.yml: positive control -- the run-block reader actually reads them', () => {
  const lines = runLines(text('tracker-tag.yml'));
  assert.ok(lines.length > 20, 'the run blocks are not being read');
  assert.ok(lines.some((l) => l.includes('git ls-remote')), 'a command known to be in a run block was not found');
});

test('tracker-tag.yml: the typed tag never appears inside a run block', () => {
  for (const line of runLines(text('tracker-tag.yml'))) {
    assert.ok(!line.includes('inputs.tag'), `the typed tag is interpolated into a shell here: ${line.trim()}`);
  }
});

test('tracker-tag.yml: the input is passed through the environment', () => {
  assert.match(text('tracker-tag.yml'), /TAG_INPUT: \$\{\{ inputs\.tag \}\}/);
});

test('tracker-tag.yml: pipefail precedes the piped git ls-remote', () => {
  const lines = codeLines(text('tracker-tag.yml'));
  const piped = lines.map((l, i) => [l, i]).filter(([l]) => l.includes('git ls-remote') && l.includes('|'));
  assert.ok(piped.length, "the tag query is gone, or no longer a pipeline");
  for (const [, at] of piped) {
    assert.ok(lines.slice(0, at).includes('set -o pipefail'), "the query's exit status is discarded by the pipe it is in");
  }
  assert.ok(lines.some((l) => l.includes('refs/tags/v*')), 'the tag query must be scoped to this repo\'s v* tags');
});

test('tracker-tag.yml: read-only steps precede every write, in order', () => {
  const t = text('tracker-tag.yml');
  const markers = [
    ['no_release_yet', '- name: This version has no release yet'],
    ['generate_notes', 'releases/generate-notes'],
    ['compose_notes', '--compose-notes'],
    ['move_version', '- name: Move the version to match the tag'],
    ['push_main', 'git push origin HEAD:main'],
    ['tag_it', 'git tag -a'],
    ['release_create', 'gh release create'],
  ];
  const positions = markers.map(([name, marker]) => [name, t.indexOf(marker)]);
  for (const [name, pos] of positions) assert.notEqual(pos, -1, `${name} is missing from the workflow`);
  for (let i = 0; i < positions.length - 1; i++) {
    const [nameA, posA] = positions[i];
    const [nameB, posB] = positions[i + 1];
    assert.ok(posA < posB, `${nameA} must precede ${nameB}`);
  }
});

test('tracker-tag.yml: the bump only happens when the tree disagrees, and commits everything at once', () => {
  const t = text('tracker-tag.yml');
  assert.match(t, /if: steps\.plan\.outputs\.bump == 'true'/);
  assert.match(t, /git commit --all/);
});

test('tracker-tag.yml: the tree is verified against the tag before tagging', () => {
  assert.match(text('tracker-tag.yml'), /set-version\.mjs --check --expect "\$VERSION"/);
});

test('tracker-tag.yml: generate-notes supplies target and previous, uses RUNNER_TEMP, never names a notes file itself', () => {
  const t = text('tracker-tag.yml');
  assert.ok(t.includes('target_commitish'));
  assert.ok(t.includes('previous_tag_name'));
  assert.ok(t.includes('$RUNNER_TEMP'));
  assert.ok(!t.includes('release-notes-v'));
});

test('tracker-tag.yml: the draft is created as a draft against the verified tag, with composed notes', () => {
  const t = text('tracker-tag.yml');
  assert.match(t, /gh release create[^\n]*--draft/);
  assert.match(t, /gh release create[^\n]*--verify-tag/);
  assert.match(t, /gh release create[^\n]*--notes-file/);
});

test('tracker-tag.yml: permissions cover the bump/tag/draft and the dispatch', () => {
  const t = text('tracker-tag.yml');
  assert.match(t, /^\s*contents: write\s*$/m);
  assert.match(t, /^\s*actions: write\s*$/m);
});

test('tracker-tag.yml: never builds, uploads or publishes', () => {
  const t = text('tracker-tag.yml');
  for (const forbidden of ['gh release edit', 'gh release upload', '--draft=false', '--latest', 'build.ps1', 'upload-artifact']) {
    assert.ok(!t.includes(forbidden), `${forbidden} is out of scope for this workflow`);
  }
});

test('tracker-tag.yml: the build is dispatched, after the draft, against main, with a real run', () => {
  const t = text('tracker-tag.yml');
  const createAt = t.indexOf('gh release create');
  const dispatchAt = t.indexOf('gh workflow run tracker-release.yml');
  assert.notEqual(createAt, -1);
  assert.notEqual(dispatchAt, -1);
  assert.ok(createAt < dispatchAt, 'starting the build before the draft exists races the tag');

  const lines = codeLines(t);
  const dispatch = lines.find((l) => l.includes('gh workflow run tracker-release.yml'));
  assert.ok(dispatch);
  assert.ok(dispatch.includes('--ref main'));
  assert.ok(dispatch.includes('-f tag="$TAG"'));
  assert.ok(dispatch.includes('-f dry_run=false'));
});

test('tracker-tag.yml and scripts/tracker-tag.mjs agree on the v prefix', () => {
  assert.equal(PREFIX, 'v');
});

// ============================================================================
// tracker-release.yml
// ============================================================================

test('tracker-release.yml exists', () => {
  assert.doesNotThrow(() => text('tracker-release.yml'));
});

test('tracker-release.yml: only workflow_dispatch triggers it, with tag and dry_run inputs', () => {
  const t = text('tracker-release.yml');
  assert.match(t, /on:\s*\n\s*workflow_dispatch:/);
  for (const trigger of ['push:', 'pull_request:', 'schedule:', 'repository_dispatch:']) {
    assert.doesNotMatch(t, new RegExp(`^\\s{2}${trigger}`, 'm'));
  }
  assert.match(t, /^\s{6}tag:\s*$/m);
  assert.match(t, /^\s{6}dry_run:\s*$/m);
  assert.match(t, /default: true/);
});

test('tracker-release.yml: refuses to run off main', () => {
  assert.match(text('tracker-release.yml'), /\[ "\$BRANCH" != "main" \]/);
});

test('tracker-release.yml: positive control -- the run-block reader actually reads them', () => {
  const lines = runLines(text('tracker-release.yml'));
  assert.ok(lines.length > 20);
  assert.ok(lines.some((l) => l.includes('gh api')), 'a command known to be in a run block was not found');
});

test('tracker-release.yml: runs on windows-latest, with Node 22 and stable Rust', () => {
  const t = text('tracker-release.yml');
  assert.match(t, /runs-on: windows-latest/);
  assert.match(t, /actions\/setup-node@v4/);
  assert.match(t, /node-version: 22/);
  assert.match(t, /dtolnay\/rust-toolchain@stable/);
  assert.match(t, /swatinem\/rust-cache@v2/);
  assert.match(t, /workspaces: src-tauri/);
});

test('tracker-release.yml: checks out only the tag, once', () => {
  const t = text('tracker-release.yml');
  const checkouts = [...t.matchAll(/uses: actions\/checkout@v4/g)];
  assert.equal(checkouts.length, 1, 'a second checkout would reintroduce the two-tree split this repository does not need');
  assert.match(t, /ref: refs\/tags\/\$\{\{ steps\.tag\.outputs\.tag \}\}/);
});

test('tracker-release.yml: fetches the pinned headers and builds the producer with CTest', () => {
  const t = text('tracker-release.yml');
  assert.match(t, /fetch-producer-sdk\.mjs/);
  assert.match(t, /build\.ps1 -Configuration Release -YytkSdkRoot/);
});

test('tracker-release.yml: two draft guards, before checkout and immediately before upload', () => {
  const t = text('tracker-release.yml');
  const guard1At = t.indexOf('guard 1');
  const checkoutAt = t.indexOf('uses: actions/checkout@v4');
  const guard2At = t.indexOf('guard 2');
  const uploadAt = t.indexOf('- name: Upload to the draft');
  assert.notEqual(guard1At, -1);
  assert.notEqual(guard2At, -1);
  assert.ok(guard1At < checkoutAt, 'guard 1 must run before any checkout');
  assert.ok(guard2At < uploadAt, 'guard 2 must run immediately before the upload');
  assert.match(t, /if: \$\{\{ !inputs\.dry_run \}\}/);
});

test('tracker-release.yml: an independent reader opens the zip after packaging and before any upload', () => {
  const t = text('tracker-release.yml');
  const packageAt = t.indexOf('package-release.mjs');
  const checkAt = t.indexOf('[System.IO.Compression.ZipFile]::OpenRead');
  const keepAt = t.indexOf('- name: Keep the artefacts on a dry run');
  const uploadAt = t.indexOf('- name: Upload to the draft');
  assert.notEqual(checkAt, -1, 'the .NET zip check is missing');
  assert.ok(packageAt < checkAt, 'the zip check must run after packaging');
  assert.ok(checkAt < keepAt && checkAt < uploadAt, 'the zip check must run before the zip is kept or uploaded');
});

test('tracker-release.yml: build order -- ci, tool tests, rust tests, loader verify, frontend, bundle, package', () => {
  const lines = codeLines(text('tracker-release.yml'));
  const idx = (needle) => lines.findIndex((l) => l.includes(needle));
  const order = ['npm ci', 'npm run tools:test', 'npm test', 'npm run loader:verify', 'npm run build', 'npm run package', 'package-release.mjs'];
  const positions = order.map(idx);
  for (let i = 0; i < positions.length; i++) assert.notEqual(positions[i], -1, `${order[i]} missing`);
  for (let i = 0; i < positions.length - 1; i++) {
    assert.ok(positions[i] < positions[i + 1], `${order[i]} must precede ${order[i + 1]}`);
  }
});

test('tracker-release.yml: version is checked against the tag', () => {
  assert.match(text('tracker-release.yml'), /set-version\.mjs --check --expect "\$VERSION"/);
});

test('tracker-release.yml: a dry run uploads nothing to the draft, only a workflow artifact', () => {
  const t = text('tracker-release.yml');
  assert.match(t, /if: inputs\.dry_run/);
  assert.match(t, /upload-artifact@v4/);
});

test('tracker-release.yml: the upload uses --clobber and all three artefacts', () => {
  const t = text('tracker-release.yml');
  assert.match(t, /gh release upload/);
  assert.match(t, /--clobber/);
  // The packager's own stdout order (zip, setup, sums) is what the workflow
  // captures into these outputs -- see package-release.mjs's main().
  assert.match(t, /zip_path=\$zip_path/);
  assert.match(t, /setup_path=\$setup_path/);
  assert.match(t, /sums_path=\$sums_path/);
  const upload = codeLines(t)
    .join('\n')
    .match(/gh release upload[\s\S]*?--clobber --repo "\$REPO"/)[0];
  assert.match(upload, /\$ZIP_PATH/);
  assert.match(upload, /\$SETUP_PATH/);
  assert.match(upload, /\$SUMS_PATH/);
});

test('tracker-release.yml: the upload step env wires ZIP_PATH/SETUP_PATH/SUMS_PATH from the package step outputs', () => {
  const t = text('tracker-release.yml');
  assert.match(t, /ZIP_PATH: \$\{\{ steps\.package\.outputs\.zip_path \}\}/);
  assert.match(t, /SETUP_PATH: \$\{\{ steps\.package\.outputs\.setup_path \}\}/);
  assert.match(t, /SUMS_PATH: \$\{\{ steps\.package\.outputs\.sums_path \}\}/);
});

test('tracker-release.yml: never creates, edits or publishes a release, and never dispatches another workflow', () => {
  const t = text('tracker-release.yml');
  for (const forbidden of ['gh release create', 'gh release edit', '--draft=false', '--latest', 'gh workflow run']) {
    assert.ok(!t.includes(forbidden), `${forbidden} is out of scope for this workflow`);
  }
});

test('tracker-release.yml: no job carries actions: write', () => {
  assert.ok(!text('tracker-release.yml').includes('actions: write'), 'this workflow starts no other workflow run');
});

// ============================================================================
// tracker-notes-cleanup.yml
// ============================================================================

test('tracker-notes-cleanup.yml exists', () => {
  assert.doesNotThrow(() => text('tracker-notes-cleanup.yml'));
});

test('tracker-notes-cleanup.yml: triggers on release published and workflow_dispatch, skips prereleases', () => {
  const t = text('tracker-notes-cleanup.yml');
  assert.match(t, /release:\s*\n\s*types: \[published\]/);
  assert.match(t, /workflow_dispatch:/);
  assert.match(t, /github\.event\.release\.prerelease == false/);
});

test('tracker-notes-cleanup.yml: refuses a release that is not exactly one published release', () => {
  const t = text('tracker-notes-cleanup.yml');
  assert.match(t, /has no release -- nothing was published/);
  assert.match(t, /refusing to delete notes a draft still needs/);
});

test('tracker-notes-cleanup.yml: deletes via --published-notes and commits as the bot', () => {
  const t = text('tracker-notes-cleanup.yml');
  assert.match(t, /--published-notes --version "\$VERSION"/);
  assert.match(t, /github-actions\[bot\]/);
});

test('tracker-notes-cleanup.yml: retries the push over a concurrent merge', () => {
  const t = text('tracker-notes-cleanup.yml');
  assert.match(t, /git pull --rebase origin main/);
  assert.match(t, /for attempt in 1 2 3/);
});

test('tracker-notes-cleanup.yml: permission is scoped to contents: write, nothing wider', () => {
  const t = text('tracker-notes-cleanup.yml');
  assert.match(t, /^\s*contents: write\s*$/m);
  assert.ok(!t.includes('actions: write'));
});
