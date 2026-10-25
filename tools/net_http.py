#!/usr/bin/env python3
"""The PC's side of curl_check (docs/scope/56-store-networking.md, step 10): a store's files over HTTP on one port and
HTTPS on the next, with Mbed TLS's test certificate and key for localhost. It serves:
- /catalog.json, {"files": [{"name", "size", "sha256"}, ...]} for the data files below, and "launch_target", the same
  for launch_target.xex once it's built; /catalog_bad.json, malformed;
- /files/data_<N>.bin, N bytes, byte i being (i * 31 + 7) & 0xFF, with Range's "bytes=a-" and "bytes=a-b";
- /redirect, a 302 to /files/data_4096.bin; /stall, headers with a Content-Length, then nothing until the client
  closes; /drop/data_<N>.bin, headers and half the body, then the connection closed;
- /launch_target.xex, build/launch_target.xex;
- /site/<path>, the file at <path> in --site's folder, such as a store catalogue's test site
  (docs/scope/63-homebrew-store.md), with Range as /files/ has it; /site-drop/<path>, half of it, then the connection
  closed.
--break changes one thing, for a check broken on purpose: "byte", one byte of every file; "range", Range ignored, the
whole file sent with 200; "catalog", one entry's size; "nosend", launch_target built to launch back with no data
(tests/breaks/m56). Logs each request; runs until stopped, or for --seconds."""

import argparse
import hashlib
import http.server
import json
import pathlib
import re
import ssl
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAUNCH_TARGET = ROOT / "build" / "launch_target.xex"
LAUNCH_TARGET_NOSEND = ROOT / "build" / "m56-breaks" / "nosend" / "launch_target.xex"
# server5 under test-ca2, good until 2033, in mbedtls-xenon beside this repository.
CERTIFICATES = ROOT.parent / "mbedtls-xenon" / "framework" / "data_files"
CATALOG_SIZES = (4096, 65536, 1048576, 8388608)
MAX_FILE_BYTES = 256 * 1024 * 1024
SITE_TYPES = {".json": "application/json", ".png": "image/png", ".jpg": "image/jpeg", ".zip": "application/zip"}
CHUNK_BYTES = 65536
STALL_BYTES = 4096
HANDLER_TIMEOUT_S = 30
BREAKS = ("byte", "range", "catalog", "nosend")
FILE_NAME = re.compile(r"data_([0-9]{1,9})\.bin")
RANGE = re.compile(r"bytes=([0-9]+)-([0-9]*)")
# (i * 31 + 7) & 0xFF depends on i's low byte only, so the pattern repeats every 256 bytes.
PERIOD = bytes((i * 31 + 7) & 0xFF for i in range(256))
MALFORMED_CATALOG = b'{"files": [{"name": "data_4096.bin", "size": 4096, "sha256": '


def log(text):
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def pattern(start, end):
    """Bytes start to end of a data file."""
    offset = start % len(PERIOD)
    return (PERIOD * ((offset + end - start) // len(PERIOD) + 1))[offset:offset + end - start]


def file_hash(size):
    digest = hashlib.sha256()
    for start in range(0, size, CHUNK_BYTES):
        digest.update(pattern(start, min(size, start + CHUNK_BYTES)))
    return digest.hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = HANDLER_TIMEOUT_S

    def handle(self):
        # The handshake here, on the connection's own thread, not in accept.
        if isinstance(self.connection, ssl.SSLSocket):
            try:
                self.connection.do_handshake()
            except (ssl.SSLError, OSError) as error:
                log(f"https {self.client_address[0]}:{self.client_address[1]}: handshake failed: {error}")
                return
        super().handle()

    def log_message(self, format, *args):
        log(f"{self.server.scheme} {self.client_address[0]}:{self.client_address[1]}: {format % args}")

    def broken(self, name):
        return self.server.break_name == name

    def send_headers(self, status, length, content_type="application/octet-stream", headers=()):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(length))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()

    def send_body(self, body, content_type):
        self.send_headers(200, len(body), content_type)
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/catalog.json":
            self.send_body(self.server.catalog(), "application/json")
        elif path == "/catalog_bad.json":
            self.send_body(MALFORMED_CATALOG, "application/json")
        elif path.startswith("/files/"):
            self.send_file(path[len("/files/"):], drop=False)
        elif path.startswith("/drop/"):
            self.send_file(path[len("/drop/"):], drop=True)
        elif path == "/redirect":
            self.send_headers(302, 0, headers=[("Location", "/files/data_4096.bin")])
        elif path == "/stall":
            self.stall()
        elif path.startswith("/site/") and self.server.site:
            self.send_site_file(urllib.parse.unquote(path[len("/site/"):]), drop=False)
        elif path.startswith("/site-drop/") and self.server.site:
            self.send_site_file(urllib.parse.unquote(path[len("/site-drop/"):]), drop=True)
        elif path == "/launch_target.xex":
            target = self.server.launch_target()
            if target.is_file():
                self.send_body(target.read_bytes(), "application/octet-stream")
            else:
                self.send_error(404, f"{target} isn't built")
        else:
            self.send_error(404)

    # The part of a size-byte file a Range header asks for: (start, end, status), or None when none of it is there.
    def asked_range(self, size):
        asked = RANGE.fullmatch(self.headers.get("Range", "").strip())
        if not asked or self.broken("range"):
            return 0, size, 200
        first = int(asked[1])
        last = int(asked[2]) if asked[2] else size - 1
        if first >= size or last < first:
            return None
        return first, min(last, size - 1) + 1, 206

    def send_file(self, name, drop):
        found = FILE_NAME.fullmatch(name)
        size = int(found[1]) if found else 0
        if not found or size > MAX_FILE_BYTES:
            self.send_error(404)
            return
        part = (0, size, 200) if drop else self.asked_range(size)
        if part is None:
            self.send_headers(416, 0, headers=[("Content-Range", f"bytes */{size}")])
            return
        start, end, status = part
        headers = [("Accept-Ranges", "bytes")]
        if status == 206:
            headers.append(("Content-Range", f"bytes {start}-{end - 1}/{size}"))
        self.send_headers(status, end - start, headers=headers)
        stop = start + (end - start) // 2 if drop else end
        changed = size // 2
        for chunk_start in range(start, stop, CHUNK_BYTES):
            chunk_end = min(stop, chunk_start + CHUNK_BYTES)
            chunk = pattern(chunk_start, chunk_end)
            if self.broken("byte") and chunk_start <= changed < chunk_end:
                chunk = bytearray(chunk)
                chunk[changed - chunk_start] ^= 0xFF
            self.wfile.write(chunk)
        if drop:
            log(f"{self.server.scheme} {self.client_address[0]}:{self.client_address[1]}: dropped after "
                f"{stop - start} of {size} bytes")
            self.close_connection = True

    def send_site_file(self, name, drop):
        root = self.server.site.resolve()
        path = (root / name).resolve()
        if root not in path.parents or not path.is_file():
            self.send_error(404)
            return
        data = path.read_bytes()
        part = (0, len(data), 200) if drop else self.asked_range(len(data))
        if part is None:
            self.send_headers(416, 0, headers=[("Content-Range", f"bytes */{len(data)}")])
            return
        start, end, status = part
        headers = [("Accept-Ranges", "bytes")]
        if status == 206:
            headers.append(("Content-Range", f"bytes {start}-{end - 1}/{len(data)}"))
        self.send_headers(status, end - start, SITE_TYPES.get(path.suffix.lower(), "application/octet-stream"), headers)
        self.wfile.write(data[start:start + (end - start) // 2] if drop else data[start:end])
        if drop:
            log(f"{self.server.scheme} {self.client_address[0]}:{self.client_address[1]}: dropped after "
                f"{(end - start) // 2} of {len(data)} bytes")
            self.close_connection = True

    def stall(self):
        self.send_headers(200, STALL_BYTES)
        self.close_connection = True
        while True:
            try:
                if not self.connection.recv(1):
                    break
            except TimeoutError:
                continue
            except OSError:
                break
        log(f"{self.server.scheme} {self.client_address[0]}:{self.client_address[1]}: the stalled client closed")


class Server(http.server.ThreadingHTTPServer):
    def __init__(self, port, context, break_name, file_hashes, site):
        super().__init__(("", port), Handler)
        self.scheme = "https" if context else "http"
        self.context = context
        self.break_name = break_name
        self.file_hashes = file_hashes
        self.site = site

    # Each connection wrapped as it's accepted, as a wrapped listening socket fails on some Pythons for Windows.
    def get_request(self):
        connection, address = super().get_request()
        if self.context:
            connection = self.context.wrap_socket(connection, server_side=True, do_handshake_on_connect=False)
        return connection, address

    def launch_target(self):
        return LAUNCH_TARGET_NOSEND if self.break_name == "nosend" else LAUNCH_TARGET

    def catalog(self):
        files = [{"name": f"data_{size}.bin", "size": size, "sha256": self.file_hashes[size]} for size in CATALOG_SIZES]
        if self.break_name == "catalog":
            files[0]["size"] += 1
        catalog = {"files": files}
        target = self.launch_target()
        if target.is_file():
            data = target.read_bytes()
            catalog["launch_target"] = {"name": "launch_target.xex", "size": len(data),
                                        "sha256": hashlib.sha256(data).hexdigest()}
        return json.dumps(catalog, indent=1).encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=7380, help="HTTP's port, 7380 by default; HTTPS takes the next")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    parser.add_argument("--break", dest="break_name", choices=BREAKS, help="one thing broken on purpose")
    parser.add_argument("--certificates", type=pathlib.Path, default=CERTIFICATES,
                        help="the folder with server5.crt and server5.key, mbedtls-xenon's framework/data_files")
    parser.add_argument("--site", type=pathlib.Path, help="a folder served under /site/")
    arguments = parser.parse_args()
    assert pattern(250, 262) == bytes((i * 31 + 7) & 0xFF for i in range(250, 262))

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    certificate = arguments.certificates / "server5.crt"
    if not certificate.is_file():
        parser.error(f"no {certificate}: give --certificates mbedtls-xenon's framework/data_files")
    context.load_cert_chain(certificate, arguments.certificates / "server5.key")
    file_hashes = {size: file_hash(size) for size in CATALOG_SIZES}
    http_server = Server(arguments.port, None, arguments.break_name, file_hashes, arguments.site)
    https_server = Server(arguments.port + 1, context, arguments.break_name, file_hashes, arguments.site)
    for server in (http_server, https_server):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    log(f"HTTP on port {arguments.port}, HTTPS on {arguments.port + 1}"
        f"{', broken: ' + arguments.break_name if arguments.break_name else ''}")
    try:
        if arguments.seconds:
            time.sleep(arguments.seconds)
        else:
            threading.Event().wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
