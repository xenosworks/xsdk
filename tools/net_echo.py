#!/usr/bin/env python3
"""The PC's side of the console's network checks (docs/scope/networking.md): echoes what arrives over TCP and UDP on
one port. On a TCP line "back <port>" it answers "ok", then connects to the sender on that port and sends
"hello from the PC"; on "addr" it answers with the sender's address. On "hold <ms>" it keeps the connection that long,
sending nothing and reading nothing, then looks once at how it stands and closes it; on "last" it answers how the last
held connection stood: "fin", "reset", "data" (bytes waiting) or "open" (docs/scope/56-store-networking.md). Logs each
event; runs until stopped, or for --seconds."""

import argparse
import socket
import threading
import time

LINE_BYTES = 1024
CONNECT_BACK_DELAY_S = 0.2
CONNECT_TIMEOUT_S = 5
# How long "last" waits for a connection still being held.
LAST_WAIT_S = 10

last_held = {"done": threading.Event(), "ending": "none"}
last_held["done"].set()
last_held_lock = threading.Lock()


def log(text):
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def connect_back(host, port):
    # Lets the console reach its accept before the connection comes.
    time.sleep(CONNECT_BACK_DELAY_S)
    try:
        with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S) as back:
            back.sendall(b"hello from the PC\n")
        log(f"tcp: connected back to {host}:{port}")
    except OSError as error:
        log(f"tcp: couldn't connect back to {host}:{port}: {error}")


def hold(client, address, milliseconds):
    global last_held
    held = {"done": threading.Event(), "ending": "open"}
    with last_held_lock:
        last_held = held
    time.sleep(milliseconds / 1000)
    client.setblocking(False)
    try:
        held["ending"] = "data" if client.recv(1) else "fin"
    except BlockingIOError:
        held["ending"] = "open"
    except ConnectionResetError:
        held["ending"] = "reset"
    except OSError as error:
        held["ending"] = f"error {error.errno}"
    log(f"tcp {address[0]}:{address[1]}: held {milliseconds} ms, then {held['ending']}")
    held["done"].set()


def answer_last(client):
    with last_held_lock:
        held = last_held
    held["done"].wait(LAST_WAIT_S)
    client.sendall(f"{held['ending']}\n".encode("ascii"))


def serve_tcp_client(client, address):
    with client:
        try:
            data = client.recv(LINE_BYTES)
        except OSError as error:
            log(f"tcp {address[0]}:{address[1]}: {error}")
            return
        text = data.decode("ascii", "replace").strip()
        log(f"tcp {address[0]}:{address[1]}: {text!r}")
        if text.startswith("back "):
            client.sendall(b"ok\n")
            threading.Thread(target=connect_back, args=(address[0], int(text.split()[1])), daemon=True).start()
        elif text == "addr":
            client.sendall(f"{address[0]}\n".encode("ascii"))
        elif text.startswith("hold "):
            hold(client, address, int(text.split()[1]))
        elif text == "last":
            answer_last(client)
        else:
            client.sendall(data)


def serve_tcp(port):
    server = socket.create_server(("", port))
    while True:
        client, address = server.accept()
        threading.Thread(target=serve_tcp_client, args=(client, address), daemon=True).start()


def serve_udp(port):
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("", port))
    while True:
        data, address = server.recvfrom(LINE_BYTES)
        log(f"udp {address[0]}:{address[1]}: {data.decode('ascii', 'replace').strip()!r}")
        server.sendto(data, address)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=7370, help="the TCP and UDP port, 7370 by default")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    arguments = parser.parse_args()
    for serve in (serve_tcp, serve_udp):
        threading.Thread(target=serve, args=(arguments.port,), daemon=True).start()
    log(f"listening on TCP and UDP port {arguments.port}")
    try:
        if arguments.seconds:
            time.sleep(arguments.seconds)
        else:
            threading.Event().wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
