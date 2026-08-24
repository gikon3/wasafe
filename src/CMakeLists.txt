add_library(wasafe)
add_library(wasafe::wasafe ALIAS wasafe)

set_target_properties(wasafe PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_compile_features(wasafe PUBLIC cxx_std_23)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(wasafe PUBLIC WASAFE_STATIC_DEFINE)
endif()

# Опциональное сжатие блоков ленивого backend. Флаг уезжает в config.hpp, а не в
# приватный define: собранность zstd видна и тестам, и потребителю пакета —
# блоки с codec == ZSTD читаются только при WASAFE_HAS_ZSTD.
if(WASAFE_WITH_ZSTD)
    find_package(zstd REQUIRED)
    target_link_libraries(wasafe PRIVATE zstd::libzstd)
    set(WASAFE_HAS_ZSTD 1)
else()
    set(WASAFE_HAS_ZSTD 0)
endif()

add_subdirectory(core)
add_subdirectory(io)
add_subdirectory(model)
add_subdirectory(storage)
add_subdirectory(types)

include(GenerateExportHeader)
generate_export_header(wasafe
    BASE_NAME WASAFE
    EXPORT_MACRO_NAME WASAFE_API
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe/export.hpp)

configure_file(
    ${PROJECT_SOURCE_DIR}/include/wasafe/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe/version.hpp
    @ONLY)

configure_file(
    ${PROJECT_SOURCE_DIR}/include/wasafe/config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe/config.hpp
    @ONLY)

target_include_directories(wasafe
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/export>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

if(WASAFE_INSTALL)
    install(TARGETS wasafe
        EXPORT wasafe-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe/export.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe/config.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe/version.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wasafe)

    # ---------------------------------------------------------------------
    # CMake-пакет для потребителей: find_package(wasafe CONFIG)
    # ---------------------------------------------------------------------
    set(WASAFE_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/wasafe)

    install(EXPORT wasafe-targets
        NAMESPACE wasafe::
        FILE wasafe-targets.cmake
        DESTINATION ${WASAFE_CMAKE_DIR})

    include(CMakePackageConfigHelpers)

    # Статический экспорт тянет zstd::libzstd в link-интерфейс, shared — нет.
    if(WASAFE_WITH_ZSTD AND NOT BUILD_SHARED_LIBS)
        set(WASAFE_EXPORT_NEEDS_ZSTD 1)
    else()
        set(WASAFE_EXPORT_NEEDS_ZSTD 0)
    endif()

    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/wasafe-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe-config.cmake
        INSTALL_DESTINATION ${WASAFE_CMAKE_DIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe-config-version.cmake
        DESTINATION ${WASAFE_CMAKE_DIR})
endif()
