#!/bin/sh
# Milestone 51's breaks: xsdk_test built with each patch here applied, as build/breaks/xsdk_test_<break>.xex. Run in WSL
# from anywhere: sh tests/breaks/build.sh. Each patch is taken back out after its build, even a failed one.
# - noflush: xsdk_save_unmount closes with no flush; fails "xam: a save is flushed before it's unmounted" alone.
# - nowait: xsdk_xam_wait never waits on the event; fails "xam: an overlapped call completes through its event" alone
#   on the console, and nothing in Xenia, which completes that call before it returns.
set -eu
cd "$(dirname "$0")/../.."
for name in noflush nowait; do
	patch -p1 --forward --quiet < "tests/breaks/$name.patch"
	trap 'patch -p1 -R --quiet < "tests/breaks/$name.patch"' EXIT INT TERM
	make -j32 BUILD="build/breaks/$name" "build/breaks/$name/xsdk_test.xex"
	patch -p1 -R --quiet < "tests/breaks/$name.patch"
	trap - EXIT INT TERM
	cp "build/breaks/$name/xsdk_test.xex" "build/breaks/xsdk_test_$name.xex"
done
