#!/usr/bin/env python3
"""Build milestone 58's xsdk_test broken on purpose from private source copies, leaving the working tree untouched: a
folder build/m58-breaks/<case> for each case in cases.tsv, holding xsdk_test.xex built with the case's patch."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build"
OUTPUT = BUILD / "m58-breaks"
PREFIX = os.environ.get("TOOLCHAIN", str(Path.home() / "xsdk-gcc/bin/powerpc-xsdk-elf-"))


def test_objects():
    printed = subprocess.run(["make", "-s", "--no-print-directory", "--eval", "m58_objects: ; @echo $(TEST_OBJECTS)",
                              "m58_objects"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE).stdout
    return [ROOT / name for name in printed.split()]


def build_case(name):
    patch = ROOT / "tests/breaks/m58" / f"{name}.patch"
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
    elf = directory / "xsdk_test.elf"
    log_path = OUTPUT / f"{name}.build.log"
    with log_path.open("w") as log:
        def run(command, cwd=ROOT):
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)

        run(["patch", "-p1", "--batch", "--forward", "--fuzz=0", "--quiet", "-i", str(patch)], source_root)
        run([PREFIX + "gcc", "-mcpu=cell", "-fno-pic", "-std=gnu17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Iinclude", "-Isrc", "-Itests", "-c", str(source), "-o", str(obj)])
        objects = test_objects()
        if relative.parts[0] == "tests":
            objects = [obj if path.stem == relative.stem else path for path in objects]
        else:
            # Ahead of the archive, so the archive's own copy isn't linked.
            objects.append(obj)
        run([PREFIX + "gcc", "-static", "-nostdlib", "-T", "src/xsdk.ld", "-o", str(elf), *map(str, objects),
             "-Wl,--start-group", "-lstdc++", str(BUILD / "libxsdk.a"), "-lc", "-lm", "-lgcc", "-Wl,--end-group"])
        run([sys.executable, "tools/elf2xex.py", str(elf), str(directory / "xsdk_test.xex")])
    if re.search(r"(?:warning|error):", log_path.read_text()):
        raise RuntimeError(f"{name}: build diagnostic; see {log_path}")
    return name


def main():
    cases = [line.split("\t")[0] for line in (ROOT / "tests/breaks/m58/cases.tsv").read_text().splitlines()]
    selected = sys.argv[1:] or cases
    if any(name not in cases for name in selected) or len(set(selected)) != len(selected):
        sys.exit("Select distinct case names from tests/breaks/m58/cases.tsv")
    subprocess.run(["make", "-j32", "build/xsdk_test.xex"], cwd=ROOT, check=True)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        jobs = {pool.submit(build_case, name): name for name in selected}
        for job in as_completed(jobs):
            try:
                print(f"Built build/m58-breaks/{job.result()}", flush=True)
            except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                sys.exit(f"{jobs[job]}: {error}; see build/m58-breaks/{jobs[job]}.build.log")


if __name__ == "__main__":
    main()
