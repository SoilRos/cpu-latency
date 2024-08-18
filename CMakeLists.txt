cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(cpu-latency CXX)

find_package(PkgConfig REQUIRED)
pkg_search_module(hwloc REQUIRED IMPORTED_TARGET hwloc)
find_package(Threads REQUIRED)

add_executable(cpu-latency cpu-latency.cc)
target_compile_features(cpu-latency PRIVATE cxx_std_17)
target_link_libraries(cpu-latency PRIVATE PkgConfig::hwloc Threads::Threads)

file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/cpu-latency-plot ${CMAKE_CURRENT_BINARY_DIR}/cpu-latency-plot)
