# minikv

A multithreaded in-memory key-value store written in C++, accessed over TCP. 
It's a small Redis-style server I'm building to learn how sockets, concurrency, 
and persistence work.

This is a work in progress

## Status

Nothing works yet. I'm starting with a single-threaded server that supports 
SET, GET, and DELETE.

## Plan

I'm building this in stages and measuring requests per second after each one:

1. Single-threaded TCP server
2. Multiple clients (thread-per-connection vs. thread pool, maybe epoll)
3. Thread-safe storage (one global lock vs. finer-grained locking)
4. Persistence with an append-only log

If there's time after that: key expiration.

## Protocol

TODO once the first version is working.

## Building and running

TODO once the first version is working.

## Benchmarks

None yet. I'll add results here after each stage, along with the machine, 
compiler flags, and workload I used.
