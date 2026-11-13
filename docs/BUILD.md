# Build it yourself

LAS Recomp's own build guides are in this repository: [BUILD-windows.md](BUILD-windows.md), [linux.md](linux.md) and the
macOS section of [LAS-RECOMP-README.md](LAS-RECOMP-README.md). The v0.1.0 packages were built this way:

- **Windows**: MSYS2 UCRT64 and Python 3.12 for the tools, then `make rando-helper`, `make gen-helper` and `make dist`.
  `tools/windows/itd_installer.sh <dist> v0.1.0 <out>` then makes the Inno installer and the zip.
- **Linux**: `tools/linux/appimage.sh x86_64`, which runs in Ubuntu 22.04 podman containers (`tools/linux/Containerfile`,
  glibc 2.35 or newer), then `tools/linux/smoketest.sh` for the release test.
- **Mac** (Apple Silicon, with cmake, ninja and pkg-config from Homebrew): `make deps` once, then `make gen-helper`,
  `make rando-helper`, `make app` and `tools/make_dmg.sh`.

No build includes any game file: the game's code is rebuilt on the player's computer, from the player's own files.
