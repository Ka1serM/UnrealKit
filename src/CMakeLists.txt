file(GLOB_RECURSE unrealkit_sources CONFIGURE_DEPENDS UnrealKit/*.cpp)
if(PLATFORM_WEB)
    list(FILTER unrealkit_sources EXCLUDE REGEX "Desktop\\.cpp$")
else()
    list(FILTER unrealkit_sources EXCLUDE REGEX "Web\\.cpp$")
endif()

add_library(UnrealKit STATIC ${unrealkit_sources})
add_library(UnrealKit::UnrealKit ALIAS UnrealKit)
target_include_directories(UnrealKit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(UnrealKit PUBLIC scene_schema bcdec CppUE5Parse::Core CppUE5Parse::CoreUObject CppUE5Parse::RenderCore
    CppUE5Parse::Engine CppUE5Parse::Conversion nlohmann_json::nlohmann_json UsmapGen::UsmapGen zstd)
target_compile_options(UnrealKit PRIVATE -fno-exceptions)
target_compile_definitions(UnrealKit
    PUBLIC PLATFORM_WINDOWS=$<BOOL:${PLATFORM_WINDOWS}> PLATFORM_LINUX=$<BOOL:${PLATFORM_LINUX}> PLATFORM_WEB=$<BOOL:${PLATFORM_WEB}>
        UNREALKIT_DEBUG_LOG=$<CONFIG:Debug,RelWithDebInfo>
    PRIVATE PRODUCT_NAME="${CMAKE_PROJECT_NAME}")
target_precompile_headers(UnrealKit PRIVATE <algorithm> <expected> <format> <memory> <optional> <stop_token> <string> <string_view>
    <utility> <vector> "${CMAKE_CURRENT_SOURCE_DIR}/UnrealKit/Core/StdGaps.h")

if(PLATFORM_WEB)
    #! Only Emscripten's own headers declare the WasmFS backend API.
    target_include_directories(UnrealKit SYSTEM PRIVATE "${EMSCRIPTEN_ROOT_PATH}/system/lib/wasmfs")
    #! The page calls Module.unrealkitStart(status) before main, and Module.unrealkitChoose(kind, extension) to mount a picked folder.
    target_link_options(UnrealKit INTERFACE -lwebsocket.js -sFETCH=1 "--js-library=${PROJECT_SOURCE_DIR}/web/library.js"
        "--pre-js=${PROJECT_SOURCE_DIR}/web/pre.js")
    set_property(TARGET UnrealKit APPEND PROPERTY INTERFACE_LINK_DEPENDS "${PROJECT_SOURCE_DIR}/web/library.js" "${PROJECT_SOURCE_DIR}/web/pre.js")
else()
    target_link_libraries(UnrealKit PUBLIC httplib)
endif()
