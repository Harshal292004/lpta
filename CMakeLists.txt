cmake_minimum_required(VERSION 3.20)
project(lpta LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LLVM is built without RTTI; consumers must match.
add_compile_options(-fno-rtti)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(lpta
  src/main.cpp
  src/instrumentor.cpp
)

# This LLVM is built as a single dylib (LLVM_LINK_LLVM_DYLIB=ON).
# On distros shipping per-library statics instead, use:
#   llvm_map_components_to_libnames(llvm_libs core support irreader passes
#                                   target targetinfo targetparser mc analysis)
#   target_link_libraries(lpta PRIVATE ${llvm_libs})
target_link_libraries(lpta PRIVATE LLVM)