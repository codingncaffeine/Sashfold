// T1's code generator for AArch64 (_plans/js-JIT-DESIGN.md §5.2, slice 5):
// the same code as Baseline.cpp writes for x86-64, template for template —
// the frame is T0's, the code is entered at any instruction through its
// table, every miss is the interpreter's own handler — in AArch64's terms.
// It is built and its output checked on every machine; it runs where
// `available` says an AArch64 backend does.
//
// Registers (AAPCS64; callee-saved, so the calls out keep them): x19 the
// interpreter, x20 the frame, x21 the number tag, x22 the frame's
// registers, x23 the operand stack's top (written back before each call
// out, read after), x24 where the next frame goes, x25 the running block's
// table (an `adr` reaches only 1 MB; it is read from the block's Code), x26
// the helpers' record. Scratch: x0–x3, x8, x16 and x17 (x17 for offsets too
// large for an instruction). x18 is never touched: the platform's.

#include "js/jit/Baseline.h"

#include "js/Bytecode.h"
#include "js/Feedback.h"
#include "js/jit/AssemblerA64.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace sashfold::js::jit {

namespace {

constexpr XReg interpreter_reg = XReg::x19;
constexpr XReg frame_reg = XReg::x20;
constexpr XReg tag_reg = XReg::x21;
constexpr XReg registers_reg = XReg::x22;
constexpr XReg top_reg = XReg::x23;
constexpr XReg next_reg = XReg::x24;
constexpr XReg table_reg = XReg::x25;
constexpr XReg helpers_reg = XReg::x26;
constexpr XReg far_reg = XReg::x17;

// The operand `below` values under the top (0: the top itself).
std::int32_t operand(std::uint32_t below)
{
    return -8 * static_cast<std::int32_t>(below + 1);
}

ACond inverse(ACond condition)
{
    return static_cast<ACond>(static_cast<std::uint8_t>(condition) ^ 1);
}

bool small(std::int32_t value)
{
    return value >= -0x8000 && value <= 0xFFFF;
}

class WriterA64 {
public:
    WriterA64(CodeBlock const& block, FeedbackVector* feedback, Layout const& layout, Helpers const& helpers, CodeSpace& space)
        : m_block(block)
        , m_feedback(feedback)
        , m_layout(layout)
        , m_helpers(helpers)
        , m_space(space)
        , m_count(static_cast<std::uint32_t>(block.code.size()))
        // Past this many instructions a block's code may pass the 1 MB a
        // conditional branch reaches: its branches go around a `b` then.
        , m_far(m_count > 6000)
    {
        a.reserve(static_cast<std::size_t>(m_count) * 24 + 128, static_cast<std::size_t>(m_count) * 3 + 16);
        m_dispatch = a.label();
        m_exit = a.label();
        m_outside = a.label();
        m_left = a.label();
        m_step_stub = a.label();
        m_table = a.label();
        m_at.resize(m_count);
        for (ALabel& label : m_at)
            label = a.label();
    }

    std::unique_ptr<Code> write();

private:
    struct Cold {
        ALabel label;
        std::uint32_t pc = 0;
    };

    ALabel cold(std::uint32_t pc)
    {
        ALabel const label = a.label();
        m_cold.push_back(Cold { label, pc });
        return label;
    }
    // A conditional branch to any label of the block, near or around a `b`.
    void branch(ACond condition, ALabel target)
    {
        if (!m_far) {
            a.b(condition, target);
            return;
        }
        ALabel const skip = a.label();
        a.b(inverse(condition), skip);
        a.b(target);
        a.bind(skip);
    }
    void branch_zero(XReg reg, bool when_zero, ALabel target)
    {
        if (!m_far) {
            if (when_zero)
                a.cbz(reg, target);
            else
                a.cbnz(reg, target);
            return;
        }
        ALabel const skip = a.label();
        if (when_zero)
            a.cbnz(reg, skip);
        else
            a.cbz(reg, skip);
        a.b(target);
        a.bind(skip);
    }
    // Loads and stores at any offset: one too large for the instruction is
    // added to the base in far_reg first.
    XReg place(XReg base, std::int64_t& offset, unsigned size)
    {
        if (offset <= INT32_MAX && offset >= INT32_MIN && AssemblerA64::fits_load(static_cast<std::int32_t>(offset), size))
            return base;
        a.mov_imm64(far_reg, static_cast<std::uint64_t>(offset));
        a.add(far_reg, base, far_reg);
        offset = 0;
        return far_reg;
    }
    void load(XReg t, XReg base, std::int64_t offset)
    {
        XReg const at = place(base, offset, 8);
        a.ldr(t, at, static_cast<std::int32_t>(offset));
    }
    void store(XReg t, XReg base, std::int64_t offset)
    {
        XReg const at = place(base, offset, 8);
        a.str(t, at, static_cast<std::int32_t>(offset));
    }
    void load32(XReg t, XReg base, std::int64_t offset)
    {
        XReg const at = place(base, offset, 4);
        a.ldr32(t, at, static_cast<std::int32_t>(offset));
    }
    void store32(XReg t, XReg base, std::int64_t offset)
    {
        XReg const at = place(base, offset, 4);
        a.str32(t, at, static_cast<std::int32_t>(offset));
    }
    void loadb(XReg t, XReg base, std::int64_t offset)
    {
        XReg const at = place(base, offset, 1);
        a.ldrb(t, at, static_cast<std::int32_t>(offset));
    }
    // A register against a constant: the instruction's immediate when it
    // has room, else the constant in x16.
    void compare(XReg reg, std::uint64_t value, bool wide)
    {
        if (value <= 0xFFF) {
            if (wide)
                a.cmp(reg, static_cast<std::uint32_t>(value));
            else
                a.cmp32(reg, static_cast<std::uint32_t>(value));
            return;
        }
        a.mov_imm64(XReg::x16, value);
        if (wide)
            a.cmp(reg, XReg::x16);
        else
            a.cmp32(reg, XReg::x16);
    }
    void push(XReg value)
    {
        a.str_post(value, top_reg, 8);
    }
    void call_helper(std::size_t offset)
    {
        a.ldr(XReg::x16, helpers_reg, static_cast<std::int32_t>(offset));
        a.blr(XReg::x16);
    }
    void call_step(std::uint32_t pc)
    {
        a.mov_imm32(XReg::x0, pc);
        a.bl(m_step_stub);
    }
    // After a step or a call that went on in this frame: on at the next
    // instruction's code when the frame is there, through the table when it
    // is elsewhere.
    void after_step(std::uint32_t pc, bool cold_path)
    {
        if (pc + 1 >= m_count) {
            a.b(m_dispatch);
            return;
        }
        load32(XReg::x1, frame_reg, m_layout.frame_pc);
        compare(XReg::x1, pc + 1, false);
        if (cold_path) {
            branch(ACond::eq, m_at[pc + 1]);
            a.b(m_dispatch);
        } else {
            branch(ACond::ne, m_dispatch);
        }
    }
    void record(std::uint8_t const* byte, std::uint8_t bit)
    {
        if ((*byte & bit) != 0)
            return;
        ALabel const done = a.label();
        a.mov_imm64(XReg::x16, reinterpret_cast<std::uint64_t>(byte));
        a.ldrb(XReg::x17, XReg::x16, 0);
        m_ok &= a.tst(XReg::x17, std::uint64_t { bit });
        a.b(ACond::ne, done);
        m_ok &= a.orr(XReg::x17, XReg::x17, std::uint64_t { bit });
        a.strb(XReg::x17, XReg::x16, 0);
        a.bind(done);
    }
    void require_object(XReg value, XReg scratch, ALabel miss)
    {
        m_ok &= a.orr(scratch, tag_reg, std::uint64_t { Value::OtherTag }); // the bits a cell has none of
        a.tst(value, scratch);
        branch(ACond::ne, miss);
        branch_zero(value, true, miss);
        loadb(scratch, value, m_layout.cell_kind);
        compare(scratch, m_layout.kind_object, false);
        branch(ACond::ne, miss);
    }
    // T0's hit test, as Baseline.cpp's: on a hit x0 the object's slots and
    // x1 the slot, the interpreter's count of hits moved.
    void property_hit(PropertySite const& site, ALabel miss)
    {
        constexpr auto first = static_cast<std::int64_t>(offsetof(PropertySite, first));
        require_object(XReg::x0, XReg::x16, miss);
        load(XReg::x2, XReg::x0, m_layout.object_shape);
        a.mov_imm64(XReg::x3, reinterpret_cast<std::uint64_t>(&site));
        loadb(XReg::x1, XReg::x3, offsetof(PropertySite, state));
        compare(XReg::x1, PropertySite::Monomorphic, false);
        branch(ACond::ne, miss);
        load(XReg::x1, XReg::x3, first + static_cast<std::int64_t>(offsetof(PropertyEntry, shape)));
        a.cmp(XReg::x2, XReg::x1);
        branch(ACond::ne, miss);
        loadb(XReg::x1, XReg::x3, first + static_cast<std::int64_t>(offsetof(PropertyEntry, kind)));
        compare(XReg::x1, static_cast<std::uint8_t>(CacheKind::OwnData), false);
        branch(ACond::ne, miss);
        loadb(XReg::x1, XReg::x2, m_layout.shape_flags);
        m_ok &= a.tst(XReg::x1, std::uint64_t { m_layout.shape_dictionary });
        branch(ACond::ne, miss);
        load(XReg::x16, interpreter_reg, m_layout.interpreter_ic_hits);
        a.add(XReg::x16, XReg::x16, 1u);
        store(XReg::x16, interpreter_reg, m_layout.interpreter_ic_hits);
        load32(XReg::x1, XReg::x3, first + static_cast<std::int64_t>(offsetof(PropertyEntry, slot)));
        load(XReg::x0, XReg::x0, m_layout.object_slots);
    }
    // The binding `hops` environments out at `slot`, reached as T0's
    // scoped_binding reaches it: x1 its address; a missing environment or
    // slot to `miss`.
    void scoped_binding(std::uint32_t hops, std::uint32_t slot, ALabel miss)
    {
        load(XReg::x0, frame_reg, m_layout.frame_envs_base);
        load32(XReg::x1, frame_reg, m_layout.frame_envs_size);
        a.subs32(XReg::x1, XReg::x1, 1u);
        a.ldr_indexed(XReg::x0, XReg::x0, XReg::x1);
        for (std::uint32_t i = 0; i < hops; ++i) {
            branch_zero(XReg::x0, true, miss);
            load(XReg::x0, XReg::x0, m_layout.env_outer);
        }
        branch_zero(XReg::x0, true, miss);
        load(XReg::x1, XReg::x0, m_layout.env_bindings_begin);
        load(XReg::x2, XReg::x0, m_layout.env_bindings_end);
        a.sub(XReg::x2, XReg::x2, XReg::x1);
        compare(XReg::x2, std::uint64_t { slot + 1 } * static_cast<std::uint64_t>(m_layout.binding_size), true);
        branch(ACond::lo, miss);
        std::uint64_t const at = std::uint64_t { slot } * static_cast<std::uint64_t>(m_layout.binding_size);
        if (at != 0) {
            a.mov_imm64(XReg::x16, at);
            a.add(XReg::x1, XReg::x1, XReg::x16);
        }
    }
    bool instruction(std::uint32_t pc);
    bool binary(std::uint32_t pc, Instruction const&);
    bool truth_jump(std::uint32_t pc, Instruction const&, bool jump_if_true, bool pop);
    void step_stub();

    AssemblerA64 a;
    CodeBlock const& m_block;
    FeedbackVector* m_feedback;
    Layout const& m_layout;
    Helpers const& m_helpers;
    CodeSpace& m_space;
    std::uint32_t m_count;
    bool m_far;
    std::vector<ALabel> m_at;
    std::vector<Cold> m_cold;
    ALabel m_dispatch;
    ALabel m_exit;
    ALabel m_outside;
    ALabel m_left;
    ALabel m_step_stub;
    ALabel m_table;
    std::uint32_t m_inline = 0;
    bool m_ok = true; // every immediate the code asked for encoded
};

bool WriterA64::binary(std::uint32_t pc, Instruction const& ins)
{
    auto const op = static_cast<BinaryOp>(ins.a);
    bool compare_op = true;
    ACond condition = ACond::eq;
    switch (op) {
    case BinaryOp::Less:
        condition = ACond::lt;
        break;
    case BinaryOp::LessEqual:
        condition = ACond::le;
        break;
    case BinaryOp::Greater:
        condition = ACond::gt;
        break;
    case BinaryOp::GreaterEqual:
        condition = ACond::ge;
        break;
    case BinaryOp::Equal:
    case BinaryOp::StrictEqual:
        condition = ACond::eq;
        break;
    case BinaryOp::NotEqual:
    case BinaryOp::StrictNotEqual:
        condition = ACond::ne;
        break;
    case BinaryOp::Add:
    case BinaryOp::Subtract:
    case BinaryOp::Multiply:
    case BinaryOp::Remainder:
    case BinaryOp::BitwiseAnd:
    case BinaryOp::BitwiseOr:
    case BinaryOp::BitwiseXor:
    case BinaryOp::LeftShift:
    case BinaryOp::RightShift:
        compare_op = false;
        break;
    default:
        return false;
    }
    ALabel const miss = cold(pc);
    a.ldr(XReg::x0, top_reg, operand(1));
    a.ldr(XReg::x1, top_reg, operand(0));
    a.cmp(XReg::x0, tag_reg);
    branch(ACond::lo, miss);
    a.cmp(XReg::x1, tag_reg);
    branch(ACond::lo, miss);
    if (ins.site != no_site && m_feedback != nullptr)
        record(&m_feedback->operands(ins.site), OperandInt32);
    if (compare_op) {
        // Fused with a conditional jump after it, as on x86 (a pop does
        // not set the flags here).
        if (Instruction const* const next = pc + 2 < m_count ? &m_block.code[pc + 1] : nullptr;
            next != nullptr && (next->op == Opcode::JumpIfFalse || next->op == Opcode::JumpIfTrue) && next->a < m_count) {
            a.sub(top_reg, top_reg, 16u);
            a.cmp32(XReg::x0, XReg::x1);
            branch(next->op == Opcode::JumpIfTrue ? condition : inverse(condition), m_at[next->a]);
            a.b(m_at[pc + 2]);
            return true;
        }
        a.cmp32(XReg::x0, XReg::x1);
        a.cset(XReg::x0, condition);
        m_ok &= a.orr(XReg::x0, XReg::x0, std::uint64_t { Value::ValueFalse }); // false | 1 is true
        a.str(XReg::x0, top_reg, operand(1));
        a.sub(top_reg, top_reg, 8u);
        return true;
    }
    switch (op) {
    case BinaryOp::Add:
        a.adds32(XReg::x0, XReg::x0, XReg::x1);
        branch(ACond::vs, miss);
        break;
    case BinaryOp::Subtract:
        a.subs32(XReg::x0, XReg::x0, XReg::x1);
        branch(ACond::vs, miss);
        break;
    case BinaryOp::Multiply: {
        // The 64-bit product an int32 when it equals its own low half sign-
        // extended; a zero one with a negative factor is −0.
        ALabel const done = a.label();
        a.smull(XReg::x2, XReg::x0, XReg::x1);
        a.cmp_sxtw(XReg::x2, XReg::x2);
        branch(ACond::ne, miss);
        a.mov32(XReg::x2, XReg::x2);
        a.cbnz32(XReg::x2, done);
        a.orr32(XReg::x3, XReg::x0, XReg::x1);
        a.cmp32(XReg::x3, 0u);
        branch(ACond::lt, miss);
        a.bind(done);
        a.mov32(XReg::x0, XReg::x2);
        break;
    }
    case BinaryOp::Remainder:
        a.cmp32(XReg::x1, 0u);
        branch(ACond::le, miss);
        a.cmp32(XReg::x0, 0u);
        branch(ACond::lt, miss);
        a.sdiv32(XReg::x2, XReg::x0, XReg::x1);
        a.msub32(XReg::x0, XReg::x2, XReg::x1, XReg::x0);
        break;
    case BinaryOp::BitwiseAnd:
        a.and32(XReg::x0, XReg::x0, XReg::x1);
        break;
    case BinaryOp::BitwiseOr:
        a.orr32(XReg::x0, XReg::x0, XReg::x1);
        break;
    case BinaryOp::BitwiseXor:
        a.eor32(XReg::x0, XReg::x0, XReg::x1);
        break;
    case BinaryOp::LeftShift:
        a.lslv32(XReg::x0, XReg::x0, XReg::x1); // the count taken mod 32, as §13.9.1 masks it
        break;
    case BinaryOp::RightShift:
        a.asrv32(XReg::x0, XReg::x0, XReg::x1);
        break;
    default:
        break;
    }
    a.orr(XReg::x0, XReg::x0, tag_reg);
    a.str(XReg::x0, top_reg, operand(1));
    a.sub(top_reg, top_reg, 8u);
    return true;
}

bool WriterA64::truth_jump(std::uint32_t pc, Instruction const& ins, bool jump_if_true, bool pop)
{
    if (ins.a >= m_count || pc + 1 >= m_count)
        return false;
    ALabel const miss = cold(pc);
    ALabel const truthy = a.label();
    ALabel const falsy = a.label();
    a.ldr(XReg::x0, top_reg, operand(0));
    a.cmp(XReg::x0, static_cast<std::uint32_t>(Value::ValueTrue));
    a.b(ACond::eq, truthy);
    a.cmp(XReg::x0, static_cast<std::uint32_t>(Value::ValueFalse));
    a.b(ACond::eq, falsy);
    a.cmp(XReg::x0, tag_reg);
    branch(ACond::lo, miss);
    a.cbz32(XReg::x0, falsy);
    a.b(truthy);
    a.bind(jump_if_true ? truthy : falsy);
    if (pop)
        a.sub(top_reg, top_reg, 8u);
    a.b(m_at[ins.a]);
    a.bind(jump_if_true ? falsy : truthy);
    if (pop)
        a.sub(top_reg, top_reg, 8u);
    return true;
}

bool WriterA64::instruction(std::uint32_t pc)
{
    Instruction const& ins = m_block.code[pc];
    auto const push_bits = [&](std::uint64_t bits) {
        a.mov_imm64(XReg::x0, bits);
        push(XReg::x0);
    };
    switch (ins.op) {
    // ---- the stack
    case Opcode::PushUndefined:
        push_bits(Value::ValueUndefined);
        return true;
    case Opcode::PushNull:
        push_bits(Value::ValueNull);
        return true;
    case Opcode::PushTrue:
        push_bits(Value::ValueTrue);
        return true;
    case Opcode::PushFalse:
        push_bits(Value::ValueFalse);
        return true;
    case Opcode::PushEmpty:
        a.str_post(XReg::zr, top_reg, 8);
        return true;
    case Opcode::PushInt:
        if (ins.a > 0x7FFFFFFFu)
            return false;
        if (ins.a <= 0xFFFFu) {
            a.mov_imm32(XReg::x0, ins.a);
        } else {
            a.mov_imm64(XReg::x16, reinterpret_cast<std::uint64_t>(&ins.a));
            a.ldr32(XReg::x0, XReg::x16, 0);
        }
        a.orr(XReg::x0, XReg::x0, tag_reg);
        push(XReg::x0);
        return true;
    case Opcode::PushConstant: {
        if (ins.a >= m_block.constants.size())
            return false;
        Value const& constant = m_block.constants[ins.a];
        if (constant.is_int32() && small(constant.as_int32())) {
            a.mov_imm32(XReg::x0, static_cast<std::uint32_t>(constant.as_int32()));
            a.orr(XReg::x0, XReg::x0, tag_reg);
        } else {
            a.mov_imm64(XReg::x16, reinterpret_cast<std::uint64_t>(&constant));
            a.ldr(XReg::x0, XReg::x16, 0);
        }
        push(XReg::x0);
        return true;
    }
    case Opcode::Pop:
        a.sub(top_reg, top_reg, 8u);
        return true;
    case Opcode::Dup:
        a.ldr(XReg::x0, top_reg, operand(0));
        push(XReg::x0);
        return true;
    case Opcode::Over:
        a.ldr(XReg::x0, top_reg, operand(1));
        push(XReg::x0);
        return true;
    case Opcode::Swap:
        a.ldr(XReg::x0, top_reg, operand(0));
        a.ldr(XReg::x1, top_reg, operand(1));
        a.str(XReg::x1, top_reg, operand(0));
        a.str(XReg::x0, top_reg, operand(1));
        return true;
    case Opcode::LoadReg:
        if (ins.a >= m_block.register_count)
            return false;
        load(XReg::x0, registers_reg, 8 * std::int64_t { ins.a });
        push(XReg::x0);
        return true;
    case Opcode::StoreReg:
        if (ins.a >= m_block.register_count)
            return false;
        a.ldr(XReg::x0, top_reg, operand(0));
        a.sub(top_reg, top_reg, 8u);
        store(XReg::x0, registers_reg, 8 * std::int64_t { ins.a });
        return true;
    case Opcode::StoreRegKeep:
        if (ins.a >= m_block.register_count)
            return false;
        a.ldr(XReg::x0, top_reg, operand(0));
        store(XReg::x0, registers_reg, 8 * std::int64_t { ins.a });
        return true;

    // ---- resolved bindings
    case Opcode::GetLocal: {
        if (ins.a >= m_block.register_count)
            return false;
        ALabel const miss = cold(pc);
        load(XReg::x0, registers_reg, 8 * std::int64_t { ins.a });
        branch_zero(XReg::x0, true, miss);
        push(XReg::x0);
        return true;
    }
    case Opcode::SetLocal: {
        if (ins.a >= m_block.register_count || (ins.flags & 1) != 0)
            return false;
        ALabel const miss = cold(pc);
        load(XReg::x0, registers_reg, 8 * std::int64_t { ins.a });
        branch_zero(XReg::x0, true, miss);
        a.ldr(XReg::x0, top_reg, operand(0));
        store(XReg::x0, registers_reg, 8 * std::int64_t { ins.a });
        return true;
    }
    case Opcode::GetScoped:
    case Opcode::SetScoped: {
        if (!m_layout.scopes || ins.a > 16 || ins.b >= (1u << 20))
            return false;
        ALabel const miss = cold(pc);
        scoped_binding(ins.a, ins.b, miss);
        loadb(XReg::x2, XReg::x1, m_layout.binding_initialized);
        branch_zero(XReg::x2, true, miss);
        if (ins.op == Opcode::GetScoped) {
            load(XReg::x0, XReg::x1, m_layout.binding_value);
            push(XReg::x0);
        } else {
            loadb(XReg::x2, XReg::x1, m_layout.binding_mutable);
            branch_zero(XReg::x2, true, miss);
            a.ldr(XReg::x0, top_reg, operand(0));
            store(XReg::x0, XReg::x1, m_layout.binding_value);
        }
        return true;
    }
    case Opcode::LoadThis: {
        ALabel const miss = cold(pc);
        load(XReg::x0, frame_reg, m_layout.frame_function_env);
        branch_zero(XReg::x0, false, miss);
        load(XReg::x0, frame_reg, m_layout.frame_this);
        branch_zero(XReg::x0, true, miss);
        push(XReg::x0);
        return true;
    }
    case Opcode::LoadArgument: {
        if (!m_layout.arguments || ins.a >= (1u << 24))
            return false;
        ALabel const done = a.label();
        a.mov_imm64(XReg::x0, Value::ValueUndefined);
        load(XReg::x1, frame_reg, m_layout.frame_incoming_size);
        compare(XReg::x1, ins.a, true);
        a.b(ACond::ls, done);
        load(XReg::x1, frame_reg, m_layout.frame_incoming_data);
        load(XReg::x0, XReg::x1, 8 * std::int64_t { ins.a });
        a.bind(done);
        push(XReg::x0);
        return true;
    }

    // ---- calls and returns through the interpreter's own functions
    case Opcode::Call:
        if (m_helpers.call == nullptr || pc + 1 >= m_count)
            return false;
        a.mov_imm32(XReg::x0, pc + 1);
        store32(XReg::x0, frame_reg, m_layout.frame_pc);
        store(top_reg, frame_reg, m_layout.frame_stack_top);
        a.mov(XReg::x0, interpreter_reg);
        a.mov(XReg::x1, frame_reg);
        a.mov(XReg::x2, next_reg);
        a.mov_imm64(XReg::x3, reinterpret_cast<std::uint64_t>(&ins));
        call_helper(offsetof(Helpers, call));
        load(top_reg, frame_reg, m_layout.frame_stack_top);
        a.cmp32(XReg::x0, static_cast<std::uint32_t>(RunStatus::Stepped));
        branch(ACond::ne, m_left);
        load32(XReg::x1, frame_reg, m_layout.frame_pc);
        compare(XReg::x1, pc + 1, false);
        branch(ACond::ne, m_dispatch);
        return true;
    case Opcode::Return:
        if (m_helpers.ret == nullptr)
            return false;
        a.mov_imm32(XReg::x0, pc + 1);
        store32(XReg::x0, frame_reg, m_layout.frame_pc);
        store(top_reg, frame_reg, m_layout.frame_stack_top);
        a.mov(XReg::x0, interpreter_reg);
        a.mov(XReg::x1, frame_reg);
        a.mov(XReg::x2, next_reg);
        call_helper(offsetof(Helpers, ret));
        a.b(m_left);
        return true;
    case Opcode::GetName: {
        if (ins.site == no_site || m_feedback == nullptr || m_helpers.global == nullptr || ins.a >= m_block.names.size())
            return false;
        ALabel const miss = cold(pc);
        a.mov(XReg::x0, interpreter_reg);
        a.mov_imm64(XReg::x1, reinterpret_cast<std::uint64_t>(&m_feedback->property(ins.site)));
        a.mov_imm64(XReg::x2, reinterpret_cast<std::uint64_t>(m_block.names[ins.a]));
        call_helper(offsetof(Helpers, global));
        branch_zero(XReg::x0, true, miss);
        push(XReg::x0);
        return true;
    }

    // ---- operators
    case Opcode::Binary:
        return binary(pc, ins);
    case Opcode::Inc:
    case Opcode::Dec: {
        ALabel const miss = cold(pc);
        a.ldr(XReg::x0, top_reg, operand(0));
        a.cmp(XReg::x0, tag_reg);
        branch(ACond::lo, miss);
        if (ins.op == Opcode::Inc)
            a.adds32(XReg::x0, XReg::x0, 1u);
        else
            a.subs32(XReg::x0, XReg::x0, 1u);
        branch(ACond::vs, miss);
        a.orr(XReg::x0, XReg::x0, tag_reg);
        a.str(XReg::x0, top_reg, operand(0));
        return true;
    }
    case Opcode::ToNumeric: {
        ALabel const miss = cold(pc);
        a.ldr(XReg::x0, top_reg, operand(0));
        a.tst(XReg::x0, tag_reg);
        branch(ACond::eq, miss);
        return true;
    }

    // ---- control
    case Opcode::Jump:
        if (ins.a >= m_count)
            return false;
        a.b(m_at[ins.a]);
        return true;
    case Opcode::JumpIfTrue:
        return truth_jump(pc, ins, true, true);
    case Opcode::JumpIfFalse:
        return truth_jump(pc, ins, false, true);
    case Opcode::JumpIfTrueKeep:
        return truth_jump(pc, ins, true, false);
    case Opcode::JumpIfFalseKeep:
        return truth_jump(pc, ins, false, false);
    case Opcode::JumpIfNullish:
    case Opcode::JumpIfNotNullishKeep:
    case Opcode::JumpIfNotUndefined:
    case Opcode::JumpIfEmpty:
        if (ins.a >= m_count)
            return false;
        a.ldr(XReg::x0, top_reg, operand(0));
        if (ins.op != Opcode::JumpIfNotNullishKeep)
            a.sub(top_reg, top_reg, 8u);
        if (ins.op == Opcode::JumpIfNullish || ins.op == Opcode::JumpIfNotNullishKeep) {
            m_ok &= a.and_(XReg::x0, XReg::x0, ~std::uint64_t { Value::UndefinedTag });
            a.cmp(XReg::x0, static_cast<std::uint32_t>(Value::ValueNull));
            branch(ins.op == Opcode::JumpIfNullish ? ACond::eq : ACond::ne, m_at[ins.a]);
        } else if (ins.op == Opcode::JumpIfNotUndefined) {
            a.cmp(XReg::x0, static_cast<std::uint32_t>(Value::ValueUndefined));
            branch(ACond::ne, m_at[ins.a]);
        } else {
            branch_zero(XReg::x0, true, m_at[ins.a]);
        }
        return true;
    case Opcode::Step: {
        if (m_layout.budget == nullptr)
            return false;
        ALabel const miss = cold(pc);
        a.mov_imm64(XReg::x16, reinterpret_cast<std::uint64_t>(m_layout.budget));
        a.ldr32(XReg::x0, XReg::x16, 0);
        a.cmp32(XReg::x0, 1u);
        branch(ACond::le, miss);
        a.subs32(XReg::x0, XReg::x0, 1u);
        a.str32(XReg::x0, XReg::x16, 0);
        return true;
    }

    // ---- members
    case Opcode::GetMemberNamed: {
        if (ins.site == no_site || m_feedback == nullptr || ins.a >= m_block.names.size())
            return false;
        PropertySite& site = m_feedback->property(ins.site);
        ALabel const miss = cold(pc);
        ALabel const other = a.label();
        ALabel const done = a.label();
        a.ldr(XReg::x0, top_reg, operand(0));
        property_hit(site, other);
        a.ldr_indexed(XReg::x0, XReg::x0, XReg::x1);
        a.str(XReg::x0, top_reg, operand(0));
        a.b(done);
        a.bind(other);
        if (m_helpers.named != nullptr) {
            a.mov(XReg::x0, interpreter_reg);
            a.mov_imm64(XReg::x1, reinterpret_cast<std::uint64_t>(&site));
            a.sub(XReg::x2, top_reg, 8u);
            a.mov_imm64(XReg::x3, reinterpret_cast<std::uint64_t>(m_block.names[ins.a]));
            call_helper(offsetof(Helpers, named));
            branch_zero(XReg::x0, true, miss);
            a.str(XReg::x0, top_reg, operand(0));
        } else {
            a.b(miss);
        }
        a.bind(done);
        return true;
    }
    case Opcode::PutMemberNamed:
    case Opcode::PutMemberNamedKeep: {
        if (ins.site == no_site || m_feedback == nullptr)
            return false;
        ALabel const miss = cold(pc);
        a.ldr(XReg::x0, top_reg, operand(1));
        property_hit(m_feedback->property(ins.site), miss);
        a.ldr(XReg::x2, top_reg, operand(0));
        a.str_indexed(XReg::x2, XReg::x0, XReg::x1);
        if (ins.op == Opcode::PutMemberNamedKeep) {
            a.str(XReg::x2, top_reg, operand(1));
            a.sub(top_reg, top_reg, 8u);
        } else {
            a.sub(top_reg, top_reg, 16u);
        }
        return true;
    }
    case Opcode::GetMember: {
        if (!m_layout.arrays)
            return false;
        ALabel const miss = cold(pc);
        a.ldr(XReg::x1, top_reg, operand(0));
        a.cmp(XReg::x1, tag_reg);
        branch(ACond::lo, miss);
        a.cmp32(XReg::x1, 0u);
        branch(ACond::lt, miss);
        a.ldr(XReg::x0, top_reg, operand(1));
        require_object(XReg::x0, XReg::x16, miss);
        loadb(XReg::x2, XReg::x0, m_layout.object_class);
        compare(XReg::x2, m_layout.class_array, false);
        branch(ACond::ne, miss);
        a.mov32(XReg::x1, XReg::x1);
        load32(XReg::x2, XReg::x0, m_layout.array_size);
        a.cmp32(XReg::x1, XReg::x2);
        branch(ACond::hs, miss);
        load(XReg::x2, XReg::x0, m_layout.array_data);
        a.ldr_indexed(XReg::x0, XReg::x2, XReg::x1);
        branch_zero(XReg::x0, true, miss);
        if (ins.site != no_site && m_feedback != nullptr)
            record(&m_feedback->element(ins.site).kinds, ElementDense);
        a.str(XReg::x0, top_reg, operand(1));
        a.sub(top_reg, top_reg, 8u);
        return true;
    }
    default:
        return false;
    }
}

void WriterA64::step_stub()
{
    // Entered by `bl` with the pc in w0; x30 kept for its own call.
    a.bind(m_step_stub);
    a.str_pre(XReg::x30, XReg::sp, -16);
    store32(XReg::x0, frame_reg, m_layout.frame_pc);
    store(top_reg, frame_reg, m_layout.frame_stack_top);
    a.mov(XReg::x0, interpreter_reg);
    a.mov(XReg::x1, frame_reg);
    a.mov(XReg::x2, next_reg);
    call_helper(offsetof(Helpers, step));
    load(top_reg, frame_reg, m_layout.frame_stack_top);
    a.ldr_post(XReg::x30, XReg::sp, 16);
    a.cmp32(XReg::x0, static_cast<std::uint32_t>(RunStatus::Stepped));
    ALabel const leave = a.label();
    a.b(ACond::ne, leave);
    a.ret();
    a.bind(leave);
    a.b(m_left); // the stub's caller is let go of: x30 is not used again
}

std::unique_ptr<Code> WriterA64::write()
{
    // The prologue: the frame record (x29, x30) first, then the pinned
    // registers in pairs, the stack 16-aligned throughout.
    a.stp_pre(XReg::x29, XReg::x30, XReg::sp, -16);
    a.mov(XReg::x29, XReg::sp);
    a.stp_pre(XReg::x19, XReg::x20, XReg::sp, -16);
    a.stp_pre(XReg::x21, XReg::x22, XReg::sp, -16);
    a.stp_pre(XReg::x23, XReg::x24, XReg::sp, -16);
    a.stp_pre(XReg::x25, XReg::x26, XReg::sp, -16);
    a.mov(interpreter_reg, XReg::x0);
    a.mov(frame_reg, XReg::x1);
    a.mov(next_reg, XReg::x2);
    a.movz(tag_reg, static_cast<std::uint16_t>(Value::NumberTag >> 48), 48);
    a.mov_imm64(helpers_reg, reinterpret_cast<std::uint64_t>(&m_helpers));
    load(registers_reg, frame_reg, m_layout.frame_registers);
    load(top_reg, frame_reg, m_layout.frame_stack_top);
    load(XReg::x0, frame_reg, m_layout.frame_code);
    load(XReg::x0, XReg::x0, m_layout.block_code);
    load(table_reg, XReg::x0, static_cast<std::int64_t>(offsetof(Code, table)));

    // frame.pc to its instruction's code, or out when it names none.
    a.bind(m_dispatch);
    load32(XReg::x0, frame_reg, m_layout.frame_pc);
    compare(XReg::x0, m_count, false);
    branch(ACond::hs, m_outside);
    a.ldr_indexed(XReg::x1, table_reg, XReg::x0);
    a.br(XReg::x1);
    a.bind(m_outside);
    a.mov_imm32(XReg::x0, bad_pc);
    a.b(m_exit);

    // A status other than Stepped: a switch to a frame whose block has
    // machine code made here (that block's table taken up), else out.
    a.bind(m_left);
    if (m_layout.switches) {
        a.cmp32(XReg::x0, static_cast<std::uint32_t>(RunStatus::Switched));
        a.b(ACond::ne, m_exit);
        a.ldr(XReg::x8, next_reg, 0);
        load(XReg::x2, XReg::x8, m_layout.frame_code);
        load(XReg::x2, XReg::x2, m_layout.block_code);
        a.cbz(XReg::x2, m_exit);
        a.mov(frame_reg, XReg::x8);
        load(registers_reg, frame_reg, m_layout.frame_registers);
        load(top_reg, frame_reg, m_layout.frame_stack_top);
        load(table_reg, XReg::x2, static_cast<std::int64_t>(offsetof(Code, table)));
        load32(XReg::x0, frame_reg, m_layout.frame_pc);
        load32(XReg::x1, XReg::x2, static_cast<std::int64_t>(offsetof(Code, instructions)));
        a.cmp32(XReg::x0, XReg::x1);
        a.b(ACond::hs, m_outside);
        a.ldr_indexed(XReg::x1, table_reg, XReg::x0);
        a.br(XReg::x1);
    }
    a.bind(m_exit);
    a.ldp_post(XReg::x25, XReg::x26, XReg::sp, 16);
    a.ldp_post(XReg::x23, XReg::x24, XReg::sp, 16);
    a.ldp_post(XReg::x21, XReg::x22, XReg::sp, 16);
    a.ldp_post(XReg::x19, XReg::x20, XReg::sp, 16);
    a.ldp_post(XReg::x29, XReg::x30, XReg::sp, 16);
    a.ret();

    std::size_t const start = a.size();
    for (std::uint32_t pc = 0; pc < m_count; ++pc) {
        a.bind(m_at[pc]);
        if (instruction(pc)) {
            ++m_inline;
            continue;
        }
        call_step(pc);
        after_step(pc, false);
    }
    a.b(m_dispatch);
    for (Cold const& path : m_cold) {
        a.bind(path.label);
        call_step(path.pc);
        after_step(path.pc, true);
    }
    step_stub();
    std::size_t const code_bytes = a.size() - start;

    // The table: each instruction's absolute address.
    a.align(8);
    a.bind(m_table);
    for (std::uint32_t pc = 0; pc < m_count; ++pc)
        a.emit_u64(0);
    if (!m_ok || !a.finish())
        return nullptr;

    std::byte* const base = m_space.reserve(a.size());
    if (base == nullptr)
        return nullptr;
    std::memcpy(base, a.words().data(), a.size());
    std::uint32_t const table_offset = a.offset_of(m_table);
    for (std::uint32_t pc = 0; pc < m_count; ++pc) {
        auto const address = reinterpret_cast<std::uint64_t>(base + a.offset_of(m_at[pc]));
        std::memcpy(base + table_offset + 8 * static_cast<std::size_t>(pc), &address, sizeof address);
    }
    if (!m_space.seal(base, a.size(), 0))
        return nullptr;
    auto code = std::make_unique<Code>();
    code->start = base;
    code->size = a.size();
    code->entry = reinterpret_cast<Code::Entry>(base);
    code->instructions = m_count;
    code->inline_instructions = m_inline;
    code->code_bytes = code_bytes;
    code->table = reinterpret_cast<void const* const*>(base + table_offset);
    code->step_stub = base + a.offset_of(m_step_stub);
    return code;
}

}

std::unique_ptr<Code> compile_a64(CodeBlock const& block, FeedbackVector* feedback, Layout const& layout, Helpers const& helpers, CodeSpace& space)
{
    return WriterA64(block, feedback, layout, helpers, space).write();
}

}
