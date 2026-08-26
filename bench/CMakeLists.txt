add_executable(wasafe_bench
    main.cpp
    report.cpp
    waveform.cpp
)
set_target_properties(wasafe_bench PROPERTIES OUTPUT_NAME wasafe-bench)
target_link_libraries(wasafe_bench PRIVATE wasafe::wasafe)
