include_guard(GLOBAL)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

#! Stages the Blender extension of the app in OUTPUT and sets BLENDER_EXTENSION_DIR, BLENDER_EXTENSION_FILES and BLENDER_TEST_COMMAND in the caller.
function(unrealkit_add_blender_extension)
    cmake_parse_arguments(PARSE_ARGV 0 IMPORTER "" "ID;NAME;VERSION;MAINTAINER;BLENDER_PORT;OUTPUT" "")
    set(unrealkit "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(source "${unrealkit}/src/UnrealKit.Blender")
    set(output "${IMPORTER_OUTPUT}")
    set(runtime "${unrealkit}/external/flatbuffers/python")
    file(STRINGS "${runtime}/flatbuffers/_version.py" runtime_version REGEX "^__version__")
    string(REGEX REPLACE "^__version__ = \"(.+)\"$" "\\1" runtime_version "${runtime_version}")
    set(FLATBUFFERS_WHEEL "flatbuffers-${runtime_version}-py3-none-any.whl")
    configure_file("${source}/blender_manifest.toml.in" "${output}/blender_manifest.toml" @ONLY)
    configure_file("${source}/product.py.in" "${output}/product.py" @ONLY)

    file(GLOB python_sources CONFIGURE_DEPENDS "${source}/*.py")
    set(staged "${output}/scene_generated.py" "${output}/wheels/${FLATBUFFERS_WHEEL}")
    foreach(python_source IN LISTS python_sources)
        get_filename_component(name "${python_source}" NAME)
        list(APPEND staged "${output}/${name}")
    endforeach()
    file(GLOB runtime_sources CONFIGURE_DEPENDS "${runtime}/flatbuffers/*.py")
    add_custom_command(OUTPUT ${staged}
        COMMAND ${CMAKE_COMMAND} -E copy ${python_sources} "${output}"
        COMMAND "${FLATC_COMMAND}" --python --gen-onefile -o "${output}" "${SCENE_SCHEMA}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${output}/wheels"
        COMMAND Python3::Interpreter -I "${unrealkit}/tools/build_flatbuffers_wheel.py" "${runtime}" "${output}/wheels/${FLATBUFFERS_WHEEL}"
        DEPENDS ${python_sources} ${FLATC_DEPENDENCY} "${SCENE_SCHEMA}" "${unrealkit}/tools/build_flatbuffers_wheel.py" ${runtime_sources}
        COMMENT "Staging the Blender extension"
        VERBATIM)
    add_custom_target(blender_extension ALL DEPENDS ${staged})
    set(BLENDER_EXTENSION_DIR "${output}" PARENT_SCOPE)
    set(BLENDER_EXTENSION_FILES "${output}/blender_manifest.toml" "${output}/product.py" ${staged} PARENT_SCOPE)
    set(BLENDER_TEST_COMMAND "${Python3_EXECUTABLE}" -I "${unrealkit}/tools/e2e/blender_test.py" --extension "${output}" --extension-id
        "${IMPORTER_ID}" PARENT_SCOPE)
endfunction()

#! Packs the Blender extension that unrealkit_add_blender_extension staged into an extension repository in OUTPUT.
function(unrealkit_add_blender_repository)
    cmake_parse_arguments(PARSE_ARGV 0 REPOSITORY "" "OUTPUT" "")
    set(pack "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/pack_blender_repository.py")
    add_custom_command(OUTPUT "${REPOSITORY_OUTPUT}/index.json"
        COMMAND Python3::Interpreter -I "${pack}" "${BLENDER_EXTENSION_DIR}" "${REPOSITORY_OUTPUT}"
        DEPENDS blender_extension "${pack}"
        COMMENT "Packing the Blender extension repository"
        VERBATIM)
    add_custom_target(blender_repository ALL DEPENDS "${REPOSITORY_OUTPUT}/index.json")
endfunction()

#! Stages the Unreal Editor plugin of the app and sets UNREAL_PLUGIN_STAGE and UNREAL_PLUGIN_FILES in the caller.
function(unrealkit_add_unreal_plugin)
    cmake_parse_arguments(PARSE_ARGV 0 IMPORTER "" "MODULE;NAME;VERSION;MAINTAINER;UNREAL_PORT;OUTPUT" "")
    set(unrealkit "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(source "${unrealkit}/src/UnrealKit.Unreal")
    set(stage "${IMPORTER_OUTPUT}")
    set(module "${stage}/Source/${IMPORTER_MODULE}")
    set(third_party "${module}/ThirdParty")
    set(flatbuffers "${unrealkit}/external/flatbuffers/include/flatbuffers")
    set(templates "${CMAKE_CURRENT_BINARY_DIR}/unreal_plugin")
    configure_file("${source}/Plugin.uplugin.in" "${templates}/${IMPORTER_MODULE}.uplugin" @ONLY)
    configure_file("${source}/Source/Module.Build.cs.in" "${templates}/${IMPORTER_MODULE}.Build.cs" @ONLY)
    configure_file("${source}/Source/Private/Module.cpp.in" "${templates}/${IMPORTER_MODULE}Module.cpp" @ONLY)
    configure_file("${source}/Source/Private/ImporterProduct.h.in" "${templates}/ImporterProduct.h" @ONLY)

    file(GLOB private_sources CONFIGURE_DEPENDS "${source}/Source/Private/*.h" "${source}/Source/Private/*.cpp")
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE "${flatbuffers}" "${flatbuffers}/*")
    set(staged "${stage}/${IMPORTER_MODULE}.uplugin" "${module}/${IMPORTER_MODULE}.Build.cs" "${module}/Private/${IMPORTER_MODULE}Module.cpp"
        "${module}/Private/ImporterProduct.h" "${third_party}/scene_generated.h" "${third_party}/bcdec.h")
    foreach(private_source IN LISTS private_sources)
        get_filename_component(name "${private_source}" NAME)
        list(APPEND staged "${module}/Private/${name}")
    endforeach()
    foreach(header IN LISTS headers)
        list(APPEND staged "${third_party}/flatbuffers/${header}")
    endforeach()

    add_custom_command(OUTPUT ${staged}
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${stage}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${module}/Private"
        COMMAND ${CMAKE_COMMAND} -E copy ${private_sources} "${templates}/${IMPORTER_MODULE}Module.cpp" "${templates}/ImporterProduct.h" "${module}/Private"
        COMMAND ${CMAKE_COMMAND} -E copy "${templates}/${IMPORTER_MODULE}.Build.cs" "${module}"
        COMMAND ${CMAKE_COMMAND} -E copy "${templates}/${IMPORTER_MODULE}.uplugin" "${stage}"
        COMMAND "${FLATC_COMMAND}" --cpp --scoped-enums -o "${third_party}" "${SCENE_SCHEMA}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${flatbuffers}" "${third_party}/flatbuffers"
        COMMAND ${CMAKE_COMMAND} -E copy "${unrealkit}/external/bcdec/bcdec.h" "${third_party}"
        DEPENDS ${private_sources} "${templates}/${IMPORTER_MODULE}.uplugin" "${templates}/${IMPORTER_MODULE}.Build.cs"
            "${templates}/${IMPORTER_MODULE}Module.cpp" "${templates}/ImporterProduct.h" ${FLATC_DEPENDENCY} "${SCENE_SCHEMA}"
        COMMENT "Staging the Unreal Editor plugin"
        VERBATIM)
    add_custom_target(unreal_plugin DEPENDS ${staged})
    set(UNREAL_PLUGIN_STAGE "${stage}" PARENT_SCOPE)
    set(UNREAL_PLUGIN_FILES ${staged} PARENT_SCOPE)
endfunction()
