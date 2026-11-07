#include "thread_pool.h"

#include <thread>

#include "server.h"

ThreadPool::ThreadPool(size_t workers, Storage& storage) : storage_(storage) {
    for (size_t i = 0; i < workers; i++) {
        // detach: workers run until the process exits, nobody joins them.
        std::thread(&ThreadPool::worker_loop, this).detach();
    }
}

void ThreadPool::add_client(int fd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.push(fd);
    }
    has_work_.notify_one();  // wake one sleeping worker
}

void ThreadPool::worker_loop() {
    while (true) {
        int fd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // The lambda re-checks the queue after every wakeup, because a
            // condition variable can wake up spuriously with nothing to do.
            has_work_.wait(lock, [this] { return !clients_.empty(); });
            fd = clients_.front();
            clients_.pop();
        }
        // Lock is released here: serving a client can take a long time and
        // must not block other workers from taking new clients.
        serve_client(fd, storage_);
    }
}
