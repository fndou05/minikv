#!/usr/bin/env python3
"""Starts the minikv server and checks its replies over a real TCP socket.

Usage: python3 tests/test_server.py build/minikv [server args after the port...]
"""
import socket
import subprocess
import sys
import time

PORT = 7391
failures = 0


def check(name, got, want):
    global failures
    if got == want:
        print(f"  ok    {name}")
    else:
        failures += 1
        print(f"  FAIL  {name}: got {got!r}, want {want!r}")


def connect():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def read_lines(s, count):
    """Read until `count` newline-terminated lines have arrived."""
    data = b""
    while data.count(b"\n") < count:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    return data.decode().split("\n")[:count]


def ask(s, line):
    s.sendall(line.encode() + b"\n")
    return read_lines(s, 1)[0]


def start_server(binary, extra):
    proc = subprocess.Popen([binary, str(PORT)] + extra, stdout=subprocess.PIPE)
    for line in proc.stdout:  # wait for the "listening" line
        if b"listening" in line:
            break
    return proc


def run_persistence_tests(binary, fsync=False):
    import os
    import signal
    import tempfile

    print("persistence (append-only log" + (", fsync on every write)" if fsync else ")"))
    log = os.path.join(tempfile.mkdtemp(), "minikv.log")
    args = ["pool", "4", "4", log] + (["fsync"] if fsync else [])  # mode, workers, shards, log file

    # First life: write some data, then kill -9 (no clean shutdown).
    server = start_server(binary, args)
    s = connect()
    ask(s, "SET keep this value stays")
    ask(s, "SET gone temporary")
    ask(s, "SET over first")
    ask(s, "SET over second")
    ask(s, "DELETE gone")
    ask(s, "SET empty ")
    s.close()
    server.send_signal(signal.SIGKILL)
    server.wait()

    # Second life: everything must come back.
    server = start_server(binary, args)
    s = connect()
    check("value restored", ask(s, "GET keep"), "this value stays")
    check("deleted key stays deleted", ask(s, "GET gone"), "NOT_FOUND")
    check("last overwrite wins", ask(s, "GET over"), "second")
    check("empty value restored", ask(s, "GET empty"), "")
    ask(s, "SET later added after restart")
    s.close()
    server.send_signal(signal.SIGKILL)
    server.wait()

    # Simulate a crash in the middle of writing a line: no trailing '\n'.
    with open(log, "ab") as f:
        f.write(b"SET torn this line is cut of")
    server = start_server(binary, args)
    s = connect()
    check("torn last line ignored", ask(s, "GET torn"), "NOT_FOUND")
    check("earlier data intact", ask(s, "GET later"), "added after restart")
    ask(s, "SET fresh after torn")
    s.close()
    server.send_signal(signal.SIGKILL)
    server.wait()

    # The write after the torn line must not have been glued onto it.
    server = start_server(binary, args)
    s = connect()
    check("write after torn line survives", ask(s, "GET fresh"), "after torn")
    check("still no torn key", ask(s, "GET torn"), "NOT_FOUND")
    s.close()
    server.terminate()
    server.wait()


def run_epoll_tests():
    """Only for the event-loop server: it must not be limited to N clients at once."""
    print("event loop: many clients, slow readers")
    # 4 event loops by default, so 300 open connections is far more than loops.
    # A pool of 4 workers would answer only 4 of them.
    many = [connect() for _ in range(300)]
    for i, c in enumerate(many):
        c.sendall(f"SET many{i} v{i}\n".encode())
    answered = sum(1 for c in many if read_lines(c, 1) == ["OK"])
    check("300 clients open at once all answered", answered, 300)
    check("they share one table", ask(many[0], "GET many299"), "v299")
    for c in many:
        c.close()

    # A client that asks for ~12 MB of replies and does not read them must not
    # block anyone else, and must still get every byte once it does read.
    slow = connect()
    big = "z" * 60000
    check("big value set", ask(slow, "SET bigz " + big), "OK")
    count = 200
    slow.sendall(b"GET bigz\n" * count)
    time.sleep(0.5)  # let the server fill the socket and hit its buffering limit
    other = connect()
    check("others served while a client is not reading", ask(other, "GET many5"), "v5")
    other.close()
    want = count * (len(big) + 1)
    got = 0
    slow.settimeout(10)
    while got < want:
        chunk = slow.recv(1 << 16)
        if not chunk:
            break
        got += len(chunk)
    check("slow reader gets every byte", got, want)
    slow.close()


def run_tests():
    s = connect()

    print("basic commands")
    check("set", ask(s, "SET name Frank"), "OK")
    check("get", ask(s, "GET name"), "Frank")
    check("get missing", ask(s, "GET ghost"), "NOT_FOUND")
    check("overwrite", ask(s, "SET name Bob"), "OK")
    check("get overwritten", ask(s, "GET name"), "Bob")
    check("delete", ask(s, "DELETE name"), "OK")
    check("delete again", ask(s, "DELETE name"), "NOT_FOUND")
    check("get after delete", ask(s, "GET name"), "NOT_FOUND")

    print("values and errors")
    check("value with spaces", ask(s, "SET msg hello big  world"), "OK")
    check("get value with spaces", ask(s, "GET msg"), "hello big  world")
    check("empty value", ask(s, "SET empty "), "OK")
    check("get empty value", ask(s, "GET empty"), "")
    check("unknown command", ask(s, "PING").startswith("ERROR"), True)
    check("set without value", ask(s, "SET lonely").startswith("ERROR"), True)
    check("get with two args", ask(s, "GET a b").startswith("ERROR"), True)
    check("crlf line ending", ask(s, "GET msg\r"), "hello big  world")

    print("framing")
    # A command split across two sends: nothing may happen until the '\n'.
    s.sendall(b"SET part he")
    time.sleep(0.2)
    s.sendall(b"llo wor")
    time.sleep(0.2)
    s.sendall(b"ld\n")
    check("partial command reply", read_lines(s, 1), ["OK"])
    check("partial command value", ask(s, "GET part"), "hello world")

    # Several commands in one send: every one must get its own reply, in order.
    s.sendall(b"SET a 1\nSET b 2\nGET a\nGET b\nDELETE a\nGET a\n")
    check(
        "pipelined commands",
        read_lines(s, 6),
        ["OK", "OK", "1", "2", "OK", "NOT_FOUND"],
    )

    # Both at once: whole commands plus a fragment, then the rest of it.
    s.sendall(b"SET c 3\nSET d ")
    check("whole + fragment first part", read_lines(s, 1), ["OK"])
    s.sendall(b"4\nGET d\n")
    check("fragment completed", read_lines(s, 2), ["OK", "4"])

    # A value bigger than one recv() chunk (4096) must survive intact.
    big = "x" * 50000
    check("large value set", ask(s, "SET big " + big), "OK")
    check("large value get", ask(s, "GET big"), big)

    print("several clients at the same time")
    # Three connections open together, taking turns. A server that serves one
    # client at a time never answers the second connection while the first is
    # open, so this only passes in thread / pool mode.
    if "single" not in sys.argv[2:] and len(sys.argv) > 2:
        a, b, c = connect(), connect(), connect()
        check("client a", ask(a, "SET ca 1"), "OK")
        check("client b", ask(b, "SET cb 2"), "OK")
        check("client c sees a's data", ask(c, "GET ca"), "1")
        check("client a sees b's data", ask(a, "GET cb"), "2")
        a.close()
        b.close()
        c.close()
    else:
        print("  (skipped in single mode)")

    if "epoll" in sys.argv[2:]:
        run_epoll_tests()

    print("reconnect")
    s.close()
    s2 = connect()
    check("data survives reconnect", ask(s2, "GET big") == "x" * 50000, True)
    s2.close()

    print("line too long")
    s3 = connect()
    s3.sendall(b"SET k " + b"y" * 70000)  # no newline, over the 64 KB limit
    reply = b""
    try:
        while not reply.endswith(b"\n"):
            chunk = s3.recv(4096)
            if not chunk:
                break
            reply += chunk
        closed = s3.recv(4096) == b""
    except ConnectionResetError:
        closed = True
    check("error reply", reply.startswith(b"ERROR"), True)
    check("connection closed", closed, True)
    s3.close()

    print("server survives a client vanishing mid-conversation")
    s4 = connect()
    s4.sendall(b"GET big\n")
    s4.close()  # leave without reading the reply
    time.sleep(0.2)
    s5 = connect()
    check("still alive", ask(s5, "SET alive yes"), "OK")
    s5.close()


if __name__ == "__main__":
    binary = sys.argv[1]
    server = start_server(binary, sys.argv[2:])
    try:
        run_tests()
    finally:
        server.terminate()
        server.wait()
    run_persistence_tests(binary)
    run_persistence_tests(binary, fsync=True)
    print("FAILED" if failures else "ALL PASSED", f"({failures} failures)")
    sys.exit(1 if failures else 0)
