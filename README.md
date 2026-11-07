# minikv

A multithreaded in-memory key-value store written in C++, accessed over TCP. 
It's a small Redis-style server I'm building to learn how sockets, concurrency, 
and persistence work.

This is a work in progress

## Status

Stage 1 works: a single-threaded server that handles one client at a time and
supports SET, GET, and DELETE. Everything below this line that isn't described
in the sections further down is still planned.

## Plan

I'm building this in stages and measuring requests per second after each one:

1. Single-threaded TCP server
2. Multiple clients (thread-per-connection vs. thread pool, maybe epoll)
3. Thread-safe storage (one global lock vs. finer-grained locking)
4. Persistence with an append-only log

If there's time after that: key expiration.

## Protocol

Text based, one command per line, each ending in `\n` (a `\r\n` also works).

| Command | Reply |
|---|---|
| `SET <key> <value>` | `OK`. The value is everything after the key, so it can contain spaces. Keys can't. |
| `GET <key>` | the value, or `NOT_FOUND` |
| `DELETE <key>` | `OK` if the key existed, otherwise `NOT_FOUND` |
| anything else | `ERROR <short reason>` |

A line longer than 64 KB gets `ERROR line too long` and the connection is closed.

TCP is a byte stream, so a command can arrive in pieces and several commands can
arrive in one read. The server buffers input and only runs a command once its
newline has arrived.

## Building and running

Needs CMake and a C++17 compiler. Builds with `-Wall -Wextra`, and the default
build type is Release (`-O2`).

```
cmake -S . -B build
cmake --build build
./build/minikv 7379
```

The server listens on `127.0.0.1` only (there's no authentication). Try it with
`nc 127.0.0.1 7379` and type commands.

## Tests

`tests/test_server.py` starts the server and checks replies over a real socket,
including a command split across sends and many commands in one send.

```
python3 tests/test_server.py build/minikv
```

## Layout

- `src/storage.*`: the key-value table (an `unordered_map` behind `set`/`get`/`remove`)
- `src/protocol.*`: turns one command line into a reply
- `src/server.*`: sockets, buffering partial reads, sending replies
- `src/main.cpp`: accept loop

## Benchmarks

None yet. I'll add results here after each stage, along with the machine, 
compiler flags, and workload I used.
