# minikv

[![ci](https://github.com/fndou05/minikv/actions/workflows/ci.yml/badge.svg)](https://github.com/fndou05/minikv/actions/workflows/ci.yml)

A multithreaded in-memory key-value store written in C++17, accessed over TCP.
It's a small Redis-style server I built to learn how sockets, concurrency, and
persistence work. Only the standard library and POSIX are used.

## Status

Version 1.1.0. All four stages are built, tested, and benchmarked:

1. Single-threaded TCP server (SET / GET / DELETE)
2. Many clients at once: thread-per-connection, a fixed thread pool, or an
   event loop (epoll on Linux, kqueue on macOS)
3. Thread-safe storage: a mutex per shard (1 shard = one global lock)
4. Persistence: an append-only log that is replayed on startup

Tested on Linux (Ubuntu, with both g++ and clang++) by GitHub Actions on every
push, and on macOS (Apple Silicon) by hand. The benchmark numbers below are from
the Mac only; I haven't benchmarked on Linux.

Not built: key expiration, group commit, and log compaction (see "Limitations").

## How it works

```
 client ──TCP──▶ accept loop (main.cpp)
                   │  single: inline │ thread: new thread │ pool: queue ─▶ workers │ epoll: event loops
                   ▼
   serve_client (server.cpp)  or  event loops (event_loop.cpp)
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
  limit but creates a thread per client. `epoll` mode fixes both: a few threads
  watch all the sockets. Its weak spot is that a loop thread does the storage
  work itself, so a slow `fsync` stalls every client on that loop.
- `epoll` mode is level-triggered and has no timeouts: a client that connects
  and never reads its replies keeps its connection (and buffered replies) until
  it disconnects.
- Every `fsync` waits for the disk while holding the log lock, so durable writes
  are serialized. Real databases batch several writes into one `fsync` (group
  commit); I haven't. The log also never shrinks.
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
./build/minikv [port] [single | thread | pool | epoll] [pool workers or event loops] [lock shards] [log file] [fsync]
```

- `single`: one client at a time (default)
- `thread`: a new thread for every connection
- `pool`: a fixed pool of worker threads (16 by default). Each worker serves one
  client until it disconnects, so with N workers only N clients are served at
  once and the rest wait.
- `epoll`: a few event-loop threads (4 by default, set with the 3rd argument),
  each watching many sockets at once with epoll on Linux or kqueue on macOS
  (macOS has no epoll). Sockets are non-blocking; a thread only touches a
  socket when the OS says it's ready, so thousands of idle clients are cheap
  and there is no limit of N clients at once. Replies that don't fit in the
  socket's send buffer are kept and sent later, and a client that isn't reading
  its replies is paused instead of making the server buffer without limit.

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

- By default each write is flushed to the OS, so the data survives the server
  process being killed (I test this with `kill -9`). A power cut or OS crash can
  still lose the most recent writes.
- Add `fsync` as the 6th argument (`./build/minikv 7379 pool 16 64 minikv.log fsync`)
  to also call `fsync` after every write, so a change is on disk before the
  client gets `OK`. This is much slower (numbers below). I can't test a real
  power cut, so the tests only check that the option works and recovery is
  unchanged.
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
python3 tests/test_server.py build/minikv [single | thread | pool | epoll]
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
- `src/event_loop.*`: epoll/kqueue event loops with non-blocking sockets
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

### More measurements

Same machine and build as above. Raw output is in `bench/results/`.

**Scaling with the number of clients** (thread-per-connection, 64 shards, same
80/20 workload, 3 runs each, median):

| clients | requests/sec | p50 latency | p99 latency |
|---|---|---|---|
| 1 | 73,810 | 13.1 us | 21.2 us |
| 2 | 122,312 | 15.9 us | 27.1 us |
| 4 | 183,558 | 16.6 us | 44.0 us |
| 8 | 236,011 | 25.5 us | 70.6 us |
| 16 | 259,817 | 47.9 us | 128.1 us |
| 32 | 206,147 | 179.5 us | 494.5 us |
| 64 | 181,532 | 387.8 us | 573.8 us |

Throughput rises to a peak at 16 clients (3.5x the single-client rate) and then
drops while latency climbs. The machine has 12 cores and the load generator runs
on it too, so past about 16 clients the server threads and the client threads
are fighting for cores. I believe that's the cause, but I didn't test it
separately (for example by pinning the two sides to different cores).

**What durability costs** (pool, 16 clients, 64 shards, 3 rounds alternating the
variants, median of each):

| log | requests/sec | p99 latency |
|---|---|---|
| none | 205,929 | 140 us |
| flush to OS (default) | 188,751 | 247 us |
| flush + `fsync()` | 156,683 | 646 us |
| flush + `F_FULLFSYNC` (one-off, see below) | 2,839 | 35,953 us |

On macOS, `fsync()` only pushes data to the drive's own cache, which is why it
looks cheap. Forcing the drive to really write it (`F_FULLFSYNC`) dropped
throughput by about 70x. I measured that with a throwaway build, not the
committed code (`bench/results/fullfsync-macos.txt`), and it's a single set of
3 runs. The takeaway is that "survives a power cut" is expensive, and that's why
databases batch writes before syncing. The no-log runs varied from 189k to 235k
between rounds, so differences under about 10% in this table aren't meaningful.

**Restart time:** replaying a 53 MB log of 467,306 commands (10,000 distinct
keys, lots of overwrites) took 306 to 326 ms over 3 runs.

**The pool limit, demonstrated** (`python3 bench/pool_limit.py build/minikv`):
with a 4-worker pool and 8 clients connected at once, 4 clients got replies
within 2 seconds and 4 did not. Once the first 4 disconnected, the other 4 were
served. That's the blocking-I/O limit described under Limitations.

**Event loop vs thread-per-connection** (`epoll` mode here means 4 kqueue loops
on this Mac; 64 lock shards; medians of 3 runs of 10 s; raw output in
`bench/results/epoll-vs-thread.txt`):

| Clients | epoll (4 loops) req/s | thread req/s |
|---|---|---|
| 16 | 252,164 | 235,297 |
| 64 | 184,075 | 201,553 |
| 256 | 172,240 | 196,487 |

The event loop is not faster. At 16 clients the two overlap, and at 256 the
thread-per-connection server was about 14% ahead in all three runs. I didn't
investigate why (the load generator shares the same 12 cores, and I haven't
profiled). What the event loop buys is the behaviour, not speed: 4 threads serve
any number of clients, where the pool serves only N at once and
thread-per-connection needs one thread per client. Linux epoll itself is only
exercised by the tests in CI, not benchmarked.

**The pool limit vs the event loop** (`python3 bench/pool_limit.py build/minikv
epoll`): with 4 threads and 8 clients connected at once, the pool answered 4 of
8 within 2 seconds, and the event loop answered all 8 (`bench/results/epoll-limit.txt`).
