#!/usr/bin/env python3
"""Finds branches that XenonRecomp could not resolve ("// ERROR <addr>" in the generated code: tail
calls or conditional branches to addresses that are not function starts) and prints `functions`
entries for the config. Size = distance to the next known function start.
Usage: fix_error_targets.py <ppc dir>"""
import glob, os, re, sys

ppc = sys.argv[1]
targets = set()
for path in glob.glob(os.path.join(ppc, 'ppc_recomp.*.cpp')):
    for m in re.finditer(r'ERROR ([0-9A-F]{8})', open(path).read()):
        targets.add(int(m.group(1), 16))

starts = sorted(int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(os.path.join(ppc, 'ppc_func_mapping.cpp')).read()))
import bisect
for t in sorted(targets):
    i = bisect.bisect_right(starts, t)
    if i < len(starts):
        size = starts[i] - t
        print('    { address = 0x%08X, size = 0x%X }, # cible de branchement non resolue' % (t, size))
    else:
        print('# 0x%08X : pas de fonction suivante' % t, file=sys.stderr)
