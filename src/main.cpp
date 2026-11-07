#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <thread>

#include "server.h"
#include "storage.h"
#include "thread_pool.h"

static void usage() {
    std::fprintf(stderr, "usage: minikv [port] [single | thread | pool [workers]]\n");
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 7379;
    std::string mode = (argc > 2) ? argv[2] : "single";
    size_t pool_workers = (argc > 3) ? static_cast<size_t>(std::atoi(argv[3])) : 16;
    if (mode != "single" && mode != "thread" && mode != "pool") {
        usage();
        return 1;
    }

    // Writing to a socket the client already closed raises SIGPIPE, which
    // kills the whole process by default. Ignore it: send() then just fails
    // with EPIPE and we drop that one connection.
    std::signal(SIGPIPE, SIG_IGN);

    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) {
        return 1;
    }
    std::printf("minikv listening on 127.0.0.1:%d (mode: %s)\n", port, mode.c_str());
    std::fflush(stdout);

    Storage storage;
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
