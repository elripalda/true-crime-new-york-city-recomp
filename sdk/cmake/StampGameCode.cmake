# Keep the format in sync with runtime/app/game_code.cpp.
file(SHA1 "${HOST}" host_sha1)
file(SHA1 "${DLL}" dll_sha1)
# The image config decides the generated code too (extra [functions] entry points), so a config-only fix
# rebuilds the game code on players' PCs.
file(SHA1 "${CONFIG_FILE}" config_sha1)
file(WRITE "${DLL}.stamp.tmp" "${host_sha1} ${XBE_SHA1} ${config_sha1} ${dll_sha1}\n")
file(RENAME "${DLL}.stamp.tmp" "${DLL}.stamp")
