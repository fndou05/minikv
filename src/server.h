#pragma once

#include "storage.h"

// Longest command line we accept, in bytes (not counting the '\n').
const size_t MAX_LINE = 64 * 1024;

// Create a listening TCP socket on 127.0.0.1:port. Returns the fd, or -1.
int make_listen_socket(int port);

// Read commands from one client until it disconnects, reply to each one,
// then close the fd.
void serve_client(int fd, Storage& storage);
