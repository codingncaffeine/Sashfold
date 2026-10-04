#pragma once

// An x86-64 assembler: the instructions the baseline compiler emits,
// encoded into a byte buffer, with labels whose uses are patched once
// they are bound. The encodings are the Intel manual's (volume 2); where
// an instruction has a shorter form for an operand (an 8-bit immediate,
// eax's own opcode) the shorter one is taken, as GNU as takes it, and
// tests/test_assembler.cpp holds each against the bytes as makes. Jumps
// to labels are always the 32-bit form. Portable C++: it only writes
// bytes, so it is built and tested on every machine.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sashfold::js::jit {

enum class Reg : std::uint8_t { rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15 };

// The condition codes, in their encoding order (the low nibble of Jcc).
enum class Cond : std::uint8_t {
    Overflow,
    NoOverflow,
    Below,
    AboveOrEqual,
    Equal,
    NotEqual,
    BelowOrEqual,
    Above,
    Sign,
    NoSign,
    Parity,
    NoParity,
    Less,
    GreaterOrEqual,
    LessOrEqual,
    Greater,
};

// A memory operand: [base + index * scale + displacement].
struct Mem {
    Reg base = Reg::rax;
    std::int32_t displacement = 0;
    bool indexed = false;
    Reg index = Reg::rax;
    std::uint8_t scale = 1; // 1, 2, 4 or 8

    static Mem at(Reg base, std::int32_t displacement = 0) { return Mem { base, displacement, false, Reg::rax, 1 }; }
    static Mem at_index(Reg base, Reg index, std::uint8_t scale, std::int32_t displacement = 0)
    {
        return Mem { base, displacement, true, index, scale };
    }
};

// A place in the code, named before it is known; used by jumps and
// rip-relative addresses.
class Label {
public:
    bool valid() const { return m_id != unset; }

private:
    friend class AssemblerX64;
    static constexpr std::uint32_t unset = 0xFFFFFFFFu;
    std::uint32_t m_id = unset;
};

class AssemblerX64 {
public:
    Label label();
    void bind(Label);
    bool bound(Label) const;
    std::uint32_t offset_of(Label) const; // once bound
    std::size_t size() const { return m_bytes.size(); }
    std::vector<std::uint8_t> const& bytes() const { return m_bytes; }
    // Every use of every label patched; false when one used was never
    // bound, or a distance does not fit 32 bits.
    bool finish();

    void push(Reg);
    void pop(Reg);
    void mov(Reg destination, Reg source); // 64-bit
    void mov(Reg destination, Mem source); // 64-bit load
    void mov(Mem destination, Reg source); // 64-bit store
    void mov_imm64(Reg destination, std::uint64_t value); // movabs
    void mov32(Reg destination, Mem source); // 32-bit load, zero-extended
    void mov32(Mem destination, std::uint32_t value); // 32-bit store of an immediate
    void lea(Reg destination, Mem source);
    void lea(Reg destination, Label); // rip-relative
    void mov(Mem destination, std::int32_t value); // 64-bit store, the immediate sign-extended
    void mov32(Reg destination, std::uint32_t value); // zero-extended
    void mov32(Reg destination, Reg source); // zero-extended
    void mov32(Mem destination, Reg source);
    void movzx8(Reg destination, Reg source); // the source's low byte, zero-extended
    void add(Reg destination, std::int32_t value); // 64-bit
    void sub(Reg destination, std::int32_t value); // 64-bit
    void add(Mem destination, std::int32_t value); // 64-bit
    void cmp(Reg, std::int32_t value); // 64-bit, the immediate sign-extended
    void and_(Reg, std::int32_t value); // 64-bit, the immediate sign-extended
    void or_(Reg, std::int32_t value); // 64-bit, the immediate sign-extended
    void cmp32(Reg, std::int32_t value);
    void cmp32(Mem, std::int32_t value);
    void add32(Reg, std::int32_t value);
    void sub32(Reg, std::int32_t value);
    void sub32(Mem, std::int32_t value);
    // Register to register, 64-bit and 32-bit (a 32-bit result clears the
    // upper half). The first operand is the destination.
    void add(Reg, Reg);
    void sub(Reg, Reg);
    void and_(Reg, Reg);
    void or_(Reg, Reg);
    void xor_(Reg, Reg);
    void cmp(Reg, Reg);
    void test(Reg, Reg);
    void cmp(Reg, Mem);
    void add32(Reg, Reg);
    void sub32(Reg, Reg);
    void and32(Reg, Reg);
    void or32(Reg, Reg);
    void xor32(Reg, Reg);
    void cmp32(Reg, Reg);
    void test32(Reg, Reg);
    void imul32(Reg destination, Reg source);
    void shl32_cl(Reg); // by cl
    void sar32_cl(Reg); // by cl
    void cdq(); // edx from eax's sign
    void idiv32(Reg); // edx:eax by the register: eax the quotient, edx the remainder
    // A byte of memory against an immediate.
    void cmp8(Mem, std::uint8_t value);
    void test8(Mem, std::uint8_t value);
    void or8(Mem, std::uint8_t value);
    void set(Cond, Reg); // the register's low byte, 1 when the condition holds
    void call(Reg);
    void call(Label); // through a 64-bit address at the label, rip-relative
    void jmp(Label);
    void jmp(Mem); // through a 64-bit address in memory
    void j(Cond, Label);
    void ret();
    void int3();
    // Pads with int3 to a multiple of `alignment` (a power of two).
    void align(std::size_t alignment);
    // Data in the code: a byte, a 32-bit word, a 64-bit word.
    void emit_u8(std::uint8_t value) { byte(value); }
    void emit_u32(std::uint32_t value) { u32(value); }
    void emit_u64(std::uint64_t);

private:
    void byte(std::uint8_t value) { m_bytes.push_back(value); }
    void u32(std::uint32_t value);
    // The REX prefix: W for a 64-bit operand, R, X and B the high bits of
    // the ModRM reg field, the SIB index and the base (or ModRM rm). None
    // is written when every bit is clear.
    void rex(bool wide, std::uint8_t reg, std::uint8_t index, std::uint8_t base);
    void rex_mem(bool wide, std::uint8_t reg, Mem const&);
    // ModRM (and SIB and displacement) for a memory operand.
    void modrm_mem(std::uint8_t reg, Mem const&);
    void modrm_reg(std::uint8_t reg, std::uint8_t rm) { byte(static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))); }
    // An instruction of the 81/83 group (/digit) on a register or memory.
    void group1(std::uint8_t digit, bool wide, Reg, std::int32_t value);
    void group1(std::uint8_t digit, bool wide, Mem const&, std::int32_t value);
    // A register-to-register instruction `opcode /r` with the destination
    // in r/m: add 01, or 09, and 21, sub 29, xor 31, cmp 39, test 85, mov 89.
    void alu(std::uint8_t opcode, bool wide, Reg destination, Reg source);
    // An instruction on a byte register: spl, bpl, sil and dil need a REX
    // prefix, even an empty one, or they would read as ah, ch, dh and bh.
    void rex_byte(std::uint8_t reg, std::uint8_t base);
    // A 32-bit displacement to a label, measured from the end of the
    // instruction, whose last `trailing` bytes follow the displacement.
    void label_use(Label, std::uint32_t trailing = 0);

    struct Use {
        std::uint32_t at = 0; // the displacement's offset
        std::uint32_t from = 0; // what it is measured from: the instruction's end
    };
    struct LabelState {
        std::int64_t offset = -1;
        std::vector<Use> uses;
    };
    std::vector<std::uint8_t> m_bytes;
    std::vector<LabelState> m_labels;
};

}
