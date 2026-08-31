import { spawnSync } from 'node:child_process';
import { homedir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const tauri = join(root, 'node_modules', '@tauri-apps', 'cli', 'tauri.js');
const env = { ...process.env };

// A long-running Codex/IDE process may not see the PATH update made by a new
// rustup install. Add the canonical rustup bin directory without shell-specific
// quoting; on normal terminals this is simply a harmless duplicate.
if (process.platform === 'win32') {
  // Preserve Windows' original key casing. Adding a second `PATH` beside
  // `Path` leaves cmd.exe free to pick the short duplicate and lose npm.
  const pathKey = Object.keys(env).find((key) => key.toLowerCase() === 'path') ?? 'Path';
  env[pathKey] = `${join(homedir(), '.cargo', 'bin')}${delimiter}${env[pathKey] ?? ''}`;
}

const result = spawnSync(process.execPath, [tauri, ...process.argv.slice(2)], {
  cwd: root,
  env,
  stdio: 'inherit',
});

if (result.error) throw result.error;
process.exit(result.status ?? 1);
