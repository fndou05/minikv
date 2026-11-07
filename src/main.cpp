#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <thread>

#include "log.h"
#include "server.h"
#include "storage.h"
#include "thread_pool.h"

static void usage() {
    std::fprintf(stderr,
                 "usage: minikv [port] [single | thread | pool] [pool workers] [lock shards] [log file]\n");
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 7379;
    std::string mode = (argc > 2) ? argv[2] : "single";
    size_t pool_workers = (argc > 3) ? static_cast<size_t>(std::atoi(argv[3])) : 16;
    size_t shards = (argc > 4) ? static_cast<size_t>(std::atoi(argv[4])) : 1;
    std::string log_path = (argc > 5) ? argv[5] : "";  // empty = no persistence
    if (mode != "single" && mode != "thread" && mode != "pool") {
        usage();
        return 1;
    }

    // Writing to a socket the client already closed raises SIGPIPE, which
    // kills the whole process by default. Ignore it: send() then just fails
    // with EPIPE and we drop that one connection.
    std::signal(SIGPIPE, SIG_IGN);

    Storage storage(shards);
    if (!log_path.empty()) {
        // Order matters: replay first (not logged), then start logging.
        size_t replayed = replay_log(log_path, storage);
        if (!storage.enable_log(log_path)) {
            std::fprintf(stderr, "cannot open log file %s\n", log_path.c_str());
            return 1;
        }
        std::printf("log: %s (replayed %zu commands)\n", log_path.c_str(), replayed);
    }

    // Only start listening once the old data is loaded, so no client can see
    // a half-restored table.
    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) {
        return 1;
    }
    std::printf("minikv listening on 127.0.0.1:%d (mode: %s, lock shards: %zu)\n", port,
                mode.c_str(), shards);
    std::fflush(stdout);

    ThreadPool pool(mode == "pool" ? pool_workers : 0, storage);

    while (true) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return 1;
        }

        if (mode == "single") {
            // One client at a time. Others wait in the listen backlog.
            serve_client(client_fd, storage);
        } else if (mode == "thread") {
            // A new thread per connection; it cleans itself up when the
            // client disconnects.
            std::thread(serve_client, client_fd, std::ref(storage)).detach();
        } else {
            pool.add_client(client_fd);
        }
    }
}
