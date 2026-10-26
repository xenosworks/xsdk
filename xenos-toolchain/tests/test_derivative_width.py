"""Derivatives of a vec3 or vec4 take two getGradients fetches, as the fetch takes two components (milestone 65).

xsc/testdata/deriv_width.frag takes dFdy of a vec2, dFdx of a vec3 and fwidth of a vec4: one fetch, then two, then two,
five in all. With one fetch each, the vec3's and vec4's z and w were the first fetch's again, which left RetroArch's XMB
ribbon unlit on the console. Runs in WSL, as the other lowering tests do.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from xsc import spirv  # noqa: E402
from xsc.lower import lower  # noqa: E402

TD = ROOT / "xsc" / "testdata"


def fetch_count(blob):
    parsed = container.Container(blob)
    return sum(rec.kind == "tfetch" for rec in xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0])


def main():
    blob = lower(spirv.Module.parse((TD / "deriv_width.frag.spv").read_bytes()))
    got = fetch_count(blob)
    if got != 5:
        print(f"  FAIL deriv_width.frag: {got} getGradients fetches, not 5")
        raise SystemExit("FAILED")
    print("  PASS deriv_width.frag: 5 getGradients fetches, one for the vec2 and two each for the vec3 and the vec4")


if __name__ == "__main__":
    main()
