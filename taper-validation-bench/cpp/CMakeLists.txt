cmake_minimum_required(VERSION 3.14)
project(taper-validation-bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=armv8-a+crc")
endif()

include(FetchContent)
FetchContent_Declare(benchmark GIT_REPOSITORY https://github.com/google/benchmark.git GIT_TAG v1.8.3)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

FetchContent_Declare(xxhash GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git GIT_TAG v0.8.2)
FetchContent_MakeAvailable(xxhash)

add_executable(taper_validation src/taper_validation.cpp)
target_include_directories(taper_validation PRIVATE vendor/taper/include ${xxhash_SOURCE_DIR})
target_link_libraries(taper_validation PRIVATE benchmark::benchmark)
