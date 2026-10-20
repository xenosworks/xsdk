#!/bin/sh
# Milestone 50 steps 3 to 8's breaks: xsdk_test built with each patch here applied, as
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
#   bone past 0 is read across the wrong rows whichever lane each load reads; on the simulator the skinning quad draws
#   over columns 493 to 711 and rows 178 to 526, leaving its cell clear, 1536 of its 3200 pixels wrong a frame; fails
#   "hlsl: a quad skinned by float4x3 bones ..." and "xsc: hlsl/grid_skin_vs.hlsl compiles into the PC's container
#   ...". The break before it, a0 from each vector's x (tests/probe_skin.patch), passed on the console only because the
#   check expected the clear everywhere: -CELL_HEIGHT was unsigned, so the reference's quad had no rows.
# - binding: HLSL's samplers packed from 0 in binding order again, as GLSL's are, so grid_binding_ps.hlsl's Low and
#   High read fetch constants 0 and 1, where the test binds other textures, not 3 and 7; on the simulator every pixel of
#   the cell takes their red 8 and green 8; fails "hlsl: samplers at s3 and s7 sample the textures bound at slots 3
#   and 7, ...", "hlsl: grid_binding_ps.hlsl's table names Low at fetch constant 0 and High at 1, ..." and
#   "xsc: hlsl/grid_binding_ps.hlsl compiles into the PC's container ...".
# - registers: D3D9's b# and i# not kept, each bool and int taking the lowest constant left, so grid_binding_ps.hlsl's
#   Bright at b2 is boolean constant 128 and Count at i1 loop constant 16, not 130 and 17; the cell, set by name, still
#   draws right; fails "hlsl: grid_binding_ps.hlsl's table names ... Bright at boolean constant 128 and Count at loop
#   constant 16, as their b2 and i1" and "xsc: hlsl/grid_binding_ps.hlsl compiles into the PC's container ...".
# - if: the selects merging an if/else the fragment lowering refused, which the unroller now takes, with their arms
#   swapped, so grid_if_ps.hlsl draws each column's other arms; on the simulator all 80 of the cell's columns wrong;
#   fails "hlsl: a pixel shader without a loop, its if with no else and nested if/else ..." and "xsc:
#   hlsl/grid_if_ps.hlsl compiles into the PC's container ...". The early returns', switch's and discards' cells go
#   through the unroller the same way, so their shaders compile otherwise and their checks fail too.
# - return: each copy of merge-return's merge taking its Phis' first value, whichever block branched there, so
#   grid_return_ps.hlsl, alone of the shaders xsdk_test compiles, draws the first return's colour wherever it returns;
#   fails "hlsl: a pixel shader without a loop, its early returns ..." and "xsc: hlsl/grid_return_ps.hlsl compiles into
#   the PC's container ...".
# - switch: a switch's case testing its first literal alone, so grid_switch_ps.hlsl, alone of the shaders xsdk_test
#   compiles, draws its default where the selector is 3, which shares a case with 1; fails "hlsl: a pixel shader
#   without a loop, its switch ..." and "xsc: hlsl/grid_switch_ps.hlsl compiles into the PC's container ...".
# - discard: each discard's kill taking the place of those before it, so grid_discard_ps.hlsl, alone of the shaders
#   xsdk_test compiles, kills where its nested discard does and draws the 10 columns of 80 its clip kills; fails "hlsl:
#   a pixel shader's clip before an if and discard in an if inside another ..." and "xsc: hlsl/grid_discard_ps.hlsl
#   compiles into the PC's container ...".
set -eu
cd "$(dirname "$0")/../../.."
for name in link vpos vface depth loop skin binding registers if return switch discard; do
	patch -p1 --forward --quiet < "tests/breaks/hlsl/$name.patch"
	trap 'patch -p1 -R --quiet < "tests/breaks/hlsl/$name.patch"' EXIT INT TERM
	make -j32 BUILD="build/breaks/hlsl_$name" "build/breaks/hlsl_$name/xsdk_test.xex"
	patch -p1 -R --quiet < "tests/breaks/hlsl/$name.patch"
	trap - EXIT INT TERM
	cp "build/breaks/hlsl_$name/xsdk_test.xex" "build/breaks/xsdk_test_hlsl_$name.xex"
done
