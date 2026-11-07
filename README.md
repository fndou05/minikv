# minikv

A multithreaded in-memory key-value store written in C++, accessed over TCP. 
It's a small Redis-style server I'm building to learn how sockets, concurrency, 
and persistence work.

This is a work in progress

## Status

Stages 1 and 2 work: the server supports SET, GET, and DELETE and can serve
many clients at once in either a thread-per-connection mode or a fixed thread
pool. **The storage is not thread-safe yet** (no locking until stage 3), so
don't run concurrent writers against this version. Anything not described in the
sections below is still planned.

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
./build/minikv 7379 [single | thread | pool [workers]]
```

- `single`: one client at a time (default)
- `thread`: a new thread for every connection
- `pool`: a fixed pool of worker threads (16 by default). Each worker serves one
  client until it disconnects, so with N workers only N clients are served at
  once and the rest wait.

The server listens on `127.0.0.1` only (there's no authentication). Try it with
`nc 127.0.0.1 7379` and type commands.

## Tests

`tests/test_server.py` starts the server and checks replies over a real socket,
including a command split across sends and many commands in one send.

```
python3 tests/test_server.py build/minikv [single | thread | pool]
```

## Layout

- `src/storage.*`: the key-value table (an `unordered_map` behind `set`/`get`/`remove`)
- `src/protocol.*`: turns one command line into a reply
- `src/server.*`: sockets, buffering partial reads, sending replies
- `src/thread_pool.*`: fixed worker threads fed by a queue of client sockets
- `src/main.cpp`: accept loop and mode selection

## Benchmarks

None yet. I'll add results here after each stage, along with the machine, 
compiler flags, and workload I used.
