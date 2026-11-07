#!/bin/bash
# Runs the benchmark the same way every time.
# usage: bench/run.sh <label> "<mode:clients> <mode:clients> ..." [server args after mode]
# e.g.   bench/run.sh global-lock "single:1 thread:1 thread:16 pool:1 pool:16"
#
# Set SHARDS=n to choose the number of lock shards (default 1 = one global lock).
# Each configuration gets a fresh server and 3 runs of 10 seconds.
# The pool always has 16 workers so that 16 clients all get served at once.
set -u
cd "$(dirname "$0")/.."
LABEL=$1
CONFIGS=$2
PORT=7500
SECONDS_PER_RUN=10
RUNS=3

echo "# $LABEL  $(date '+%Y-%m-%d %H:%M')  commit $(git rev-parse --short HEAD)"
echo "# $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sysctl -n hw.ncpu 2>/dev/null || nproc) cores, $(uname -sr)"
echo "# $(c++ --version | head -1); built with CMake Release: -O2 -Wall -Wextra, C++17"

for config in $CONFIGS; do
    mode=${config%%:*}
    clients=${config##*:}
    ./build/minikv $PORT $mode 16 ${SHARDS:-1} >/dev/null &
    server=$!
    sleep 0.5
    for run in $(seq $RUNS); do
        echo "## mode=$mode clients=$clients run=$run"
        ./build/bench $PORT $clients $SECONDS_PER_RUN
    done
    kill $server
    wait $server 2>/dev/null
    sleep 1
done
