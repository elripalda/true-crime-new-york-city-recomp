# Shared identity for the generated guest translation and its source image.
function(xbe_generation_inputs result repo config xbe)
    file(GLOB inputs LIST_DIRECTORIES FALSE
        "${repo}/config/*.toml"
        "${repo}/sdk/xdk/*/*.toml"
        "${repo}/sdk/tools/recomp/*.cpp"
        "${repo}/sdk/tools/recomp/*.h"
        "${repo}/sdk/runtime/cpu/freestanding/*")
    list(APPEND inputs "${xbe}" "${repo}/config/${config}"
        "${repo}/sdk/runtime/xbe/xbe.h"
        "${repo}/sdk/runtime/xbe/grid_patch.h"
        "${repo}/sdk/runtime/cpu/x86_context.h"
        "${repo}/sdk/runtime/cpu/x86_ops.h"
        "${repo}/sdk/tools/grid_patch/main.cpp"
        "${repo}/sdk/cmake/XbeRecomp.cmake"
        "${repo}/sdk/cmake/GenerateGuest.cmake"
        "${repo}/sdk/cmake/GuestManifest.cmake")
    list(REMOVE_DUPLICATES inputs)
    list(SORT inputs)
    set(${result} "${inputs}" PARENT_SCOPE)
endfunction()

function(xbe_generation_manifest result repo config xbe generated partitions)
    xbe_generation_inputs(inputs "${repo}" "${config}" "${xbe}")
    foreach(input IN LISTS inputs)
        file(SHA256 "${input}" hash)
        file(RELATIVE_PATH name "${repo}" "${input}")
        string(APPEND identity "${hash} ${name}\n")
    endforeach()
    string(SHA256 fingerprint "${identity}")
    set(text "tcnyc-guest-generation 1\n${config}\n${partitions}\n${fingerprint}\n")

    set(outputs x86_func_mapping.cpp x86_recomp_shared.h image_symbols.h hook_exports.h)
    math(EXPR last "${partitions} - 1")
    foreach(i RANGE 0 ${last})
        list(APPEND outputs "x86_recomp.${i}.cpp")
    endforeach()
    foreach(name IN LISTS outputs)
        if(NOT EXISTS "${generated}/${name}")
            set(${result} "" PARENT_SCOPE)
            return()
        endif()
        file(SHA256 "${generated}/${name}" hash)
        string(APPEND text "${hash} ${name}\n")
    endforeach()
    set(${result} "${text}" PARENT_SCOPE)
endfunction()
