// Hammers Storage from several threads at once. Build with ThreadSanitizer to
// catch data races:
//   c++ -std=c++17 -g -fsanitize=thread -Isrc tests/storage_stress.cpp \
//       src/storage.cpp -o storage_stress -pthread && ./storage_stress
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "storage.h"

int main() {
    const int threads = 8;
    const int keys_per_thread = 500;
    const int rounds = 200;

    Storage storage;
    std::vector<std::thread> workers;
    int failures = 0;
    std::vector<int> thread_failures(threads, 0);

    for (int t = 0; t < threads; t++) {
        workers.emplace_back([&, t] {
            // Each thread writes its own keys (so we know what the right
            // answer is), but all threads share the one table, which is what
            // makes unsynchronized access dangerous: inserts can rehash it.
            for (int r = 0; r < rounds; r++) {
                for (int k = 0; k < keys_per_thread; k++) {
                    std::string key = "t" + std::to_string(t) + "_" + std::to_string(k);
                    std::string value = std::to_string(r);
                    storage.set(key, value);
                    auto got = storage.get(key);
                    if (!got || *got != value) {
                        thread_failures[t]++;
                    }
                    if (k % 5 == 0) {
                        storage.remove(key);
                        if (storage.get(key)) {
                            thread_failures[t]++;
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    for (int f : thread_failures) {
        failures += f;
    }
    std::printf("%s (%d wrong answers)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
