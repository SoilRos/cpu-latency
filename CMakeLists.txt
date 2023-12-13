cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(cpu-latency CXX)

find_package(PkgConfig REQUIRED)
pkg_search_module(hwloc REQUIRED IMPORTED_TARGET hwloc)
find_package(Threads REQUIRED)

add_executable(cpu_latency cpu_latency.cc)
target_compile_features(cpu_latency PRIVATE cxx_std_17)
target_link_libraries(cpu_latency PRIVATE PkgConfig::hwloc Threads::Threads)

file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/cpu_latency_plot ${CMAKE_CURRENT_BINARY_DIR}/cpu_latency_plot)
