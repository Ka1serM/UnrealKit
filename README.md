# UnrealKit

A C++23 library for Unreal Engine game files: it mounts the archives, reads maps, meshes, materials, and textures, and
streams them as a FlatBuffers scene to a Blender extension and an Unreal Editor plugin.

## Usage

```cmake
include(external/UnrealKit/cmake/Importers.cmake)
add_subdirectory(external/UnrealKit EXCLUDE_FROM_ALL)

unrealkit_add_blender_extension(ID example NAME Example VERSION 1.0.0 MAINTAINER "Name" BLENDER_PORT 17472
    OUTPUT "${PROJECT_BINARY_DIR}/blender_extension")
unrealkit_add_unreal_plugin(MODULE Example NAME Example VERSION 1.0.0 MAINTAINER "Name" UNREAL_PORT 17473
    OUTPUT "${PROJECT_BINARY_DIR}/plugins/Example")

add_executable(App Main.cpp)
target_link_libraries(App PRIVATE UnrealKit::UnrealKit)
```

```cpp
constexpr UnrealKit::Transport::Product product{"Example", "1.0.0", 17472, 17473};

UnrealKit::Assets::Provider provider;
auto archives = UnrealKit::Assets::openArchiveDirectory(paksDirectory);
auto mounted = provider.mount({.game = ue5parse::EGame::GAME_UE5_4, .archives = std::move(*archives), .aesKey = key}, stop);

UnrealKit::Transport::SceneSession session(product, UnrealKit::Transport::Importer::Blender, stop, onVersionMismatch);
```

## Dependencies

- CMake 3.29+ and Ninja
- Clang 22+ with libc++, compiler-rt, libunwind and LLD
- Windows: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
- The browser: Emscripten
- The importers: Blender 4.5+ and Unreal Engine 5

Git submodules in `external/`: CppUE5Parse, UsmapGen, FlatBuffers, zstd, Mbed TLS, cpp-httplib, and bcdec.
