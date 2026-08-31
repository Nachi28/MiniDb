add_executable(minidb_tests
  test_main.cpp
  test_value.cpp
  test_lexer.cpp
  test_parser.cpp
  test_key.cpp
  test_record.cpp
  test_pager.cpp
  test_btree.cpp
  test_index.cpp
  test_plan.cpp
  test_storage.cpp
  test_wal.cpp
  test_recovery.cpp
  test_executor.cpp
)

target_link_libraries(minidb_tests PRIVATE minidb_core)
target_include_directories(minidb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME minidb_tests COMMAND minidb_tests)
