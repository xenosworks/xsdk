#!/bin/sh
# xsdk_test's breaks: xsdk_test built with each patch here applied, as build/breaks/xsdk_test_<break>.xex. Run in WSL
# from anywhere: sh tests/breaks/build.sh. Each patch is taken back out after its build, even a failed one.
# Milestone 51's:
# - noflush: xsdk_save_unmount closes with no flush; fails "xam: a save is flushed before it's unmounted" alone.
# - nowait: xsdk_xam_wait never waits on the event; fails "xam: an overlapped call completes through its event" alone
#   on the console, and nothing in Xenia, which completes that call before it returns.
# Milestone 44 step 3's, each failing the checks results/2026-10-20-milestone-44-step-3/README.md names for it:
# - msaa_sample0: every resolve copies sample 0; the edge counts at 2 and 4 samples fail.
# - msaa_onesample: xsdk_gpu_set_targets_samples sets 1 sample; its edge counts and the tiled frames' references fail.
# - msaa_tiles64: a 64-bit target's tiles not doubled at 4 samples; the tile counts and four at once at 4 fail.
# - msaa_bands2: a 4-sample pass's bands sized as for 2 samples, its colour wrapping past eDRAM's end; the band counts
#   and the 4-sample tiled frame fail.
# Milestone 44 step 4's, each failing the checks results/2026-10-20-milestone-44-step-4/README.md names for it:
# - atoc_noenable: alpha to mask's enable bit left off; every alpha below 1 covers every sample, and the alpha to
#   coverage checks at 1, 2 and 4 samples fail.
# - atoc_offsets0: the offsets left at 0; each threshold moves up half a step, and the same three fail.
# Milestone 54's:
# - pwrite_position: pwrite writes at the descriptor's position, not the offset; fails "pwrite: writes at an offset,
#   read back by pread, the position and size unchanged" alone.
# - tz_sign: TZ's offsets get the wrong sign; fails "settings: localtime is UTC moved by the console's time zone, and
#   mktime takes it back" alone, wherever the console's time is off UTC as the test runs.
set -eu
cd "$(dirname "$0")/../.."
for name in noflush nowait msaa_sample0 msaa_onesample msaa_tiles64 msaa_bands2 atoc_noenable atoc_offsets0 \
	pwrite_position tz_sign; do
	patch -p1 --forward --quiet < "tests/breaks/$name.patch"
	trap 'patch -p1 -R --quiet < "tests/breaks/$name.patch"' EXIT INT TERM
	make -j32 BUILD="build/breaks/$name" "build/breaks/$name/xsdk_test.xex"
	patch -p1 -R --quiet < "tests/breaks/$name.patch"
	trap - EXIT INT TERM
	cp "build/breaks/$name/xsdk_test.xex" "build/breaks/xsdk_test_$name.xex"
done
