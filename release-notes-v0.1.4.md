Closing Hero Siege no longer ends in a crash report.

## Fixed

**Hero Siege could crash as you closed it.** With the Tracker's live sensor installed, closing the game could end with Windows reporting that Hero Siege had crashed (faulting module `ucrtbase.dll`, exception `0xc0000409`) and a crash dump in `%LOCALAPPDATA%\CrashDumps`. The crash came at the very end, after the game had finished closing. The sensor left a background task running, and cleaning it up during the game's shutdown ended in that crash. 9 of the 10 crash dumps Windows had kept on 25–26 September were this one.

The sensor now leaves nothing running that the shutdown has to clean up. In checks on 26 and 27 September, the old sensor crashed a close from the main menu. With the new one, the same close ended cleanly three times out of three, and so did a normal close after about seven minutes of play.

## Changed

**The mod loader is now the toolkit's own build of Aurie.** Every time a mod hooks into the game, the loader pauses the game's other threads for a moment. The original loader found those threads by listing every thread on the whole computer, twice, for each hook. The new one lists only the game's own threads. With ForgePact loaded, measured on one machine, that brought its start-up hook setup down from about 1.3 seconds to under 0.05 seconds. ForgePact already ships this same loader, so if you use both tools, they now install the identical file.

## How to update

Run the new setup, or unzip the portable build. Then open Settings > Game link and press **Update live sensor**: that puts the fixed sensor and the new loader in your game folder. Until you do, the game still has the old ones. If your game folder has a loader the Tracker does not recognise (for example, one from a newer ForgePact), it is left alone, and Settings says so.

## Notes

If you also use ForgePact: its Install Mod Plugin button installs the copy of the sensor ForgePact ships. That copy is still the old one, until a ForgePact update carries this fix. After installing or reinstalling ForgePact's plugin, Settings > Game link shows Update live sensor again; press it.

The sensor only reads; it never writes to the game.

## Files

| File | What it is |
| --- | --- |
| `HS Offline Tracker_0.1.4_x64-setup.exe` | Installer, carries the sensor and the loader (the loader's notice is in `aurie-loader/aurie-modified/`) |
| `hs-offline-tracker-0.1.4-portable.zip` | Portable build, no installer |
| `SHA256SUMS-0.1.4.txt` | Checksums for the two files above |
