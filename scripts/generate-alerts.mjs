import { createHash } from 'node:crypto';
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const out = join(root, 'src', 'assets', 'sounds');
const work = mkdtempSync(join(tmpdir(), 'hsot-alerts-'));
const supplied = join(root, 'scripts', 'sound-sources', 'elevenlabs');

// The five rarity alerts are project-supplied ElevenLabs Sound Effects
// generations. Mail and zone remain documented CC0 recordings. Pinning every
// raw input keeps regeneration deterministic and prevents a changed download
// or accidental replacement from silently entering a release. Full provenance
// and prompts are recorded in src/assets/sounds/SOURCES.md.
const tracks = [
  {
    key: 'set',
    local: join(supplied, 'set.wav'),
    sha256: 'd9805920a4ddd970dddc0b2ffeda885730487846baa406f36ae41d107757970e',
    channels: 2,
    filter: 'atrim=start=0:end=1.14,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.006,afade=t=out:st=1.12:d=0.01,loudnorm=I=-16:TP=-1.5:LRA=7',
  },
  {
    key: 'satanic',
    local: join(supplied, 'satanic.wav'),
    sha256: '24d71ddb5f55ed0dd3766e9e36784b6b7ede17aef7cd36d0162fa65fabd409c5',
    channels: 2,
    filter: 'atrim=start=0:end=1.385,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.006,afade=t=out:st=1.365:d=0.01,loudnorm=I=-16:TP=-1.5:LRA=7',
  },
  {
    key: 'heroic',
    local: join(supplied, 'heroic.wav'),
    sha256: '8e112af28ad28106b52848704a883d5d66d258f1fe9f0ff9ee1ffe41f5ef3619',
    channels: 2,
    filter: 'atrim=start=0:end=1.519,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.006,afade=t=out:st=1.499:d=0.01,loudnorm=I=-16:TP=-1.5:LRA=7',
  },
  {
    key: 'angelic',
    local: join(supplied, 'angelic.wav'),
    sha256: '9dc08a7dc6f3901ddf390246ccd9550ccdaa6d86e5d067a93b9757582e97b733',
    channels: 2,
    filter: 'atrim=start=0:end=1.373,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.006,afade=t=out:st=1.353:d=0.01,loudnorm=I=-16:TP=-1.5:LRA=7',
  },
  {
    key: 'unholy',
    local: join(supplied, 'unholy.wav'),
    sha256: 'a5101558025b904755038520dd2ab13859fda30d7b1d6d71b0e039ac95fa897d',
    channels: 2,
    filter: 'atrim=start=0:end=2.009,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.006,afade=t=out:st=1.989:d=0.01,loudnorm=I=-16:TP=-1.5:LRA=7',
  },
  {
    key: 'mail',
    url: 'https://cdn.freesound.org/previews/242/242502_4414128-hq.mp3',
    sha256: 'fa9ade19a2894ff92c691d4a1d38f818cf501feb3e57c53e8be5a4058771253f',
    filter: 'atrim=start=0:duration=0.423,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.008,afade=t=out:st=0.34:d=0.075,loudnorm=I=-18:TP=-3:LRA=7,alimiter=limit=0.70:level=false',
  },
  {
    key: 'zone',
    local: join(root, 'scripts', 'sound-sources', 'zone-dark-ambience-1.wav'),
    sha256: '23c9438b65835f8e493a00de244387be33e0303e774e15022692cabb1b707df0',
    filter: 'atrim=start=0:duration=4.62,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=0.04,afade=t=out:st=4.05:d=0.52,loudnorm=I=-23:TP=-3:LRA=7,volume=7dB,alimiter=limit=0.70:level=false',
  },
];

function sha256(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

async function sourceFor(track) {
  if (track.local) return track.local;
  const target = join(work, `${track.key}.mp3`);
  const response = await fetch(track.url);
  if (!response.ok) throw new Error(`${track.key}: download failed (HTTP ${response.status})`);
  writeFileSync(target, Buffer.from(await response.arrayBuffer()));
  return target;
}

function encode(track, source) {
  const target = join(out, `${track.key}.wav`);
  const result = spawnSync('ffmpeg', [
    '-hide_banner', '-loglevel', 'error', '-y',
    '-i', source,
    '-af', track.filter,
    '-ar', '48000', '-ac', String(track.channels ?? 1), '-c:a', 'pcm_s16le',
    target,
  ], { stdio: 'inherit' });
  if (result.error?.code === 'ENOENT') {
    throw new Error('ffmpeg was not found on PATH. Install FFmpeg 7 or newer and retry.');
  }
  if (result.status !== 0) throw new Error(`${track.key}: ffmpeg exited with ${result.status}`);
}

mkdirSync(out, { recursive: true });

try {
  for (const track of tracks) {
    const source = await sourceFor(track);
    const actual = sha256(source);
    if (actual !== track.sha256) {
      throw new Error(`${track.key}: source hash changed (expected ${track.sha256}, got ${actual})`);
    }
    encode(track, source);
    console.log(`prepared ${track.key}.wav`);
  }
  console.log(`Prepared seven documented default alerts in ${out}`);
} finally {
  rmSync(work, { recursive: true, force: true });
}
