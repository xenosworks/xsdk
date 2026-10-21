#!/usr/bin/env python3
"""Build M55's negative titles from private source copies, leaving the working tree untouched."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build"
OUTPUT = BUILD / "m55-breaks"
PREFIX = os.environ.get("TOOLCHAIN", str(Path.home() / "xsdk-gcc/bin/powerpc-xsdk-elf-"))
TEST_OBJECTS = ["m55_check", "test_pthread", "test_objects", "test_thread_control",
                "test_semaphore", "test_pages", "test_cpp_threads", "jit_pool"]


def build_case(name):
    patch = ROOT / "tests/breaks/m55" / f"{name}.patch"
    paths = [line[6:] for line in patch.read_text().splitlines() if line.startswith("+++ b/")]
    if len(paths) != 1 or not re.fullmatch(r"(?:src|tests)/[a-z_]+\.(?:c|S)", paths[0]):
        raise ValueError(f"{name}: expected one SDK or test source")
    relative = Path(paths[0])
    directory = OUTPUT / name
    source_root = directory / "source"
    source = source_root / relative
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes((ROOT / relative).read_bytes())
    obj = directory / "changed.o"
    elf = directory / "m55_check.elf"
    log_path = OUTPUT / f"{name}.build.log"
    with log_path.open("w") as log:
        def run(command, cwd=ROOT):
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)

        run(["patch", "-p1", "--batch", "--forward", "--fuzz=0", "--quiet", "-i", str(patch)], source_root)
        flags = ["-mcpu=cell", "-Iinclude", "-Isrc", "-Itests"]
        if source.suffix == ".S":
            flags += ["-Wa,-mregnames"]
        else:
            flags += ["-fno-pic", "-std=gnu17", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
        run([PREFIX + "gcc", *flags, "-c", str(source), "-o", str(obj)])
        objects = [BUILD / "tests" / f"{stem}.o" for stem in TEST_OBJECTS]
        if relative.parts[0] == "tests":
            objects[TEST_OBJECTS.index(relative.stem)] = obj
        else:
            # Satisfy this object's symbols before the archive is searched.
            objects.append(obj)
        run([PREFIX + "gcc", "-static", "-nostdlib", "-T", "src/xsdk.ld", "-o", str(elf),
             *map(str, objects), "-Wl,--start-group", "-lstdc++", str(BUILD / "libxsdk.a"),
             "-lc", "-lm", "-lgcc", "-Wl,--end-group"])
        run([sys.executable, "tools/elf2xex.py", str(elf), str(OUTPUT / f"m55_{name}.xex")])
        if name == "jit_noflush":
            with (OUTPUT / "noflush-check.log").open("w") as check_log:
                result = subprocess.run([sys.executable, "tests/check_jit_flush.py", PREFIX + "objdump", str(obj)],
                                        cwd=ROOT, stdout=check_log, stderr=subprocess.STDOUT)
            diagnostic = (OUTPUT / "noflush-check.log").read_text()
            if result.returncode != 1 or "FAIL: cache flush" not in diagnostic:
                raise RuntimeError("no-flush mutation did not fail the compiled-sequence check as expected")
    if re.search(r"(?:warning|error):", log_path.read_text()):
        raise RuntimeError(f"{name}: build diagnostic; see {log_path}")
    return name


def main():
    cases = [line.split("\t", 1)[0] for line in (ROOT / "tests/breaks/m55/cases.tsv").read_text().splitlines()]
    selected = sys.argv[1:] or cases
    if any(name not in cases for name in selected) or len(set(selected)) != len(selected):
        sys.exit("Select distinct case names from tests/breaks/m55/cases.tsv")
    subprocess.run(["make", "-j32", "build/m55_check.xex"], cwd=ROOT, check=True)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        jobs = {pool.submit(build_case, name): name for name in selected}
        for job in as_completed(jobs):
            try:
                print(f"Built m55_{job.result()}.xex", flush=True)
            except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                sys.exit(f"{jobs[job]}: {error}; see build/m55-breaks/{jobs[job]}.build.log")


if __name__ == "__main__":
    main()
