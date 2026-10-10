cmake_minimum_required(VERSION 3.20)
project(pmr_arena LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(pmra INTERFACE)
target_include_directories(pmra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(FetchContent)

option(PMRA_BUILD_TESTS "Build unit tests" ON)
option(PMRA_BUILD_BENCH "Build benchmarks" ON)

if(PMRA_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(pmra_tests tests/test_resources.cpp)
  target_link_libraries(pmra_tests PRIVATE pmra GTest::gtest_main)
  add_test(NAME pmra_tests COMMAND pmra_tests)
endif()

if(PMRA_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(pmra_bench bench/bench_resources.cpp)
  target_link_libraries(pmra_bench PRIVATE pmra benchmark::benchmark)
  target_compile_options(pmra_bench PRIVATE -O2 -fno-omit-frame-pointer)
endif()
