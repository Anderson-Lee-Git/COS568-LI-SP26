#!/bin/bash
# Create a minimal CMakeLists.txt

set -e  # Exit on error

# Backup the original CMakeLists.txt if not already backed up
if [ ! -f CMakeLists.txt.original ]; then
    echo "Backing up original CMakeLists.txt..."
    cp CMakeLists.txt CMakeLists.txt.original
    echo "Backup created: CMakeLists.txt.original"
else
    echo "Original CMakeLists.txt already backed up."
fi

# Create a new minimal CMakeLists.txt
echo "Creating minimal CMakeLists.txt..."
cat > CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.10)
project(WOSD)

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

option(TLI_ENABLE_MARCH_NATIVE "Build with -march=native" ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffast-math -Wall -Wfatal-errors")
if (TLI_ENABLE_MARCH_NATIVE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()

# Enable OpenMP if available
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fopenmp HAS_OPENMP)
if (HAS_OPENMP)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")
endif()

set(CMAKE_CXX_STANDARD 17)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS chrono)

add_subdirectory(dtl)

if (${APPLE})
    include_directories(/usr/local/include/) # required by Mac OS to find boost
endif ()

set(SOURCE_FILES util.h)

# We only include the indexes used by the final mixed-workload benchmark.
set(BENCH_SOURCES
    "benchmarks/benchmark_dynamic_pgm.cc"
    "benchmarks/benchmark_pgm.cc"
    "benchmarks/benchmark_lipp.cc"
    "benchmarks/benchmark_btree.cc"
    "benchmarks/benchmark_hybrid_pgm_lipp.cc"
    "benchmarks/benchmark_async_hybrid_pgm_lipp.cc"
    "benchmarks/benchmark_bloom_hybrid_pgm_lipp.cc"
    "benchmarks/benchmark_bloom_async_hybrid_pgm_lipp.cc")

file(GLOB_RECURSE SEARCH_SOURCES "searches/*.h" "searches/search.cpp")

add_executable(benchmark benchmark.cc ${SOURCE_FILES} ${BENCH_SOURCES} ${SEARCH_SOURCES})
add_executable(generate generate.cc ${SOURCE_FILES})

target_compile_definitions(benchmark PRIVATE NDEBUGGING)

target_include_directories(benchmark
        PRIVATE "competitors/PGM-index/include"  # For PGM
        PRIVATE "competitors/stx-btree-0.9/include"  # For B+Tree
        PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(benchmark
        PRIVATE Threads::Threads dtl
        PRIVATE ${Boost_LIBRARIES})
        
target_link_libraries(benchmark
        PRIVATE ${CMAKE_THREAD_LIBS_INIT})

target_link_libraries(benchmark
        PRIVATE dl)

target_include_directories(generate PRIVATE competitors/finedex/include)
EOF

echo "Minimal CMakeLists.txt created!"
