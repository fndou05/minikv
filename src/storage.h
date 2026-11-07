#pragma once

#include <cstdio>
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
    ~Storage();  // closes the log file

    // Store the value. If the key already exists, overwrite it.
    void set(const std::string& key, const std::string& value);

    // Return the value, or std::nullopt if the key doesn't exist.
    // optional is used (not "") so a stored empty value and a missing key
    // can be told apart.
    std::optional<std::string> get(const std::string& key) const;

    // Delete the key. Returns true if it existed, false if it didn't.
    bool remove(const std::string& key);

    // From now on, append every change to this file (the append-only log).
    // Call it AFTER replaying the old log, and before serving clients.
    // Returns false if the file can't be opened.
    // sync = true also calls fsync after every write, so each change is on
    // disk before the client gets its reply (slower). With sync = false the
    // bytes only reach the OS, which survives this process dying but not a
    // power cut.
    bool enable_log(const std::string& path, bool sync = false);

private:
    // Write one line to the log, if logging is on. Called while the key's
    // shard lock is held, so for any one key the log order matches the order
    // the changes were applied in memory.
    void append_log(const std::string& line);

    struct Shard {
        mutable std::mutex mutex;  // mutable so const get() can lock
        std::unordered_map<std::string, std::string> map;
    };

    // Pick the shard that owns this key.
    Shard& shard_for(const std::string& key) const;

    // mutable: shard_for() is const (get() uses it) but returns a Shard& that
    // callers modify.
    mutable std::vector<Shard> shards_;

    // One log file shared by all shards, so writing to it needs its own lock.
    std::mutex log_mutex_;
    std::FILE* log_ = nullptr;  // nullptr = logging is off
    bool log_sync_ = false;
};
