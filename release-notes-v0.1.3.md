A cleaner mod loader, and an existing install now keeps it up to date.

## What is new

**The bundled mod loader is replaced with a documented build.** The Tracker ships YYToolkit, the loader that lets the live sensor run inside the game. The copy it used to ship was an older, undocumented build, the same one ForgePact shipped, and nobody could say exactly what was in it. It is now a build made from a published, documented set of changes. A small record of exactly what it was built from now ships next to it.

**Updating the Tracker now updates the loader in your game folder too.** Before, the loader was installed once and never touched again: a newer Tracker brought a newer loader, but the old one stayed in your game folder, and "Reinstall live sensor" said it worked while changing nothing. Settings > Game link now tells you when the installed loader is an older build, and Reinstall live sensor replaces it. A loader it does not recognise, for example a newer one from another tool, is left alone and reported, never overwritten.

**Every build is checked before it ships.** The mod loader files in a release are now checked against a list of their expected fingerprints, so a wrong or incomplete file stops the release instead of reaching you as a sensor that silently refuses to start.

## Install

Run the setup, open Settings > Game link, press Install, then start Hero Siege. If the game folder has no mod loader, the Tracker installs one for you. If it has the older one, Settings says so, and Reinstall live sensor brings it up to date. The portable zip is the same program without an installer; keep the `producer` and `aurie-loader` folders next to the exe.

## Notes

If you also use ForgePact, use version 1.4.4 or newer: it ships the same loader. Both tools install the loader into the same place, so installing an older ForgePact after this puts the old loader back. Reinstall live sensor fixes that.

Magic find is still not shown: the game's stat routine cannot be observed on this build without destabilising it. The sensor only reads; it never writes to the game.

## Files

| File | What it is |
| --- | --- |
| `HS Offline Tracker_0.1.3_x64-setup.exe` | Installer, carries the sensor and the loader |
| `hs-offline-tracker-0.1.3-portable.zip` | Portable build, no installer |
| `SHA256SUMS-0.1.3.txt` | Checksums for the two files above |
