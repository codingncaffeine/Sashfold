#include "js/jit/AssemblerX64.h"

namespace sashfold::js::jit {

namespace {

std::uint8_t number(Reg reg)
{
    return static_cast<std::uint8_t>(reg);
}

bool fits_int8(std::int32_t value)
{
    return value >= -128 && value <= 127;
}

}

Label AssemblerX64::label()
{
    Label made;
    made.m_id = static_cast<std::uint32_t>(m_labels.size());
    m_labels.emplace_back();
    return made;
}

void AssemblerX64::bind(Label label)
{
    m_labels[label.m_id].offset = static_cast<std::int64_t>(m_bytes.size());
}

bool AssemblerX64::bound(Label label) const
{
    return label.valid() && m_labels[label.m_id].offset >= 0;
}

std::uint32_t AssemblerX64::offset_of(Label label) const
{
    return static_cast<std::uint32_t>(m_labels[label.m_id].offset);
}

bool AssemblerX64::finish()
{
    for (LabelState const& state : m_labels) {
        if (state.uses.empty())
            continue;
        if (state.offset < 0)
            return false;
        for (Use const& use : state.uses) {
            std::int64_t const distance = state.offset - static_cast<std::int64_t>(use.from);
            if (distance < INT32_MIN || distance > INT32_MAX)
                return false;
            auto const value = static_cast<std::uint32_t>(static_cast<std::int32_t>(distance));
            for (int i = 0; i < 4; ++i)
                m_bytes[use.at + static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }
    return true;
}

void AssemblerX64::u32(std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        byte(static_cast<std::uint8_t>(value >> (8 * i)));
}

void AssemblerX64::emit_u64(std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        byte(static_cast<std::uint8_t>(value >> (8 * i)));
}

void AssemblerX64::rex(bool wide, std::uint8_t reg, std::uint8_t index, std::uint8_t base)
{
    auto const value = static_cast<std::uint8_t>(0x40 | (wide ? 8 : 0) | (((reg >> 3) & 1) << 2) | (((index >> 3) & 1) << 1) | ((base >> 3) & 1));
    if (value != 0x40)
        byte(value);
}

void AssemblerX64::rex_mem(bool wide, std::uint8_t reg, Mem const& memory)
{
    rex(wide, reg, memory.indexed ? number(memory.index) : 0, number(memory.base));
}

// rsp and r12 as a base need a SIB byte; rbp and r13 as a base have no
// form without a displacement (that encoding is rip-relative), so they
// take a zero byte.
void AssemblerX64::modrm_mem(std::uint8_t reg, Mem const& memory)
{
    std::uint8_t const base = number(memory.base) & 7;
    bool const sib = memory.indexed || base == 4;
    std::uint8_t mode = 2;
    if (memory.displacement == 0 && base != 5)
        mode = 0;
    else if (fits_int8(memory.displacement))
        mode = 1;
    if (!sib) {
        byte(static_cast<std::uint8_t>((mode << 6) | ((reg & 7) << 3) | base));
    } else {
        std::uint8_t const scale = memory.scale == 8 ? 3 : memory.scale == 4 ? 2 : memory.scale == 2 ? 1 : 0;
        std::uint8_t const index = memory.indexed ? (number(memory.index) & 7) : 4; // 100: no index
        byte(static_cast<std::uint8_t>((mode << 6) | ((reg & 7) << 3) | 4));
        byte(static_cast<std::uint8_t>((scale << 6) | (index << 3) | base));
    }
    if (mode == 1)
        byte(static_cast<std::uint8_t>(static_cast<std::int8_t>(memory.displacement)));
    else if (mode == 2)
        u32(static_cast<std::uint32_t>(memory.displacement));
}

void AssemblerX64::group1(std::uint8_t digit, bool wide, Reg reg, std::int32_t value)
{
    if (fits_int8(value)) {
        rex(wide, 0, 0, number(reg));
        byte(0x83);
        modrm_reg(digit, number(reg));
        byte(static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
        return;
    }
    if (reg == Reg::rax) {
        rex(wide, 0, 0, 0);
        byte(static_cast<std::uint8_t>(digit * 8 + 5)); // add eax 05, sub 2D, cmp 3D: the accumulator's own form
        u32(static_cast<std::uint32_t>(value));
        return;
    }
    rex(wide, 0, 0, number(reg));
    byte(0x81);
    modrm_reg(digit, number(reg));
    u32(static_cast<std::uint32_t>(value));
}

void AssemblerX64::group1(std::uint8_t digit, bool wide, Mem const& memory, std::int32_t value)
{
    rex_mem(wide, 0, memory);
    byte(fits_int8(value) ? 0x83 : 0x81);
    modrm_mem(digit, memory);
    if (fits_int8(value))
        byte(static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
    else
        u32(static_cast<std::uint32_t>(value));
}

void AssemblerX64::label_use(Label label, std::uint32_t trailing)
{
    auto const at = static_cast<std::uint32_t>(m_bytes.size());
    m_labels[label.m_id].uses.push_back(Use { at, at + 4 + trailing });
    u32(0);
}

void AssemblerX64::push(Reg reg)
{
    rex(false, 0, 0, number(reg));
    byte(static_cast<std::uint8_t>(0x50 + (number(reg) & 7)));
}

void AssemblerX64::pop(Reg reg)
{
    rex(false, 0, 0, number(reg));
    byte(static_cast<std::uint8_t>(0x58 + (number(reg) & 7)));
}

void AssemblerX64::mov(Reg destination, Reg source)
{
    rex(true, number(source), 0, number(destination));
    byte(0x89);
    modrm_reg(number(source), number(destination));
}

void AssemblerX64::mov(Reg destination, Mem source)
{
    rex_mem(true, number(destination), source);
    byte(0x8B);
    modrm_mem(number(destination), source);
}

void AssemblerX64::mov(Mem destination, Reg source)
{
    rex_mem(true, number(source), destination);
    byte(0x89);
    modrm_mem(number(source), destination);
}

void AssemblerX64::mov_imm64(Reg destination, std::uint64_t value)
{
    rex(true, 0, 0, number(destination));
    byte(static_cast<std::uint8_t>(0xB8 + (number(destination) & 7)));
    emit_u64(value);
}

void AssemblerX64::mov32(Reg destination, Mem source)
{
    rex_mem(false, number(destination), source);
    byte(0x8B);
    modrm_mem(number(destination), source);
}

void AssemblerX64::mov32(Mem destination, std::uint32_t value)
{
    rex_mem(false, 0, destination);
    byte(0xC7);
    modrm_mem(0, destination);
    u32(value);
}

void AssemblerX64::lea(Reg destination, Mem source)
{
    rex_mem(true, number(destination), source);
    byte(0x8D);
    modrm_mem(number(destination), source);
}

void AssemblerX64::lea(Reg destination, Label label)
{
    rex(true, number(destination), 0, 0);
    byte(0x8D);
    byte(static_cast<std::uint8_t>(0x05 | ((number(destination) & 7) << 3))); // mod 00, rm 101: [rip + disp32]
    label_use(label);
}

void AssemblerX64::add(Reg destination, std::int32_t value)
{
    group1(0, true, destination, value);
}

void AssemblerX64::sub(Reg destination, std::int32_t value)
{
    group1(5, true, destination, value);
}

void AssemblerX64::cmp32(Reg reg, std::int32_t value)
{
    group1(7, false, reg, value);
}

void AssemblerX64::cmp32(Mem memory, std::int32_t value)
{
    group1(7, false, memory, value);
}

void AssemblerX64::call(Reg reg)
{
    rex(false, 0, 0, number(reg));
    byte(0xFF);
    modrm_reg(2, number(reg));
}

void AssemblerX64::jmp(Label label)
{
    byte(0xE9);
    label_use(label);
}

void AssemblerX64::jmp(Mem memory)
{
    rex_mem(false, 0, memory);
    byte(0xFF);
    modrm_mem(4, memory);
}

void AssemblerX64::j(Cond condition, Label label)
{
    byte(0x0F);
    byte(static_cast<std::uint8_t>(0x80 + static_cast<std::uint8_t>(condition)));
    label_use(label);
}

void AssemblerX64::ret()
{
    byte(0xC3);
}

void AssemblerX64::int3()
{
    byte(0xCC);
}

void AssemblerX64::align(std::size_t alignment)
{
    while (m_bytes.size() % alignment != 0)
        byte(0xCC);
}

void AssemblerX64::alu(std::uint8_t opcode, bool wide, Reg destination, Reg source)
{
    rex(wide, number(source), 0, number(destination));
    byte(opcode);
    modrm_reg(number(source), number(destination));
}

void AssemblerX64::rex_byte(std::uint8_t reg, std::uint8_t base)
{
    auto const value = static_cast<std::uint8_t>(0x40 | (((reg >> 3) & 1) << 2) | ((base >> 3) & 1));
    if (value != 0x40 || (reg & 7) >= 4 || (base & 7) >= 4)
        byte(value);
}

void AssemblerX64::mov(Mem destination, std::int32_t value)
{
    rex_mem(true, 0, destination);
    byte(0xC7);
    modrm_mem(0, destination);
    u32(static_cast<std::uint32_t>(value));
}

void AssemblerX64::mov32(Reg destination, std::uint32_t value)
{
    rex(false, 0, 0, number(destination));
    byte(static_cast<std::uint8_t>(0xB8 + (number(destination) & 7)));
    u32(value);
}

void AssemblerX64::mov32(Reg destination, Reg source)
{
    alu(0x89, false, destination, source);
}

void AssemblerX64::mov32(Mem destination, Reg source)
{
    rex_mem(false, number(source), destination);
    byte(0x89);
    modrm_mem(number(source), destination);
}

void AssemblerX64::movzx8(Reg destination, Reg source)
{
    rex_byte(number(destination), number(source));
    byte(0x0F);
    byte(0xB6);
    modrm_reg(number(destination), number(source));
}

void AssemblerX64::add(Mem destination, std::int32_t value)
{
    group1(0, true, destination, value);
}

void AssemblerX64::cmp(Reg reg, std::int32_t value)
{
    group1(7, true, reg, value);
}

void AssemblerX64::and_(Reg reg, std::int32_t value)
{
    group1(4, true, reg, value);
}

void AssemblerX64::or_(Reg reg, std::int32_t value)
{
    group1(1, true, reg, value);
}

void AssemblerX64::add32(Reg reg, std::int32_t value)
{
    group1(0, false, reg, value);
}

void AssemblerX64::sub32(Reg reg, std::int32_t value)
{
    group1(5, false, reg, value);
}

void AssemblerX64::sub32(Mem memory, std::int32_t value)
{
    group1(5, false, memory, value);
}

void AssemblerX64::add(Reg destination, Reg source)
{
    alu(0x01, true, destination, source);
}
void AssemblerX64::sub(Reg destination, Reg source)
{
    alu(0x29, true, destination, source);
}
void AssemblerX64::and_(Reg destination, Reg source)
{
    alu(0x21, true, destination, source);
}
void AssemblerX64::or_(Reg destination, Reg source)
{
    alu(0x09, true, destination, source);
}
void AssemblerX64::xor_(Reg destination, Reg source)
{
    alu(0x31, true, destination, source);
}
void AssemblerX64::cmp(Reg left, Reg right)
{
    alu(0x39, true, left, right);
}
void AssemblerX64::test(Reg left, Reg right)
{
    alu(0x85, true, left, right);
}
void AssemblerX64::add32(Reg destination, Reg source)
{
    alu(0x01, false, destination, source);
}
void AssemblerX64::sub32(Reg destination, Reg source)
{
    alu(0x29, false, destination, source);
}
void AssemblerX64::and32(Reg destination, Reg source)
{
    alu(0x21, false, destination, source);
}
void AssemblerX64::or32(Reg destination, Reg source)
{
    alu(0x09, false, destination, source);
}
void AssemblerX64::xor32(Reg destination, Reg source)
{
    alu(0x31, false, destination, source);
}
void AssemblerX64::cmp32(Reg left, Reg right)
{
    alu(0x39, false, left, right);
}
void AssemblerX64::test32(Reg left, Reg right)
{
    alu(0x85, false, left, right);
}

void AssemblerX64::cmp(Reg left, Mem right)
{
    rex_mem(true, number(left), right);
    byte(0x3B);
    modrm_mem(number(left), right);
}

void AssemblerX64::imul32(Reg destination, Reg source)
{
    rex(false, number(destination), 0, number(source));
    byte(0x0F);
    byte(0xAF);
    modrm_reg(number(destination), number(source));
}

void AssemblerX64::shl32_cl(Reg reg)
{
    rex(false, 0, 0, number(reg));
    byte(0xD3);
    modrm_reg(4, number(reg));
}

void AssemblerX64::sar32_cl(Reg reg)
{
    rex(false, 0, 0, number(reg));
    byte(0xD3);
    modrm_reg(7, number(reg));
}

void AssemblerX64::cdq()
{
    byte(0x99);
}

void AssemblerX64::idiv32(Reg reg)
{
    rex(false, 0, 0, number(reg));
    byte(0xF7);
    modrm_reg(7, number(reg));
}

void AssemblerX64::cmp8(Mem memory, std::uint8_t value)
{
    rex_mem(false, 0, memory);
    byte(0x80);
    modrm_mem(7, memory);
    byte(value);
}

void AssemblerX64::test8(Mem memory, std::uint8_t value)
{
    rex_mem(false, 0, memory);
    byte(0xF6);
    modrm_mem(0, memory);
    byte(value);
}

void AssemblerX64::or8(Mem memory, std::uint8_t value)
{
    rex_mem(false, 0, memory);
    byte(0x80);
    modrm_mem(1, memory);
    byte(value);
}

void AssemblerX64::set(Cond condition, Reg reg)
{
    rex_byte(0, number(reg));
    byte(0x0F);
    byte(static_cast<std::uint8_t>(0x90 + static_cast<std::uint8_t>(condition)));
    modrm_reg(0, number(reg));
}

void AssemblerX64::call(Label label)
{
    byte(0xFF);
    byte(0x15); // mod 00, /2, rm 101: [rip + disp32]
    label_use(label);
}

}
