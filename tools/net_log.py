#!/usr/bin/env python3
"""The PC's side of a title's network log (docs/scope/53-network-log.md): a title with net_log.txt beside its .xex,
holding this PC's address and the port, sends each line of its log here as one UDP datagram, its 32-bit sequence
number first. Prints each line as it arrives, writes each run to a file of its own in --out, byte for byte the
title's log file, and reports lines that never came. A run starts where a sender's numbers go back to 0. Runs until
stopped, or for --seconds."""

import argparse
import pathlib
import socket
import struct
import sys
import time

SEQUENCE_BYTES = 4
DATAGRAM_BYTES = SEQUENCE_BYTES + 512
POLL_S = 1.0


def log(text):
    print(f"{time.strftime('%H:%M:%S')} net_log: {text}", flush=True)


class Run:
    """One title's log from one sender, from its line 0."""

    def __init__(self, out, host):
        stem = f"{time.strftime('%Y%m%d-%H%M%S')}-{host}"
        path = out / f"{stem}.log"
        for again in range(1, 100):
            if not path.exists():
                break
            path = out / f"{stem}-{again}.log"
        self.file = path.open("wb")
        self.host = host
        self.expected = 0
        self.received = 0
        self.lost = 0
        log(f"{host}: a run starts, written to {path}")

    def add(self, sequence, line):
        if sequence > self.expected:
            self.lost += sequence - self.expected
            log(f"{self.host}: lines {self.expected} to {sequence - 1} never came")
        elif sequence < self.expected:
            log(f"{self.host}: line {sequence} came after line {self.expected - 1}, written where it arrived")
        self.file.write(line)
        self.file.flush()
        self.expected = max(self.expected, sequence + 1)
        self.received += 1

    def close(self):
        self.file.close()
        log(f"{self.host}: {self.received} lines, {self.lost} never came")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=7371, help="the UDP port, 7371 by default")
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("net_log"),
                        help="the folder each run's log goes to, net_log by default")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("", arguments.port))
    server.settimeout(POLL_S)
    log(f"listening on UDP port {arguments.port}")
    end = time.monotonic() + arguments.seconds if arguments.seconds else None
    runs = {}
    try:
        while end is None or time.monotonic() < end:
            try:
                data, (host, _) = server.recvfrom(DATAGRAM_BYTES)
            except socket.timeout:
                continue
            if len(data) < SEQUENCE_BYTES:
                log(f"{host}: a datagram of {len(data)} bytes, too short for a line")
                continue
            (sequence,) = struct.unpack(">I", data[:SEQUENCE_BYTES])
            line = data[SEQUENCE_BYTES:]
            if host not in runs or sequence == 0:
                if host in runs:
                    runs[host].close()
                runs[host] = Run(arguments.out, host)
            runs[host].add(sequence, line)
            sys.stdout.write(line.decode("utf-8", "replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    for run in runs.values():
        run.close()


if __name__ == "__main__":
    main()
