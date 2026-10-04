// The x86-64 assembler (js/jit/AssemblerX64): each instruction's bytes
// against the bytes GNU as (2.45, `.intel_syntax noprefix`) makes of the
// same instruction, and the labels — forward, backward, rip-relative,
// unbound — against bytes counted by hand.

#include "Test.h"

#include "js/jit/AssemblerX64.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::js::jit;

namespace {

std::string hex(std::vector<std::uint8_t> const& bytes)
{
    std::string out;
    for (std::uint8_t const b : bytes) {
        char buffer[4];
        std::snprintf(buffer, sizeof buffer, "%02x ", b);
        out += buffer;
    }
    if (!out.empty())
        out.pop_back();
    return out;
}

// The bytes one call to the assembler makes, as GNU as prints them.
std::string assembled(std::function<void(AssemblerX64&)> const& write)
{
    AssemblerX64 a;
    write(a);
    CHECK(a.finish());
    return hex(a.bytes());
}

#define CHECK_ENCODING(expected, statement) CHECK_EQ(assembled([](AssemblerX64& a) { statement; }), std::string(expected))

void test_instructions()
{
    CHECK_ENCODING("53", a.push(Reg::rbx));
    CHECK_ENCODING("41 54", a.push(Reg::r12));
    CHECK_ENCODING("41 57", a.push(Reg::r15));
    CHECK_ENCODING("41 5e", a.pop(Reg::r14));
    CHECK_ENCODING("5d", a.pop(Reg::rbp));
    CHECK_ENCODING("48 89 fb", a.mov(Reg::rbx, Reg::rdi));
    CHECK_ENCODING("49 89 f4", a.mov(Reg::r12, Reg::rsi));
    CHECK_ENCODING("4c 89 ef", a.mov(Reg::rdi, Reg::r13));
    CHECK_ENCODING("48 8b 03", a.mov(Reg::rax, Mem::at(Reg::rbx)));
    CHECK_ENCODING("48 8b 43 08", a.mov(Reg::rax, Mem::at(Reg::rbx, 8)));
    CHECK_ENCODING("4d 8b 8c 24 00 02 00 00", a.mov(Reg::r9, Mem::at(Reg::r12, 0x200)));
    CHECK_ENCODING("48 8b 4c 24 10", a.mov(Reg::rcx, Mem::at(Reg::rsp, 16)));
    CHECK_ENCODING("48 8b 55 00", a.mov(Reg::rdx, Mem::at(Reg::rbp)));
    CHECK_ENCODING("4d 8b 55 00", a.mov(Reg::r10, Mem::at(Reg::r13)));
    CHECK_ENCODING("49 89 44 24 08", a.mov(Mem::at(Reg::r12, 8), Reg::rax));
    CHECK_ENCODING("4c 89 3c 24", a.mov(Mem::at(Reg::rsp), Reg::r15));
    CHECK_ENCODING("48 b8 f0 de bc 9a 78 56 34 12", a.mov_imm64(Reg::rax, 0x123456789abcdef0ull));
    CHECK_ENCODING("49 bb 05 00 00 00 00 00 00 00", a.mov_imm64(Reg::r11, 5));
    CHECK_ENCODING("41 8b 06", a.mov32(Reg::rax, Mem::at(Reg::r14)));
    CHECK_ENCODING("44 8b 43 04", a.mov32(Reg::r8, Mem::at(Reg::rbx, 4)));
    CHECK_ENCODING("41 c7 06 07 00 00 00", a.mov32(Mem::at(Reg::r14), 7));
    CHECK_ENCODING("41 c7 44 24 10 78 56 34 12", a.mov32(Mem::at(Reg::r12, 0x10), 0x12345678));
    CHECK_ENCODING("48 8d 4c c3 10", a.lea(Reg::rcx, Mem::at_index(Reg::rbx, Reg::rax, 8, 16)));
    CHECK_ENCODING("4b 8d 14 ac", a.lea(Reg::rdx, Mem::at_index(Reg::r12, Reg::r13, 4)));
    CHECK_ENCODING("48 83 c4 08", a.add(Reg::rsp, 8));
    CHECK_ENCODING("48 05 00 10 00 00", a.add(Reg::rax, 0x1000));
    CHECK_ENCODING("48 81 ec 88 00 00 00", a.sub(Reg::rsp, 0x88));
    CHECK_ENCODING("49 83 e9 ff", a.sub(Reg::r9, -1));
    CHECK_ENCODING("83 f8 05", a.cmp32(Reg::rax, 5));
    CHECK_ENCODING("3d 00 10 00 00", a.cmp32(Reg::rax, 0x1000));
    CHECK_ENCODING("81 f9 2c 01 00 00", a.cmp32(Reg::rcx, 300));
    CHECK_ENCODING("41 83 fa fe", a.cmp32(Reg::r10, -2));
    CHECK_ENCODING("41 83 3e 09", a.cmp32(Mem::at(Reg::r14), 9));
    CHECK_ENCODING("41 81 3e 00 00 01 00", a.cmp32(Mem::at(Reg::r14), 0x10000));
    CHECK_ENCODING("ff d0", a.call(Reg::rax));
    CHECK_ENCODING("41 ff d7", a.call(Reg::r15));
    CHECK_ENCODING("ff 24 c1", a.jmp(Mem::at_index(Reg::rcx, Reg::rax, 8)));
    CHECK_ENCODING("41 ff 64 c4 08", a.jmp(Mem::at_index(Reg::r12, Reg::rax, 8, 8)));
    CHECK_ENCODING("c3", a.ret());
    CHECK_ENCODING("cc", a.int3());
}

void test_labels()
{
    // Forward: the distance from the jump's end to the label, patched at finish.
    CHECK_EQ(assembled([](AssemblerX64& a) {
        Label const target = a.label();
        a.jmp(target);
        a.int3();
        a.bind(target);
        a.ret();
    }),
        std::string("e9 01 00 00 00 cc c3"));
    // Backward, conditional: -7 from the end of the 6-byte jne to offset 0.
    CHECK_EQ(assembled([](AssemblerX64& a) {
        Label const top = a.label();
        a.bind(top);
        a.int3();
        a.j(Cond::NotEqual, top);
    }),
        std::string("cc 0f 85 f9 ff ff ff"));
    // Every condition's opcode in order.
    CHECK_EQ(assembled([](AssemblerX64& a) {
        Label const here = a.label();
        a.bind(here);
        a.j(Cond::Overflow, here);
        a.j(Cond::Greater, here);
    }),
        std::string("0f 80 fa ff ff ff 0f 8f f4 ff ff ff"));
    // Rip-relative: measured from the end of the lea.
    CHECK_EQ(assembled([](AssemblerX64& a) {
        Label const data = a.label();
        a.lea(Reg::rcx, data);
        a.ret();
        a.bind(data);
    }),
        std::string("48 8d 0d 01 00 00 00 c3"));
    CHECK_EQ(assembled([](AssemblerX64& a) {
        Label const data = a.label();
        a.lea(Reg::r9, data);
        a.bind(data);
    }),
        std::string("4c 8d 0d 00 00 00 00"));
    // A label used and never bound fails the finish; one never used does not.
    {
        AssemblerX64 a;
        Label const nowhere = a.label();
        a.jmp(nowhere);
        CHECK(!a.finish());
    }
    {
        AssemblerX64 a;
        static_cast<void>(a.label());
        a.ret();
        CHECK(a.finish());
    }
    // Alignment with int3, and data in the code.
    CHECK_EQ(assembled([](AssemblerX64& a) {
        a.ret();
        a.align(4);
        a.emit_u64(0x0102030405060708ull);
    }),
        std::string("c3 cc cc cc 08 07 06 05 04 03 02 01"));
}

}

int main()
{
    test_instructions();
    test_labels();
    return ::sashfold::test::report("test_assembler");
}
