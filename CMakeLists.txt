cmake_minimum_required(VERSION 3.24)
project(iox VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Unified async IO for Linux")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---- the library -----------------------------------------------------------

add_library(iox STATIC src/error.cc)
target_compile_features(iox PUBLIC cxx_std_23)
target_include_directories(iox SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                             ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include)

# liburing: prefer the user-local install, fall back to system paths.
find_path(IOX_LIBURING_INCLUDE_DIR liburing.h
          HINTS $ENV{HOME}/.local/include /usr/include /usr/local/include REQUIRED)
find_library(IOX_LIBURING_LIB NAMES uring
             HINTS $ENV{HOME}/.local/lib /usr/lib /usr/local/lib REQUIRED)
target_include_directories(iox SYSTEM PUBLIC ${IOX_LIBURING_INCLUDE_DIR})
target_link_libraries(iox PUBLIC ${IOX_LIBURING_LIB})

target_compile_options(iox PRIVATE -Wall -Wextra -Wpedantic)

option(IOX_SANITIZE "Build with address+undefined sanitizers" OFF)
if(IOX_SANITIZE)
  target_compile_options(iox PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(iox PUBLIC -fsanitize=address,undefined)
endif()

# ---- tests -----------------------------------------------------------------

option(IOX_BUILD_TESTS "Build the test suite" ON)
if(IOX_BUILD_TESTS)
  enable_testing()
  add_executable(iox_tests
    tests/test_main.cc
    tests/test_ring.cc
    tests/test_io_context.cc
    tests/test_ops.cc
    tests/test_capabilities.cc
    tests/test_fs.cc
    tests/test_cancel.cc
    tests/test_mr.cc
    tests/test_conformance.cc
    tests/test_net.cc
    tests/test_dns.cc
    tests/test_signal.cc
    tests/test_process.cc
    tests/test_inotify.cc
    tests/test_splice.cc
    tests/test_redteam.cc
    tests/test_driver.cc
    tests/test_fault.cc
    tests/test_nvme.cc
    tests/test_xdp.cc)
  target_include_directories(iox_tests SYSTEM PRIVATE third_party/doctest)
  target_link_libraries(iox_tests PRIVATE iox)
  add_test(NAME iox_tests COMMAND iox_tests)
endif()

# ---- examples --------------------------------------------------------------

option(IOX_BUILD_EXAMPLES "Build examples" ON)
if(IOX_BUILD_EXAMPLES)
  add_executable(timer_hello examples/timer_hello.cc)
  target_link_libraries(timer_hello PRIVATE iox)

  add_executable(stdin_echo examples/stdin_echo.cc)
  target_link_libraries(stdin_echo PRIVATE iox)

  add_executable(file_copy examples/file_copy.cc)
  target_link_libraries(file_copy PRIVATE iox)

  add_executable(echo_server examples/echo_server.cc)
  target_link_libraries(echo_server PRIVATE iox)

  add_executable(eventfd_device examples/eventfd_device.cc)
  target_link_libraries(eventfd_device PRIVATE iox)

  add_executable(ioxpump examples/ioxpump.cc)
  target_link_libraries(ioxpump PRIVATE iox)

  add_executable(child examples/child.cc)
  target_link_libraries(child PRIVATE iox)
endif()

# ---- python bindings ---------------------------------------------------------

option(IOX_BUILD_PYTHON "Build the nanobind python module (needs a python with dev headers + nanobind)" ON)
if(IOX_BUILD_PYTHON)
  find_package(Python COMPONENTS Interpreter Development.Module QUIET)
  find_package(nanobind CONFIG QUIET)
  if(NOT nanobind_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanobind/cmake)
    set(nanobind_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanobind/cmake)
    find_package(nanobind CONFIG REQUIRED) # the checked-out submodule
  endif()
  if(Python_Development.Module_FOUND AND nanobind_FOUND)
    nanobind_add_module(iox_python python/iox.cc python/async.cc)
    set_target_properties(iox_python PROPERTIES OUTPUT_NAME iox)
    # liburing lives in ~/.local on this class of box; the wheel-installed
    # module needs to find it at import time.
    set_target_properties(iox_python PROPERTIES
        BUILD_RPATH "$ENV{HOME}/.local/lib" INSTALL_RPATH "$ENV{HOME}/.local/lib")
    install(TARGETS iox_python LIBRARY DESTINATION .)
    target_link_libraries(iox_python PRIVATE iox)
    if(Python_FOUND)
      add_test(NAME iox_python COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python.py)
      set_tests_properties(iox_python PROPERTIES
        ENVIRONMENT PYTHONPATH=$<TARGET_FILE_DIR:iox_python>)
      add_test(NAME iox_python_async COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_async.py)
      set_tests_properties(iox_python_async PROPERTIES
        ENVIRONMENT PYTHONPATH=$<TARGET_FILE_DIR:iox_python>)
    endif()
  else()
    message(STATUS "python bindings skipped: nanobind or Development.Module not found (uv venv: PATH=.venv/bin:$PATH cmake -Dnanobind_DIR=$(.venv/bin/python -c 'import nanobind; print(nanobind.cmake_dir())'))")
  endif()
endif()

# ---- benchmarks ------------------------------------------------------------

option(IOX_BUILD_BENCH "Build benchmarks" ON)
if(IOX_BUILD_BENCH)
  add_executable(bench_submit_path bench/submit_path.cc)
  target_link_libraries(bench_submit_path PRIVATE iox)

  add_executable(bench_file_rw bench/file_rw.cc)
  target_link_libraries(bench_file_rw PRIVATE iox)

  add_executable(bench_echo bench/echo.cc)
  target_link_libraries(bench_echo PRIVATE iox)

  add_executable(bench_nvme bench/nvme_rw.cc)
  target_link_libraries(bench_nvme PRIVATE iox)

  add_executable(bench_splice bench/splice.cc)
  target_link_libraries(bench_splice PRIVATE iox)
endif()
