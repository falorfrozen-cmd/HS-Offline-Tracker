A cleaner, documented mod loader with real provenance, verified before it ships, and an existing install now updates itself.

## What is new

**The bundled YYToolkit is now the toolkit hub's documented `hs.1` build.** The previous `YYToolkit.dll` was the same undocumented binary ForgePact used to ship, whose notice claimed only two changes while the binary's own strings said otherwise. It is replaced with a launch-gated build produced from the hub's own patch series, with a `YYToolkit-BUILD-INFO.json` provenance record now bundled alongside the loader notice so the exact patches and commit a copy was built from are always on hand.

**An existing installation updates its loader instead of being left behind.** Previously the loader was only ever installed once; a Tracker upgrade that replaced the bundled `YYToolkit.dll` left the old file on disk forever, and pressing "Reinstall live sensor" reported success while changing nothing. Game Link now classifies the installed file by its sha256 against a checked-in manifest and replaces a missing or known-superseded copy in place -- a copy from another tool it does not recognise is left alone and reported, never overwritten.

**The bundled binaries are verified before they ship.** A new check hashes `aurie-loader/`'s three binaries against the same manifest the installer reads, and confirms every loader resource `tauri.conf.json` bundles actually exists on disk -- so a wrong or partial file is caught before packaging, not as a fail-closed sensor at runtime or a late, confusing Tauri error.

## Install

Run the setup, open Settings > Game link, press Install, then start Hero Siege. If the game folder has no mod loader the tracker installs one for you; if it already has an older one, Install now brings it up to date instead of leaving it as-is. The portable zip is the same program without an installer; keep the `producer` and `aurie-loader` folders next to the exe.

## Notes

Magic find is still not shown: the game's stat routine cannot be observed on this build without destabilising it. The sensor only reads; it never writes to the game.

## Files

| File | What it is |
| --- | --- |
| `HS Offline Tracker_0.1.3_x64-setup.exe` | Installer, carries the sensor and the loader |
| `hs-offline-tracker-0.1.3-portable.zip` | Portable build, no installer |
| `SHA256SUMS-0.1.3.txt` | Checksums for the two files above |
