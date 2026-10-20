cmake_minimum_required(VERSION 3.20)
project(LPTA VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LPTA_BUILD_TESTS "Build LPTA unit and integration tests" ON)

# LPTA is a pass plugin.  It must be built against the exact LLVM installation
# used by the compiler that loads it.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "LPTA: using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

if(LLVM_PACKAGE_VERSION VERSION_LESS 22)
  message(FATAL_ERROR "LPTA targets LLVM 22 or newer (new pass manager).")
endif()

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

# LLVM is normally built without RTTI.  Keep the plugin consistent even on
# distributions that enable RTTI for LLVM itself.
add_compile_options(-fno-rtti -Wall -Wextra -Wno-unused-parameter)

# LLVM >= 23 ships an imported "LLVM" dylib target.  Fall back to component
# libraries for older installations.
if(TARGET LLVM)
  set(LPTA_LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(LPTA_LLVM_LIBS
    Core Support IRReader Analysis Passes TransformUtils Demangle AsmParser)
endif()

add_library(lpta_analysis STATIC
  src/Metrics.cpp
  src/Diff.cpp
  src/Report.cpp
  src/Trace.cpp
)
target_include_directories(lpta_analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lpta_analysis PUBLIC ${LPTA_LLVM_LIBS})
set_target_properties(lpta_analysis PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(LPTA MODULE
  src/Plugin.cpp
  src/Collector.cpp
  src/Options.cpp
)
target_link_libraries(LPTA PRIVATE lpta_analysis)
target_include_directories(LPTA PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(LPTA PROPERTIES
  PREFIX ""
  OUTPUT_NAME "LPTA"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_executable(lpta-report tools/lpta-report.cpp)
target_link_libraries(lpta-report PRIVATE lpta_analysis)
set_target_properties(lpta-report PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS LPTA lpta-report
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(PROGRAMS scripts/lpta DESTINATION bin)

if(LPTA_BUILD_TESTS)
  enable_testing()

  add_executable(lpta-metrics-test tests/unit/metrics_test.cpp)
  target_link_libraries(lpta-metrics-test PRIVATE lpta_analysis)
  set_target_properties(lpta-metrics-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )

  add_test(NAME lpta-metrics COMMAND lpta-metrics-test)
  add_test(NAME lpta-integration
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/lpta/run.sh
            $<TARGET_FILE:LPTA>
            $<TARGET_FILE:lpta-report>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  set_tests_properties(lpta-integration PROPERTIES TIMEOUT 180)
endif()
