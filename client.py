#!/usr/bin/env python3
"""Interactive CLI client for bytekv. Handles length-prefix framing."""

import socket
import struct
import sys

HOST = "localhost"
PORT = 9999


def send_framed(sock, message: bytes):
    header = struct.pack("!I", len(message))
    sock.sendall(header + message)


def recv_framed(sock) -> bytes:
    header = recv_exact(sock, 4)
    if not header:
        return None
    length = struct.unpack("!I", header)[0]
    return recv_exact(sock, length)


def recv_exact(sock, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def main():
    # One-shot mode: python3 client.py SET foo bar
    if len(sys.argv) > 1:
        cmd = " ".join(sys.argv[1:])
        with socket.socket() as s:
            s.connect((HOST, PORT))
            send_framed(s, cmd.encode())
            resp = recv_framed(s)
            if resp:
                print(resp.decode(), end="")
        return

    # Interactive mode
    print(f"bytekv client — connected to {HOST}:{PORT}")
    print("Type commands (SET key val, GET key, ...). Ctrl-C to quit.\n")
    with socket.socket() as s:
        s.connect((HOST, PORT))
        while True:
            try:
                cmd = input("bytekv> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not cmd:
                continue
            send_framed(s, cmd.encode())
            resp = recv_framed(s)
            if resp is None:
                print("Server closed connection.")
                break
            print(resp.decode(), end="")


if __name__ == "__main__":
    main()
