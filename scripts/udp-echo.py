#!/usr/bin/env python3
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
"""UDP echo server for the pf-smoke NRO's LAN test (`nxlink -s spike/pf-smoke.nro`)."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
print(f"udp echo on :{port}", flush=True)
while True:
    data, addr = s.recvfrom(65535)
    s.sendto(data, addr)
    print(f"echo {len(data)}B -> {addr[0]}:{addr[1]}", flush=True)
