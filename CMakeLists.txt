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
  src/main.cpp src/storage.cpp src/protocol.cpp src/server.cpp src/thread_pool.cpp src/log.cpp)
target_compile_options(minikv PRIVATE -Wall -Wextra)
target_link_libraries(minikv PRIVATE Threads::Threads)

# Tests: run them all with `ctest --output-on-failure` from the build folder.
# They share one TCP port, so ctest must run them one at a time (its default).
enable_testing()

add_executable(storage_stress tests/storage_stress.cpp src/storage.cpp)
target_compile_options(storage_stress PRIVATE -Wall -Wextra)
target_include_directories(storage_stress PRIVATE src)
target_link_libraries(storage_stress PRIVATE Threads::Threads)

foreach(mode single thread pool)
  add_test(NAME server_${mode}
           COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/test_server.py $<TARGET_FILE:minikv> ${mode})
endforeach()
add_test(NAME storage_stress_1_shard COMMAND storage_stress 1)
add_test(NAME storage_stress_64_shards COMMAND storage_stress 64)

# Load generator used for the benchmark numbers in the README.
add_executable(bench bench/bench.cpp)
target_compile_options(bench PRIVATE -Wall -Wextra)
target_link_libraries(bench PRIVATE Threads::Threads)
