#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

#include "storage.h"

// A fixed number of worker threads created once at startup. The accept loop
// pushes each new client fd into a queue, and an idle worker takes it and
// serves that client until it disconnects.
//
// Limit: a worker is busy for the whole life of its client (blocking I/O),
// so with N workers only N clients are served at once; extra clients wait
// in the queue until one disconnects.
class ThreadPool {
public:
    ThreadPool(size_t workers, Storage& storage);

    // Called by the accept loop.
    void add_client(int fd);

private:
    void worker_loop();

    Storage& storage_;
    std::mutex mutex_;
    std::condition_variable has_work_;
    std::queue<int> clients_;
};
