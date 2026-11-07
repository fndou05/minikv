#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The key-value table. The networking code only talks to this class and never
// touches the map directly, so we could add locking and logging here without
// changing any socket code.
//
// Locking: the table is split into `shards` independent maps, each with its
// own mutex. A key always goes to the same shard (by hash), so two threads
// only wait on each other if their keys land in the same shard.
// shards = 1 means a single global lock.
class Storage {
public:
    explicit Storage(size_t shards = 1);

    // Store the value. If the key already exists, overwrite it.
    void set(const std::string& key, const std::string& value);

    // Return the value, or std::nullopt if the key doesn't exist.
    // optional is used (not "") so a stored empty value and a missing key
    // can be told apart.
    std::optional<std::string> get(const std::string& key) const;

    // Delete the key. Returns true if it existed, false if it didn't.
    bool remove(const std::string& key);

private:
    struct Shard {
        mutable std::mutex mutex;  // mutable so const get() can lock
        std::unordered_map<std::string, std::string> map;
    };

    // Pick the shard that owns this key.
    Shard& shard_for(const std::string& key) const;

    // mutable: shard_for() is const (get() uses it) but returns a Shard& that
    // callers modify.
    mutable std::vector<Shard> shards_;
};
