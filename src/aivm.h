// Interpreter of the original AI bytecode (F22.EXE 0x36d17; instruction set and scripts: tools/f22script.py, native/AI_VM.md).
// The VM core is here; instructions touching the aircraft state (target, speed, fields, condition and action routines) are implemented by the Host,
// so the helper routines can be swapped one by one, verified against the original.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sim {

struct AiCtx {                      // ebp + 0xa7
  int32_t r[4] = {0, 0, 0, 0};      // esi + 0 .. 0xc
  uint32_t ret = 0;                 // esi + 0x10: CALL / RET
  uint32_t flags = 0;               // esi + 0x14: x86 flags (CF = 1, ZF = 0x40, SF = 0x80); CF is the "condition met" flag
  uint32_t pc = 0;                  // esi + 0x18: address of the script instruction in the EXE image
  uint32_t pending = 0;             // esi + ... ebp + 0xeb: new entry (0: none)
};

enum : uint32_t { F_CF = 1, F_ZF = 0x40, F_SF = 0x80 };

// The Host: instructions that handle the aircraft state. Return: true = the interpreter continues with the next instruction (carry),
// false = the script stops in this step. The instruction advances c.pc itself when done.
struct AiHost {
  virtual ~AiHost() {}
  virtual bool op(int code, const uint8_t* ins, AiCtx& c) = 0;
  virtual uint32_t readWord(uint32_t off) = 0;
};

class AiVm {
 public:
  explicit AiVm(const std::vector<uint8_t>& image) : mem_(image) {}
  // One AI step: takes over the pending entry, then runs instructions until a handler stops
  // (the original 0x36d17). maxSteps: guard against infinite loops. Returns: the number of instructions executed.
  int run(AiCtx& c, AiHost& h, int maxSteps = 256) const;
  static const char* name(int op);
  std::string disasm(uint32_t addr) const;
  const std::vector<uint8_t>& mem() const { return mem_; }
 private:
  const std::vector<uint8_t>& mem_;
};

}  // namespace sim
