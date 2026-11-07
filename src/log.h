#pragma once

#include <string>

#include "storage.h"

// Rebuild the table from an append-only log file by running each line as a
// command. Does nothing if the file doesn't exist yet. Must be called BEFORE
// Storage::enable_log, otherwise the replayed commands would be logged again.
// Returns how many commands were replayed.
size_t replay_log(const std::string& path, Storage& storage);
