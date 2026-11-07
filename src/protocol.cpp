#include "protocol.h"

std::string handle_command(Storage& storage, std::string line) {
    // telnet and some clients end lines with "\r\n"; drop the '\r'.
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    // Split at the first space: "SET a b c" -> cmd "SET", rest "a b c".
    size_t space = line.find(' ');
    std::string cmd = line.substr(0, space);  // npos means "to the end"
    std::string rest = (space == std::string::npos) ? "" : line.substr(space + 1);

    if (cmd == "SET") {
        // Split again: key is up to the next space, value is EVERYTHING after
        // it, which is why values can contain spaces but keys can't.
        size_t key_end = rest.find(' ');
        if (key_end == std::string::npos || key_end == 0) {
            return "ERROR usage: SET <key> <value>\n";
        }
        storage.set(rest.substr(0, key_end), rest.substr(key_end + 1));
        return "OK\n";
    }

    if (cmd == "GET" || cmd == "DELETE") {
        // The key is the whole rest, so a space in it means a malformed command.
        if (rest.empty() || rest.find(' ') != std::string::npos) {
            return "ERROR usage: " + cmd + " <key>\n";
        }
        if (cmd == "GET") {
            auto value = storage.get(rest);
            return value ? *value + "\n" : "NOT_FOUND\n";
        }
        return storage.remove(rest) ? "OK\n" : "NOT_FOUND\n";
    }

    return "ERROR unknown command\n";
}
