// Check the tag a Tracker release is being cut as, and compose its notes.
//
//   node scripts/tracker-tag.mjs --tag v0.1.4 --existing v0.1.1 v0.1.2 --tree 0.1.3
//   node scripts/tracker-tag.mjs --compose-notes --version 0.1.3 --previous v0.1.2 \
//     --generated generated.md --out release-notes.md
//   node scripts/tracker-tag.mjs --published-notes --version 0.1.3
//   node scripts/tracker-tag.mjs --normalize --tag v0.1.3
//
// `--normalize` is the one mode with no git and no comparison: it only
// agrees with plan mode on what a version looks like (SHAPE, one optional
// leading `v`), and is what tracker-release.yml uses to turn its own typed
// `tag` input into `tag=`/`version=` -- it needs no existing-tags list and no
// tree, because it never decides whether a tag can be created, only what one
// spells out to.
//
// Ported from ForgePact/tools/forgepact_tag.py -- same reasoning, same `v`
// prefix, same refusals, rewritten for this repository's Node tooling (D6)
// and its scripts/set-version.mjs in place of cut_release.py (D3). Do not
// re-derive the reasoning below; it is copied deliberately.
//
// **Plan mode** (the default) prints four `key=value` lines for
// `$GITHUB_OUTPUT` and nothing else:
//
//   version=0.1.4
//   tag=v0.1.4
//   bump=true
//   previous=v0.1.2
//
// `bump=true` means the tree still says something else and has to be
// rewritten with set-version.mjs before the tag is created. `previous` is
// the highest released `v*` tag numerically below the version, or empty when
// there is none.
//
// Anything wrong exits non-zero having printed nothing to stdout, so a
// workflow that ignored the exit status still has no version to act on.
//
// Four refusals, because everything downstream trusts this tag: the tree is
// rewritten to match it, the commit is pushed to `main`, the tag is pushed,
// and a draft release is created on it.
//
// 1. **Wrong shape** -- not three plain ASCII numbers (no leading zero),
//    optionally one lowercase `v`. The string reaches a shell and `git tag`
//    either way.
// 2. **The tag already exists** -- a second release on one tag makes
//    releases/latest ambiguous, and the hub reads exactly that endpoint.
// 3. **Below the highest existing `v*` tag** -- releases/latest would point
//    backwards at something older than players are running.
// 4. **Below what `main` already holds** -- the tree and the highest tag can
//    disagree; without this, tagging an older number relabels newer code.
//
// (A fifth refusal -- the version already has a release, drafts included --
// lives in the workflow, as a `gh api releases` query: a draft does not
// create its tag, so it is invisible to refusals 2 and 3.)
//
// Comparison is numeric, never lexical; tags from other schemes are ignored.
// Release notes are never a refusal: compose-notes falls back to GitHub's
// generated notes under a banner demanding a rewrite before publishing.
// Skipped versions' notes are concatenated newest first, because the hub
// truncates a release body at 8000 characters (tools/build_catalog.py) and
// the cut must never land on the version players are updating to.
// `## How to update` is kept only in the first file-sourced section.
//
// **Published notes** (`--published-notes --version X`) lists the bare
// filenames of every `release-notes-v<version>.md` at or below `X`,
// numerically oldest first -- what tracker-notes-cleanup.yml deletes once a
// release is published.

import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = join(new URL('.', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'), '..');

export const PREFIX = 'v';

// Anchored at both ends, ASCII-digit only, no leading zero -- matches
// ForgePact's cut_release.VERSION. \d would also match a non-ASCII digit.
const SHAPE = /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;

const NOTES_NAME = /^release-notes-v(.+)\.md$/;

const HOW_TO_UPDATE_HEADING = '## How to update';

const BANNER = (version) =>
  `> **Draft notes, generated from pull request titles.** Rewrite the HS Offline Tracker ${version} section for players before publishing: the Toolkit Hub shows this text to players.`;

/** Thrown by every refusal below. `main()` is the only thing that catches
 * it -- printed to stderr, nothing on stdout, exit 1. Library callers (the
 * tests, and any future caller) see a normal, catchable error instead of
 * the process disappearing under them. */
export class TagRefusal extends Error {}

function refuse(message) {
  throw new TagRefusal(message);
}

function asNumbers(version) {
  return version.split('.').map((p) => parseInt(p, 10));
}

function compareNumeric(a, b) {
  const na = asNumbers(a);
  const nb = asNumbers(b);
  for (let i = 0; i < 3; i++) {
    if (na[i] !== nb[i]) return na[i] - nb[i];
  }
  return 0;
}

/** Tag names out of whatever git printed: `git ls-remote --tags` gives
 * `refs/tags/v0.1.2` and a peeled `refs/tags/v0.1.2^{}` for an annotated
 * tag; `git tag --list` gives the bare name. Both arrive here. */
function tagNames(refs) {
  const names = new Set();
  for (const ref of refs) {
    let name = ref.trim();
    if (!name) continue;
    const idx = name.lastIndexOf('refs/tags/');
    if (idx !== -1) name = name.slice(idx + 'refs/tags/'.length);
    if (name.endsWith('^{}')) name = name.slice(0, -3);
    names.add(name);
  }
  return names;
}

/** @returns {{version: string, tag: string, bump: boolean, previous: string}} */
export function plan(raw, refs, tree) {
  let version = raw.trim();
  if (version.startsWith(PREFIX)) version = version.slice(PREFIX.length);

  if (!SHAPE.test(version)) {
    let hint = '';
    if (/^[0-9]+\.[0-9]+\.[0-9]+$/.test(version)) {
      hint = ' Drop the leading zero: set-version.mjs will not write 01.1.4.';
    }
    refuse(`${raw.trim()} is not a version this can tag. Give three numbers, as 1.2.3 or ${PREFIX}1.2.3.${hint}`);
  }

  const tag = PREFIX + version;
  const taken = tagNames(refs);
  if (taken.has(tag)) {
    refuse(
      `${tag} already exists. Tagging it again would give it a second release, and releases/latest would ` +
        'resolve to whichever one GitHub calls latest. Pick a higher version, or delete that tag and its release first.',
    );
  }

  const released = [...taken].filter((name) => name.startsWith(PREFIX) && SHAPE.test(name.slice(PREFIX.length)));
  let highest = null;
  if (released.length) {
    highest = released.reduce((best, name) => (compareNumeric(name.slice(PREFIX.length), best.slice(PREFIX.length)) > 0 ? name : best));
    if (compareNumeric(version, highest.slice(PREFIX.length)) < 0) {
      refuse(
        `${tag} is behind ${highest}, which is already tagged. Publishing it would point releases/latest at ` +
          'an older version than the one players are running. To release an older line on purpose, push the tag by hand.',
      );
    }
  }

  if (compareNumeric(version, tree) < 0) {
    refuse(
      `${tag} is behind the tree, which is already at ${tree}. Tagging it would relabel ${tree}'s code as ` +
        `${version}. If you mean to tag the code that is on main right now, use ${tree}.`,
    );
  }

  return { version, tag, bump: tree !== version, previous: highest ?? '' };
}

/** Which bare filenames are `release-notes-v<version>.md` for an accepted
 * version. Order is unspecified; callers that need one sort it themselves. */
export function noteVersions(names) {
  const versions = [];
  for (const name of names) {
    const match = NOTES_NAME.exec(name);
    if (!match) continue;
    if (SHAPE.test(match[1])) versions.push(match[1]);
  }
  return versions;
}

function previousBound(previous) {
  if (!previous) return null;
  const bare = previous.startsWith(PREFIX) ? previous.slice(PREFIX.length) : previous;
  return asNumbers(bare);
}

function cmpTuple(a, b) {
  for (let i = 0; i < 3; i++) {
    if (a[i] !== b[i]) return a[i] - b[i];
  }
  return 0;
}

/** @returns {{topFromFile: boolean, skipped: string[]}} skipped is newest
 * first, numerically. With no previous tag, skipped is empty by definition
 * -- there is no lower bound to bound it with. */
export function notesPlan(available, version, previous) {
  const availableSet = new Set(available);
  const topFromFile = availableSet.has(version);

  const bound = previousBound(previous);
  const versionKey = asNumbers(version);

  let skipped = [];
  if (bound !== null) {
    for (const candidate of available) {
      if (candidate === version) continue;
      const key = asNumbers(candidate);
      if (cmpTuple(bound, key) < 0 && cmpTuple(key, versionKey) < 0) skipped.push(candidate);
    }
  }
  skipped.sort((a, b) => compareNumeric(b, a));
  return { topFromFile, skipped };
}

/** Versions whose notes a published `version` has made redundant, oldest first. */
export function publishedNotes(available, version) {
  const ceiling = asNumbers(version);
  return [...available].filter((v) => cmpTuple(asNumbers(v), ceiling) <= 0).sort(compareNumeric);
}

function normalise(text) {
  return text.replace(/\r\n/g, '\n').trim();
}

function englishList(items) {
  if (items.length === 1) return items[0];
  if (items.length === 2) return `${items[0]} and ${items[1]}`;
  return `${items.slice(0, -1).join(', ')} and ${items[items.length - 1]}`;
}

function stripHowToUpdate(text) {
  const lines = text.split('\n');
  const idx = lines.findIndex((line) => line.trim() === HOW_TO_UPDATE_HEADING);
  if (idx === -1) return text;
  return lines.slice(0, idx).join('\n').replace(/\s+$/, '');
}

/** Prepend a `# HS Offline Tracker <v>` heading if the section does not
 * already have one. */
function labelled(text, version) {
  for (const line of text.split('\n')) {
    const stripped = line.trim();
    if (!stripped) continue;
    if (stripped.startsWith('# ')) return text;
    break;
  }
  return `# HS Offline Tracker ${version}\n\n${text}`;
}

/** @returns {[string, string]} [body, source] */
export function composeBody(version, topText, generated, skipped) {
  generated = normalise(generated);
  skipped = skipped.map(([v, t]) => [v, normalise(t)]);

  const sections = [];
  const preamble = [];
  let firstFileSectionUsed = false;
  let topIsFile;

  if (topText !== null) {
    sections.push(labelled(normalise(topText), version));
    firstFileSectionUsed = true;
    topIsFile = true;
  } else {
    sections.push(`# HS Offline Tracker ${version}\n\n${generated}`);
    topIsFile = false;
    preamble.push(BANNER(version));
  }

  if (skipped.length) {
    const names = skipped.map(([v]) => v);
    preamble.push(`This release also carries the notes for ${englishList(names)}, which were never released on their own.`);
  }

  for (const [v, text] of skipped) {
    let section = labelled(text, v);
    if (!firstFileSectionUsed) {
      firstFileSectionUsed = true;
    } else {
      section = stripHowToUpdate(section);
    }
    sections.push(section);
  }

  let body = sections.join('\n\n---\n\n');
  if (preamble.length) body = `${preamble.join('\n\n')}\n\n${body}`;
  body = `${body.trim()}\n`;

  let source;
  if (topIsFile) source = skipped.length ? 'files' : 'file';
  else source = skipped.length ? 'mixed' : 'generated';

  return [body, source];
}

function readNotesText(path) {
  // Strip a UTF-8 BOM the way Python's utf-8-sig codec does.
  const text = readFileSync(path, 'utf8');
  return text.charCodeAt(0) === 0xfeff ? text.slice(1) : text;
}

function stripPrefix(raw) {
  const v = raw.trim();
  return v.startsWith(PREFIX) ? v.slice(PREFIX.length) : v;
}

function composeNotesMain(args) {
  const version = stripPrefix(args.version);
  if (!SHAPE.test(version)) refuse(`"${args.version}" is not a version this can compose notes for.`);

  const root = args.root ?? REPO_ROOT;
  const names = readdirSync(root);
  const available = noteVersions(names);

  const planResult = notesPlan(available, version, args.previous ?? '');

  const topText = planResult.topFromFile ? readNotesText(join(root, `release-notes-v${version}.md`)) : null;
  const generated = readNotesText(args.generated);
  const skippedPairs = planResult.skipped.map((v) => [v, readNotesText(join(root, `release-notes-v${v}.md`))]);

  const [body, source] = composeBody(version, topText, generated, skippedPairs);
  writeFileSync(args.out, body, 'utf8');

  const versionsInOrder = [version, ...planResult.skipped];
  console.log(`source=${source}`);
  console.log(`versions=${versionsInOrder.join(' ')}`);
}

function publishedNotesMain(args) {
  const version = stripPrefix(args.version);
  if (!SHAPE.test(version)) refuse(`"${args.version}" is not a version this can list notes for.`);
  const root = args.root ?? REPO_ROOT;
  const available = noteVersions(readdirSync(root));
  for (const v of publishedNotes(available, version)) console.log(`release-notes-v${v}.md`);
}

function parseArgs(argv) {
  const args = { existing: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case '--tag':
        args.tag = argv[++i];
        break;
      case '--tree':
        args.tree = argv[++i];
        break;
      case '--existing': {
        const values = [];
        while (argv[i + 1] && !argv[i + 1].startsWith('--')) values.push(argv[++i]);
        args.existing = values;
        break;
      }
      case '--root':
        args.root = resolve(argv[++i]);
        break;
      case '--normalize':
        args.normalize = true;
        break;
      case '--compose-notes':
        args.composeNotes = true;
        break;
      case '--published-notes':
        args.publishedNotes = true;
        break;
      case '--version':
        args.version = argv[++i];
        break;
      case '--previous':
        args.previous = argv[++i];
        break;
      case '--generated':
        args.generated = argv[++i];
        break;
      case '--out':
        args.out = argv[++i];
        break;
      default:
        console.error(`unrecognised argument: ${a}`);
        process.exit(2);
    }
  }
  return args;
}

/** Normalise a typed tag/version the same way `plan()` does (SHAPE, one
 * optional leading `v`), with no git and no comparison against anything --
 * used by tracker-release.yml, which only needs to agree with plan mode on
 * what a version looks like, never to decide whether it can be tagged. */
function normalizeMain(args) {
  const version = stripPrefix(args.tag);
  if (!SHAPE.test(version)) {
    refuse(`"${args.tag}" is not a version this can build. Give three numbers, as 1.2.3 or ${PREFIX}1.2.3.`);
  }
  console.log(`tag=${PREFIX}${version}`);
  console.log(`version=${version}`);
}

function main(argv) {
  const args = parseArgs(argv);

  if (args.normalize) {
    if (!args.tag) refuse('--normalize needs --tag');
    normalizeMain(args);
    return;
  }

  if (args.publishedNotes) {
    if (args.composeNotes) refuse('--published-notes and --compose-notes are separate modes');
    if (!args.version) refuse('--published-notes needs --version');
    if (args.tag || args.existing.length || args.generated || args.out || args.previous !== undefined) {
      refuse('--published-notes takes only --version and --root');
    }
    publishedNotesMain(args);
    return;
  }

  if (args.composeNotes) {
    if (!args.version || !args.generated || !args.out) refuse('--compose-notes needs --version, --generated and --out');
    if (args.tag || args.existing.length) refuse('--compose-notes does not take --tag or --existing');
    composeNotesMain(args);
    return;
  }

  if (args.version || args.generated || args.out) refuse('--version/--generated/--out are only for --compose-notes');
  if (!args.tag) refuse('give --tag, or --compose-notes, or --published-notes');

  const tree = args.tree;
  if (tree === undefined) refuse('--tree is required in plan mode (set-version.mjs reads the live tree; this tool does not shell out to it)');

  const chosen = plan(args.tag, args.existing, tree);

  // Four bare lines, appended straight to $GITHUB_OUTPUT. Nothing is
  // printed on the refusal path, so a workflow that ignored the exit code
  // would still have no version to act on.
  console.log(`version=${chosen.version}`);
  console.log(`tag=${chosen.tag}`);
  console.log(`bump=${chosen.bump ? 'true' : 'false'}`);
  console.log(`previous=${chosen.previous}`);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  try {
    main(process.argv.slice(2));
  } catch (err) {
    if (err instanceof TagRefusal) {
      console.error(err.message);
      process.exit(1);
    }
    throw err;
  }
}
