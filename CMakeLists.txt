cmake_minimum_required(VERSION 3.29)
project(faultline VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FAULTLINE_WERROR "Treat compiler warnings as errors in our own targets" ON)
# Set by the asan/tsan presets, e.g. "address,undefined" or "thread".
set(FAULTLINE_SANITIZE "" CACHE STRING "Value passed to -fsanitize= (empty = off)")

if(FAULTLINE_SANITIZE)
  # Applied globally on purpose: GoogleTest must be built with the same
  # sanitizer runtime as our code, otherwise ASan/TSan report false positives.
  add_compile_options(-fsanitize=${FAULTLINE_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${FAULTLINE_SANITIZE})
endif()

# g++ 11's ThreadSanitizer aborts with "unexpected memory mapping" on newer
# kernels that use more address randomization bits. Running the test programs
# with address randomization switched off (setarch -R, no root needed) avoids
# it. Child processes inherit the setting, so the Python tests are covered too.
set(FAULTLINE_TEST_LAUNCHER "")
if(FAULTLINE_SANITIZE MATCHES "thread")
  set(FAULTLINE_TEST_LAUNCHER setarch ${CMAKE_SYSTEM_PROCESSOR} -R)
endif()

# Warning flags live in a function so they apply to our targets only, not to
# third-party code fetched below.
function(faultline_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(FAULTLINE_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

add_library(faultline_core
  src/netlist.cpp
  src/bench_parser.cpp
  src/levelize.cpp
  src/scalar_simulator.cpp
  src/vectors.cpp
  src/bit_parallel_simulator.cpp
  src/fault_simulator.cpp
  src/thread_pool.cpp
  src/parallel_fault_simulator.cpp
  src/socket.cpp
  src/protocol.cpp
  src/server.cpp
)
target_include_directories(faultline_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(faultline_core PUBLIC Threads::Threads)
faultline_warnings(faultline_core)

add_executable(faultline apps/faultline_cli.cpp)
target_link_libraries(faultline PRIVATE faultline_core)
faultline_warnings(faultline)

add_executable(faultline_server apps/faultline_server.cpp)
target_link_libraries(faultline_server PRIVATE faultline_core)
faultline_warnings(faultline_server)

add_executable(faultline_bench apps/faultline_bench.cpp)
target_link_libraries(faultline_bench PRIVATE faultline_core)
faultline_warnings(faultline_bench)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  # Pinned by URL + SHA256 so a changed or tampered archive fails the build.
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(faultline_tests
    tests/test_bench_parser.cpp
    tests/test_levelize.cpp
    tests/test_scalar_simulator.cpp
    tests/test_bit_parallel_simulator.cpp
    tests/test_fault_simulator.cpp
    tests/test_bounded_queue.cpp
    tests/test_thread_pool.cpp
    tests/test_parallel_fault_simulator.cpp
    tests/test_socket.cpp
    tests/test_protocol.cpp
  )
  target_link_libraries(faultline_tests PRIVATE faultline_core GTest::gtest_main)
  target_compile_definitions(faultline_tests PRIVATE
    FAULTLINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  faultline_warnings(faultline_tests)
  # Used by gtest_discover_tests both to list and to run the tests.
  set_target_properties(faultline_tests PROPERTIES TEST_LAUNCHER "${FAULTLINE_TEST_LAUNCHER}")

  include(GoogleTest)
  gtest_discover_tests(faultline_tests)

  find_package(Python3 3.10 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME python_unittests
      COMMAND ${FAULTLINE_TEST_LAUNCHER} ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests/python -v)
    set_tests_properties(python_unittests PROPERTIES
      ENVIRONMENT "FAULTLINE_CLI=$<TARGET_FILE:faultline>")

    add_test(NAME faultsim_check_c17
      COMMAND ${FAULTLINE_TEST_LAUNCHER} ${Python3_EXECUTABLE}
              ${CMAKE_SOURCE_DIR}/tools/faultsim_check.py
              --cli $<TARGET_FILE:faultline>
              ${CMAKE_SOURCE_DIR}/benchmarks/iscas85/c17.bench)
    # Same oracle check through the multithreaded path of the CLI.
    add_test(NAME faultsim_check_c17_threads4
      COMMAND ${FAULTLINE_TEST_LAUNCHER} ${Python3_EXECUTABLE}
              ${CMAKE_SOURCE_DIR}/tools/faultsim_check.py --threads 4
              --cli $<TARGET_FILE:faultline>
              ${CMAKE_SOURCE_DIR}/benchmarks/iscas85/c17.bench)

    # Starts the real server binary on port 0 and talks to it with the Python
    # client. test_server.py skips itself when FAULTLINE_SERVER is not set, so
    # the discover run above does not start servers a second time.
    add_test(NAME server_integration
      COMMAND ${FAULTLINE_TEST_LAUNCHER} ${Python3_EXECUTABLE} -m unittest -v test_server
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/python)
    set_tests_properties(server_integration PROPERTIES
      TIMEOUT 600
      ENVIRONMENT "FAULTLINE_SERVER=$<TARGET_FILE:faultline_server>;FAULTLINE_CLI=$<TARGET_FILE:faultline>")

    find_program(IVERILOG_EXECUTABLE iverilog)
    if(IVERILOG_EXECUTABLE)
      add_test(NAME iverilog_check_c17
        COMMAND ${FAULTLINE_TEST_LAUNCHER} ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/iverilog_check.py
                --cli $<TARGET_FILE:faultline>
                ${CMAKE_SOURCE_DIR}/benchmarks/iscas85/c17.bench)
    endif()
  endif()
endif()
