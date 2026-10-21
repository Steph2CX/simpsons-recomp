#!/usr/bin/env python3
"""Retire l'ecran de mentions legales Fox du demarrage (frontend/frontend.str : SToc + blocs RefPack).

Usage : retirer_fox.py <frontend.str du disque> <sortie.str>

Le dernier gros bloc du fichier contient l'interface (APT, equivalent Flash d'EA). Le clip "LegalScreen" a 540 images :
le texte Fox de l'image 0 a 263 (~4,4 s, etiquette "BypassLegal" a 263), puis il s'efface jusqu'a l'image 287. Le script
de l'image 0 est remplace par gotoAndPlay(287) : le jeu passe directement a la suite (logo EA, titre).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refpack import compress, decompress  # noqa: E402
from modifier_textes import max_output_lead, round_up  # noqa: E402

ENTRY = 0x33C  # entree du bloc dans l'en-tete SToc : taille decompressee, tampon, taille des donnees, decalage de chargement
CHUNK = 0x5A1000  # position du bloc dans le fichier
APT = 0x6F8F0  # debut des donnees APT dans le bloc (les pointeurs du script sont relatifs a ce debut)
SCRIPT = 0x8E194  # script de l'image 0 de LegalScreen (commence par ActionConstantPool 0x88)
TARGET_FRAME = 287


def main():
    source, target = sys.argv[1:3]
    raw = open(source, 'rb').read()
    decompressed_size, _, data_size, _ = struct.unpack_from('>4I', raw, ENTRY + 8)
    data = bytearray(decompress(raw, CHUNK)[0])
    if len(data) != decompressed_size or data[SCRIPT] != 0x88 or data[0x8E240:0x8E24B] != b'BypassLegal':
        raise SystemExit('frontend.str inattendu (pas le disque francais ?)')

    # ActionGotoFrame (operande alignee sur 4 octets), ActionPlay, ActionEnd
    patch = bytes([0x81, 0, 0, 0]) + struct.pack('>I', TARGET_FRAME) + bytes([0x06, 0x00])
    data[SCRIPT:SCRIPT + len(patch)] = patch

    # Bloc recompresse, decompresse sur place par le jeu : donnees chargees a la fin du tampon
    packed = compress(bytes(data))
    new_size = round_up(len(packed), 0x800)
    load_offset = round_up(max_output_lead(packed), 0x20)
    buffer_size = max(round_up(load_offset + new_size, 0x100), round_up(decompressed_size, 0x100))
    header = bytearray(raw[:0x2800])
    struct.pack_into('>3I', header, ENTRY + 12, buffer_size, new_size, buffer_size - new_size)
    output = (bytes(header) + raw[0x2800:CHUNK] + packed + b'\0' * (new_size - len(packed)) +
              raw[CHUNK + data_size:])

    assert decompress(output, CHUNK)[0] == bytes(data), 'relecture incorrecte'
    os.makedirs(os.path.dirname(os.path.abspath(target)), exist_ok=True)
    open(target, 'wb').write(output)
    print(f'ecran Fox retire -> {target}', file=sys.stderr)


if __name__ == '__main__':
    main()
