#include "aivm.h"

#include <cstdio>

namespace sim {

namespace {
int32_t sx(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }
// relative jump: word * 4, +4 for positive (0x36dcd..0x36dd5)
int32_t rel(int32_t w) { int32_t o = w * 4; return o < 0 ? o : o + 4; }
void setFlags(AiCtx& c, uint32_t res, bool carry) {
  c.flags = (res == 0 ? F_ZF : 0) | (int32_t(res) < 0 ? F_SF : 0) | (carry ? F_CF : 0);
}
}  // namespace

const char* AiVm::name(int op) {
  static const char* N[0x2b] = {"END", "JMP", "CALL", "RET", "FLAGS", "JCLR", "JSET", "LD0", "LDR", "?", "STF", "C_25b31", "C_25cde", "WPSET",
                                "TGT", "C_25dab", "SPD", "C_25d48", "DEC", "RND", "C_26142", "LDF", "ADD", "SHL", "C_26028", "A_25e4b",
                                "ACT", "RADIO", "CMP", "JGE", "AGL", "C_25b9f", "LEADER", "C_25be6", "TKOFF", "C_25c5a", "CLS", "FORM",
                                "A_26d32", "A_26d9a", "STATE", "VMAX", "VTGT"};
  return op >= 0 && op < 0x2b ? N[op] : "OP?";
}

std::string AiVm::disasm(uint32_t a) const {
  char b[96];
  if (a + 4 > mem_.size()) return "?";
  const uint8_t* p = &mem_[a];
  std::snprintf(b, sizeof b, "%#07x %-8s %02x %02x %02x", a, name(p[0]), p[1], p[2], p[3]);
  return b;
}

// The interpreter loop (0x36d17): a pending entry replaces the PC; each instruction continues per the handler result
// (carry) or stops. The VM's own instructions (control flow, registers, arithmetic) are here.
int AiVm::run(AiCtx& c, AiHost& h, int maxSteps) const {
  if (c.pending) { c.pc = c.pending; c.pending = 0; }
  int n = 0;
  while (n++ < maxSteps && c.pc + 4 <= mem_.size()) {
    const uint8_t* p = &mem_[c.pc];
    int op = p[0], a = p[1], b = p[2], d = p[3];
    bool cont = true;
    switch (op) {
      case 0x00: c.pc += 4; cont = false; break;                                           // END: stops in this step
      case 0x01: c.pc += uint32_t(rel(sx(uint32_t(a) << 16 | b << 8 | d, 24))); break;     // JMP
      case 0x02: c.ret = c.pc; c.pc += uint32_t(rel(sx(uint32_t(a) << 16 | b << 8 | d, 24))); break;   // CALL
      case 0x03: c.pc = c.ret + 4; break;                                                  // RET
      case 0x04: c.flags = ((uint32_t(b) << 8 | uint32_t(a & b)) & (c.flags & ~uint32_t(a))); c.pc += 4; break;   // 0x36e08 verbatim (the scripts do not use it)
      case 0x05: if ((a & c.flags) == 0) c.pc += uint32_t(rel(sx(uint32_t(b) << 8 | d, 16))); else c.pc += 4; break;   // JCLR: jumps if the flag(s) are not set
      case 0x06: if ((a & c.flags) != 0) c.pc += uint32_t(rel(sx(uint32_t(b) << 8 | d, 16))); else c.pc += 4; break;
      case 0x07: c.r[0] = int32_t(sx(uint32_t(a), 8) * 65536) | (b << 8) | d; c.pc += 4; break;   // LD0
      case 0x08: c.r[a & 3] = (b << 8) | d; c.pc += 4; break;                                    // LDR
      case 0x12: { int32_t& r = c.r[a & 3]; uint32_t res = uint32_t(r - 1); bool cf = r == 0; r = int32_t(res); setFlags(c, res, cf); c.pc += 4; break; }   // DEC
      case 0x16: { int32_t& r = c.r[a & 3]; int64_t s = int64_t(uint32_t(r)) + uint32_t(c.r[b & 3]); r = int32_t(uint32_t(s)); setFlags(c, uint32_t(s), s > 0xFFFFFFFFLL); c.pc += 4; break; }  // ADD
      case 0x17: { int32_t& r = c.r[a & 3]; uint32_t res = uint32_t(r) << (b & 31); bool cf = b ? ((uint32_t(r) >> (32 - (b & 31))) & 1) : false; r = int32_t(res); setFlags(c, res, cf); c.pc += 4; break; }  // SHL
      case 0x1c: { uint32_t x = uint32_t(c.r[a & 3]), y = uint32_t(c.r[b & 3]); setFlags(c, x - y, x < y); c.pc += 4; break; }   // CMP
      case 0x1d: {                                                                         // JGE (the original: compares 0x400 and SF<<3)
        bool lt = ((c.flags & 0x400) == ((c.flags & F_SF) << 3));
        if (lt) c.pc += uint32_t(rel(sx(uint32_t(a) << 16 | b << 8 | d, 24))); else c.pc += 4;
        break;
      }
      default: cont = h.op(op, p, c); break;                                               // instructions that touch the aircraft state
    }
    if (!cont) break;
  }
  return n;
}

}  // namespace sim
