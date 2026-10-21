// Outil de test : traduit des fichiers de microcode Xenos (vertex et pixel) et affiche le resultat.
// Usage: xenos_test [--hlsl] fichier.bin...
#include "xenos_translate.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

int main(int argc, char** argv)
{
    bool printHlsl = false;
    int ok = 0, total = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--hlsl") == 0)
        {
            printHlsl = true;
            continue;
        }
        std::ifstream file(argv[i], std::ios::binary);
        std::vector<uint8_t> microcode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        bool pixel = strstr(argv[i], "_ps") != nullptr;

        XenosShader shader = TranslateXenosShader(microcode.data(), uint32_t(microcode.size()), pixel);
        total++;
        ok += shader.valid;
        printf("%s (%s) : %s, %zu mots SPIR-V, %zu attributs, sorties 0x%X %s\n", argv[i], pixel ? "pixel" : "vertex",
            shader.valid ? "OK" : "ECHEC", shader.spirv.size(), shader.attributes.size(), shader.pixelOutputs, shader.error.c_str());
        for (auto& a : shader.attributes)
            printf("    location %u : constante %u, format %u, decalage %u, pas %u%s%s\n", a.location, a.fetchConstant,
                a.format, a.offsetDwords, a.strideDwords, a.isSigned ? ", signe" : "", a.isNormalized ? ", normalise" : "");
        if (printHlsl || !shader.valid)
        {
            fwrite(shader.hlsl.data(), 1, shader.hlsl.size(), stdout);
            printf("\n");
        }
    }
    printf("%d/%d shaders traduits\n", ok, total);
    return ok == total ? 0 : 1;
}
