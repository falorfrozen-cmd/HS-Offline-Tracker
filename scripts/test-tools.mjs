// Runs the Node tool tests under tests/ -- the unit tests for the scripts/
// this repository's release mechanism added (set-version, tracker-tag,
// fetch-producer-sdk, package-release), not the Rust test suite `npm test`
// already runs.
//
//   npm run tools:test
//
// `node --test tests/` fails on Node 24.19.0 (`Cannot find module ...tests`
// -- the path is resolved as a module, not walked; directory arguments to
// --test were dropped after Node 20). Bare `node --test` with no argument
// works, but would also walk src-tauri/target/, which is large once built.
// So this collects the *.test.mjs files under tests/ itself and hands them
// to node --test as explicit paths -- version-independent across this
// repository's supported Node range (^20.19.0 || >=22.12.0), the same idiom
// scripts/test.mjs and scripts/tauri.mjs already use for this class of
// shell-and-path problem.

import { spawnSync } from 'node:child_process';
import { readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const testsDir = join(root, 'tests');

const files = readdirSync(testsDir)
  .filter((name) => name.endsWith('.test.mjs'))
  .sort()
  .map((name) => join(testsDir, name));

if (!files.length) {
  console.error(`no *.test.mjs files found under ${testsDir}`);
  process.exit(1);
}

const result = spawnSync(process.execPath, ['--test', ...files], { cwd: root, stdio: 'inherit' });
if (result.error) throw result.error;
process.exit(result.status ?? 1);
