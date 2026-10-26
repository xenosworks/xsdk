"""clamp between a block's values as well as between constants, as PPSSPP's pixel shaders clamp texture coordinates
to u_texclamp (milestone 66).

xost took only constant bounds and refused such a shader with "is not a constant scalar". xsc/testdata/
vk_clamp_bounds.frag clamps its coordinate between push constants, and its x and y between constants, 0.2 to 0.4 and
0 to 1; the simulator gives what the GLSL says with the coordinate below a bound, inside and above. Runs in WSL, as
the other lowering tests do.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from test_local_stores import check, run_fragment  # noqa: E402

# The low bounds in xy, the high ones in zw.
BOUNDS = [0.1, 0.3, 0.6, 0.9]
UNUSED = [0.0, 0.0, 0.0, 0.0]


def clamp(x, lo, hi):
    return min(max(x, lo), hi)


def main():
    for uv in ([0.75, 0.25], [0.05, 0.95], [0.3, 0.5]):
        want = [clamp(uv[0], BOUNDS[0], BOUNDS[2]), clamp(uv[1], BOUNDS[1], BOUNDS[3]), clamp(uv[0], 0.2, 0.4),
                clamp(uv[1], 0.0, 1.0)]
        check(f"vk_clamp_bounds.frag at {uv}", run_fragment("vk_clamp_bounds.frag", [BOUNDS, UNUSED], uv=uv), want)
    print("OK")


if __name__ == "__main__":
    main()
