#!/usr/bin/env python3
"""The Homebrew App Store's catalogue (docs/scope/63-homebrew-store.md): a signing key, an app's package, the signature.

  keygen <key.pem>       a P-256 key, and its public half beside it as <key>.pub.pem for the store to build in; keep
                         the key itself off line
  pack <catalogue> <app id> <folder> <zip> [--url URL]
                         zips the app's folder, then writes into the app's entry the zip's size and SHA-256, each
                         file's, and the title ID and version of the .xex the entry launches
  sign <catalogue> <key.pem>
                         raises the serial, then signs the file into <catalogue>.sig
  check <catalogue> [--public <key.pub.pem>] [--packages <folder>]
                         checks the entries, then the signature, then the packages the entries name, found in the
                         folder under their URL's last part

The catalogue is JSON:
  {"format": 1, "serial": <raised by each signing; the store refuses one lower than the last it took>,
   "url": "<where it's served; a signed new one moves the store there>",
   "categories": [{"id", "name", "sentence"}],
   "apps": [{"id", "name", "author", "category", "sentence", "needs", "changes", "source", "license",
             "screenshot": "<a .png or .jpg beside the catalogue>", "title_id": "<8 hex digits>",
             "version": "1.2.300.4", "version_number": <packed as the .xex packs it>,
             "launch": "<the .xex to start, among the files>", "package": {"url", "size", "sha256"},
             "files": [{"path", "size", "sha256"}]}]}
The signature is ECDSA on P-256 over the file's SHA-256, DER-encoded, as Mbed TLS's mbedtls_pk_verify takes it. An
app installs to hdd:\\apps\\<id>\\, each file written as <name>.part first, so a name has 37 characters at most: FATX
takes 42 (docs/HARDWARE.md, "Shaders compiled as the game asks")."""

import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import struct
import sys
import urllib.parse
import zipfile
from pathlib import Path

from elf2xex import HEADER_EXECUTION_INFO, parse_version

FORMAT = 1
# NIST P-256 (FIPS 186-4, D.1.2.3); N is its order, RFC 6979's q (A.2.5).
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

# SEC 1's ECPrivateKey with the curve named and the public key (RFC 5915), and X.509's SubjectPublicKeyInfo
# (RFC 5480), for P-256: the bytes around the keys, which OpenSSL writes the same way.
PRIVATE_KEY_START = bytes.fromhex("307702010104" "20")
PRIVATE_KEY_MIDDLE = bytes.fromhex("a00a06082a8648ce3d030107" "a144034200" "04")
PUBLIC_KEY_START = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200" "04")

ID = re.compile(r"[a-z0-9][a-z0-9-]{0,31}")
TITLE_ID = re.compile(r"[0-9A-F]{8}")
SHA256 = re.compile(r"[0-9a-f]{64}")
NAME_BYTES = 37
NOT_IN_NAMES = set('\\/:*?"<>|')
# Fixed, so the same files always make the same zip.
ZIP_TIME = (1980, 1, 1, 0, 0, 0)
ZIP_FLAG_DATA_DESCRIPTOR = 0x08


def fail(message):
    sys.exit(f"store_catalogue: {message}")


def sha256(data):
    return hashlib.sha256(data).digest()


# shortcut: not constant time, which signing off line on the key holder's own machine doesn't need.
def add(first, second):
    if first is None:
        return second
    if second is None:
        return first
    (x1, y1), (x2, y2) = first, second
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if first == second:
        slope = (3 * x1 * x1 - 3) * pow(2 * y1, -1, P)
    else:
        slope = (y2 - y1) * pow(x2 - x1, -1, P)
    x3 = (slope * slope - x1 - x2) % P
    return x3, (slope * (x1 - x3) - y1) % P


def multiply(scalar, point):
    result = None
    while scalar:
        if scalar & 1:
            result = add(result, point)
        point = add(point, point)
        scalar >>= 1
    return result


def on_curve(point):
    x, y = point
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x - 3 * x + B)) % P == 0


def nonces(private, digest):
    """RFC 6979's k, section 3.2, for a 256-bit order and a 256-bit hash."""
    x = private.to_bytes(32, "big")
    h = (int.from_bytes(digest, "big") % N).to_bytes(32, "big")
    k, v = bytes(32), b"\x01" * 32
    for separator in (b"\x00", b"\x01"):
        k = hmac.digest(k, v + separator + x + h, "sha256")
        v = hmac.digest(k, v, "sha256")
    while True:
        v = hmac.digest(k, v, "sha256")
        candidate = int.from_bytes(v, "big")
        if 1 <= candidate < N:
            yield candidate
        k = hmac.digest(k, v + b"\x00", "sha256")
        v = hmac.digest(k, v, "sha256")


def sign(private, digest):
    e = int.from_bytes(digest, "big")
    for k in nonces(private, digest):
        r = multiply(k, G)[0] % N
        s = pow(k, -1, N) * (e + r * private) % N
        if r and s:
            return r, s


def verify(public, digest, r, s):
    if not (0 < r < N and 0 < s < N):
        return False
    w = pow(s, -1, N)
    point = add(multiply(int.from_bytes(digest, "big") * w % N, G), multiply(r * w % N, public))
    return point is not None and point[0] % N == r


def der_signature(r, s):
    body = b""
    for value in (r, s):
        data = value.to_bytes((value.bit_length() + 8) // 8, "big")
        body += bytes([0x02, len(data)]) + data
    return bytes([0x30, len(body)]) + body


def read_signature(data):
    """(r, s) from a DER signature, or None if it isn't one."""
    if len(data) < 8 or data[0] != 0x30 or data[1] != len(data) - 2:
        return None
    values = []
    offset = 2
    for _ in range(2):
        if offset + 2 > len(data) or data[offset] != 0x02:
            return None
        end = offset + 2 + data[offset + 1]
        values.append(int.from_bytes(data[offset + 2:end], "big"))
        offset = end
    return tuple(values) if offset == len(data) else None


def pem(label, der):
    text = base64.b64encode(der).decode()
    lines = [text[i:i + 64] for i in range(0, len(text), 64)]
    return "\n".join([f"-----BEGIN {label}-----", *lines, f"-----END {label}-----", ""])


def read_pem(path, label):
    text = path.read_text()
    match = re.search(f"-----BEGIN {label}-----(.*?)-----END {label}-----", text, re.S)
    if not match:
        fail(f"{path} holds no {label}")
    return base64.b64decode(match[1])


def public_key_der(point):
    return PUBLIC_KEY_START + point[0].to_bytes(32, "big") + point[1].to_bytes(32, "big")


def read_private_key(path):
    der = read_pem(path, "EC PRIVATE KEY")
    start, middle = len(PRIVATE_KEY_START), len(PRIVATE_KEY_START) + 32
    if len(der) != middle + len(PRIVATE_KEY_MIDDLE) + 64 or not der.startswith(PRIVATE_KEY_START) \
            or der[middle:middle + len(PRIVATE_KEY_MIDDLE)] != PRIVATE_KEY_MIDDLE:
        fail(f"{path} isn't a P-256 key with its public key, as keygen and OpenSSL's ecparam write")
    private = int.from_bytes(der[start:middle], "big")
    public = der[middle + len(PRIVATE_KEY_MIDDLE):]
    if not 0 < private < N or multiply(private, G) != (int.from_bytes(public[:32], "big"),
                                                        int.from_bytes(public[32:], "big")):
        fail(f"{path}: the public key doesn't belong to the private one")
    return private


def read_public_key(path):
    der = read_pem(path, "PUBLIC KEY")
    if len(der) != len(PUBLIC_KEY_START) + 64 or not der.startswith(PUBLIC_KEY_START):
        fail(f"{path} isn't a P-256 public key")
    point = (int.from_bytes(der[-64:-32], "big"), int.from_bytes(der[-32:], "big"))
    if not on_curve(point):
        fail(f"{path}: the point isn't on P-256")
    return point


def encode(catalogue):
    return (json.dumps(catalogue, indent=2, ensure_ascii=False) + "\n").encode()


def read_catalogue(path):
    try:
        return json.loads(path.read_bytes())
    except (OSError, ValueError) as error:
        fail(f"{path}: {error}")


def version_text(number):
    return f"{number >> 28}.{number >> 24 & 0xF}.{number >> 8 & 0xFFFF}.{number & 0xFF}"


def xex_identity(data):
    """The title ID and version in a .xex's execution info, or None if it hasn't one."""
    try:
        if data[:4] != b"XEX2":
            return None
        count = struct.unpack_from(">I", data, 0x14)[0]
        for index in range(count):
            key, value = struct.unpack_from(">II", data, 0x18 + 8 * index)
            if key == HEADER_EXECUTION_INFO:
                _, version_number, _, title_id = struct.unpack_from(">IIII", data, value)
                return title_id, version_number
    except struct.error:
        pass
    return None


def path_problem(path):
    """Why a file's path can't be installed, or None."""
    if not isinstance(path, str) or not path:
        return "isn't a path"
    for name in path.split("/"):
        if name in ("", ".", ".."):
            return "has an empty, . or .. part"
        if len(name) > NAME_BYTES:
            return f"has a name of {len(name)} characters, over {NAME_BYTES}"
        if any(ord(c) < 0x20 or ord(c) > 0x7E or c in NOT_IN_NAMES for c in name):
            return "has a character FATX or the store doesn't take"
    return None


def text_fields(entry, fields, where, problems, empty_ok=()):
    for field in fields:
        value = entry.get(field)
        if not isinstance(value, str) or (not value and field not in empty_ok):
            problems.append(f"{where}: {field} isn't {'text' if field in empty_ok else 'text, not empty'}")


def check_app(app, categories, problems):
    where = f"app {app.get('id')!r}"
    if not isinstance(app.get("id"), str) or not ID.fullmatch(app["id"]):
        problems.append(f"{where}: the id isn't lower-case letters, digits and -, 32 at most")
    text_fields(app, ("name", "author", "sentence", "license", "needs", "changes"), where, problems,
                ("needs", "changes"))
    if not str(app.get("source", "")).startswith("https://"):
        problems.append(f"{where}: source isn't an https:// address")
    if app.get("category") not in categories:
        problems.append(f"{where}: category {app.get('category')!r} isn't one of the categories")
    screenshot = app.get("screenshot")
    if path_problem(screenshot) or not str(screenshot).lower().endswith((".png", ".jpg", ".jpeg")):
        problems.append(f"{where}: screenshot isn't a .png or .jpg path beside the catalogue")
    if not isinstance(app.get("title_id"), str) or not TITLE_ID.fullmatch(app["title_id"]):
        problems.append(f"{where}: title_id isn't 8 upper-case hex digits")
    try:
        if parse_version(str(app.get("version"))) != app.get("version_number"):
            problems.append(f"{where}: version_number isn't version {app.get('version')} packed")
    except (ValueError, argparse.ArgumentTypeError):
        problems.append(f"{where}: version isn't major.minor.build.fix in 4, 4, 16 and 8 bits")

    package = app.get("package")
    if not isinstance(package, dict) or not str(package.get("url", "")).startswith("https://") \
            or not isinstance(package.get("size"), int) or package["size"] <= 0 \
            or not SHA256.fullmatch(str(package.get("sha256"))):
        problems.append(f"{where}: package isn't an https:// url, a size and a lower-case SHA-256")
    files = app.get("files")
    if not isinstance(files, list) or not files:
        problems.append(f"{where}: files isn't a list of files")
        return
    seen = set()
    for file in files:
        path = file.get("path") if isinstance(file, dict) else None
        problem = path_problem(path)
        if problem:
            problems.append(f"{where}: file {path!r} {problem}")
        elif path.lower() in seen:
            problems.append(f"{where}: file {path!r} comes twice, as FATX ignores case")
        seen.add(str(path).lower())
        if not isinstance(file, dict) or not isinstance(file.get("size"), int) or file["size"] < 0 \
                or not SHA256.fullmatch(str(file.get("sha256"))):
            problems.append(f"{where}: file {path!r} hasn't a size and a lower-case SHA-256")
    launch = app.get("launch")
    if not isinstance(launch, str) or not launch.lower().endswith(".xex") or launch.lower() not in seen:
        problems.append(f"{where}: launch isn't a .xex among the files")


def check_entries(catalogue):
    problems = []
    if not isinstance(catalogue, dict) or catalogue.get("format") != FORMAT:
        return [f"format isn't {FORMAT}"]
    if not isinstance(catalogue.get("serial"), int) or catalogue["serial"] < 0:
        problems.append("serial isn't a whole number")
    if not str(catalogue.get("url", "")).startswith("https://"):
        problems.append("url isn't an https:// address")
    for field in ("categories", "apps"):
        if not isinstance(catalogue.get(field), list):
            problems.append(f"{field} isn't a list")
            catalogue[field] = []
    categories = set()
    for category in catalogue["categories"]:
        if not isinstance(category, dict) or not ID.fullmatch(str(category.get("id"))):
            problems.append(f"category {category!r} hasn't an id of lower-case letters, digits and -")
            continue
        text_fields(category, ("name", "sentence"), f"category {category['id']!r}", problems)
        if category["id"] in categories:
            problems.append(f"category {category['id']!r} comes twice")
        categories.add(category["id"])
    ids = set()
    for app in catalogue["apps"]:
        if not isinstance(app, dict):
            problems.append(f"app {app!r} isn't an entry")
            continue
        check_app(app, categories, problems)
        if app.get("id") in ids:
            problems.append(f"app {app.get('id')!r} comes twice")
        ids.add(app.get("id"))
    return problems


def package_name(app):
    return Path(urllib.parse.unquote(urllib.parse.urlparse(app["package"]["url"]).path)).name


def check_package(app, path, problems):
    where = f"app {app['id']!r}, {path.name}"
    try:
        data = path.read_bytes()
    except OSError as error:
        problems.append(f"{where}: {error.strerror}")
        return
    if len(data) != app["package"]["size"] or sha256(data).hex() != app["package"]["sha256"]:
        problems.append(f"{where}: its size or SHA-256 isn't the entry's")
        return
    expected = {file["path"]: file for file in app["files"]}
    try:
        with zipfile.ZipFile(path) as archive:
            names = []
            for info in archive.infolist():
                names.append(info.filename)
                file = expected.get(info.filename)
                if info.flag_bits & ZIP_FLAG_DATA_DESCRIPTOR or info.compress_type not in (zipfile.ZIP_STORED,
                                                                                          zipfile.ZIP_DEFLATED):
                    problems.append(f"{where}: {info.filename} isn't stored or deflated with its size up front")
                elif file is None:
                    problems.append(f"{where}: {info.filename} isn't among the entry's files")
                else:
                    content = archive.read(info)
                    if len(content) != file["size"] or sha256(content).hex() != file["sha256"]:
                        problems.append(f"{where}: {info.filename}'s size or SHA-256 isn't the entry's")
                    if info.filename == app["launch"] and xex_identity(content) != (
                            int(app["title_id"], 16), app["version_number"]):
                        problems.append(f"{where}: {info.filename}'s title ID or version isn't the entry's")
            if sorted(names) != sorted(expected):
                problems.append(f"{where}: its files aren't the entry's, each once")
    except (zipfile.BadZipFile, OSError) as error:
        problems.append(f"{where}: {error}")


def keygen(args):
    public_path = args.key.with_name(args.key.stem + ".pub.pem")
    private = secrets.randbelow(N - 1) + 1
    public = multiply(private, G)
    der = PRIVATE_KEY_START + private.to_bytes(32, "big") + PRIVATE_KEY_MIDDLE + public_key_der(public)[-64:]
    try:
        # Readable by its owner alone, and never over a key already there.
        descriptor = os.open(args.key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        fail(f"{args.key} is there already")
    with os.fdopen(descriptor, "w") as file:
        file.write(pem("EC PRIVATE KEY", der))
    public_path.write_text(pem("PUBLIC KEY", public_key_der(public)))
    print(f"store_catalogue: the key is {args.key}; keep it off line. The store builds in {public_path}.")


def pack(args):
    catalogue = read_catalogue(args.catalogue)
    apps = [app for app in catalogue.get("apps", []) if isinstance(app, dict) and app.get("id") == args.app]
    if not apps:
        fail(f"{args.catalogue} has no entry for {args.app}; write its name, author and the rest first")
    app = apps[0]
    paths = sorted((path for path in args.folder.rglob("*") if path.is_file()), key=lambda path: path.as_posix())
    files = []
    # shortcut: each file read whole, fine for apps of a few hundred MB; stream them if one is ever larger.
    with zipfile.ZipFile(args.zip, "w") as archive:
        for path in paths:
            name = path.relative_to(args.folder).as_posix()
            data = path.read_bytes()
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, data, compresslevel=9)
            files.append({"path": name, "size": len(data), "sha256": sha256(data).hex()})
    launch = app.get("launch") or next((file["path"] for file in files if file["path"].lower().endswith(".xex")), "")
    found = [path for path in paths if path.relative_to(args.folder).as_posix() == launch]
    identity = xex_identity(found[0].read_bytes()) if found else None
    if identity is None:
        fail(f"{args.folder} has no .xex {launch!r} with an execution info header to launch")
    data = args.zip.read_bytes()
    app["launch"] = launch
    app["title_id"] = f"{identity[0]:08X}"
    app["version"] = version_text(identity[1])
    app["version_number"] = identity[1]
    app["package"] = {"url": args.url or app.get("package", {}).get("url", ""), "size": len(data),
                      "sha256": sha256(data).hex()}
    app["files"] = files
    args.catalogue.write_bytes(encode(catalogue))
    print(f"store_catalogue: {args.zip}, {len(data)} bytes, {len(files)} files, {args.app} {app['version']}")
    problems = [problem for problem in check_entries(catalogue) if problem.startswith(f"app {args.app!r}")]
    for problem in problems:
        print(f"store_catalogue: {problem}")


def sign_catalogue(args):
    catalogue = read_catalogue(args.catalogue)
    problems = check_entries(catalogue)
    if problems:
        fail("not signed: " + "; ".join(problems))
    private = read_private_key(args.key)
    catalogue["serial"] += 1
    data = encode(catalogue)
    digest = sha256(data)
    r, s = sign(private, digest)
    if not verify(multiply(private, G), digest, r, s):
        fail("the signature doesn't verify; nothing written")
    args.catalogue.write_bytes(data)
    args.catalogue.with_name(args.catalogue.name + ".sig").write_bytes(der_signature(r, s))
    print(f"store_catalogue: {args.catalogue} signed, serial {catalogue['serial']}")


def check(args):
    data = args.catalogue.read_bytes()
    catalogue = read_catalogue(args.catalogue)
    problems = check_entries(catalogue)
    if args.public:
        try:
            signature = read_signature(args.catalogue.with_name(args.catalogue.name + ".sig").read_bytes())
        except OSError:
            signature = None
        if signature is None or not verify(read_public_key(args.public), sha256(data), *signature):
            problems.append(f"{args.catalogue.name}.sig isn't the key's signature of {args.catalogue.name}")
    if args.packages and not problems:
        for app in catalogue["apps"]:
            check_package(app, args.packages / package_name(app), problems)
    for problem in problems:
        print(f"store_catalogue: {problem}")
    if problems:
        sys.exit(1)
    print(f"store_catalogue: {args.catalogue}, serial {catalogue['serial']}: {len(catalogue['apps'])} apps in "
          f"{len(catalogue['categories'])} categories, each as it should be")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("keygen")
    command.add_argument("key", type=Path)
    command.set_defaults(run=keygen)
    command = commands.add_parser("pack")
    command.add_argument("catalogue", type=Path)
    command.add_argument("app")
    command.add_argument("folder", type=Path)
    command.add_argument("zip", type=Path)
    command.add_argument("--url", help="where the zip is served, its GitHub release's asset")
    command.set_defaults(run=pack)
    command = commands.add_parser("sign")
    command.add_argument("catalogue", type=Path)
    command.add_argument("key", type=Path)
    command.set_defaults(run=sign_catalogue)
    command = commands.add_parser("check")
    command.add_argument("catalogue", type=Path)
    command.add_argument("--public", type=Path, help="the public key the store builds in")
    command.add_argument("--packages", type=Path, help="a folder holding the packages")
    command.set_defaults(run=check)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
