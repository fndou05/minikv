#include "storage.h"

void Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);  // released automatically at the end of the function
    map_[key] = value;
}

std::optional<std::string> Storage::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    // Returns a COPY of the value. Returning a reference into the map would
    // be unsafe: another thread could erase or overwrite it after we unlock.
    return it->second;
}

bool Storage::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    // erase(key) returns how many elements it removed: 1 or 0.
    return map_.erase(key) == 1;
}
