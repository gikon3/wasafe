include_guard()

if(NOT BUILD_TESTING)
    return()
endif()

find_package(GTest REQUIRED)

add_executable(wasafe_tests
    test_logic.cpp
    test_types.cpp
    test_time.cpp
    test_time_column.cpp
    test_hierarchy.cpp
    test_lazy_backend.cpp
    test_foreign_source.cpp
    test_multi_cursor.cpp
    test_memory.cpp
    test_indexing.cpp
    test_ingest.cpp
)

set_target_properties(wasafe_tests PROPERTIES
    OUTPUT_NAME wasafe-tests)

target_link_libraries(wasafe_tests PRIVATE wasafe::wasafe GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(wasafe_tests)
