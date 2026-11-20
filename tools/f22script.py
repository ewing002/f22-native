#!/usr/bin/env python3
"""F-22 Lightning II AI script disassembler (based on the bytecode interpreter around 0x36d17 in F22.EXE).

The VM: the aircraft's (ebp) script context at ebp+0xa7 = esi:
  [esi+0x00..0x0f]  4 32-bit registers (r0..r3; instructions access address esi + n*4)
  [esi+0x10]        return address (CALL / RET)      [esi+0x14] flags (x86 flags + bit 1: "condition true")
  [esi+0x18]        PC (address of the script's 4-byte instructions)
An instruction is 4 bytes: op, a, b, c. The handler returns with CF: carry = continue with the next instruction in the same
step, no carry = the script stops in this step (the AI returns control).
Relative jumps are 16 / 24-bit signed words: offset = word * 4 (+4 if positive).

Usage:  py tools/f22script.py [addr_hex ...]    (default: all known scripts)
"""
import struct
import sys

EXE = 'extracted/F22_decrypted.bin'

NAMES = {
    0x00: 'END', 0x01: 'JMP', 0x02: 'CALL', 0x03: 'RET', 0x04: 'FLAGS', 0x05: 'JCLR', 0x06: 'JSET', 0x07: 'LD0',
    0x08: 'LDR', 0x0a: 'STF', 0x0b: 'C_25b31', 0x0c: 'C_25cde', 0x0d: 'WPSET', 0x0e: 'TGT', 0x0f: 'C_25dab',
    0x10: 'SPD', 0x11: 'C_25d48', 0x12: 'DEC', 0x13: 'RND', 0x14: 'C_26142', 0x15: 'LDF', 0x16: 'ADD', 0x17: 'SHL',
    0x18: 'C_26028', 0x19: 'A_25e4b', 0x1a: 'ACT', 0x1b: 'RADIO', 0x1c: 'CMP', 0x1d: 'JGE', 0x1e: 'AGL', 0x1f: 'C_25b9f',
    0x20: 'LEADER', 0x21: 'C_25be6', 0x22: 'TKOFF', 0x23: 'C_25c5a', 0x24: 'CLS', 0x25: 'FORM', 0x26: 'A_26d32',
    0x27: 'A_26d9a', 0x28: 'STATE', 0x29: 'VMAX', 0x2a: 'VTGT',
}

# script entries (addresses referenced in the EXE)
ENTRIES = [0x374c0, 0x374e8, 0x37528, 0x3753c, 0x37548, 0x37550, 0x3759c, 0x375a8, 0x376b0, 0x37970, 0x3796c, 0x37998,
           0x379ac, 0x379c4, 0x379d0]


def sx(v, bits):
    return v - (1 << bits) if v >= 1 << (bits - 1) else v


def dis(d, a):
    op, b1, b2, b3 = d[a:a + 4]
    n = NAMES.get(op, f'OP{op:02x}')
    arg = f'{b1:02x} {b2:02x} {b3:02x}'
    extra = ''
    if op in (0x01, 0x02, 0x1d):
        w = sx((b1 << 16 | b2 << 8 | b3), 24) * 4
        extra = f'-> {a + (w if w < 0 else w + 4):#x}'
    elif op in (0x05, 0x06):
        w = sx((b2 << 8 | b3), 16) * 4
        extra = f'if flags&{b1:#x} {"clear" if op == 5 else "set"} -> {a + (w if w < 0 else w + 4):#x}'
    elif op == 0x08:
        extra = f'r{b1} = {b2 << 8 | b3}'
    elif op == 0x07:
        extra = f'r0 = {sx(b1, 8) << 16 | b2 << 8 | b3}'
    elif op == 0x28:
        extra = f'state = {b1}'
    return f'{a:#07x}  {n:<8} {arg:<9} {extra}'


def main():
    d = open(EXE, 'rb').read()
    starts = [int(x, 16) for x in sys.argv[1:]] or ENTRIES
    lo, hi = min(starts), max(starts) + 0x100
    if len(sys.argv) <= 1:
        lo, hi = 0x374c0, 0x37a40
    marks = {e: f'\n; --- entry {e:#x}' for e in starts}
    for a in range(lo, hi, 4):
        if a in marks:
            print(marks[a])
        print(dis(d, a))


if __name__ == '__main__':
    main()
