# The dependencies, built from source as static libraries without warnings. CppUE5Parse takes zstd and mbedcrypto from
# here, and TBB::tbb from the app when the app made it first.

# A third-party project or target compiles without warnings: they are not ours to fix.
set(no_warnings -w)
function(add_external_subdirectory source)
    get_directory_property(options COMPILE_OPTIONS)
    add_compile_options(${no_warnings})
    add_subdirectory(${source} ${ARGN} EXCLUDE_FROM_ALL SYSTEM)
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "${options}")
endfunction()

foreach(submodule bcdec cpp-httplib CppUE5Parse flatbuffers json mbedtls UsmapGen zstd)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${submodule}/.git")
        message(FATAL_ERROR "external/${submodule} is missing. Run: git submodule update --init --recursive")
    endif()
endforeach()

include(ExternalProject)

# cpp-httplib is the desktop's HTTPS client and the WebSocket client of the scene stream, with Mbed TLS for its TLS. The
# browser has no sockets: the page makes the requests and the WebSocket.
if(NOT PLATFORM_WEB)
    foreach(mbedtls_option PROGRAMS TESTING)
        set(ENABLE_${mbedtls_option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(LINK_WITH_PTHREAD ON CACHE BOOL "" FORCE)
    add_external_subdirectory(mbedtls)
    # The TBB threads make HTTPS requests at the same time, and PSA crypto keeps global state. The mutexes guard it.
    # llvm-mingw has winpthreads, so pthread is the threading layer on Windows too. The macros change the layout of the
    # Mbed TLS structures, so every target that includes the headers takes them (PUBLIC).
    target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
    add_library(httplib INTERFACE)
    target_include_directories(httplib SYSTEM INTERFACE cpp-httplib)
    target_compile_definitions(httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT CPPHTTPLIB_NO_EXCEPTIONS)
    target_link_libraries(httplib INTERFACE mbedtls mbedx509 mbedcrypto "$<$<PLATFORM_ID:Windows>:ws2_32;crypt32>")
endif()

# zstd: the archives of CppUE5Parse and the mappings that UsmapGen generates.
file(GLOB zstd_sources zstd/lib/common/*.c zstd/lib/compress/*.c zstd/lib/decompress/*.c)
add_library(zstd STATIC ${zstd_sources})
target_include_directories(zstd SYSTEM PUBLIC zstd/lib)
target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM PUBLIC XXH_STATIC_LINKING_ONLY ZSTD_STATIC_LINKING_ONLY)
target_compile_options(zstd PRIVATE ${no_warnings})

# CppUE5Parse reads the game archives and runs the Unreal runtime. It also gives oneTBB, ACL with RTM, and SIMDe. The app
# sets the UE5PARSE_GAME_* options of its game first.
set(UE5PARSE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(UE5PARSE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UE5PARSE_BUILD_CONVERSION ON CACHE BOOL "" FORCE)
add_external_subdirectory(CppUE5Parse)

# UsmapGen generates the mappings from the game executable.
add_external_subdirectory(UsmapGen)

# nlohmann/json reads and writes the apps' settings.json. Headers only. The code that includes it compiles without
# exceptions, so it parses without them and checks each value before it reads it.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
add_external_subdirectory(json)
target_compile_definitions(nlohmann_json INTERFACE JSON_NOEXCEPTION)

# bcdec decodes the BC blocks of the game textures. Headers only.
add_library(bcdec INTERFACE)
target_include_directories(bcdec SYSTEM INTERFACE bcdec)

# FlatBuffers: the runtime of the scene stream (headers only), and flatc, which generates its readers. flatc is a host
# tool: a cross build (the browser, or Windows from Linux) builds it separately with the Clang of the build machine. It
# only generates two files, so it compiles without optimization, several times faster. The Windows preset sets
# CMAKE_SYSTEM_NAME, which makes CMAKE_CROSSCOMPILING true on a Windows host too: the check compares the systems instead.
foreach(flatbuffers_option BUILD_TESTS INSTALL BUILD_FLATLIB)
    set(FLATBUFFERS_${flatbuffers_option} OFF CACHE BOOL "" FORCE)
endforeach()
if(NOT CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME)
    set(FLATBUFFERS_BUILD_FLATC OFF CACHE BOOL "" FORCE)
    set(host_flatc_dir "${CMAKE_CURRENT_BINARY_DIR}/host-flatc")
    set(FLATC_COMMAND "${host_flatc_dir}/flatc${CMAKE_HOST_EXECUTABLE_SUFFIX}" CACHE INTERNAL "")
    ExternalProject_Add(host_flatc
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers"
        BINARY_DIR "${host_flatc_dir}"
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
            "-DCMAKE_CXX_FLAGS=-O0 -stdlib=libc++"
            "-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ --rtlib=compiler-rt --unwindlib=libunwind"
            -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF
            -DFLATBUFFERS_INSTALL=OFF
        BUILD_COMMAND "${CMAKE_COMMAND}" --build "${host_flatc_dir}" --target flatc
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${host_flatc_dir}/flatc${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(FLATC_DEPENDENCY host_flatc CACHE INTERNAL "")
else()
    set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
    set(FLATC_COMMAND "$<TARGET_FILE:flatc>" CACHE INTERNAL "")
    set(FLATC_DEPENDENCY flatc CACHE INTERNAL "")
endif()
add_external_subdirectory(flatbuffers)
if(TARGET flatc)
    target_compile_options(flatc PRIVATE -O0)
    set_target_properties(flatc PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endif()
