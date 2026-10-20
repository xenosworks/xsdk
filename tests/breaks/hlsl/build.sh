#!/bin/sh
# Milestone 50 steps 3, 4 and 5's breaks: xsdk_test built with each patch here applied, as
# build/breaks/xsdk_test_hlsl_<break>.xex. Each but link breaks xost in the title, which compiles the HLSL grid's
# shaders on the console (tests/test_hlsl.c), so test_xsc's check of that shader against the PC's container fails too.
# Run in WSL from anywhere: sh tests/breaks/hlsl/build.sh. Each patch is taken back out after its build, even a failed
# one.
# - link: xsdk_shader_link leaving each pair as loaded, each stage's interpolators as xost packed them; fails "hlsl: a
#   vertex shader's TEXCOORD0 and COLOR0 meet ..." and "hlsl: a vertex shader's TEXCOORD3, COLOR1, TEXCOORD0 and FOG
#   meet ...".
# - vpos: VPOS as gl_FragCoord; fails "hlsl: VPOS is each pixel's whole column and row ..." and
#   "xsc: hlsl/grid_vpos_ps.hlsl compiles into the PC's container ...".
# - vface: a front face's VFACE -1; fails "hlsl: VFACE is -1 on a front face, clockwise, and +1 on a back one ..." and
#   "xsc: hlsl/grid_vface_ps.hlsl compiles into the PC's container ...".
# - depth: DEPTH left a colour at its location; fails "hlsl: DEPTH from TEXCOORD0's x ..." and
#   "xsc: hlsl/grid_depth_ps.hlsl compiles into the PC's container ...".
# - loop: a loop merge's Phi held in its header's Phi's variable not stored at a break, so a pixel that broke keeps its
#   pass's top; fails "hlsl: a hardware loop an int in $Global counts, ..." and
#   "xsc: hlsl/grid_loop_ps.hlsl compiles into the PC's container ...".
# - skin: a0 loaded from an int vector's lane as a float index, floored and not times the bone's 3 registers, so every
#   bone past 0 is read across the wrong rows whichever lane each load reads; on the simulator the skinning quad lands
#   within columns 501 to 711 and rows 173 to 523, leaving its cell empty; fails "hlsl: a quad skinned by float4x3
#   bones ..." and "xsc: hlsl/grid_skin_vs.hlsl compiles into the PC's container ...". The break before it, a0 from
#   each vector's x, drew the quad right on the console (tests/probe_skin.patch).
set -eu
cd "$(dirname "$0")/../../.."
for name in link vpos vface depth loop skin; do
	patch -p1 --forward --quiet < "tests/breaks/hlsl/$name.patch"
	trap 'patch -p1 -R --quiet < "tests/breaks/hlsl/$name.patch"' EXIT INT TERM
	make -j32 BUILD="build/breaks/hlsl_$name" "build/breaks/hlsl_$name/xsdk_test.xex"
	patch -p1 -R --quiet < "tests/breaks/hlsl/$name.patch"
	trap - EXIT INT TERM
	cp "build/breaks/hlsl_$name/xsdk_test.xex" "build/breaks/xsdk_test_hlsl_$name.xex"
done
