// The AArch64 assembler (js/jit/AssemblerA64): each instruction's word
// against the word clang's assembler (`--target=aarch64-linux-gnu`) makes of
// the same instruction, its labels against branches clang resolved, and
// the bitmask immediates against values the manual's decoding gives.

#include "Test.h"

#include "js/jit/AssemblerA64.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::js::jit;

namespace {

std::string hex(std::vector<std::uint32_t> const& words)
{
    std::string out;
    for (std::uint32_t const word : words) {
        char buffer[12];
        std::snprintf(buffer, sizeof buffer, "%08x ", word);
        out += buffer;
    }
    if (!out.empty())
        out.pop_back();
    return out;
}

std::string assembled(std::function<void(AssemblerA64&)> const& write)
{
    AssemblerA64 a;
    write(a);
    CHECK(a.finish());
    return hex(a.words());
}

#define CHECK_ENCODING(expected, statement) CHECK_EQ(assembled([](AssemblerA64& a) { statement; }), std::string(expected))

void test_instructions()
{
    CHECK_ENCODING("aa0103e0", a.mov(XReg::x0, XReg::x1)); // mov x0, x1
    CHECK_ENCODING("910003fd", a.mov(XReg::x29, XReg::sp)); // mov x29, sp
    CHECK_ENCODING("910003bf", a.mov(XReg::sp, XReg::x29)); // mov sp, x29
    CHECK_ENCODING("2a0303e2", a.mov32(XReg::x2, XReg::x3)); // mov w2, w3
    CHECK_ENCODING("d2824684", a.movz(XReg::x4, 0x1234, 0)); // movz x4, #0x1234
    CHECK_ENCODING("d2aacf04", a.movz(XReg::x4, 0x5678, 16)); // movz x4, #0x5678, lsl #16
    CHECK_ENCODING("f2ffffc5", a.movk(XReg::x5, 0xfffe, 48)); // movk x5, #0xfffe, lsl #48
    CHECK_ENCODING("f2c00025", a.movk(XReg::x5, 0x1, 32)); // movk x5, #0x1, lsl #32
    CHECK_ENCODING("528acf06 72a24686", a.mov_imm32(XReg::x6, 0x12345678u)); // mov w6, #0x5678; movk w6, #0x1234, lsl #16
    CHECK_ENCODING("f9400420", a.ldr(XReg::x0, XReg::x1, 8)); // ldr x0, [x1, #8]
    CHECK_ENCODING("f97ffc20", a.ldr(XReg::x0, XReg::x1, 32760)); // ldr x0, [x1, #32760]
    CHECK_ENCODING("f85f8020", a.ldr(XReg::x0, XReg::x1, -8)); // ldr x0, [x1, #-8]
    CHECK_ENCODING("f8403020", a.ldr(XReg::x0, XReg::x1, 3)); // ldr x0, [x1, #3]
    CHECK_ENCODING("f9004a62", a.str(XReg::x2, XReg::x19, 144)); // str x2, [x19, #144]
    CHECK_ENCODING("f81f0262", a.str(XReg::x2, XReg::x19, -16)); // str x2, [x19, #-16]
    CHECK_ENCODING("b9400a83", a.ldr32(XReg::x3, XReg::x20, 8)); // ldr w3, [x20, #8]
    CHECK_ENCODING("b9000683", a.str32(XReg::x3, XReg::x20, 4)); // str w3, [x20, #4]
    CHECK_ENCODING("b85fc283", a.ldr32(XReg::x3, XReg::x20, -4)); // ldur w3, [x20, #-4]
    CHECK_ENCODING("3940a024", a.ldrb(XReg::x4, XReg::x1, 40)); // ldrb w4, [x1, #40]
    CHECK_ENCODING("3903fc24", a.strb(XReg::x4, XReg::x1, 255)); // strb w4, [x1, #255]
    CHECK_ENCODING("f86778c5", a.ldr_indexed(XReg::x5, XReg::x6, XReg::x7)); // ldr x5, [x6, x7, lsl #3]
    CHECK_ENCODING("f82778c5", a.str_indexed(XReg::x5, XReg::x6, XReg::x7)); // str x5, [x6, x7, lsl #3]
    CHECK_ENCODING("f80086e0", a.str_post(XReg::x0, XReg::x23, 8)); // str x0, [x23], #8
    CHECK_ENCODING("f81f86e5", a.str_post(XReg::x5, XReg::x23, -8)); // str x5, [x23], #-8
    CHECK_ENCODING("f81f0ffe", a.str_pre(XReg::x30, XReg::sp, -16)); // str x30, [sp, #-16]!
    CHECK_ENCODING("f84107fe", a.ldr_post(XReg::x30, XReg::sp, 16)); // ldr x30, [sp], #16
    CHECK_ENCODING("a9bf7bfd", a.stp_pre(XReg::x29, XReg::x30, XReg::sp, -16)); // stp x29, x30, [sp, #-16]!
    CHECK_ENCODING("a9bc53f3", a.stp_pre(XReg::x19, XReg::x20, XReg::sp, -64)); // stp x19, x20, [sp, #-64]!
    CHECK_ENCODING("a8c17bfd", a.ldp_post(XReg::x29, XReg::x30, XReg::sp, 16)); // ldp x29, x30, [sp], #16
    CHECK_ENCODING("a8c453f3", a.ldp_post(XReg::x19, XReg::x20, XReg::sp, 64)); // ldp x19, x20, [sp], #64
    CHECK_ENCODING("91004020", a.add(XReg::x0, XReg::x1, 16u)); // add x0, x1, #16
    CHECK_ENCODING("910003e0", a.add(XReg::x0, XReg::sp, 0u)); // add x0, sp, #0
    CHECK_ENCODING("91400420", a.add(XReg::x0, XReg::x1, 0x1000u)); // add x0, x1, #0x1000
    CHECK_ENCODING("d10083ff", a.sub(XReg::sp, XReg::sp, 32u)); // sub sp, sp, #32
    CHECK_ENCODING("d13ffc62", a.sub(XReg::x2, XReg::x3, 4095u)); // sub x2, x3, #4095
    CHECK_ENCODING("31000420", a.adds32(XReg::x0, XReg::x1, 1u)); // adds w0, w1, #1
    CHECK_ENCODING("71000420", a.subs32(XReg::x0, XReg::x1, 1u)); // subs w0, w1, #1
    CHECK_ENCODING("f100181f", a.cmp(XReg::x0, 6u)); // cmp x0, #6
    CHECK_ENCODING("7100141f", a.cmp32(XReg::x0, 5u)); // cmp w0, #5
    CHECK_ENCODING("8b020020", a.add(XReg::x0, XReg::x1, XReg::x2)); // add x0, x1, x2
    CHECK_ENCODING("cb020020", a.sub(XReg::x0, XReg::x1, XReg::x2)); // sub x0, x1, x2
    CHECK_ENCODING("2b020020", a.adds32(XReg::x0, XReg::x1, XReg::x2)); // adds w0, w1, w2
    CHECK_ENCODING("6b020020", a.subs32(XReg::x0, XReg::x1, XReg::x2)); // subs w0, w1, w2
    CHECK_ENCODING("eb15001f", a.cmp(XReg::x0, XReg::x21)); // cmp x0, x21
    CHECK_ENCODING("6b01001f", a.cmp32(XReg::x0, XReg::x1)); // cmp w0, w1
    CHECK_ENCODING("eb21c01f", a.cmp_sxtw(XReg::x0, XReg::x1)); // cmp x0, w1, sxtw
    CHECK_ENCODING("8a020020", a.and_(XReg::x0, XReg::x1, XReg::x2)); // and x0, x1, x2
    CHECK_ENCODING("aa160020", a.orr(XReg::x0, XReg::x1, XReg::x22)); // orr x0, x1, x22
    CHECK_ENCODING("ca020020", a.eor(XReg::x0, XReg::x1, XReg::x2)); // eor x0, x1, x2
    CHECK_ENCODING("0a020020", a.and32(XReg::x0, XReg::x1, XReg::x2)); // and w0, w1, w2
    CHECK_ENCODING("2a020020", a.orr32(XReg::x0, XReg::x1, XReg::x2)); // orr w0, w1, w2
    CHECK_ENCODING("4a020020", a.eor32(XReg::x0, XReg::x1, XReg::x2)); // eor w0, w1, w2
    CHECK_ENCODING("ea01001f", a.tst(XReg::x0, XReg::x1)); // tst x0, x1
    CHECK_ENCODING("6a00001f", a.tst32(XReg::x0, XReg::x0)); // tst w0, w0
    CHECK_ENCODING("927cf820", CHECK(a.and_(XReg::x0, XReg::x1, std::uint64_t { 0xfffffffffffffff7 }))); // and x0, x1, #0xfffffffffffffff7
    CHECK_ENCODING("b24f3820", CHECK(a.orr(XReg::x0, XReg::x1, std::uint64_t { 0xfffe000000000000 }))); // orr x0, x1, #0xfffe000000000000
    CHECK_ENCODING("f27d001f", CHECK(a.tst(XReg::x0, std::uint64_t { 0x8 }))); // tst x0, #0x8
    CHECK_ENCODING("1b027c20", a.mul32(XReg::x0, XReg::x1, XReg::x2)); // mul w0, w1, w2
    CHECK_ENCODING("9b227c20", a.smull(XReg::x0, XReg::x1, XReg::x2)); // smull x0, w1, w2
    CHECK_ENCODING("1ac20c20", a.sdiv32(XReg::x0, XReg::x1, XReg::x2)); // sdiv w0, w1, w2
    CHECK_ENCODING("1b028c20", a.msub32(XReg::x0, XReg::x1, XReg::x2, XReg::x3)); // msub w0, w1, w2, w3
    CHECK_ENCODING("1ac22020", a.lslv32(XReg::x0, XReg::x1, XReg::x2)); // lsl w0, w1, w2
    CHECK_ENCODING("1ac22820", a.asrv32(XReg::x0, XReg::x1, XReg::x2)); // asr w0, w1, w2
    CHECK_ENCODING("9a9f17e0", a.cset(XReg::x0, ACond::eq)); // cset x0, eq
    CHECK_ENCODING("9a9fa7e1", a.cset(XReg::x1, ACond::lt)); // cset x1, lt
    CHECK_ENCODING("9a9f97e2", a.cset(XReg::x2, ACond::hi)); // cset x2, hi
    CHECK_ENCODING("d63f0200", a.blr(XReg::x16)); // blr x16
    CHECK_ENCODING("d61f0220", a.br(XReg::x17)); // br x17
    CHECK_ENCODING("d65f03c0", a.ret()); // ret
    CHECK_ENCODING("d4200000", a.brk(0)); // brk #0
}

void test_labels()
{
    // Forward and backward, every field a label fills, as clang resolved
    // the same lines.
    CHECK_EQ(assembled([](AssemblerA64& a) {
        ALabel const one = a.label();
        ALabel const two = a.label();
        ALabel const three = a.label();
        ALabel const four = a.label();
        a.b(one);
        a.brk(0);
        a.bind(one);
        a.ret();
        a.bind(two);
        a.brk(0);
        a.b(ACond::ne, two);
        a.cbz(XReg::x0, three);
        a.brk(0);
        a.bind(three);
        a.ldr_literal(XReg::x16, four);
        a.adr(XReg::x1, four);
        a.bl(three);
        a.cbnz32(XReg::x2, three);
        a.bind(four);
        a.emit_u64(0x0102030405060708ull);
    }),
        std::string("14000002 d4200000 d65f03c0 d4200000 54ffffe1 b4000040 d4200000 58000090 10000061 97fffffe 35ffffa2 05060708 01020304"));
    // A label used and never bound fails the finish.
    {
        AssemblerA64 a;
        ALabel const nowhere = a.label();
        a.b(nowhere);
        CHECK(!a.finish());
    }
}

void test_bitmask_immediates()
{
    // Not encodable: zero, all ones, and a value no rotated run repeats.
    CHECK(!bitmask_immediate(0, true).has_value());
    CHECK(!bitmask_immediate(~std::uint64_t { 0 }, true).has_value());
    CHECK(!bitmask_immediate(0x12345678, true).has_value());
    CHECK(!bitmask_immediate(0xFFFFFFFF, false).has_value());
    // Each against the N:immr:imms clang wrote for it above.
    CHECK_EQ(*bitmask_immediate(0xfffffffffffffff7ull, true), (0x927cf820u >> 10) & 0x1FFFu);
    CHECK_EQ(*bitmask_immediate(0xfffe000000000000ull, true), (0xb24f3820u >> 10) & 0x1FFFu);
    CHECK_EQ(*bitmask_immediate(0x8, true), (0xf27d001fu >> 10) & 0x1FFFu);
    // An element smaller than the register, repeated: 0x5555… is 01 in 2-bit elements.
    CHECK(bitmask_immediate(0x5555555555555555ull, true).has_value());
    CHECK(bitmask_immediate(0x00FF00FF00FF00FFull, true).has_value());
}

}

int main()
{
    test_instructions();
    test_labels();
    test_bitmask_immediates();
    return ::sashfold::test::report("test_assembler_a64");
}
