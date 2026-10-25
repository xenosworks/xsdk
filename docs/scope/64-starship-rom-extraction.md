# Milestone 64: Starship makes its sf64.o2r on the console

Agreed on 2026-10-25. Starship needs `sf64.o2r`, the game's assets converted from the player's own ROM; 0.1.0.0 asks
the player to make it on a PC with Starship 2.0.0 (docs/HARDWARE.md, "Starship's assets from memory, milestone 61";
the release's notes). The user asked for the Xbox 360 build to make it itself on its first run, from a ROM copied
beside the `.xex`, and then delete the ROM, with a message for the player.

## What there is

- **Starship's PC build does this** (starship:src/port/extractor/GameExtractor.cpp): it reads the ROM, checks its
  SHA-1 against the versions it knows, and runs Torch, linked in, as
  `Companion(rom, ArchiveType::O2R, false, assets_path, game_path)` then `Init(ExportType::Binary)`. Torch reads
  `config.yml` and the asset definitions, `assets/yaml/<region>/<version>/*.yaml` (103 files, 1.1 MB; US 1.0 and 1.1
  about 0.5 MB each), from beside the executable, and writes `sf64.o2r` there. The xsdk build leaves Torch out
  (starship:CMakeLists.txt:399; src/port/Engine.cpp, the `__XSDK__` message).
- **Torch** (HarbourMasters/Torch `cd92cc0`, Starship's pin) is C++17 on yaml-cpp (`2f86d13`), spdlog (`7e635fc`),
  tinyxml2 10.0.0, libgfxd (`96fd3b8`), GSL, and its own lib/: binarytools, n64graphics, libmio0, libyay0, miniz;
  StormLib only for OTR archives, not O2R. It starts no threads.
- **A big-endian host.** Torch's BinaryReader and BinaryWriter swap only when the stream's byte order isn't the
  host's (lib/binarytools/endianness.h, `Endianness::Native`), and miniz reads and writes its zip a byte at a time
  when the host isn't little-endian (lib/miniz/zip_file.hpp:242, 998-1003). But Torch's own code calls BSWAP16/32
  directly in 57 places, written for a little-endian PC: the audio factories (naudio, sf64's AudioDecompressor),
  `n64/Cartridge.cpp` and `utils/TextureUtils.cpp` among them. Each is wrong on the console until made to depend on
  the host.
- **The check is exact.** `sf64.o2r` from the user's US 1.1 ROM, made on the PC by the Torch Starship pins, holds
  15,898 entries; v2.0.0's Torch made all of them byte for byte the same (2026-10-25). The console's must match it
  entry for entry.
- **xsdk** has the text to draw a screen with (`include/xsdk/text.h`, milestone 57), `std::filesystem` and files
  through its POSIX layer (milestone 54), and the splash that stays up until a title's first frame
  (`include/xsdk/splash.h`).

## Decisions (the user, 2026-10-25)

1. **Any ROM beside the `.xex`:** a `.z64`, `.n64` or `.v64` of any name, put in `.z64` order in memory; the first
   whose SHA-1 is US 1.0 or 1.1 is used.
2. **The ROM is deleted only once `sf64.o2r` is checked:** written whole as `sf64.o2r.part`, renamed into place and
   opened by the game. Any failure keeps the ROM and says why.
3. **Torch is a local fork,** `Z:\Torch-xenon` on branch `xenon` from `cd92cc0`, no repository in xenosworks for now;
   pushing it, or making one, waits for the user.
4. **A progress screen,** in xsdk's text: what it's doing and how far, then that it's done and the ROM removed, for a
   few seconds; an error stays until a button is pressed: no ROM found, a ROM Starship doesn't take, too little space.

## Steps

1. **Torch on the console, checked against the PC's.** Torch-xenon's library built for powerpc-xsdk-elf, with
   yaml-cpp and libgfxd at Torch's pins, by local/m61/build.sh's way of building libraries; a check title that runs
   Torch on the ROM from its folder into a file of its own, and a PC script that compares that file with the PC's
   entry by entry. The byte-order sites are fixed in Torch-xenon until all 15,898 match, each fix kept to the host's
   order (the BE/LE macros endianness.h already has). The extraction's time and memory are logged.
2. **Starship's first run.** In Starship's xsdk start-up, before the archives open: no `sf64.o2r`, then the ROM
   found, swapped and checked, Torch run with its progress on the screen, `sf64.o2r.part` renamed, the ROM deleted
   once the game opens the archive; the errors of decision 4. Checked on the console from a folder as the store
   installs it, with a `.v64` and a `.n64` copy, a ROM it doesn't take, and an interrupted run (a `.part` left).
3. **Release 0.2.0.0.** The package gains `config.yml` and the US yamls; README.txt and the release notes lose the PC
   step; a release, the catalogue entry packed, and the user signs.

Breaks, each failing what it names: a byte-order fix taken out (entries differ), a wrong ROM (its error), the ROM kept
by a failing check (still there).

## Not in it

EU and JP ROMs for Starship's voice replacement; extraction for any other libultraship game.

## Size

Medium. Step 1 is most of it, and how many of the 57 sites matter, and what else assumes a little-endian host, isn't
known until the console's first comparison.
