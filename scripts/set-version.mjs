// package.json is the single source of the version. This moves it into every
// other file that carries one, and can also just check that they all agree.
//
// Tauri can point at a package.json itself, but it resolves that path against
// the current working directory rather than the config file, so it depends on
// where the build was started from. Copying is boring and always right.
//
//   npm run ver                        # sync the other files to package.json
//   npm run ver 1.1.0                  # set the version everywhere
//   node scripts/set-version.mjs --check
//   node scripts/set-version.mjs --check --expect 1.1.0
//   node scripts/set-version.mjs --root <dir> ...   # operate on another tree (tests only)
//
// `--check` reports the version at every site and fails if they disagree;
// `--expect` additionally requires that agreed-on version to be a specific
// one -- what tracker-release.yml uses to prove the tag it is building
// actually matches the tree it checked out.
//
// One `sites()` list is used by both the writer and `--check`, so the setter
// and the checker cannot drift apart (the property worth copying from
// ForgePact/tools/cut_release.py and the hub's own tools/cut_release.py). A
// site that matches the current version zero times or more than once is a
// refusal, not a rewrite: a half-bumped tree is worse than an un-bumped one,
// because --check would otherwise pass on the files it happened to look at.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
export const SEMVER = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;

function escapeRegExp(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// Every place the version is written down, in the order they are patched.
// `make(version)` returns a fresh RegExp matching that *exact* version at
// this site, with the version between capture groups 1 and 2 -- so the same
// list both proves a specific version is present (--check) and rewrites it
// (the bump). package-lock.json carries the version twice, at two different
// indentation depths, so it is two sites over one file.
export function sites() {
  return [
    {
      path: 'package.json',
      what: 'the package, which is where the version is canonical',
      make: (v) => new RegExp(`("version"\\s*:\\s*")${escapeRegExp(v)}(")`),
    },
    {
      path: join('src-tauri', 'tauri.conf.json'),
      what: 'the Tauri app version -- stamps the binary and names the installer',
      make: (v) => new RegExp(`("version"\\s*:\\s*")${escapeRegExp(v)}(")`),
    },
    {
      path: join('src-tauri', 'Cargo.toml'),
      what: 'the crate version',
      make: (v) => new RegExp(`(^version\\s*=\\s*")${escapeRegExp(v)}(")`, 'm'),
    },
    {
      path: join('src-tauri', 'Cargo.lock'),
      what: 'the hs-offline-tracker package entry -- CRLF on disk, so \\r? before the newline',
      make: (v) => new RegExp(`(name = "hs-offline-tracker"\\r?\\nversion = ")${escapeRegExp(v)}(")`),
    },
    {
      path: 'package-lock.json',
      what: 'the lockfile header',
      make: (v) => new RegExp(`("name": "hs-offline-tracker",\\n {2}"version": ")${escapeRegExp(v)}(")`),
    },
    {
      path: 'package-lock.json',
      what: 'the lockfile root package entry',
      make: (v) => new RegExp(`("name": "hs-offline-tracker",\\n {6}"version": ")${escapeRegExp(v)}(")`),
    },
  ];
}

export function currentVersion(root) {
  const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
  return pkg.version;
}

/** How many times `site`'s pattern (for `version`) appears in its file. */
function hitCount(root, site, version) {
  const pattern = site.make(version);
  const global = new RegExp(pattern.source, pattern.flags.includes('g') ? pattern.flags : `${pattern.flags}g`);
  const blob = readFileSync(join(root, site.path), 'utf8');
  return (blob.match(global) ?? []).length;
}

/** Does every site hold exactly one occurrence of `version`? */
export function report(root, version) {
  let ok = true;
  const lines = [];
  for (const site of sites()) {
    const hits = hitCount(root, site, version);
    if (hits === 1) {
      lines.push(`  ok       ${version}  ${site.path} -- ${site.what}`);
    } else {
      ok = false;
      lines.push(`  MISSING  ${version}  ${site.path} -- ${site.what} (${hits} matches)`);
    }
  }
  return { ok, lines };
}

/** Rewrite every site's current version to `next`. Refuses (writes nothing)
 * if the tree does not already agree on `current`. */
export function write(root, current, next) {
  const pre = report(root, current);
  if (!pre.ok) {
    console.error('the tree does not agree about its current version, so it cannot be bumped safely:');
    for (const line of pre.lines) console.error(line);
    process.exit(1);
  }

  const touched = [];
  for (const site of sites()) {
    const filePath = join(root, site.path);
    const before = readFileSync(filePath, 'utf8');
    const after = before.replace(site.make(current), (_m, pre_, post_) => `${pre_}${next}${post_}`);
    if (after !== before) {
      writeFileSync(filePath, after);
      if (!touched.includes(site.path)) touched.push(site.path);
    }
  }
  return touched;
}

function main(argv) {
  const rootIdx = argv.indexOf('--root');
  const root = rootIdx !== -1 ? resolve(argv[rootIdx + 1]) : REPO_ROOT;
  const checkMode = argv.includes('--check');
  const expectIdx = argv.indexOf('--expect');
  const expect = expectIdx !== -1 ? argv[expectIdx + 1] : null;
  const consumed = new Set();
  if (rootIdx !== -1) consumed.add(rootIdx).add(rootIdx + 1);
  if (expectIdx !== -1) consumed.add(expectIdx).add(expectIdx + 1);
  const positional = argv.filter((a, i) => a !== '--check' && a !== '--expect' && a !== '--root' && !consumed.has(i));

  if (checkMode) {
    const here = currentVersion(root);
    if (!SEMVER.test(here)) {
      console.error(`package.json holds "${here}", which is not a semver version`);
      process.exit(1);
    }
    const { ok, lines } = report(root, here);
    for (const line of lines) console.log(line);
    let allOk = ok;
    if (expect !== null && expect !== here) {
      console.log(`  MISMATCH the tree says ${here}, expected ${expect}`);
      allOk = false;
    }
    process.exit(allOk ? 0 : 1);
  }

  const wanted = positional[0];
  if (wanted && !SEMVER.test(wanted)) {
    console.error(`"${wanted}" is not a version — expected something like 1.1.0`);
    process.exit(1);
  }
  const current = currentVersion(root);
  if (!SEMVER.test(current)) {
    console.error(`package.json holds "${current}", which is not a semver version`);
    process.exit(1);
  }
  const version = wanted ?? current;

  const touched = write(root, current, version);
  console.log(
    touched.length
      ? `v${version} — updated ${touched.join(', ')}`
      : `v${version} — everything already in sync`,
  );
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  main(process.argv.slice(2));
}
