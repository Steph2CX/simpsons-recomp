# Writes OUT (shader_translator_version.h): a hash of the shader translator's sources and of the DXC library.
# The cache of translated shaders (renderer.cpp) is kept as long as this hash does not change.
string(REPLACE "," ";" files "${FILES}")
set(hashes "")
foreach(file IN LISTS files)
    file(SHA1 "${file}" hash)
    string(APPEND hashes "${hash}")
endforeach()
string(SHA1 total "${hashes}")
string(SUBSTRING "${total}" 0 16 version)
string(TOUPPER "${version}" version)
file(WRITE "${OUT}.tmp" "#define SHADER_TRANSLATOR_VERSION \"${version}\"\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
