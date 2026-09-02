add_executable(wasafe_bench
    main.cpp
    report.cpp
    waveform.cpp
)
set_target_properties(wasafe_bench PROPERTIES OUTPUT_NAME wasafe-bench)
target_link_libraries(wasafe_bench PRIVATE wasafe::wasafe)

# Стенд меряет и внутренние вещи (пропускную способность Crc32), поэтому видит
# приватные заголовки библиотеки.
target_include_directories(wasafe_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
