cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED WASAFE_VERSION)
    set(WASAFE_VERSION 0.0.0)
endif()

project(wasafe
    VERSION ${WASAFE_VERSION}
    DESCRIPTION "${WASAFE_DESCRIPTION}"
    HOMEPAGE_URL "${WASAFE_HOMEPAGE_URL}"
    LANGUAGES CXX)

if(NOT PROJECT_VERSION_MINOR)
    set(PROJECT_VERSION_MINOR 0)
endif()
if(NOT PROJECT_VERSION_PATCH)
    set(PROJECT_VERSION_PATCH 0)
endif()

# ---------------------------------------------------------------------------
# Опции сборки
# ---------------------------------------------------------------------------
option(WASAFE_WITH_ZSTD "Сжатие блоков ленивого backend через zstd" ON)
option(WASAFE_INSTALL "Генерировать правила установки" ${PROJECT_IS_TOP_LEVEL})
option(WASAFE_BUILD_TESTS "Собирать тесты" ${PROJECT_IS_TOP_LEVEL})
option(WASAFE_BUILD_EXAMPLES "Собирать примеры" OFF)
option(WASAFE_BUILD_BENCHMARKS "Собирать замеры" OFF)

# ---------------------------------------------------------------------------
# Общие настройки компилятора
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
    endif()

    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

    include(CTest)

    # Предупреждения — только в собственной сборке. Потребитель, подключивший
    # проект через add_subdirectory, имеет своё представление о строгости, а
    # add_compile_options действует на каталог и ниже, не уезжая в usage
    # requirements цели.
    #
    # Кому нужна сборка строже, передаёт флаг снаружи: CXXFLAGS=-Werror
    # либо -DCMAKE_CXX_FLAGS=-Werror.
    #
    # -Wconversion/-Wsign-conversion нет намеренно. Они требуют ручных
    # static_cast, которые глушат предупреждение, но не убирают переполнение.
    if(MSVC)
        add_compile_options(/W4)
    else()
        add_compile_options(
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wold-style-cast
            -Wcast-align
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough)
    endif()
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Основная библиотека (ядро): иерархия, типы, хранилище, ingestion API.
# Чтение конкретных форматов (VCD, FST, ...) в этот проект не входит: парсеры
# живут в отдельных проектах, реализуют WaSafe::Reader и подключаются
# пользователем — см. docs/writing_a_reader.md.
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(WASAFE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(WASAFE_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(WASAFE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Установка публичных заголовков
# ---------------------------------------------------------------------------
if(WASAFE_INSTALL)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN "*.in" EXCLUDE)
endif()
