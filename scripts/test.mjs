// `npm test`, on whichever machine this is. Windows uses Rustup's stable cargo
// directly so project paths containing spaces never pass through cmd parsing.

import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { homedir } from 'node:os';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const manifest = join('src-tauri', 'Cargo.toml');

// Calling a batch file through `cmd /c` made a checkout whose path contained
// spaces stop at the first word (for example `...\\Hero Siege\\...`). Rustup's
// cargo executable can be launched directly and does not need shell quoting.
const file = process.platform === 'win32' ? join(homedir(), '.cargo', 'bin', 'cargo.exe') : 'cargo';
const argv = ['test', '--manifest-path', manifest, ...args];

execFileSync(file, argv, { cwd: root, stdio: 'inherit' });
