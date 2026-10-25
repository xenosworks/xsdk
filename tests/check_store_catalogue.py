#!/usr/bin/env python3
"""Checks tools/store_catalogue.py: its ECDSA against RFC 6979's vector and OpenSSL, then a catalogue packed, signed
and checked, and each change a store must refuse refused. Takes a .xex with a version, such as
build/probe_replace_v2.xex."""

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "store_catalogue.py"
sys.path.insert(0, str(TOOL.parent))
import store_catalogue as store  # noqa: E402

# RFC 6979, A.2.5: P-256, SHA-256, message "sample".
RFC_PRIVATE = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
RFC_PUBLIC = (0x60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6,
              0x7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299)
RFC_SIGNATURE = (0xEFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716,
                 0xF7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8)

counts = {True: 0, False: 0}


def check(name, passed):
    counts[bool(passed)] += 1
    print(f"{'PASS' if passed else 'FAIL'}: {name}")


def tool(*arguments):
    return subprocess.run([sys.executable, str(TOOL), *map(str, arguments)], capture_output=True, text=True)


def check_ecdsa():
    digest = hashlib.sha256(b"sample").digest()
    check("ecdsa: P-256's generator is on the curve, and RFC 6979's key gives its public key",
          store.on_curve(store.G) and store.multiply(RFC_PRIVATE, store.G) == RFC_PUBLIC)
    signature = store.sign(RFC_PRIVATE, digest)
    check("ecdsa: RFC 6979's key signs \"sample\" as its vector has it, and that verifies",
          signature == RFC_SIGNATURE and store.verify(RFC_PUBLIC, digest, *signature))
    check("ecdsa: with s one more, or another message, it doesn't verify",
          not store.verify(RFC_PUBLIC, digest, signature[0], signature[1] + 1)
          and not store.verify(RFC_PUBLIC, hashlib.sha256(b"samplf").digest(), *signature))
    der = store.der_signature(*signature)
    check("ecdsa: the DER signature reads back", store.read_signature(der) == signature
          and store.read_signature(der[:-1]) is None)


def check_openssl(work):
    openssl = shutil.which("openssl")
    if not openssl:
        print("SKIP: openssl: none on the PATH")
        return
    message = work / "message"
    message.write_bytes(b"a catalogue's bytes\n")
    key = work / "ours.pem"
    tool("keygen", key)
    signature = store.der_signature(*store.sign(store.read_private_key(key), hashlib.sha256(message.read_bytes())
                                                .digest()))
    (work / "ours.sig").write_bytes(signature)
    verified = subprocess.run([openssl, "dgst", "-sha256", "-verify", work / "ours.pub.pem", "-signature",
                               work / "ours.sig", message], capture_output=True, text=True)
    check("openssl: verifies keygen's key's signature", verified.returncode == 0 and "Verified OK" in verified.stdout)
    theirs = work / "theirs.pem"
    subprocess.run([openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", theirs], check=True)
    subprocess.run([openssl, "ec", "-in", theirs, "-pubout", "-out", work / "theirs.pub.pem"], check=True,
                   capture_output=True)
    subprocess.run([openssl, "dgst", "-sha256", "-sign", theirs, "-out", work / "theirs.sig", message], check=True)
    read = store.read_signature((work / "theirs.sig").read_bytes())
    check("openssl: its key reads, and its signature verifies here",
          store.read_private_key(theirs) > 0 and read is not None
          and store.verify(store.read_public_key(work / "theirs.pub.pem"), hashlib.sha256(message.read_bytes())
                           .digest(), *read))


def write_catalogue(path, url):
    path.write_text(json.dumps({
        "format": 1, "serial": 0, "url": "https://example.github.io/catalogue/catalogue.json",
        "categories": [{"id": "tools", "name": "Tools", "sentence": "Things to run."}],
        "apps": [{"id": "probe-replace", "name": "probe_replace", "author": "xenosworks", "category": "tools",
                  "sentence": "Replaces itself.", "needs": "", "changes": "The first.", "source":
                  "https://github.com/xenosworks/xsdk", "license": "MIT", "screenshot": "shots/probe.png",
                  "launch": "probe_replace.xex", "package": {"url": url}}]}))


def check_catalogue(work, xex):
    folder = work / "app"
    (folder / "data").mkdir(parents=True)
    shutil.copy(xex, folder / "probe_replace.xex")
    (folder / "data" / "readme.txt").write_text("a data file\n")
    packages = work / "packages"
    packages.mkdir()
    catalogue = work / "catalogue.json"
    (work / "shots").mkdir()
    (work / "shots" / "probe.png").write_bytes(b"\x89PNG\r\n\x1a\n" + bytes(64))
    url = "https://github.com/xenosworks/xsdk/releases/download/v1/probe-replace-0.0.0.2.zip"
    write_catalogue(catalogue, url)
    zip_path = packages / "probe-replace-0.0.0.2.zip"
    packed = tool("pack", catalogue, "probe-replace", folder, zip_path)
    tool("keygen", work / "key.pem")
    signed = tool("sign", catalogue, work / "key.pem")
    app = json.loads(catalogue.read_bytes())["apps"][0]
    checked = tool("check", catalogue, "--public", work / "key.pub.pem", "--packages", packages)
    check("catalogue: packed with its .xex's title ID and version, signed and checked", packed.returncode == 0
          and signed.returncode == 0 and checked.returncode == 0 and app["version"] == "0.0.0.2"
          and app["title_id"] == "5853444B" and len(app["files"]) == 2 and "serial 1" in checked.stdout)
    first_zip = zip_path.read_bytes()
    tool("pack", catalogue, "probe-replace", folder, zip_path)
    check("catalogue: the same files pack into the same zip", zip_path.read_bytes() == first_zip)
    tool("sign", catalogue, work / "key.pem")
    check("catalogue: each signing raises the serial", json.loads(catalogue.read_bytes())["serial"] == 2)

    good = catalogue.read_bytes()
    catalogue.write_bytes(good.replace(b"Replaces itself.", b"Replaces itselg."))
    check("catalogue: one byte changed, the signature is refused",
          tool("check", catalogue, "--public", work / "key.pub.pem").returncode == 1)
    catalogue.write_bytes(good)
    tool("keygen", work / "other.pem")
    check("catalogue: another key's public half refuses it",
          tool("check", catalogue, "--public", work / "other.pub.pem").returncode == 1)

    with zipfile.ZipFile(zip_path, "a") as archive:
        archive.comment = b"changed"
    (work / "shots" / "probe.png").write_bytes(b"\x89PNG\r\n\x1a\n" + bytes(63) + b"\x01")
    check("catalogue: a screenshot changed after signing is refused", tool("check", catalogue).returncode == 1)
    (work / "shots" / "probe.png").write_bytes(b"\x89PNG\r\n\x1a\n" + bytes(64))
    check("catalogue: a package changed outside its files, its comment, is refused",
          tool("check", catalogue, "--packages", packages).returncode == 1)
    zip_path.write_bytes(first_zip)
    entries = json.loads(good)
    entries["apps"][0]["files"][0]["sha256"] = "0" * 64
    catalogue.write_text(json.dumps(entries))
    check("catalogue: an entry whose file isn't the package's is refused",
          tool("check", catalogue, "--packages", packages).returncode == 1)

    entries = json.loads(good)
    entries["apps"][0]["version"], entries["apps"][0]["version_number"] = "0.0.0.3", 3
    catalogue.write_text(json.dumps(entries))
    check("catalogue: an entry whose version isn't its .xex's is refused",
          tool("check", catalogue, "--packages", packages).returncode == 1)
    for path, what in (("data/" + "n" * 38, "a name over 37 characters"),
                       ("../escape.txt", "a path out of the app's folder")):
        entries = json.loads(good)
        data_file = next(file for file in entries["apps"][0]["files"] if file["path"] == "data/readme.txt")
        data_file["path"] = path
        catalogue.write_text(json.dumps(entries))
        check(f"catalogue: {what} is refused", tool("check", catalogue).returncode == 1)


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <a .xex with a version>")
    check_ecdsa()
    with tempfile.TemporaryDirectory() as directory:
        check_openssl(Path(directory))
        check_catalogue(Path(directory), Path(sys.argv[1]))
    print(f"tests: {counts[True]} passed, {counts[False]} failed")
    sys.exit(counts[False] != 0)


if __name__ == "__main__":
    main()
