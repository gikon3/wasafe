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
    test_byte_io.cpp
    test_hierarchy.cpp
    test_lazy_backend.cpp
    test_foreign_source.cpp
    test_multi_cursor.cpp
    test_memory.cpp
    test_indexing.cpp
    test_ingest.cpp

    # Внутренние модули, проверяемые напрямую: библиотека собрана со скрытой
    # видимостью, поэтому при shared-сборке её символы тестам не видны.
    ${PROJECT_SOURCE_DIR}/src/core/byte_io.cpp
)

set_target_properties(wasafe_tests PROPERTIES
    OUTPUT_NAME wasafe-tests)

# Тесты белого ящика видят внутренние заголовки (src/core/byte_io.hpp и т.п.)
# так же, как их видит сама библиотека.
target_include_directories(wasafe_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(wasafe_tests PRIVATE wasafe::wasafe GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(wasafe_tests)
