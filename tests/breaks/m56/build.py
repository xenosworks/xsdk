#!/usr/bin/env python3
"""Build milestone 56's titles broken on purpose from private source copies, leaving the working tree untouched: a
folder build/m56-breaks/<case> for each case in cases.tsv, holding xsdk_test.xex, or launch_check.xex and
launch_target.xex, one of them built with the case's patch."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build"
OUTPUT = BUILD / "m56-breaks"
PREFIX = os.environ.get("TOOLCHAIN", str(Path.home() / "xsdk-gcc/bin/powerpc-xsdk-elf-"))
# Each title's packing, as the Makefile's.
XEX_ARGUMENTS = {
    "xsdk_test": [],
    "launch_check": ["--title-id", "0x58534452", "--title-name", "xsdk launch_check"],
    "launch_target": ["--title-id", "0x58534453", "--title-name", "xsdk launch_target"],
}
# The titles a case's folder holds: the one it breaks, and its partner unchanged.
SETS = {"xsdk_test": ["xsdk_test"], "launch_check": ["launch_check", "launch_target"],
        "launch_target": ["launch_check", "launch_target"]}


def title_objects(title):
    if title != "xsdk_test":
        return [BUILD / "tests" / f"{title}.o"]
    printed = subprocess.run(["make", "-s", "--no-print-directory", "--eval", "m56_objects: ; @echo $(TEST_OBJECTS)",
                              "m56_objects"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE).stdout
    return [ROOT / name for name in printed.split()]


def build_case(name, broken_title):
    patch = ROOT / "tests/breaks/m56" / f"{name}.patch"
    paths = [line[6:] for line in patch.read_text().splitlines() if line.startswith("+++ b/")]
    if len(paths) != 1 or not re.fullmatch(r"(?:src|tests)/[a-z_]+\.c", paths[0]):
        raise ValueError(f"{name}: expected one SDK or test source")
    relative = Path(paths[0])
    directory = OUTPUT / name
    source_root = directory / "source"
    source = source_root / relative
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes((ROOT / relative).read_bytes())
    obj = directory / "changed.o"
    elf = directory / f"{broken_title}.elf"
    log_path = OUTPUT / f"{name}.build.log"
    with log_path.open("w") as log:
        def run(command, cwd=ROOT):
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)

        run(["patch", "-p1", "--batch", "--forward", "--fuzz=0", "--quiet", "-i", str(patch)], source_root)
        run([PREFIX + "gcc", "-mcpu=cell", "-fno-pic", "-std=gnu17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Iinclude", "-Isrc", "-Itests", "-c", str(source), "-o", str(obj)])
        objects = title_objects(broken_title)
        if relative.parts[0] == "tests":
            objects = [obj if path.stem == relative.stem else path for path in objects]
        else:
            # Ahead of the archive, so the archive's own copy isn't linked.
            objects.append(obj)
        run([PREFIX + "gcc", "-static", "-nostdlib", "-T", "src/xsdk.ld", "-o", str(elf), *map(str, objects),
             "-Wl,--start-group", "-lstdc++", str(BUILD / "libxsdk.a"), "-lc", "-lm", "-lgcc", "-Wl,--end-group"])
        run([sys.executable, "tools/elf2xex.py", *XEX_ARGUMENTS[broken_title], str(elf),
             str(directory / f"{broken_title}.xex")])
    if re.search(r"(?:warning|error):", log_path.read_text()):
        raise RuntimeError(f"{name}: build diagnostic; see {log_path}")
    for title in SETS[broken_title]:
        if title != broken_title:
            shutil.copyfile(BUILD / f"{title}.xex", directory / f"{title}.xex")
    return name


def main():
    cases = dict(line.split("\t")[:2] for line in (ROOT / "tests/breaks/m56/cases.tsv").read_text().splitlines())
    selected = sys.argv[1:] or list(cases)
    if any(name not in cases for name in selected) or len(set(selected)) != len(selected):
        sys.exit("Select distinct case names from tests/breaks/m56/cases.tsv")
    subprocess.run(["make", "-j32", *(f"build/{title}.xex" for title in XEX_ARGUMENTS)], cwd=ROOT, check=True)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        jobs = {pool.submit(build_case, name, cases[name]): name for name in selected}
        for job in as_completed(jobs):
            try:
                print(f"Built build/m56-breaks/{job.result()}", flush=True)
            except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                sys.exit(f"{jobs[job]}: {error}; see build/m56-breaks/{jobs[job]}.build.log")


if __name__ == "__main__":
    main()
