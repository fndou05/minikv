#include "log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "protocol.h"

size_t replay_log(const std::string& path, Storage& storage) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;  // first run: no log yet
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // If the server was killed in the middle of a write, the last line is
    // cut off (no '\n'). Replaying it would store a wrong value, so ignore it,
    // and chop it off the file. Otherwise the next append would be glued
    // onto the fragment and corrupt that line too.
    size_t last_newline = data.rfind('\n');
    size_t complete = (last_newline == std::string::npos) ? 0 : last_newline + 1;
    if (complete < data.size()) {
        std::fprintf(stderr, "log: dropping %zu bytes of unfinished last line\n",
                     data.size() - complete);
        std::filesystem::resize_file(path, complete);
    }

    size_t replayed = 0;
    size_t start = 0;
    while (start < complete) {
        size_t newline = data.find('\n', start);
        // Reuse the protocol parser: log lines are exactly client commands.
        std::string reply = handle_command(storage, data.substr(start, newline - start));
        if (reply.compare(0, 5, "ERROR") == 0) {
            std::fprintf(stderr, "log: skipping bad line: %s", reply.c_str());
        } else {
            replayed++;
        }
        start = newline + 1;
    }
    return replayed;
}
