#pragma once

#include <cstddef>
#include <vector>

#include "storage.h"

// Event-loop server: a few threads, each watching MANY client sockets at once
// with epoll (Linux) or kqueue (macOS, which has no epoll). A thread only
// does work for a socket when the OS says that socket is ready, so a client
// that is just sitting there costs one table entry instead of one whole thread.
// This removes the thread pool's limit of "only N clients served at once".
class EventLoops {
public:
    // `loops` threads, each with its own epoll/kqueue instance.
    EventLoops(size_t loops, Storage& storage);

    // Hand a freshly accepted socket to one of the loops (round robin).
    // Call from the accept thread only.
    void add_client(int fd);

    // "epoll" or "kqueue", depending on the OS this was built on.
    static const char* backend();

private:
    struct Loop;
    // The loops run on detached threads until the process exits, so these are
    // intentionally never freed (freeing them would race with those threads).
    std::vector<Loop*> loops_;
    size_t next_ = 0;
};
