include("${CMAKE_CURRENT_LIST_DIR}/GuestManifest.cmake")
file(MAKE_DIRECTORY "${GEN}")
file(LOCK "${GEN}/generation.lock" GUARD PROCESS TIMEOUT 120)

execute_process(COMMAND "${RECOMP}" "config/${CONFIG}" "${XBE}" "${GEN}" --xdk "${REPO}/sdk/xdk"
    WORKING_DIRECTORY "${REPO}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GRID_PATCH}" "${GEN}" "${PARTITIONS}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${LLD}" /lib /machine:x64 "/def:${GEN}/imports.def" "/out:${GEN}/code.lib"
    COMMAND_ERROR_IS_FATAL ANY)
xbe_generation_manifest(manifest "${REPO}" "${CONFIG}" "${XBE}" "${GEN}" "${PARTITIONS}")
if(NOT manifest)
    message(FATAL_ERROR "The recompiler did not emit every expected guest source file.")
endif()
file(WRITE "${GEN}/generation.manifest.tmp" "${manifest}")
file(RENAME "${GEN}/generation.manifest.tmp" "${GEN}/generation.manifest")
file(TOUCH "${GEN}/codegen.stamp")
