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
    set(pal_xbe "${launcher_dir}/../assets-pal/default.xbe")
    if(NOT TCNYC_PAL AND EXISTS "${pal_xbe}")
        # Detection uses the public SHA-1s in app/game_source.h. These definitions
        # tell the updater that the package bundles the PAL executables.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pal_xbe}")
        file(SHA1 "${pal_xbe}" pal_sha1)
        target_compile_definitions(${target} PRIVATE TCNYC_PAL_SHA1="${pal_sha1}")
    endif()
    set(pal_eu_xbe "${launcher_dir}/../assets-pal-eu/default.xbe")
    if(NOT TCNYC_PAL AND EXISTS "${pal_eu_xbe}")
        include("${launcher_dir}/../sdk/cmake/CheckPalEu.cmake")
        tcnyc_check_pal_eu("${pal_eu_xbe}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pal_eu_xbe}")
        file(SHA1 "${pal_eu_xbe}" pal_eu_sha1)
        target_compile_definitions(${target} PRIVATE TCNYC_PAL_EU_SHA1="${pal_eu_sha1}")
    endif()
    target_include_directories(${target} PRIVATE
        "${launcher_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_property(SOURCE "${launcher_dir}/launcher.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES
        "${launcher_dir}/../platform/linux/third_party/stb")
endfunction()
