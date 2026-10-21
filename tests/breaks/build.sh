#!/bin/sh
# The breaks: xsdk_test, or the title a break names, built with each patch here applied, as
# build/breaks/<title>_<break>.xex. Run in WSL from anywhere: sh tests/breaks/build.sh. Each patch is taken back out
# after its build, even a failed one.
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
# - hdd_device: hdd: names the USB stick's device; fails "drives: hdd: lists its root and holds the .xex game: runs
#   from" alone, where the .xex runs from the hard disk.
# - terminate_noregister, of terminate_check: xsdk_on_terminate never registers; the read case after a run fails
#   "terminate check: the routine ran as the title was ended from outside" and "the routine that ran is the one
#   registered last". It takes nothing out, as a registration left in place freezes the console.
# - letterbox_row0: a letterboxed frame resolves at the front buffer's row 0, its bars all below it; fails "scale:
#   letterboxed, the front buffer is black above row 128 and from row 848, the frame between" alone.
# - ftruncate_nozeros: a file's size is set with no zeros written over what it adds; fails "ftruncate and truncate:
#   shorten a file, then lengthen it with zeros" alone.
# - fsync_socket: fsync of a socket returns 0; fails "fsync: flushes a file and fails on a socket" alone.
# - statvfs_sector: statvfs's block is the sector, not the allocation unit; fails "statvfs: a written file takes its
#   allocation units from the free ones, fstatvfs agreeing on the unit" alone.
# - language_zero: xsdk_system_language gives 0; fails "settings: the console's language and country read" alone.
# - letterbox_720: the 1280x960 front buffer's display and scaler planned for 720 rows; fails "scale: letterboxed, the
#   display shows a 1280x960 front buffer, the scaler planned for it" alone.
# - letterbox_kept: letterboxing's scaler isn't taken back as the present closes; fails "scale: letterboxing's scaler is
#   taken back as the present closes" alone. The scaler stays letterboxed until the dashboard sets its own.
# - usb_mass1: usb: names \Device\Mass1\; with a stick in the first port, fails "drives: usb: lists its root as
#   \Device\Mass0\ does, or fails with ENOENT where it does" alone.
set -eu
cd "$(dirname "$0")/../.."
build_break()
{
	name=$1
	title=$2
	patch -p1 --forward --quiet < "tests/breaks/$name.patch"
	trap 'patch -p1 -R --quiet < "tests/breaks/$name.patch"' EXIT INT TERM
	make -j32 BUILD="build/breaks/$name" "build/breaks/$name/$title.xex"
	patch -p1 -R --quiet < "tests/breaks/$name.patch"
	trap - EXIT INT TERM
	cp "build/breaks/$name/$title.xex" "build/breaks/${title}_$name.xex"
}
for name in noflush nowait msaa_sample0 msaa_onesample msaa_tiles64 msaa_bands2 atoc_noenable atoc_offsets0 \
	pwrite_position tz_sign hdd_device letterbox_row0 ftruncate_nozeros fsync_socket statvfs_sector language_zero \n	letterbox_720 letterbox_kept usb_mass1; do
	build_break "$name" xsdk_test
done
build_break terminate_noregister terminate_check
