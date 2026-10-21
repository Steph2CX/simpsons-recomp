#!/usr/bin/env python3
"""Find inline 0xE0000000-view physical offset computations (bit 0x1000) in text.asm.
Prints, for each `rlwinm X, Y, 0, 19, 19`, the instruction that defined Y."""
import re, sys, collections
lines = open(sys.argv[1]).read().splitlines()
ins = []
for l in lines:
    m = re.match(r'([0-9a-f]{8}):\s+(\S+)\s*(.*)', l.strip())
    if m:
        ins.append((int(m.group(1), 16), m.group(2), [a.strip() for a in m.group(3).split(',')] if m.group(3) else []))
kinds = collections.Counter()
for i, (addr, op, args) in enumerate(ins):
    if op == 'rlwinm' and args[2:] == ['0', '19', '19']:
        src = args[1]
        for j in range(i - 1, max(i - 60, 0), -1):
            a2, op2, args2 = ins[j]
            if op2 in ('blr', 'b') : break
            if args2 and args2[0] == src and not op2.startswith(('st', 'cmp', 'b', 'dcb')):
                kind = op2 + ' ' + ', '.join(args2[1:]) if op2 != 'addi' else 'addi _, %s' % args2[2]
                if op2 == 'addi': kind = 'addi %s' % args2[2]
                kinds[op2 if op2 != 'addi' else kind] += 1
                print('%08x %s %s <- %08x %s %s' % (addr, op, ','.join(args), a2, op2, ','.join(args2)))
                break
        else:
            print('%08x %s %s <- ?' % (addr, op, ','.join(args)))
print(kinds, file=sys.stderr)
