# minikv

[![ci](https://github.com/fndou05/minikv/actions/workflows/ci.yml/badge.svg)](https://github.com/fndou05/minikv/actions/workflows/ci.yml)

A multithreaded in-memory key-value store written in C++17, accessed over TCP.
It's a small Redis-style server I built to learn how sockets, concurrency, and
persistence work. Only the standard library and POSIX are used.

## Status

Version 1.0.0. All four stages are built, tested, and benchmarked:

1. Single-threaded TCP server (SET / GET / DELETE)
2. Many clients at once: thread-per-connection or a fixed thread pool
3. Thread-safe storage: a mutex per shard (1 shard = one global lock)
4. Persistence: an append-only log that is replayed on startup

Tested on Linux (Ubuntu, with both g++ and clang++) by GitHub Actions on every
push, and on macOS (Apple Silicon) by hand. The benchmark numbers below are from
the Mac only; I haven't benchmarked on Linux.

Not built: key expiration, epoll, and log compaction (see "Limitations").

## How it works

```
 client ──TCP──▶ accept loop (main.cpp)
                   │  single: serve inline │ thread: new thread │ pool: queue ─▶ workers
                   ▼
              serve_client (server.cpp)
                 recv ─▶ buffer ─▶ split on '\n' ─▶ handle_command (protocol.cpp)
                                                          │
                                                          ▼
                                                  Storage (storage.cpp)
                                       hash(key) picks a shard ─▶ lock that shard's mutex
                                          ├─ unordered_map get/set/erase
                                          └─ append "SET k v" / "DELETE k" to the log
                                                          │
                                  on startup: replay_log (log.cpp) feeds the log back
                                  through handle_command before the socket opens
```

Design decisions:

- **Storage hides the map and the locking.** The network code only calls
  `set`, `get`, and `remove`, so locking and logging were added without touching
  any socket code.
- **Line framing.** TCP is a byte stream, so each connection has a buffer. A
  command only runs once its `\n` has arrived, and several commands in one read
  are all run, in order.
- **`get` returns a copy.** A reference into the map could be invalidated by
  another thread as soon as the lock is released.
- **The log reuses the protocol.** Log lines are the same text as client commands,
  so replay is just running them through the same parser.
- **Log order matches memory order.** The log is written while the key's shard
  lock is held, so for any one key the log replays in the order it was applied.
- **Crash safety for the log.** A half-written last line is ignored and trimmed
  on startup so the next write doesn't get glued onto it.

## Limitations

- In `pool` mode a worker serves one client until it disconnects, so with N
  workers only N clients are served at once. Thread-per-connection has no such
  limit but creates a thread per client. Fixing this properly means an event
  loop (epoll), which I haven't built.
- The log is flushed to the OS on every write but never `fsync`ed, so it survives
  the server process being killed but not a power cut. It also never shrinks.
- No authentication (it only listens on `127.0.0.1`), no TLS, no key expiration.
- Values can't contain a newline, because the protocol is line based.

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
./build/minikv [port] [single | thread | pool] [pool workers] [lock shards] [log file]
```

- `single`: one client at a time (default)
- `thread`: a new thread for every connection
- `pool`: a fixed pool of worker threads (16 by default). Each worker serves one
  client until it disconnects, so with N workers only N clients are served at
  once and the rest wait.

`lock shards` (default 1) splits the table into that many pieces, each with its
own mutex. A key always maps to the same piece, so threads only wait on each
other when their keys land in the same piece. 1 shard means one global lock.

The server listens on `127.0.0.1` only (there's no authentication). Try it with
`nc 127.0.0.1 7379` and type commands.

## Persistence

Start the server with a log file as the 5th argument to turn it on:

```
./build/minikv 7379 pool 16 64 minikv.log
```

Every `SET` and every `DELETE` that actually removed something is appended to
the file as one line, in the same text format as the protocol. On startup the
server replays the file through the normal command parser before it starts
listening. Without a log file argument, nothing is saved.

What it does and doesn't promise:

- Each write is flushed to the OS, so the data survives the server process being
  killed (I test this with `kill -9`). There is no `fsync`, so a power cut or OS
  crash can lose the most recent writes.
- If the server dies in the middle of a write, the last line can be cut off. On
  startup a line without its `\n` is ignored and trimmed from the file.
- The log only grows. There's no compaction, so overwriting the same key
  repeatedly keeps adding lines.
- Every write takes one extra lock and a `write` call, which costs throughput
  (numbers below).

## Tests

`tests/test_server.py` starts the server and checks replies over a real socket,
including a command split across sends and many commands in one send, and
restarting the server after `kill -9` to check the log (including a torn last line).

To run everything (the socket tests in all three modes plus the stress test):

```
ctest --test-dir build --output-on-failure
```

The same suite runs on Ubuntu in CI (`.github/workflows/ci.yml`), plus a
ThreadSanitizer run of the stress test.

```
python3 tests/test_server.py build/minikv [single | thread | pool]
```

`tests/storage_stress.cpp` hammers `Storage` from 8 threads. I built it with
ThreadSanitizer: it reported a data race before the mutex was added, and
reports nothing with 1, 4 and 64 shards.

```
c++ -std=c++17 -g -fsanitize=thread -Isrc tests/storage_stress.cpp src/storage.cpp -o stress -pthread
./stress 64
```

## Layout

- `src/storage.*`: the key-value table (sharded `unordered_map`s, each with a mutex, behind `set`/`get`/`remove`)
- `src/protocol.*`: turns one command line into a reply
- `src/server.*`: sockets, buffering partial reads, sending replies
- `src/thread_pool.*`: fixed worker threads fed by a queue of client sockets
- `src/log.*`: replays the append-only log on startup
- `src/main.cpp`: accept loop and mode selection
- `bench/`: load generator (`bench.cpp`), run script, and raw results
- `tests/`: socket test script and storage stress test
- `.github/workflows/ci.yml`: Linux CI

## Benchmarks

Machine: Apple M2 Pro (12 cores), macOS 14.6.1, Apple clang 16.0.0, CMake
Release build (`-O2 -Wall -Wextra`, C++17). The server and the load generator
run on the same machine over loopback, so they compete for the same CPU, and
other programs were open. Treat the numbers as rough, not as a clean lab result.

Workload (the same for every number): 80% GET / 20% SET, 10,000 keys picked
uniformly at random, 100-byte values. Each client is a thread with one
connection that sends a request and waits for the reply. Each run is 10
seconds. Raw output for everything is in `bench/results/`.

```
cmake --build build
bench/run.sh my-label "pool:16"      # mode:clients, uses build/bench
```

**Throughput by mode** (global lock, 3 runs each, median requests/sec):

| mode | 1 client | 16 clients |
|---|---|---|
| single (stage 1) | 73,878 | can't test (see below) |
| thread-per-connection | 73,248 | 267,610 |
| pool (16 workers) | 73,856 | 242,940 |

- With 1 client all three modes are the same, as expected (about 13 us per
  round trip, mostly the network stack).
- The single-threaded server can't be benchmarked with many clients: it serves
  one connection until it disconnects, so the other clients just wait.
- Going from 1 to 16 clients gave about 3.3x to 3.6x the throughput.
- Thread-per-connection vs the pool: the runs overlapped a lot (a single
  configuration varied by 20% or more between runs), so I can't say either one
  is faster.

**One global lock vs 64 lock shards** (pool, 16 clients, 5 rounds alternating
between the two so machine drift hits both equally):

| lock | median requests/sec | median p99 latency |
|---|---|---|
| 1 shard (global lock) | 227,742 | 169.0 us |
| 64 shards | 249,210 | 134.2 us |

64 shards had a higher median (about 9%) and won 4 of the 5 rounds, but the
spread between runs is about as big as that gap, so it's a small effect at best.
That makes sense: each operation only holds the lock for a hash-map lookup,
which is tiny next to the cost of the `recv`/`send` system calls, so the global
lock isn't the bottleneck at this scale. I'd expect sharding to matter more
with more cores doing less I/O per request, but I haven't measured that.

**Cost of the append-only log** (pool, 16 clients, 64 shards, 3 rounds
alternating without and with the log, median requests/sec):

| | median requests/sec | median p99 latency |
|---|---|---|
| no log | 232,278 | 139.2 us |
| log (flush on every write) | 205,784 | 255.9 us |

So the log cost about 11% of throughput here, and the p99 got worse. Only 3
rounds, and runs varied by 10% or more, so this is a rough number. I haven't
tried batching writes or calling `fsync`.
