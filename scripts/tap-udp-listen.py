#!/usr/bin/env python3
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
"""Pull TAP from the Switch (TCP 28773 to SWITCH_IP, plus UDP 28772)."""
import os
import socket
import sys
import threading
import time

SWITCH_IP = os.environ.get("SWITCH_IP", "")
UDP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 28772
TCP_PORT = int(os.environ.get("SWITCH_TAP_TCP", "28773"))
lock = threading.Lock()


def emit(data: bytes) -> None:
    with lock:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()


def udp_loop() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", UDP_PORT))
    print(f"TAP UDP listening on :{UDP_PORT}", file=sys.stderr, flush=True)
    while True:
        data, _ = sock.recvfrom(65535)
        emit(data)


def tcp_loop() -> None:
    if not SWITCH_IP:
        print("TAP TCP skipped (no SWITCH_IP)", file=sys.stderr, flush=True)
        return
    print(f"TAP TCP connecting to {SWITCH_IP}:{TCP_PORT}", file=sys.stderr, flush=True)
    announced = False
    while True:
        sock = None
        try:
            sock = socket.create_connection((SWITCH_IP, TCP_PORT), timeout=1.0)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(1.0)
            got = False
            idle_since = time.monotonic()
            while True:
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    # Idle is normal between TAP lines. A connect that never
                    # delivers data is a backlog ghost — drop it.
                    if not got and time.monotonic() - idle_since > 2.0:
                        break
                    continue
                if not data:
                    break
                if not got and not announced:
                    print(f"TAP TCP connected to {SWITCH_IP}:{TCP_PORT}", file=sys.stderr, flush=True)
                    announced = True
                got = True
                emit(data)
        except OSError:
            pass
        finally:
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
        time.sleep(0.5 if announced else 0.25)


threading.Thread(target=udp_loop, daemon=True).start()
tcp_loop()
