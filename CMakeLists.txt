cmake_minimum_required(VERSION 3.16)
project(minikv VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release so benchmark numbers always come from an optimized build.
# For debugging: cmake -DCMAKE_BUILD_TYPE=Debug ..
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Release gets exactly -O2 (CMake's default would be -O3 -DNDEBUG).
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(Threads REQUIRED)

add_executable(minikv
  src/main.cpp src/storage.cpp src/protocol.cpp src/server.cpp src/thread_pool.cpp)
target_compile_options(minikv PRIVATE -Wall -Wextra)
target_link_libraries(minikv PRIVATE Threads::Threads)
