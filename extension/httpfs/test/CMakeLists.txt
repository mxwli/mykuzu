add_definitions(-DTEST_FILES_DIR="extension/httpfs/test/test_files")
add_subdirectory(${CMAKE_SOURCE_DIR}/test/gtest ${CMAKE_CURRENT_BINARY_DIR}/test/gtest EXCLUDE_FROM_ALL)
# Make gtest available to subdirectories.
add_library(GTest::GTest INTERFACE IMPORTED)
target_link_libraries(GTest::GTest INTERFACE gtest_main)
target_link_libraries(GTest::GTest INTERFACE gmock_main)
enable_testing()
add_subdirectory(${CMAKE_SOURCE_DIR}/test/test_helper ${CMAKE_CURRENT_BINARY_DIR}/test/test_helper)
add_subdirectory(${CMAKE_SOURCE_DIR}/test/test_runner ${CMAKE_CURRENT_BINARY_DIR}/test/test_runner)
add_subdirectory(${CMAKE_SOURCE_DIR}/test/graph_test ${CMAKE_CURRENT_BINARY_DIR}/test/graph_test)
add_subdirectory(${CMAKE_SOURCE_DIR}/test/runner ${CMAKE_CURRENT_BINARY_DIR}/test/runner)
