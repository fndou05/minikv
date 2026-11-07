#pragma once

#include <string>

#include "storage.h"

// Takes one complete command line (without its trailing '\n'), runs it
// against the storage, and returns the full reply including the final '\n'.
// Knows nothing about sockets, so it can be tested on its own.
std::string handle_command(Storage& storage, std::string line);
