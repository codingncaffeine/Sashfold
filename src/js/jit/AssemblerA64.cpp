#include "js/jit/AssemblerA64.h"

#include <bit>

namespace sashfold::js::jit {

namespace {

std::uint32_t number(XReg reg)
{
    return static_cast<std::uint32_t>(reg);
}

std::uint32_t sf(bool wide)
{
    return wide ? 1u << 31 : 0u;
}

// A run of ones from bit 0 (`value` nonzero): 0b0111 is one, 0b0110 is not.
bool is_mask(std::uint64_t value)
{
    return value != 0 && ((value + 1) & value) == 0;
}

// A run of ones anywhere: 0b0110 is one.
bool is_shifted_mask(std::uint64_t value)
{
    return value != 0 && is_mask((value - 1) | value);
}

}

// The manual's DecodeBitMasks run backwards, as LLVM's
// processLogicalImmediate does it: the smallest element the value repeats
// in, the run of ones in that element and its rotation.
std::optional<std::uint32_t> bitmask_immediate(std::uint64_t value, bool wide)
{
    unsigned const register_size = wide ? 64 : 32;
    std::uint64_t const all = wide ? ~std::uint64_t { 0 } : 0xFFFFFFFFull;
    value &= all;
    if (value == 0 || value == all)
        return std::nullopt;
    unsigned size = register_size;
    do {
        size /= 2;
        std::uint64_t const mask = (std::uint64_t { 1 } << size) - 1;
        if ((value & mask) != ((value >> size) & mask)) {
            size *= 2;
            break;
        }
    } while (size > 2);
    std::uint64_t const element = ~std::uint64_t { 0 } >> (64 - size);
    value &= element;
    unsigned rotation = 0;
    unsigned ones = 0;
    if (is_shifted_mask(value)) {
        rotation = static_cast<unsigned>(std::countr_zero(value));
        ones = static_cast<unsigned>(std::countr_one(value >> rotation));
    } else {
        value |= ~element;
        if (!is_shifted_mask(~value))
            return std::nullopt;
        unsigned const leading = static_cast<unsigned>(std::countl_one(value));
        rotation = 64 - leading;
        ones = leading + static_cast<unsigned>(std::countr_one(value)) - (64 - size);
    }
    unsigned const immr = (size - rotation) & (size - 1);
    std::uint64_t n_imms = ~std::uint64_t { size - 1 } << 1;
    n_imms |= ones - 1;
    unsigned const n = static_cast<unsigned>(((n_imms >> 6) & 1) ^ 1);
    return static_cast<std::uint32_t>(n << 12 | immr << 6 | (n_imms & 0x3f));
}

ALabel AssemblerA64::label()
{
    ALabel made;
    made.m_id = static_cast<std::uint32_t>(m_labels.size());
    m_labels.push_back(-1);
    return made;
}

void AssemblerA64::bind(ALabel label)
{
    m_labels[label.m_id] = static_cast<std::int64_t>(m_words.size());
}

bool AssemblerA64::bound(ALabel label) const
{
    return label.valid() && m_labels[label.m_id] >= 0;
}

std::uint32_t AssemblerA64::offset_of(ALabel label) const
{
    return static_cast<std::uint32_t>(m_labels[label.m_id]) * 4;
}

void AssemblerA64::reserve(std::size_t instructions, std::size_t labels)
{
    m_words.reserve(instructions);
    m_labels.reserve(labels);
    m_uses.reserve(labels);
}

bool AssemblerA64::finish()
{
    for (Use const& use : m_uses) {
        std::int64_t const target = m_labels[use.label];
        if (target < 0)
            return false;
        std::int64_t const distance = target - static_cast<std::int64_t>(use.at); // in words
        std::uint32_t& instruction = m_words[use.at];
        switch (use.field) {
        case Field::Branch26:
            if (distance < -(1 << 25) || distance >= (1 << 25))
                return false;
            instruction |= static_cast<std::uint32_t>(distance) & 0x03FFFFFFu;
            break;
        case Field::Branch19:
            if (distance < -(1 << 18) || distance >= (1 << 18))
                return false;
            instruction |= (static_cast<std::uint32_t>(distance) & 0x7FFFFu) << 5;
            break;
        case Field::Adr21: {
            std::int64_t const bytes = distance * 4;
            if (bytes < -(1 << 20) || bytes >= (1 << 20))
                return false;
            auto const value = static_cast<std::uint32_t>(bytes) & 0x1FFFFFu;
            instruction |= (value & 3) << 29 | (value >> 2) << 5;
            break;
        }
        }
    }
    return true;
}

void AssemblerA64::label_use(ALabel label, Field field)
{
    m_uses.push_back(Use { label.m_id, static_cast<std::uint32_t>(m_words.size()), field });
}

// ---- moves

void AssemblerA64::mov(XReg destination, XReg source)
{
    if (destination == XReg::sp || source == XReg::sp)
        add(destination, source, 0u);
    else
        orr(destination, XReg::zr, source);
}

void AssemblerA64::mov32(XReg destination, XReg source)
{
    orr32(destination, XReg::zr, source);
}

void AssemblerA64::movz(XReg reg, std::uint16_t value, unsigned shift)
{
    word(0xD2800000u | (shift / 16) << 21 | std::uint32_t { value } << 5 | number(reg));
}

void AssemblerA64::movk(XReg reg, std::uint16_t value, unsigned shift)
{
    word(0xF2800000u | (shift / 16) << 21 | std::uint32_t { value } << 5 | number(reg));
}

void AssemblerA64::mov_imm64(XReg reg, std::uint64_t value)
{
    bool written = false;
    for (unsigned shift = 0; shift < 64; shift += 16) {
        auto const part = static_cast<std::uint16_t>(value >> shift);
        if (part == 0)
            continue;
        if (written) {
            movk(reg, part, shift);
        } else {
            movz(reg, part, shift);
            written = true;
        }
    }
    if (!written)
        movz(reg, 0, 0);
}

void AssemblerA64::mov_imm32(XReg reg, std::uint32_t value)
{
    word(0x52800000u | (value & 0xFFFFu) << 5 | number(reg)); // movz w
    if ((value >> 16) != 0)
        word(0x72A00000u | (value >> 16) << 5 | number(reg)); // movk w, lsl #16
}

// ---- loads and stores

bool AssemblerA64::fits_load(std::int32_t offset, unsigned size)
{
    if (offset >= 0 && offset % static_cast<std::int32_t>(size) == 0 && offset / static_cast<std::int32_t>(size) < 4096)
        return true;
    return offset >= -256 && offset <= 255;
}

void AssemblerA64::load_store(std::uint32_t scaled, std::uint32_t unscaled, unsigned size, XReg t, XReg base, std::int32_t offset)
{
    if (offset >= 0 && offset % static_cast<std::int32_t>(size) == 0 && offset / static_cast<std::int32_t>(size) < 4096)
        word(scaled | static_cast<std::uint32_t>(offset / static_cast<std::int32_t>(size)) << 10 | number(base) << 5 | number(t));
    else
        word(unscaled | (static_cast<std::uint32_t>(offset) & 0x1FFu) << 12 | number(base) << 5 | number(t));
}

void AssemblerA64::ldr(XReg t, XReg base, std::int32_t offset)
{
    load_store(0xF9400000u, 0xF8400000u, 8, t, base, offset);
}

void AssemblerA64::str(XReg t, XReg base, std::int32_t offset)
{
    load_store(0xF9000000u, 0xF8000000u, 8, t, base, offset);
}

void AssemblerA64::ldr32(XReg t, XReg base, std::int32_t offset)
{
    load_store(0xB9400000u, 0xB8400000u, 4, t, base, offset);
}

void AssemblerA64::str32(XReg t, XReg base, std::int32_t offset)
{
    load_store(0xB9000000u, 0xB8000000u, 4, t, base, offset);
}

void AssemblerA64::ldrb(XReg t, XReg base, std::int32_t offset)
{
    load_store(0x39400000u, 0x38400000u, 1, t, base, offset);
}

void AssemblerA64::strb(XReg t, XReg base, std::int32_t offset)
{
    load_store(0x39000000u, 0x38000000u, 1, t, base, offset);
}

void AssemblerA64::ldr_indexed(XReg t, XReg base, XReg index)
{
    word(0xF8607800u | number(index) << 16 | number(base) << 5 | number(t));
}

void AssemblerA64::str_indexed(XReg t, XReg base, XReg index)
{
    word(0xF8207800u | number(index) << 16 | number(base) << 5 | number(t));
}

void AssemblerA64::ldr_literal(XReg t, ALabel label)
{
    label_use(label, Field::Branch19);
    word(0x58000000u | number(t));
}

void AssemblerA64::str_post(XReg t, XReg base, std::int32_t offset)
{
    word(0xF8000400u | (static_cast<std::uint32_t>(offset) & 0x1FFu) << 12 | number(base) << 5 | number(t));
}

void AssemblerA64::str_pre(XReg t, XReg base, std::int32_t offset)
{
    word(0xF8000C00u | (static_cast<std::uint32_t>(offset) & 0x1FFu) << 12 | number(base) << 5 | number(t));
}

void AssemblerA64::ldr_post(XReg t, XReg base, std::int32_t offset)
{
    word(0xF8400400u | (static_cast<std::uint32_t>(offset) & 0x1FFu) << 12 | number(base) << 5 | number(t));
}

void AssemblerA64::stp_pre(XReg first, XReg second, XReg base, std::int32_t offset)
{
    word(0xA9800000u | (static_cast<std::uint32_t>(offset / 8) & 0x7Fu) << 15 | number(second) << 10 | number(base) << 5 | number(first));
}

void AssemblerA64::ldp_post(XReg first, XReg second, XReg base, std::int32_t offset)
{
    word(0xA8C00000u | (static_cast<std::uint32_t>(offset / 8) & 0x7Fu) << 15 | number(second) << 10 | number(base) << 5 | number(first));
}

void AssemblerA64::adr(XReg reg, ALabel label)
{
    label_use(label, Field::Adr21);
    word(0x10000000u | number(reg));
}

// ---- arithmetic and logic

void AssemblerA64::add_sub_immediate(std::uint32_t base, bool wide, XReg d, XReg n, std::uint32_t immediate)
{
    std::uint32_t shift = 0;
    if (immediate > 0xFFF) {
        shift = 1;
        immediate >>= 12;
    }
    word(base | sf(wide) | shift << 22 | (immediate & 0xFFFu) << 10 | number(n) << 5 | number(d));
}

void AssemblerA64::add(XReg d, XReg n, std::uint32_t immediate)
{
    add_sub_immediate(0x11000000u, true, d, n, immediate);
}

void AssemblerA64::sub(XReg d, XReg n, std::uint32_t immediate)
{
    add_sub_immediate(0x51000000u, true, d, n, immediate);
}

void AssemblerA64::adds32(XReg d, XReg n, std::uint32_t immediate)
{
    add_sub_immediate(0x31000000u, false, d, n, immediate);
}

void AssemblerA64::subs32(XReg d, XReg n, std::uint32_t immediate)
{
    add_sub_immediate(0x71000000u, false, d, n, immediate);
}

void AssemblerA64::cmp(XReg n, std::uint32_t immediate)
{
    add_sub_immediate(0x71000000u, true, XReg::zr, n, immediate);
}

void AssemblerA64::cmp32(XReg n, std::uint32_t immediate)
{
    add_sub_immediate(0x71000000u, false, XReg::zr, n, immediate);
}

void AssemblerA64::add(XReg d, XReg n, XReg m)
{
    word(0x8B000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::sub(XReg d, XReg n, XReg m)
{
    word(0xCB000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::adds32(XReg d, XReg n, XReg m)
{
    word(0x2B000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::subs32(XReg d, XReg n, XReg m)
{
    word(0x6B000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::cmp(XReg n, XReg m)
{
    word(0xEB000000u | number(m) << 16 | number(n) << 5 | 31u);
}

void AssemblerA64::cmp32(XReg n, XReg m)
{
    word(0x6B000000u | number(m) << 16 | number(n) << 5 | 31u);
}

void AssemblerA64::cmp_sxtw(XReg n, XReg m)
{
    word(0xEB20C000u | number(m) << 16 | number(n) << 5 | 31u); // subs xzr, n, m, sxtw
}

void AssemblerA64::and_(XReg d, XReg n, XReg m)
{
    word(0x8A000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::orr(XReg d, XReg n, XReg m)
{
    word(0xAA000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::eor(XReg d, XReg n, XReg m)
{
    word(0xCA000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::and32(XReg d, XReg n, XReg m)
{
    word(0x0A000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::orr32(XReg d, XReg n, XReg m)
{
    word(0x2A000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::eor32(XReg d, XReg n, XReg m)
{
    word(0x4A000000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::tst(XReg n, XReg m)
{
    word(0xEA000000u | number(m) << 16 | number(n) << 5 | 31u);
}

void AssemblerA64::tst32(XReg n, XReg m)
{
    word(0x6A000000u | number(m) << 16 | number(n) << 5 | 31u);
}

bool AssemblerA64::and_(XReg d, XReg n, std::uint64_t immediate)
{
    std::optional<std::uint32_t> const fields = bitmask_immediate(immediate, true);
    if (!fields)
        return false;
    word(0x92000000u | *fields << 10 | number(n) << 5 | number(d));
    return true;
}

bool AssemblerA64::orr(XReg d, XReg n, std::uint64_t immediate)
{
    std::optional<std::uint32_t> const fields = bitmask_immediate(immediate, true);
    if (!fields)
        return false;
    word(0xB2000000u | *fields << 10 | number(n) << 5 | number(d));
    return true;
}

bool AssemblerA64::tst(XReg n, std::uint64_t immediate)
{
    std::optional<std::uint32_t> const fields = bitmask_immediate(immediate, true);
    if (!fields)
        return false;
    word(0xF2000000u | *fields << 10 | number(n) << 5 | 31u);
    return true;
}

void AssemblerA64::mul32(XReg d, XReg n, XReg m)
{
    word(0x1B007C00u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::smull(XReg d, XReg n, XReg m)
{
    word(0x9B207C00u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::sdiv32(XReg d, XReg n, XReg m)
{
    word(0x1AC00C00u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::msub32(XReg d, XReg n, XReg m, XReg a)
{
    word(0x1B008000u | number(m) << 16 | number(a) << 10 | number(n) << 5 | number(d));
}

void AssemblerA64::lslv32(XReg d, XReg n, XReg m)
{
    word(0x1AC02000u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::asrv32(XReg d, XReg n, XReg m)
{
    word(0x1AC02800u | number(m) << 16 | number(n) << 5 | number(d));
}

void AssemblerA64::cset(XReg d, ACond condition)
{
    // csinc d, zr, zr, !condition
    auto const inverse = static_cast<std::uint32_t>(condition) ^ 1u;
    word(0x9A9F07E0u | inverse << 12 | number(d));
}

// ---- control

void AssemblerA64::b(ALabel label)
{
    label_use(label, Field::Branch26);
    word(0x14000000u);
}

void AssemblerA64::b(ACond condition, ALabel label)
{
    label_use(label, Field::Branch19);
    word(0x54000000u | static_cast<std::uint32_t>(condition));
}

void AssemblerA64::cbz(XReg reg, ALabel label)
{
    label_use(label, Field::Branch19);
    word(0xB4000000u | number(reg));
}

void AssemblerA64::cbnz(XReg reg, ALabel label)
{
    label_use(label, Field::Branch19);
    word(0xB5000000u | number(reg));
}

void AssemblerA64::cbz32(XReg reg, ALabel label)
{
    label_use(label, Field::Branch19);
    word(0x34000000u | number(reg));
}

void AssemblerA64::cbnz32(XReg reg, ALabel label)
{
    label_use(label, Field::Branch19);
    word(0x35000000u | number(reg));
}

void AssemblerA64::bl(ALabel label)
{
    label_use(label, Field::Branch26);
    word(0x94000000u);
}

void AssemblerA64::blr(XReg reg)
{
    word(0xD63F0000u | number(reg) << 5);
}

void AssemblerA64::br(XReg reg)
{
    word(0xD61F0000u | number(reg) << 5);
}

void AssemblerA64::ret()
{
    word(0xD65F03C0u);
}

void AssemblerA64::brk(std::uint16_t code)
{
    word(0xD4200000u | std::uint32_t { code } << 5);
}

// ---- data

void AssemblerA64::align(std::size_t bytes)
{
    while (size() % bytes != 0)
        brk(0);
}

void AssemblerA64::emit_u64(std::uint64_t value)
{
    word(static_cast<std::uint32_t>(value));
    word(static_cast<std::uint32_t>(value >> 32));
}

}
