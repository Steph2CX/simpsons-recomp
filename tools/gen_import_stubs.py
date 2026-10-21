"""Genere des stubs faibles pour chaque fonction systeme importee par le jeu.

Usage: gen_import_stubs.py imports.txt > import_stubs.cpp
Chaque stub affiche l'appel et renvoie 0 ; les vraies implementations
(fonctions fortes du meme nom dans kernel.cpp) les remplacent a l'edition de liens.
"""
import sys

names = [line.strip() for line in open(sys.argv[1]) if line.strip()]
print('// Fichier genere par tools/gen_import_stubs.py - ne pas modifier\n#include "runtime.h"\n')
for name in names:
    print(f'PPC_WEAK_FUNC(__imp__{name})\n{{\n    LogImportCall("{name}", ctx, false);\n    ctx.r3.u64 = 0;\n}}\n')
