#!/usr/bin/env python3
"""Shows the thread pool's limit: with N workers, only N clients are served at once.

Starts the server in pool mode with 4 workers, opens 8 connections at once, and
sends each one a command. Reports which connections got a reply within 2 seconds,
then closes the served ones and checks that the waiting ones finally get served.

usage: python3 bench/pool_limit.py build/minikv [pool | epoll]

With "epoll" the same 4 threads are event loops instead of pool workers, and all
8 clients should be served at once.
"""
import socket
import subprocess
import sys
import time

PORT = 7900
WORKERS = 4
CLIENTS = 8


def got_reply(sock, timeout):
    sock.settimeout(timeout)
    try:
        return sock.recv(100).startswith(b"OK")
    except socket.timeout:
        return False


MODE = sys.argv[2] if len(sys.argv) > 2 else "pool"
server = subprocess.Popen([sys.argv[1], str(PORT), MODE, str(WORKERS)], stdout=subprocess.PIPE)
for line in server.stdout:
    if b"listening" in line:
        break

try:
    socks = []
    for i in range(CLIENTS):
        s = socket.create_connection(("127.0.0.1", PORT))  # TCP connect succeeds either way
        s.sendall(f"SET k{i} v\n".encode())
        socks.append(s)

    served = [got_reply(s, 2.0) for s in socks]
    print(f"{MODE} with {WORKERS} threads, {CLIENTS} clients connected at once")
    print(f"  replied within 2s: {sum(served)} of {CLIENTS}  (clients {[i for i, ok in enumerate(served) if ok]})")

    # Free the workers by disconnecting the served clients.
    for s, ok in zip(socks, served):
        if ok:
            s.close()
    time.sleep(0.5)
    rest = [(i, got_reply(s, 2.0)) for i, (s, ok) in enumerate(zip(socks, served)) if not ok]
    print(f"  after the served clients disconnect, the waiting ones got replies: "
          f"{sum(ok for _, ok in rest)} of {len(rest)}")
finally:
    server.terminate()
    server.wait()
