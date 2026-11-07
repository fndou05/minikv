#include "server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // Without this, restarting the server right after stopping it fails with
    // "Address already in use" while old connections sit in TIME_WAIT.
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // Loopback only: the store has no authentication, so don't expose it.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

// send() may accept fewer bytes than we asked for (a "short write"), so keep
// calling it until the whole reply is out. Returns false if the connection broke.
static bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal, just retry
            }
            return false;  // e.g. EPIPE: client is gone (SIGPIPE is ignored in main)
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void serve_client(int fd, Storage& storage) {
    // Replies are small and the client waits for each one, so don't let the
    // kernel delay them to batch with later data.
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    // TCP is a byte stream: one recv() may hold half a command, or several
    // commands. So received bytes pile up in `pending`, and we only run a
    // command once its '\n' has arrived.
    std::string pending;
    char chunk[4096];

    while (true) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
            break;  // client closed the connection normally
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");  // a real error, unlike n == 0
            break;
        }
        pending.append(chunk, static_cast<size_t>(n));

        // Run every complete line currently in the buffer.
        size_t start = 0;
        size_t newline;
        bool too_long = false;
        while ((newline = pending.find('\n', start)) != std::string::npos) {
            if (newline - start > MAX_LINE) {
                too_long = true;
                break;
            }
            std::string reply = handle_command(storage, pending.substr(start, newline - start));
            if (!send_all(fd, reply)) {
                close(fd);
                return;
            }
            start = newline + 1;
        }
        pending.erase(0, start);  // keep only the unfinished tail

        // The leftover has no '\n' yet. If it is already too big, the client
        // is either broken or hostile, and we can't buffer forever.
        if (too_long || pending.size() > MAX_LINE) {
            send_all(fd, "ERROR line too long\n");
            break;
        }
    }
    close(fd);
}
