#include "storage.h"

#include <functional>
#include <unistd.h>  // fsync

Storage::Storage(size_t shards) : shards_(shards == 0 ? 1 : shards) {}

Storage::~Storage() {
    if (log_) {
        std::fclose(log_);
    }
}

Storage::Shard& Storage::shard_for(const std::string& key) const {
    return shards_[std::hash<std::string>{}(key) % shards_.size()];
}

void Storage::set(const std::string& key, const std::string& value) {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);  // released automatically at the end of the function
    shard.map[key] = value;
    append_log("SET " + key + " " + value);
}

std::optional<std::string> Storage::get(const std::string& key) const {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return std::nullopt;
    }
    // Returns a COPY of the value. Returning a reference into the map would
    // be unsafe: another thread could erase or overwrite it after we unlock.
    return it->second;
}

bool Storage::remove(const std::string& key) {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    // erase(key) returns how many elements it removed: 1 or 0.
    bool existed = shard.map.erase(key) == 1;
    if (existed) {
        append_log("DELETE " + key);  // deleting a missing key changes nothing, so don't log it
    }
    return existed;
}

bool Storage::enable_log(const std::string& path, bool sync) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_ = std::fopen(path.c_str(), "ab");  // "a": every write goes to the end of the file
    log_sync_ = sync;
    return log_ != nullptr;
}

void Storage::append_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!log_) {
        return;
    }
    std::fwrite(line.data(), 1, line.size(), log_);
    std::fputc('\n', log_);
    // Hand the bytes to the OS right away, so a crash of this process doesn't
    // lose them.
    std::fflush(log_);
    if (log_sync_) {
        // Ask the OS to push them to the disk itself, so even a power cut
        // can't lose them. Much slower: this waits for the disk. (On macOS,
        // fsync only reaches the drive's cache; F_FULLFSYNC goes further.)
        fsync(fileno(log_));
    }
}
