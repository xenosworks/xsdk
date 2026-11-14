"""Milestone 77 step 2's harness (docs/scope/77-exfat.md): drives build/host/exfat_host over disk images against
exfatprogs and fuse-exfat as the judges. Trees written by Linux come back through ours byte for byte and the other
way round, fsck.exfat is clean after every write test, a file over 4 GB and a volume over 2 TB work, writes cut short
leave earlier files intact, corrupted images are refused or contained, and the formatter's images pass fsck, mount
under Linux and, for the stick's size, match the Windows-formatted boot sector and MBR. Needs root (loop devices,
fuse) in WSL:

    sudo python3 tests/exfat_host/run.py build/host/exfat_host [--quick] [--keep] [--work DIR]
"""
import argparse
import contextlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time

MB = 1 << 20
GB = 1 << 30
RECIPE = os.path.join(os.path.dirname(__file__), "..", "..", "results", "2026-11-14-milestone-77-step-1", "recipe")
STICK_SECTORS = 120176640
SIZES = [0, 1, 511, 512, 513, 4095, 4096, 4097, 32767, 32768, 32769, 131071, 131072, 131073, 1000000, 3 * MB + 5]
CHUNKS = [None, 777, 4096, 65536]
FILETIME_1970 = 116444736000000000


def latin1(path):
    return path.encode("latin-1")


def utf8(path):
    return path.encode("utf-8")


def make_tree(seed):
    """Directories and files (path -> bytes) with the sizes and names that matter: sector and cluster edges, case,
    Latin-1 characters, a 255-character name, spaces and dots."""
    rnd = random.Random(seed)
    dirs = ["docs", "docs/deep", "docs/deep/deeper", "UPPER", "é ñ ü"]
    files = {}
    for i, size in enumerate(SIZES):
        where = ["", "docs", "docs/deep", "docs/deep/deeper", "UPPER"][i % 5]
        name = f"file{i:02d}_{size}.bin"
        files[f"{where}/{name}" if where else name] = rnd.randbytes(size)
    files["a" * 255] = rnd.randbytes(100)
    files["MiXeD CaSe.TXT"] = rnd.randbytes(2000)
    files["dot.in.name.tar.gz"] = rnd.randbytes(70000)
    files["é ñ ü/é_ñ_ü.bin"] = rnd.randbytes(5000)
    return dirs, files


class Harness:
    def __init__(self, cli, work, quick):
        self.cli_path = os.path.abspath(cli)
        self.work = work
        self.quick = quick
        self.passed = 0
        self.failed = 0
        self.serial = 0
        # The device's sector size for the command line and the loop devices; the 2.5 TB format needs 4 KB.
        self.sector = 512

    def check(self, name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + (f": {detail}" if detail and not ok else ""), flush=True)
        if ok:
            self.passed += 1
        else:
            self.failed += 1

    def path(self, name):
        return os.path.join(self.work, name)

    def tmp(self, name="tmp"):
        self.serial += 1
        return self.path(f"{name}.{self.serial}")

    @staticmethod
    def run(args, check=True, env=None, timeout=600):
        return subprocess.run(args, capture_output=True, check=check, env=env, timeout=timeout)

    def cli(self, image, *args, env=None, expect_fail=None):
        """Runs one command; returns stdout. With expect_fail, the command must fail and name that status."""
        full_env = dict(os.environ, EXFAT_HOST_SECTOR=str(self.sector), **(env or {}))
        result = self.run([self.cli_path, image, *args], check=False, env=full_env)
        if expect_fail is None:
            if result.returncode != 0:
                raise RuntimeError(f"exfat_host {args[0]} failed: {result.stderr.decode('latin-1').strip()}")
            return result.stdout
        text = result.stderr.decode("latin-1").strip()
        label = " ".join(a.decode("latin-1") if isinstance(a, bytes) else a for a in args[:2])
        self.check(f"{label} fails '{expect_fail}'", result.returncode != 0 and expect_fail in text,
                   text or "succeeded")
        return result.stderr

    def sparse(self, name, size):
        path = self.path(name)
        with open(path, "wb") as file:
            file.truncate(size)
        return path

    def mkfs(self, name, size, cluster=None, label="LINUX"):
        path = self.sparse(name, size)
        args = ["mkfs.exfat", "-L", label] + (["-c", cluster] if cluster else []) + [path]
        self.run(args)
        return path

    def fsck(self, image, offset=0, size=None, fix=False):
        """fsck.exfat's exit code and output on an image, or on the partition at offset within it."""
        with self.loop(image, offset, size) as device:
            result = self.run(["fsck.exfat", "-y" if fix else "-n", device], check=False)
        return result.returncode, (result.stdout + result.stderr).decode(errors="replace")

    @contextlib.contextmanager
    def loop(self, image, offset=0, size=None):
        args = ["losetup", "--find", "--show", "--sector-size", str(self.sector)]
        if offset:
            args += ["-o", str(offset)]
        if size:
            args += ["--sizelimit", str(size)]
        device = self.run(args + [image]).stdout.decode().strip()
        try:
            yield device
        finally:
            self.run(["losetup", "-d", device], check=False)

    @contextlib.contextmanager
    def mounted(self, image, offset=0, size=None):
        point = self.tmp("mnt")
        os.mkdir(point)
        with self.loop(image, offset, size) as device:
            self.run(["mount.exfat-fuse", device, point])
            try:
                yield point
            finally:
                for _ in range(20):
                    if self.run(["umount", point], check=False).returncode == 0:
                        break
                    time.sleep(0.2)
        os.rmdir(point)

    @staticmethod
    def read_at(image, offset, count):
        with open(image, "rb") as file:
            file.seek(offset)
            return file.read(count)

    @staticmethod
    def write_at(image, offset, data):
        with open(image, "r+b") as file:
            file.seek(offset)
            file.write(data)

    @staticmethod
    def boot_fields(boot):
        names = ("partition_offset volume_length fat_offset fat_length heap_offset cluster_count root_cluster serial "
                 "revision flags sector_shift cluster_shift fats drive_select percent_in_use").split()
        return dict(zip(names, struct.unpack_from("<QQIIIIIIHHBBBBB", boot, 64)))

    def dirty(self, image, offset=0):
        return self.read_at(image, offset + 106, 2)[0] & 2 != 0

    # Trees through fuse and through ours.

    def linux_write_tree(self, mount, dirs, files):
        for directory in dirs:
            os.makedirs(os.path.join(utf8(mount), utf8(directory)), exist_ok=True)
        for path, data in files.items():
            with open(os.path.join(utf8(mount), utf8(path)), "wb") as file:
                file.write(data)

    @staticmethod
    def linux_read_tree(mount):
        dirs, files = set(), {}
        root = utf8(mount)
        for here, names, file_names in os.walk(root):
            rel = os.path.relpath(here, root).decode("utf-8")
            for name in names:
                dirs.add(os.path.join(rel, name.decode("utf-8")).replace("./", ""))
            for name in file_names:
                with open(os.path.join(here, name), "rb") as file:
                    files[os.path.join(rel, name.decode("utf-8")).replace("./", "")] = file.read()
        return dirs, files

    def ours_write_tree(self, image, dirs, files):
        for directory in dirs:
            self.cli(image, "mkdir", latin1(directory))
        for i, (path, data) in enumerate(files.items()):
            source = self.tmp("in")
            with open(source, "wb") as file:
                file.write(data)
            chunk = CHUNKS[i % len(CHUNKS)]
            self.cli(image, "put", latin1(path), source, *([str(chunk)] if chunk else []))
            os.unlink(source)

    def ours_list(self, image, path=""):
        entries = []
        for line in self.cli(image, "ls", latin1(path)).decode("latin-1").splitlines():
            kind, size, name = line.split(" ", 2)
            entries.append((kind, int(size), name))
        return entries

    def listed(self, image, name):
        return any(n == name for _, _, n in self.ours_list(image))

    def ours_read_tree(self, image):
        dirs, files = set(), {}
        pending = [""]
        while pending:
            here = pending.pop()
            for kind, size, name in self.ours_list(image, here):
                path = f"{here}/{name}" if here else name
                if kind == "D":
                    dirs.add(path)
                    pending.append(path)
                else:
                    out = self.tmp("out")
                    self.cli(image, "get", latin1(path), out)
                    with open(out, "rb") as file:
                        files[path] = file.read()
                    os.unlink(out)
                    if len(files[path]) != size:
                        raise RuntimeError(f"{path}: listed {size}, read {len(files[path])}")
        return dirs, files

    def compare_trees(self, name, expected_dirs, expected_files, dirs, files):
        same_dirs = set(expected_dirs) == set(dirs)
        same_names = set(expected_files) == set(files)
        differing = [p for p in expected_files if p in files and files[p] != expected_files[p]]
        self.check(f"{name}: directories", same_dirs, f"{set(expected_dirs) ^ set(dirs)}")
        self.check(f"{name}: file names", same_names, f"{set(expected_files) ^ set(files)}")
        self.check(f"{name}: file bytes", not differing, f"{differing[:5]}")

    # The tests.

    def test_mkfs_images(self):
        for cluster, size in [(None, 256 * MB), ("32K", 512 * MB), ("128K", 1 * GB)]:
            tag = f"mkfs {cluster or 'default'}"
            image = self.mkfs(f"linux_{cluster or 'default'}.img", size, cluster)
            dirs, files = make_tree(1)
            with self.mounted(image) as mount:
                self.linux_write_tree(mount, dirs, files)
                probe = os.path.join(utf8(mount), utf8("docs/deep/file07_4097.bin"))
                linux_mtime = os.stat(probe).st_mtime
            info = self.cli(image, "info").decode()
            self.check(f"{tag}: info", "label=LINUX" in info and "dirty=0" in info, info)
            self.compare_trees(f"{tag}: Linux writes, ours reads", dirs, files, *self.ours_read_tree(image))
            out = self.tmp("case")
            self.cli(image, "get", b"DOCS/Deep/FILE07_4097.BIN", out)
            with open(out, "rb") as file:
                self.check(f"{tag}: case-insensitive lookup", file.read() == files["docs/deep/file07_4097.bin"])
            stat = dict(f.split("=") for f in self.cli(image, "stat", b"docs/deep/file07_4097.bin").decode().split())
            ours_mtime = (int(stat["modified"]) - FILETIME_1970) / 1e7
            self.check(f"{tag}: modified time matches Linux", abs(ours_mtime - linux_mtime) <= 2,
                       f"ours {ours_mtime} linux {linux_mtime}")

            image = self.mkfs(f"ours_{cluster or 'default'}.img", size, cluster)
            self.ours_write_and_change(tag, image)

    def ours_write_and_change(self, tag, image, seed=2):
        """Ours writes a tree and changes it every way the core can; fsck and Linux judge."""
        dirs, files = make_tree(seed)
        self.ours_write_tree(image, dirs, files)
        cluster = int(dict(f.split("=") for f in self.cli(image, "info").decode().split())["cluster"])
        # Two files grown a cluster at a time in turns, so both get FAT chains.
        rnd = random.Random(seed + 100)
        frag = {"frag_a.bin": rnd.randbytes(5 * cluster), "frag_b.bin": rnd.randbytes(5 * cluster)}
        sources = {}
        for name, data in frag.items():
            sources[name] = self.tmp("frag")
            with open(sources[name], "wb") as file:
                file.write(data)
        for i in range(5):
            for name in frag:
                self.cli(image, "write", latin1(name), sources[name], str(i * cluster), str(cluster))
        files.update(frag)
        stat = self.cli(image, "stat", b"frag_a.bin").decode()
        self.check(f"{tag}: turn-taken writes make a chain", "contiguous=0" in stat, stat)
        stat = self.cli(image, "stat", b"file15_3145733.bin").decode()
        self.check(f"{tag}: a file written in one go is contiguous", "contiguous=1" in stat, stat)
        # Rename, move, remove, truncate, attributes.
        self.cli(image, "mv", b"MiXeD CaSe.TXT", b"docs/moved.txt")
        files["docs/moved.txt"] = files.pop("MiXeD CaSe.TXT")
        self.cli(image, "mv", b"UPPER", b"docs/deep/renamed")
        dirs = [d for d in dirs if d != "UPPER"] + ["docs/deep/renamed"]
        for path in [p for p in files if p.startswith("UPPER/")]:
            files["docs/deep/renamed/" + path[len("UPPER/"):]] = files.pop(path)
        self.cli(image, "mv", b"dot.in.name.tar.gz", b"DOT.IN.NAME.TAR.GZ")
        files["DOT.IN.NAME.TAR.GZ"] = files.pop("dot.in.name.tar.gz")
        self.cli(image, "rm", b"docs/deep/renamed/file04_513.bin")
        del files["docs/deep/renamed/file04_513.bin"]
        self.cli(image, "mkdir", b"empty")
        self.cli(image, "rm", b"empty")
        self.cli(image, "rm", b"docs", expect_fail="not empty")
        self.cli(image, "mkdir", b"docs", expect_fail="exists")
        self.cli(image, "mkdir", b"bad:name", expect_fail="bad name")
        self.cli(image, "mv", b"docs", b"docs/deep/inside", expect_fail="bad name")
        self.cli(image, "truncate", b"docs/deep/renamed/file09_32768.bin", "1000")
        files["docs/deep/renamed/file09_32768.bin"] = files["docs/deep/renamed/file09_32768.bin"][:1000]
        self.cli(image, "truncate", b"file05_4095.bin", str(4095 + 100000))
        files["file05_4095.bin"] = files["file05_4095.bin"] + bytes(100000)
        self.cli(image, "truncate", b"file10_32769.bin", "0")
        files["file10_32769.bin"] = b""
        self.cli(image, "attrib", b"docs/moved.txt", "21")
        stat = self.cli(image, "stat", b"docs/moved.txt").decode()
        self.check(f"{tag}: attributes written", "attributes=21" in stat, stat)
        # Many small files grow a directory past its first cluster.
        many = {f"docs/deep/deeper/many_{i:03d}.txt": bytes([i & 0xFF]) * i for i in range(300)}
        self.ours_write_tree(image, [], many)
        files.update(many)
        self.check(f"{tag}: dirty flag clear after sync", not self.dirty(image))
        code, output = self.fsck(image)
        self.check(f"{tag}: fsck.exfat clean after ours wrote", code == 0, output[-600:])
        self.compare_trees(f"{tag}: ours reads back", dirs, files, *self.ours_read_tree(image))
        with self.mounted(image) as mount:
            self.compare_trees(f"{tag}: Linux reads what ours wrote", dirs, files, *self.linux_read_tree(mount))
            mtime = os.stat(os.path.join(utf8(mount), b"docs/moved.txt")).st_mtime
            self.check(f"{tag}: Linux sees our modified time", abs(mtime - time.time()) < 600, f"{mtime}")
            with open(os.path.join(utf8(mount), b"from_linux.bin"), "wb") as file:
                file.write(b"L" * 70001)
        out = self.tmp("fl")
        self.cli(image, "get", b"from_linux.bin", out)
        with open(out, "rb") as file:
            self.check(f"{tag}: ours reads what Linux added", file.read() == b"L" * 70001)
        files["from_linux.bin"] = b"L" * 70001
        return dirs, files

    def test_power_cut(self):
        """Writes cut short at every stage leave the files that were there intact and a volume fsck repairs."""
        base = self.mkfs("cut_base.img", 256 * MB)
        dirs, files = self.ours_write_and_change("cut setup", base, seed=5)
        source = self.tmp("cut_in")
        with open(source, "wb") as file:
            file.write(random.Random(9).randbytes(3 * MB))
        for fail_after in [1, 2, 3, 5, 8, 13, 21, 34]:
            image = self.path(f"cut_{fail_after}.img")
            shutil.copyfile(base, image)
            self.cli(image, "put", b"docs/cut.bin", source, "65536", env={"EXFAT_HOST_FAIL_AFTER": str(fail_after)},
                     expect_fail="device error")
            # The first write is the dirty flag itself, so every cut here leaves the volume marked dirty.
            self.check(f"cut after {fail_after} writes: volume left marked dirty", self.dirty(image))
            _, repair = self.fsck(image, fix=True)
            code, output = self.fsck(image)
            self.check(f"cut after {fail_after} writes: fsck repairs to clean", code == 0,
                       repair[-400:] + output[-400:])
            self.check(f"cut after {fail_after} writes: no entry damage",
                       "corrupt" not in repair.lower() and "checksum" not in repair.lower(), repair[-600:])
            kept_dirs, kept = self.ours_read_tree(image)
            partial = kept.pop("docs/cut.bin", None)
            self.compare_trees(f"cut after {fail_after} writes: earlier files intact", dirs, files, kept_dirs, kept)
            if partial is not None:
                expected = self.read_at(source, 0, len(partial))
                self.check(f"cut after {fail_after} writes: the cut file holds what was written", partial == expected)
            os.unlink(image)

    def test_large_file(self):
        """A file of 4 GiB + 1 MiB: its head, a hole of valid-length zeros, and its tail past 4 GiB."""
        image = self.mkfs("large.img", 6 * GB)
        rnd = random.Random(7)
        head, tail = rnd.randbytes(MB), rnd.randbytes(MB)
        size = 4 * GB + MB
        expected = self.sparse("large_expected.bin", size)
        self.write_at(expected, 0, head)
        self.write_at(expected, 4 * GB, tail)
        started = time.time()
        self.cli(image, "write", b"large.bin", expected, "0", str(MB))
        self.cli(image, "truncate", b"large.bin", str(size))
        self.cli(image, "write", b"large.bin", expected, str(4 * GB), str(MB))
        print(f"  large file written in {time.time() - started:.1f} s", flush=True)
        stat = dict(f.split("=") for f in self.cli(image, "stat", b"large.bin").decode().split())
        self.check("large file: size and valid length", stat["size"] == str(size) and stat["valid"] == str(size), stat)
        out = self.path("large_out.bin")
        started = time.time()
        self.cli(image, "get", b"large.bin", out)
        print(f"  large file read in {time.time() - started:.1f} s", flush=True)
        same = self.run(["cmp", expected, out], check=False).returncode == 0
        self.check("large file: ours reads it back byte for byte", same)
        os.unlink(out)
        code, output = self.fsck(image)
        self.check("large file: fsck.exfat clean", code == 0, output[-600:])
        with self.mounted(image) as mount:
            same = self.run(["cmp", expected, os.path.join(mount, "large.bin")], check=False).returncode == 0
            self.check("large file: Linux reads it byte for byte", same)
        os.unlink(expected)
        os.unlink(image)

    def test_huge_volume(self):
        image = self.mkfs("huge.img", 2560 * GB)
        info = dict(f.split("=") for f in self.cli(image, "info").decode().split())
        self.check("2.5 TB volume: cluster count", int(info["clusters"]) > 20_000_000, info)
        dirs, files = make_tree(3)
        self.ours_write_tree(image, dirs, files)
        code, output = self.fsck(image)
        self.check("2.5 TB volume: fsck.exfat clean after ours wrote", code == 0, output[-600:])
        with self.mounted(image) as mount:
            self.compare_trees("2.5 TB volume: Linux reads what ours wrote", dirs, files, *self.linux_read_tree(mount))
        os.unlink(image)

    def find_set(self, image, name):
        """The root directory offset of the entry set named `name` on a one-cluster root."""
        boot = self.read_at(image, 0, 512)
        fields = self.boot_fields(boot)
        cluster_bytes = 1 << (fields["sector_shift"] + fields["cluster_shift"])
        root = fields["heap_offset"] * 512 + (fields["root_cluster"] - 2) * cluster_bytes
        wanted = name.encode("utf-16-le")
        at = root
        while True:
            entry = self.read_at(image, at, 32)
            if entry[0] == 0:
                raise RuntimeError(f"{name} not in the root directory")
            if entry[0] == 0x85:
                count = entry[1] + 1
                entries = self.read_at(image, at, 32 * count)
                text = b"".join(entries[32 * i + 2:32 * i + 32] for i in range(2, count))[:2 * entries[32 + 3]]
                if text == wanted:
                    return at, entries, fields
                at += 32 * count
            else:
                at += 32

    @staticmethod
    def set_checksum(entries):
        checksum = 0
        for i, byte in enumerate(entries):
            if i in (2, 3):
                continue
            checksum = (((checksum & 1) << 15) | (checksum >> 1)) + byte & 0xFFFF
        return checksum

    def patch_set(self, image, at, entries):
        entries = bytearray(entries)
        struct.pack_into("<H", entries, 2, self.set_checksum(entries))
        self.write_at(image, at, bytes(entries))

    def test_corruptions(self):
        base = self.mkfs("corrupt_base.img", 256 * MB)
        self.ours_write_and_change("corruption setup", base, seed=6)

        image = self.path("corrupt_boot.img")
        shutil.copyfile(base, image)
        self.write_at(image, 113, b"\x01")
        self.cli(image, "info", expect_fail="corrupt")
        code, _ = self.fsck(image)
        self.check("bad boot checksum: fsck.exfat rejects it too", code != 0)

        image = self.path("corrupt_loop.img")
        shutil.copyfile(base, image)
        at, entries, fields = self.find_set(image, "frag_a.bin")
        cluster = 1 << (fields["sector_shift"] + fields["cluster_shift"])
        first = struct.unpack_from("<I", entries, 32 + 20)[0]
        fat = fields["fat_offset"] * 512
        cluster_at, chain = first, [first]
        while True:
            cluster_at = struct.unpack_from("<I", self.read_at(image, fat + 4 * cluster_at, 4))[0]
            if cluster_at == 0xFFFFFFFF:
                break
            chain.append(cluster_at)
        self.check("FAT loop setup: frag_a.bin has a chain", len(chain) == 5, f"{chain}")
        # A chain that loops is read up to the file's size and no further; a looping root chain refuses the mount.
        self.write_at(image, fat + 4 * chain[1], struct.pack("<I", chain[0]))
        out = self.tmp("loop")
        started = time.time()
        result = self.run([self.cli_path, image, "get", b"frag_a.bin", out], check=False, timeout=60)
        self.check("FAT loop in a file: the read ends, bounded by the size",
                   result.returncode in (0, 1) and time.time() - started < 10 and os.path.getsize(out) <= 5 * cluster,
                   result.stderr.decode(errors="replace"))
        code, _ = self.fsck(image)
        self.check("FAT loop in a file: fsck.exfat rejects it", code != 0)
        self.write_at(image, fat + 4 * fields["root_cluster"], struct.pack("<I", fields["root_cluster"]))
        self.cli(image, "info", expect_fail="corrupt")
        code, _ = self.fsck(image)
        self.check("FAT loop in the root: fsck.exfat rejects it too", code != 0)

        image = self.path("corrupt_cluster.img")
        shutil.copyfile(base, image)
        at, entries, fields = self.find_set(image, "frag_b.bin")
        patched = bytearray(entries)
        struct.pack_into("<I", patched, 32 + 20, fields["cluster_count"] + 5)
        self.patch_set(image, at, patched)
        self.cli(image, "get", b"frag_b.bin", out, expect_fail="corrupt")
        self.check("cluster past the end: the entry still lists", self.listed(image, "frag_b.bin"))
        code, _ = self.fsck(image)
        self.check("cluster past the end: fsck.exfat rejects it too", code != 0)

        image = self.path("corrupt_hash.img")
        shutil.copyfile(base, image)
        at, entries, fields = self.find_set(image, "frag_b.bin")
        patched = bytearray(entries)
        patched[32 + 4] ^= 1
        self.patch_set(image, at, patched)
        self.cli(image, "get", b"frag_b.bin", out, expect_fail="not found")
        self.check("wrong name hash: the entry still lists", self.listed(image, "frag_b.bin"))
        code, _ = self.fsck(image)
        self.check("wrong name hash: fsck.exfat rejects it too", code != 0)

        image = self.path("corrupt_checksum.img")
        shutil.copyfile(base, image)
        at, entries, fields = self.find_set(image, "frag_b.bin")
        patched = bytearray(entries)
        patched[2] ^= 1
        self.write_at(image, at, bytes(patched))
        self.cli(image, "get", b"frag_b.bin", out, expect_fail="not found")
        self.check("wrong set checksum: the entry is skipped", not self.listed(image, "frag_b.bin"))
        code, _ = self.fsck(image)
        self.check("wrong set checksum: fsck.exfat rejects it too", code != 0)

    def test_formatter(self):
        """Each cluster size, the stick's size, and 2.5 TB, which an MBR holds only with 4 KB sectors: Windows
        formats a 512-byte-sector drive that large as GPT, which the formatter refuses rather than mimics."""
        cases = [("128 MB", 512, 128 * MB // 512, 12), ("8 GB", 512, 8 * GB // 512, 15),
                 ("stick", 512, STICK_SECTORS, 17)]
        if not self.quick:
            cases.append(("2.5 TB", 4096, 2560 * GB // 4096, 17))
        over_2tb = self.sparse("format_over2tb.img", 2560 * GB)
        self.cli(over_2tb, "format", "XSDK", "643fc9ed", expect_fail="unsupported")
        os.unlink(over_2tb)
        for name, sector, sectors, cluster_bytes_shift in cases:
            tag = f"format {name}"
            self.sector = sector
            image = self.sparse(f"format_{name.replace(' ', '')}.img", sectors * sector)
            started = time.time()
            self.cli(image, "format", "XSDK", "643fc9ed")
            print(f"  {tag}: {time.time() - started:.1f} s", flush=True)
            mbr = self.read_at(image, 0, 512)
            entry = mbr[446:462]
            start, count = struct.unpack_from("<II", entry, 8)
            self.check(f"{tag}: MBR partition", entry[4] == 0x07 and start * sector == MB and
                       mbr[510:] == b"\x55\xaa" and start + count <= sectors, entry.hex())
            offset, size = start * sector, count * sector
            boot = self.read_at(image, offset, 512)
            # Before anything mounts it: a mount updates VolumeFlags and PercentInUse in the main region only.
            backup = self.read_at(image, offset + 12 * sector, 12 * sector)
            self.check(f"{tag}: backup boot region equals the main one",
                       backup == self.read_at(image, offset, 12 * sector))
            fields = self.boot_fields(boot)
            self.check(f"{tag}: cluster size", fields["sector_shift"] + fields["cluster_shift"] == cluster_bytes_shift,
                       f"{fields}")
            code, output = self.fsck(image, offset, size)
            self.check(f"{tag}: fsck.exfat clean", code == 0, output[-600:])
            info = self.cli(image, "info").decode()
            self.check(f"{tag}: ours mounts it", "label=XSDK serial=643fc9ed" in info, info)
            dirs, files = make_tree(4)
            with self.mounted(image, offset, size) as mount:
                self.linux_write_tree(mount, dirs, files)
            code, output = self.fsck(image, offset, size)
            self.check(f"{tag}: fsck.exfat clean after Linux wrote", code == 0, output[-600:])
            self.compare_trees(f"{tag}: ours reads what Linux wrote", dirs, files, *self.ours_read_tree(image))
            self.ours_write_tree(image, [], {"from_ours.bin": b"O" * 123457})
            code, output = self.fsck(image, offset, size)
            self.check(f"{tag}: fsck.exfat clean after ours wrote", code == 0, output[-600:])
            with self.mounted(image, offset, size) as mount:
                with open(os.path.join(mount, "from_ours.bin"), "rb") as file:
                    self.check(f"{tag}: Linux reads what ours wrote", file.read() == b"O" * 123457)
            if name == "stick":
                with open(os.path.join(RECIPE, "image_boot.bin"), "rb") as file:
                    windows_boot = file.read(512)
                with open(os.path.join(RECIPE, "image_mbr.bin"), "rb") as file:
                    windows_mbr = file.read(512)
                ours = bytearray(boot[:120])
                theirs = bytearray(windows_boot[:120])
                ours[100:104] = theirs[100:104] = b"\0\0\0\0"
                self.check(f"{tag}: boot sector fields equal Windows's", ours == theirs,
                           f"ours {ours.hex()} windows {theirs.hex()}")
                self.check(f"{tag}: boot sector tail equals Windows's", boot[510:] == windows_boot[510:])
                self.check(f"{tag}: MBR partition entry equals Windows's", mbr[446:512] == windows_mbr[446:512],
                           f"ours {mbr[446:462].hex()} windows {windows_mbr[446:462].hex()}")
            if name != "128 MB":
                os.unlink(image)
        self.sector = 512

    def run_all(self):
        self.test_mkfs_images()
        self.test_power_cut()
        self.test_corruptions()
        self.test_formatter()
        if not self.quick:
            self.test_large_file()
            self.test_huge_volume()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cli", help="the exfat_host binary")
    parser.add_argument("--quick", action="store_true", help="skip the 4 GB file and the 2.5 TB volumes")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("--work", help="work directory (default: a new one under /var/tmp)")
    args = parser.parse_args()
    if os.geteuid() != 0:
        sys.exit("run as root: loop devices and fuse mounts need it")
    for tool in ["mkfs.exfat", "fsck.exfat", "mount.exfat-fuse", "losetup"]:
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")
    # /var/tmp, not /tmp: WSL mounts /tmp in RAM, and the large tests write 14 GB.
    work = args.work or tempfile.mkdtemp(prefix="exfat_host.", dir="/var/tmp")
    os.makedirs(work, exist_ok=True)
    harness = Harness(args.cli, work, args.quick)
    started = time.time()
    try:
        harness.run_all()
    finally:
        print(f"passed {harness.passed} failed {harness.failed} in {time.time() - started:.0f} s", flush=True)
        if not args.keep and harness.failed == 0:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f"work directory kept: {work}")
    sys.exit(1 if harness.failed else 0)


if __name__ == "__main__":
    main()
