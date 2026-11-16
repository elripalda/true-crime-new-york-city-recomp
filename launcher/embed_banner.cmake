file(READ "${INPUT}" bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
# Keep generated source lines short for compiler diagnostics and source limits.
string(REPLACE "," ",\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "${bytes}")
