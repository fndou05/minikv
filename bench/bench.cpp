// Load generator. Keep this IDENTICAL across stages so results are comparable.
//
// usage: bench <port> <clients> <seconds>
//
// Workload (fixed): 80% GET / 20% SET, 10,000 keys chosen uniformly at random,
// keys like "key_1234", 100-byte values. Each client is one thread with one
// connection that sends a request, waits for the reply, then sends the next.
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static const int KEY_COUNT = 10000;
static const int VALUE_SIZE = 100;
static const int GET_PERCENT = 80;

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        std::exit(1);
    }
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return fd;
}

// Send one request line and wait for the one-line reply.
// Returns false if the connection broke.
static bool roundtrip(int fd, const std::string& request, std::string& reply) {
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    reply.clear();
    char buf[1024];
    while (reply.empty() || reply.back() != '\n') {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        reply.append(buf, static_cast<size_t>(n));
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: bench <port> <clients> <seconds>\n");
        return 1;
    }
    int port = std::atoi(argv[1]);
    int clients = std::atoi(argv[2]);
    int seconds = std::atoi(argv[3]);
    std::string value(VALUE_SIZE, 'v');

    // Fill every key once (not timed), so GETs find something.
    {
        int fd = connect_to(port);
        std::string reply;
        for (int k = 0; k < KEY_COUNT; k++) {
            roundtrip(fd, "SET key_" + std::to_string(k) + " " + value + "\n", reply);
        }
        close(fd);
    }

    using Clock = std::chrono::steady_clock;
    std::atomic<bool> go{false};
    std::atomic<int> errors{0};
    std::vector<std::vector<long long>> latencies_ns(clients);  // per thread: no sharing
    std::vector<std::thread> threads;

    // Connect all clients first, then release them together.
    std::vector<int> fds(clients);
    for (int i = 0; i < clients; i++) {
        fds[i] = connect_to(port);
    }
    std::atomic<long long> deadline_ns{0};

    for (int i = 0; i < clients; i++) {
        threads.emplace_back([&, i] {
            std::mt19937 rng(1234 + i);  // fixed seeds: same workload every run
            std::string request, reply;
            latencies_ns[i].reserve(1 << 20);
            while (!go) {
                std::this_thread::yield();
            }
            while (Clock::now().time_since_epoch().count() < deadline_ns) {
                int key = static_cast<int>(rng() % KEY_COUNT);
                bool is_get = static_cast<int>(rng() % 100) < GET_PERCENT;
                request = is_get ? "GET key_" + std::to_string(key) + "\n"
                                 : "SET key_" + std::to_string(key) + " " + value + "\n";
                auto t0 = Clock::now();
                if (!roundtrip(fds[i], request, reply)) {
                    errors++;
                    return;
                }
                auto t1 = Clock::now();
                latencies_ns[i].push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
        });
    }

    auto start = Clock::now();
    deadline_ns = (start + std::chrono::seconds(seconds)).time_since_epoch().count();
    go = true;
    for (auto& t : threads) {
        t.join();
    }
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<long long> all;
    for (auto& v : latencies_ns) {
        all.insert(all.end(), v.begin(), v.end());
    }
    std::sort(all.begin(), all.end());
    if (all.empty()) {
        std::fprintf(stderr, "no requests completed\n");
        return 1;
    }
    auto pct = [&](double p) { return all[static_cast<size_t>(p * (all.size() - 1))] / 1000.0; };

    std::printf("clients=%d requests=%zu errors=%d seconds=%.2f\n", clients, all.size(),
                errors.load(), elapsed);
    std::printf("requests/sec=%.0f  latency_us p50=%.1f p99=%.1f max=%.1f\n", all.size() / elapsed,
                pct(0.50), pct(0.99), all.back() / 1000.0);
    return errors ? 1 : 0;
}
