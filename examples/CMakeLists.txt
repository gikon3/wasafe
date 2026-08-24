add_executable(dump_hierarchy dump_hierarchy.cpp)
set_target_properties(dump_hierarchy PROPERTIES OUTPUT_NAME dump-hierarchy)
target_link_libraries(dump_hierarchy PRIVATE wasafe::wasafe)

add_executable(query_range query_range.cpp)
set_target_properties(query_range PROPERTIES OUTPUT_NAME query-range)
target_link_libraries(query_range PRIVATE wasafe::wasafe)

add_executable(build_inmemory build_inmemory.cpp)
set_target_properties(build_inmemory PROPERTIES OUTPUT_NAME build-inmemory)
target_link_libraries(build_inmemory PRIVATE wasafe::wasafe)

add_executable(export_changes export_changes.cpp)
set_target_properties(export_changes PROPERTIES OUTPUT_NAME export-changes)
target_link_libraries(export_changes PRIVATE wasafe::wasafe)
