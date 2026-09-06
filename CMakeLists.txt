cmake_minimum_required(VERSION 3.16)
project(minidb VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MINIDB_BUILD_TESTS "Build the MiniDB test suite" ON)
option(MINIDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(minidb_core
  src/value.cpp
  src/diagnostic.cpp
  src/lexer.cpp
  src/ast.cpp
  src/parser.cpp
  src/record.cpp
  src/pager.cpp
  src/wal.cpp
  src/btree.cpp
  src/key.cpp
  src/index.cpp
  src/plan.cpp
  src/table.cpp
  src/database.cpp
  src/executor.cpp
)

target_include_directories(minidb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(minidb_core PRIVATE /W4 /permissive-)
  if(MINIDB_WARNINGS_AS_ERRORS)
    target_compile_options(minidb_core PRIVATE /WX)
  endif()
else()
  target_compile_options(minidb_core PRIVATE -Wall -Wextra -Wpedantic)
  if(MINIDB_WARNINGS_AS_ERRORS)
    target_compile_options(minidb_core PRIVATE -Werror)
  endif()
endif()

add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_core)
if(WIN32)
  target_link_libraries(minidb PRIVATE ws2_32)
endif()

add_executable(minidb_bench bench/main.cpp)
target_link_libraries(minidb_bench PRIVATE minidb_core)

add_executable(minidb_crash tools/crash.cpp)
target_link_libraries(minidb_crash PRIVATE minidb_core)

if(MINIDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)

  # Kills a writer process mid-transaction, over and over, and checks that
  # the database always reopens holding exactly the committed rows.
  add_test(NAME minidb_crash_recovery
           COMMAND minidb_crash torture --db crash_ctest.db --rounds 20
                   --seed 20260826)
endif()
