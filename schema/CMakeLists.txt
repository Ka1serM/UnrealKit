set(SCENE_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/scene.fbs" CACHE INTERNAL "The FlatBuffers schema of the scene stream")
set(scene_header "${CMAKE_CURRENT_BINARY_DIR}/scene_generated.h")
add_custom_command(OUTPUT "${scene_header}"
    COMMAND "${FLATC_COMMAND}" --cpp --scoped-enums -o "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/scene.fbs"
    DEPENDS ${FLATC_DEPENDENCY} "${CMAKE_CURRENT_SOURCE_DIR}/scene.fbs"
    COMMENT "Generating the C++ scene stream"
    VERBATIM)
add_custom_target(scene_schema_header DEPENDS "${scene_header}")

add_library(scene_schema INTERFACE)
add_dependencies(scene_schema scene_schema_header)
target_include_directories(scene_schema INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(scene_schema INTERFACE FlatBuffers::FlatBuffers)
