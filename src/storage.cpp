#include "storage.h"

void Storage::set(const std::string& key, const std::string& value) {
    map_[key] = value;
}

std::optional<std::string> Storage::get(const std::string& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Storage::remove(const std::string& key) {
    // erase(key) returns how many elements it removed: 1 or 0.
    return map_.erase(key) == 1;
}
