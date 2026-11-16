function(tcnyc_add_launcher target)
    set(launcher_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(banner "${launcher_dir}/../docs/banner.png")
    set(embedded "${CMAKE_CURRENT_BINARY_DIR}/launcher_banner.inc")
    add_custom_command(OUTPUT "${embedded}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${banner}" "-DOUTPUT=${embedded}"
                -P "${launcher_dir}/embed_banner.cmake"
        DEPENDS "${banner}" "${launcher_dir}/embed_banner.cmake"
        VERBATIM)
    target_sources(${target} PRIVATE
        "${embedded}"
        "${launcher_dir}/launcher.cpp"
        "${launcher_dir}/updater.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
        "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp")
    # the release this build is; the updater compares it with the latest GitHub release
    file(STRINGS "${launcher_dir}/VERSION" version LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${launcher_dir}/VERSION")
    target_compile_definitions(${target} PRIVATE TCNYC_VERSION="${version}")
    target_include_directories(${target} PRIVATE
        "${launcher_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_property(SOURCE "${launcher_dir}/launcher.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES
        "${launcher_dir}/../platform/linux/third_party/stb")
endfunction()
