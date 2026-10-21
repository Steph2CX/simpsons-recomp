// xexdump: charge un XEX via XenonUtils, liste les sections et symboles,
// cherche les motifs des fonctions save/restore et sauvegarde l'image brute.
#include <cstdio>
#include <cstring>
#include <vector>
#include <file.h>
#include <image.h>

struct Pattern
{
    const char* name;
    std::vector<uint8_t> bytes;
};

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: xexdump <default.xex> [image.bin]\n");
        return 1;
    }

    auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        printf("Impossible de lire %s\n", argv[1]);
        return 1;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    printf("base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);

    for (auto& section : image.sections)
        printf("section %-10s base=0x%zX size=0x%X flags=%d\n", section.name.c_str(), section.base, section.size, section.flags);

    printf("symboles: %zu\n", image.symbols.size());
    for (auto& symbol : image.symbols)
        printf("sym 0x%zX %s\n", symbol.address, symbol.name.c_str());

    const Pattern patterns[] =
    {
        { "restgprlr_14", { 0xe9, 0xc1, 0xff, 0x68 } },
        { "savegprlr_14", { 0xf9, 0xc1, 0xff, 0x68 } },
        { "restfpr_14", { 0xc9, 0xcc, 0xff, 0x70 } },
        { "savefpr_14", { 0xd9, 0xcc, 0xff, 0x70 } },
        { "restvmx_14", { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x60, 0xce } },
        { "savevmx_14", { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x61, 0xce } },
        { "restvmx_64", { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x60, 0xcb } },
        { "savevmx_64", { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x61, 0xcb } },
    };

    for (auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        for (auto& pattern : patterns)
        {
            for (size_t i = 0; i + pattern.bytes.size() <= section.size; i += 4)
            {
                if (memcmp(section.data + i, pattern.bytes.data(), pattern.bytes.size()) == 0)
                    printf("match %-14s 0x%zX\n", pattern.name, section.base + i);
            }
        }
    }

    if (argc >= 3)
    {
        FILE* out = fopen(argv[2], "wb");
        if (out)
        {
            fwrite(image.data.get(), 1, image.size, out);
            fclose(out);
            printf("image sauvegardee: %s\n", argv[2]);
        }
    }

    return 0;
}
