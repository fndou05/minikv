#include "storage.h"

#include <functional>

Storage::Storage(size_t shards) : shards_(shards == 0 ? 1 : shards) {}

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

bool Storage::enable_log(const std::string& path) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_.open(path, std::ios::app | std::ios::binary);
    return log_.is_open();
}

void Storage::append_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!log_.is_open()) {
        return;
    }
    log_ << line << '\n';
    // Hand the bytes to the OS right away, so a crash of this process doesn't
    // lose them. (No fsync, so a power cut or OS crash still can.)
    log_.flush();
}
