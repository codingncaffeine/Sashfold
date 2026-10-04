#pragma once

// An AArch64 assembler: the instructions the baseline compiler emits on
// ARM, encoded into words, with labels whose uses are patched once they are
// bound. The encodings are the Arm Architecture Reference Manual's (A64);
// tests/test_assembler_a64.cpp holds each against the words clang's
// assembler makes of the same instruction. Portable C++: it only writes
// words, so it is built and tested on every machine.
//
// A register is a number: x0–x30, and 31, which an instruction reads as the
// stack pointer or as the zero register as the manual says it does (the
// names `sp` and `zr` are both 31). The `32` forms work on the low halves
// (w registers) and clear the upper halves of what they write.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold::js::jit {

enum class XReg : std::uint8_t {
    x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15,
    x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30,
    sp = 31,
    zr = 31,
};

// The condition codes, in their encoding order; a condition's inverse is
// the code with its low bit flipped.
enum class ACond : std::uint8_t { eq, ne, hs, lo, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, al };

class ALabel {
public:
    bool valid() const { return m_id != unset; }

private:
    friend class AssemblerA64;
    static constexpr std::uint32_t unset = 0xFFFFFFFFu;
    std::uint32_t m_id = unset;
};

// The N:immr:imms field of a logical instruction's immediate (a bitmask
// immediate: a rotated run of ones, repeated), or nothing when the value is
// not one — 0, all ones, and most others are not.
std::optional<std::uint32_t> bitmask_immediate(std::uint64_t value, bool wide);

class AssemblerA64 {
public:
    ALabel label();
    void bind(ALabel);
    bool bound(ALabel) const;
    std::uint32_t offset_of(ALabel) const; // in bytes, once bound
    std::size_t size() const { return m_words.size() * 4; }
    std::vector<std::uint32_t> const& words() const { return m_words; }
    void reserve(std::size_t instructions, std::size_t labels);
    // Every use of every label patched; false when one used was never
    // bound, or a distance does not fit its instruction's field.
    bool finish();

    // ---- moves
    void mov(XReg destination, XReg source); // orr from zr, or add #0 when either is sp
    void mov32(XReg destination, XReg source);
    void movz(XReg, std::uint16_t value, unsigned shift); // shift 0, 16, 32 or 48
    void movk(XReg, std::uint16_t value, unsigned shift);
    void mov_imm64(XReg, std::uint64_t value); // the fewest movz/movk
    void mov_imm32(XReg, std::uint32_t value);

    // ---- loads and stores: an offset the scaled form takes (a multiple of
    // the size, below 4096 of them), else the unscaled one (−256…255); the
    // caller keeps to those
    void ldr(XReg, XReg base, std::int32_t offset); // 64-bit
    void str(XReg, XReg base, std::int32_t offset);
    void ldr32(XReg, XReg base, std::int32_t offset); // zero-extended
    void str32(XReg, XReg base, std::int32_t offset);
    void ldrb(XReg, XReg base, std::int32_t offset);
    void strb(XReg, XReg base, std::int32_t offset);
    void ldr_indexed(XReg, XReg base, XReg index); // [base, index, lsl #3]
    void str_indexed(XReg, XReg base, XReg index);
    void ldr_literal(XReg, ALabel); // the 64-bit word at the label
    void str_post(XReg, XReg base, std::int32_t offset); // [base], #offset: base moves after
    void str_pre(XReg, XReg base, std::int32_t offset); // [base, #offset]!: base moves first
    void ldr_post(XReg, XReg base, std::int32_t offset);
    void stp_pre(XReg first, XReg second, XReg base, std::int32_t offset); // [base, #offset]!
    void ldp_post(XReg first, XReg second, XReg base, std::int32_t offset); // [base], #offset
    void adr(XReg, ALabel);
    static bool fits_load(std::int32_t offset, unsigned size);

    // ---- arithmetic and logic; immediates of 12 bits, or 12 shifted by 12
    void add(XReg d, XReg n, std::uint32_t immediate);
    void sub(XReg d, XReg n, std::uint32_t immediate);
    void adds32(XReg d, XReg n, std::uint32_t immediate);
    void subs32(XReg d, XReg n, std::uint32_t immediate);
    void cmp(XReg n, std::uint32_t immediate);
    void cmp32(XReg n, std::uint32_t immediate);
    void add(XReg d, XReg n, XReg m);
    void sub(XReg d, XReg n, XReg m);
    void adds32(XReg d, XReg n, XReg m);
    void subs32(XReg d, XReg n, XReg m);
    void cmp(XReg n, XReg m);
    void cmp32(XReg n, XReg m);
    void cmp_sxtw(XReg n, XReg m); // n against m's low half sign-extended
    void and_(XReg d, XReg n, XReg m);
    void orr(XReg d, XReg n, XReg m);
    void eor(XReg d, XReg n, XReg m);
    void and32(XReg d, XReg n, XReg m);
    void orr32(XReg d, XReg n, XReg m);
    void eor32(XReg d, XReg n, XReg m);
    void tst(XReg n, XReg m);
    void tst32(XReg n, XReg m);
    // With a bitmask immediate; false (and nothing written) when the value
    // is not one.
    [[nodiscard]] bool and_(XReg d, XReg n, std::uint64_t immediate);
    [[nodiscard]] bool orr(XReg d, XReg n, std::uint64_t immediate);
    [[nodiscard]] bool tst(XReg n, std::uint64_t immediate);
    void mul32(XReg d, XReg n, XReg m);
    void smull(XReg d, XReg n, XReg m); // 64-bit product of two 32-bit halves
    void sdiv32(XReg d, XReg n, XReg m);
    void msub32(XReg d, XReg n, XReg m, XReg a); // a − n × m
    void lslv32(XReg d, XReg n, XReg m);
    void asrv32(XReg d, XReg n, XReg m);
    void cset(XReg d, ACond); // 1 when the condition holds, else 0

    // ---- control
    void b(ALabel);
    void b(ACond, ALabel);
    void cbz(XReg, ALabel);
    void cbnz(XReg, ALabel);
    void cbz32(XReg, ALabel);
    void cbnz32(XReg, ALabel);
    void bl(ALabel);
    void blr(XReg);
    void br(XReg);
    void ret();
    void brk(std::uint16_t);

    // ---- data in the code
    void align(std::size_t bytes); // with brk
    void emit_u32(std::uint32_t word) { m_words.push_back(word); }
    void emit_u64(std::uint64_t);

private:
    enum class Field : std::uint8_t { Branch26, Branch19, Adr21 };
    struct Use {
        std::uint32_t label = 0;
        std::uint32_t at = 0; // the instruction's word
        Field field = Field::Branch26;
    };
    void word(std::uint32_t value) { m_words.push_back(value); }
    void label_use(ALabel, Field);
    void add_sub_immediate(std::uint32_t base, bool wide, XReg d, XReg n, std::uint32_t immediate);
    void load_store(std::uint32_t scaled, std::uint32_t unscaled, unsigned size, XReg t, XReg base, std::int32_t offset);

    std::vector<std::uint32_t> m_words;
    std::vector<std::int64_t> m_labels; // each label's word, -1 until bound
    std::vector<Use> m_uses;
};

}
