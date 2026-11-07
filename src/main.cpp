#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>

#include "server.h"
#include "storage.h"

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 7379;

    // Writing to a socket the client already closed raises SIGPIPE, which
    // kills the whole process by default. Ignore it: send() then just fails
    // with EPIPE and we drop that one connection.
    std::signal(SIGPIPE, SIG_IGN);

    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) {
        return 1;
    }
    std::printf("minikv listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);

    Storage storage;
    while (true) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return 1;
        }
        // Stage 1: one client at a time. Others wait in the listen backlog.
        serve_client(client_fd, storage);
    }
}
