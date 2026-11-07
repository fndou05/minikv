#pragma once

#include <optional>
#include <string>
#include <unordered_map>

// The key-value table. The networking code only talks to this class and never
// touches the map directly, so later we can add locking (stage 3) and
// logging (stage 4) here without changing any socket code.
class Storage {
public:
    // Store the value. If the key already exists, overwrite it.
    void set(const std::string& key, const std::string& value);

    // Return the value, or std::nullopt if the key doesn't exist.
    // optional is used (not "") so a stored empty value and a missing key
    // can be told apart.
    std::optional<std::string> get(const std::string& key) const;

    // Delete the key. Returns true if it existed, false if it didn't.
    bool remove(const std::string& key);

private:
    std::unordered_map<std::string, std::string> map_;
};
