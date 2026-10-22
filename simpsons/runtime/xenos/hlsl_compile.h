#pragma once

// Compilation en SPIR-V des shaders propres au runtime (passes d'affichage), avec le DXC de la traduction.
// A part de xenos_translate pour ne pas changer l'empreinte du traducteur (cache des shaders traduits).

#include <cstdint>
#include <string>
#include <vector>

// Vide en cas d'echec (erreurs de DXC sur stderr). Les vertex shaders sont compiles avec -fvk-invert-y.
std::vector<uint32_t> CompileHlslToSpirv(const std::string& source, bool isPixelShader);
