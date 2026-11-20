#!/usr/bin/env python3
"""Unpacking F22.EXE for analysis (does NOT modify the game).

Layout:
  0x00000  MZ stub (16 bites)
  0x0053D  second MZ: FlashTek X-386 DOS extender (Doug Huffman) + 16-bit loader
  0x0C8ED  start of the 32-bit flat program image (load address 0)
           - 0x2496..0x2800: encryption/check stub (obfuscated with junk bytes)
           - 0x24B8: checksum, 0x24C4..0x8A9A6: encrypted code+data
  The stub decrypts the image with an XOR+rotate based key, then through the second
  section at 0x8A7B1 it jumps to 0x5BE2C (C runtime startup).

We do not reimplement the decryption by hand: the stub is run in the Unicorn CPU emulator
and the memory is dumped at the real entry point.

Requires: py -m pip install unicorn
Output: flat memory image where file offset = virtual address (from 0).
"""
import argparse
import os
import sys

IMAGE_FILE_OFFSET = 0xC8ED
STUB_ENTRY = 0x2514
STACK_TOP = 0x21C0
ALLOWED = [(0x2300, 0x2800), (0x8A000, 0x8B000)]  # the two unpacker sections
DUMP_SIZE = 0x200000


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(root, "F22.EXE"))
    ap.add_argument("-o", "--out", default=os.path.join(root, "extracted", "F22_decrypted.bin"))
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    try:
        from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_INTR
        from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EFLAGS
    except ImportError:
        sys.exit("The unicorn package is missing: py -m pip install unicorn")

    with open(a.exe, "rb") as f:
        img = f.read()[IMAGE_FILE_OFFSET:]
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(0, 64 * 1024 * 1024)
    mu.mem_write(0, img)
    mu.reg_write(UC_X86_REG_ESP, STACK_TOP)
    state = {"entry": None}

    def on_int(uc, intno, _):
        # The stub only makes DOS/X-386 calls (vector setup, page attributes);
        # we report them as successful.
        uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) & ~1)

    def on_code(uc, addr, size, _):
        if not any(lo <= addr < hi for lo, hi in ALLOWED):
            state["entry"] = addr
            uc.emu_stop()

    mu.hook_add(UC_HOOK_INTR, on_int)
    mu.hook_add(UC_HOOK_CODE, on_code)
    try:
        mu.emu_start(STUB_ENTRY, 0xFFFFFFFF, count=50_000_000)
    except UcError as e:
        sys.exit(f"Emulation error: {e} @ {mu.reg_read(UC_X86_REG_EIP):#x}")
    if state["entry"] is None:
        sys.exit("The stub did not reach the program (different EXE version?)")
    dump = bytes(mu.mem_read(0, DUMP_SIZE))
    if b"bin01.bin" not in dump:
        sys.exit("The decryption appears to have failed (no readable text)")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(dump)
    print(f"Entry point: {state['entry']:#x}; output: {a.out}")


if __name__ == "__main__":
    main()
