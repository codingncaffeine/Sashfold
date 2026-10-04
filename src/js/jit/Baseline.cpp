#include "js/jit/Baseline.h"

#include "js/Bytecode.h"
#include "js/Feedback.h"
#include "js/jit/AssemblerX64.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sashfold::js::jit {

namespace {

// The code's registers for its whole run (callee-saved, so the calls out
// keep them): the interpreter, the frame, the number tag (an int32 is a
// value at or above it, unsigned, and boxing one is an or with it), the
// frame's registers and the operand stack's top. The top is the code's own
// between call outs: written to the frame before each, read back after.
constexpr Reg interpreter_reg = Reg::rbx;
constexpr Reg frame_reg = Reg::r12;
constexpr Reg tag_reg = Reg::r13;
constexpr Reg registers_reg = Reg::r14;
constexpr Reg top_reg = Reg::r15;
// Where the next frame goes (Frame**): the prologue's alignment slot, under
// rbp and the five registers it saves.
constexpr std::int32_t next_slot = -48;

// The operand `below` values under the top (0: the top itself).
Mem operand(std::uint32_t below)
{
    return Mem::at(top_reg, -8 * static_cast<std::int32_t>(below + 1));
}

Cond inverse(Cond condition)
{
    return static_cast<Cond>(static_cast<std::uint8_t>(condition) ^ 1);
}

// An immediate a page cannot choose more than sixteen bits of: wider
// numbers are loaded from the block's pools (§7, no page-chosen constant in
// executable bytes).
bool small(std::int32_t value)
{
    return value >= -0x8000 && value <= 0xFFFF;
}

class Writer {
public:
    Writer(CodeBlock const& block, FeedbackVector* feedback, Layout const& layout)
        : m_block(block)
        , m_feedback(feedback)
        , m_layout(layout)
        , m_count(static_cast<std::uint32_t>(block.code.size()))
    {
        m_dispatch = a.label();
        m_exit = a.label();
        m_outside = a.label();
        m_table = a.label();
        m_step = a.label();
        m_at.resize(m_count);
        for (Label& label : m_at)
            label = a.label();
    }

    std::unique_ptr<Code> write(StepHelper step);

private:
    struct Cold {
        Label label;
        std::uint32_t pc = 0;
    };

    // The instruction at `pc` run by the interpreter's handler: its pc and
    // the top written to the frame, the step called, the top read back,
    // and out of the code with any status but Stepped.
    void call_step(std::uint32_t pc);
    // A path out of line to the step for the instruction at `pc`, taken
    // when a template's fast path does not apply — before the template has
    // changed anything, so the handler runs the instruction whole.
    Label cold(std::uint32_t pc)
    {
        Label const label = a.label();
        m_cold.push_back(Cold { label, pc });
        return label;
    }
    void push(Reg value)
    {
        a.mov(Mem::at(top_reg), value);
        a.add(top_reg, 8);
    }
    Mem reg(std::uint32_t index) const { return Mem::at(registers_reg, 8 * static_cast<std::int32_t>(index)); }
    // The template for the instruction at `pc`; false when it has none.
    bool instruction(std::uint32_t pc);
    bool binary(std::uint32_t pc, Instruction const&);
    bool truth_jump(std::uint32_t pc, Instruction const&, bool jump_if_true, bool pop);
    // `object` (a value) an object of the one shape its site has seen, the
    // property an own data property of a shared shape — T0's hit test —
    // else to `miss`; on a hit the interpreter's count of them moved, rdx
    // the slot and `object` the object's slots.
    void property_hit(Reg object, PropertySite const&, Label miss);
    // `value` an object, else to `miss`; `scratch` is used.
    void require_object(Reg value, Reg scratch, Label miss);
    // The binding `hops` environments out from the frame's innermost, at
    // `slot`, reached as T0's scoped_binding reaches it: rcx the start of
    // the environment's bindings on the way on, a missing environment or
    // slot to `miss` (the step reports the engine's fault).
    void scoped_binding(std::uint32_t hops, std::uint32_t slot, Label miss);
    // A bit of a site's feedback set as T0 sets it, unless it already was
    // when the code was written (bits are only ever added).
    void record(std::uint8_t const* byte, std::uint8_t bit);

    AssemblerX64 a;
    CodeBlock const& m_block;
    FeedbackVector* m_feedback;
    Layout const& m_layout;
    std::uint32_t m_count;
    std::vector<Label> m_at;
    std::vector<Cold> m_cold;
    Label m_dispatch;
    Label m_exit;
    Label m_outside;
    Label m_table;
    Label m_step;
    std::uint32_t m_inline = 0;
};

void Writer::call_step(std::uint32_t pc)
{
    a.mov32(Mem::at(frame_reg, m_layout.frame_pc), pc);
    a.mov(Mem::at(frame_reg, m_layout.frame_stack_top), top_reg);
    a.mov(Reg::rdi, interpreter_reg);
    a.mov(Reg::rsi, frame_reg);
    a.mov(Reg::rdx, Mem::at(Reg::rbp, next_slot));
    a.call(m_step);
    a.mov(top_reg, Mem::at(frame_reg, m_layout.frame_stack_top));
    a.cmp32(Reg::rax, static_cast<std::int32_t>(RunStatus::Stepped));
    a.j(Cond::NotEqual, m_exit);
}

void Writer::record(std::uint8_t const* byte, std::uint8_t bit)
{
    if ((*byte & bit) != 0)
        return;
    Label const done = a.label();
    a.mov_imm64(Reg::rdx, reinterpret_cast<std::uint64_t>(byte));
    a.test8(Mem::at(Reg::rdx), bit);
    a.j(Cond::NotEqual, done);
    a.or8(Mem::at(Reg::rdx), bit);
    a.bind(done);
}

void Writer::scoped_binding(std::uint32_t hops, std::uint32_t slot, Label miss)
{
    a.mov(Reg::rax, Mem::at(frame_reg, m_layout.frame_envs_base));
    a.mov32(Reg::rcx, Mem::at(frame_reg, m_layout.frame_envs_size));
    a.mov(Reg::rax, Mem::at_index(Reg::rax, Reg::rcx, 8, -8));
    for (std::uint32_t i = 0; i < hops; ++i) {
        a.test(Reg::rax, Reg::rax);
        a.j(Cond::Equal, miss);
        a.mov(Reg::rax, Mem::at(Reg::rax, m_layout.env_outer));
    }
    a.test(Reg::rax, Reg::rax);
    a.j(Cond::Equal, miss);
    a.mov(Reg::rcx, Mem::at(Reg::rax, m_layout.env_bindings_begin));
    a.mov(Reg::rdx, Mem::at(Reg::rax, m_layout.env_bindings_end));
    a.sub(Reg::rdx, Reg::rcx);
    a.cmp(Reg::rdx, static_cast<std::int32_t>((slot + 1) * static_cast<std::uint32_t>(m_layout.binding_size)));
    a.j(Cond::Below, miss);
}

void Writer::require_object(Reg value, Reg scratch, Label miss)
{
    // A cell: no number tag, no other tag, not the empty value's zero.
    a.lea(scratch, Mem::at(tag_reg, static_cast<std::int32_t>(Value::OtherTag)));
    a.test(value, scratch);
    a.j(Cond::NotEqual, miss);
    a.test(value, value);
    a.j(Cond::Equal, miss);
    a.cmp8(Mem::at(value, m_layout.cell_kind), m_layout.kind_object);
    a.j(Cond::NotEqual, miss);
}

void Writer::property_hit(Reg object, PropertySite const& site, Label miss)
{
    constexpr auto first = static_cast<std::int32_t>(offsetof(PropertySite, first));
    require_object(object, Reg::rcx, miss);
    a.mov(Reg::rdx, Mem::at(object, m_layout.object_shape));
    a.mov_imm64(Reg::rcx, reinterpret_cast<std::uint64_t>(&site));
    a.cmp8(Mem::at(Reg::rcx, static_cast<std::int32_t>(offsetof(PropertySite, state))), PropertySite::Monomorphic);
    a.j(Cond::NotEqual, miss);
    a.cmp(Reg::rdx, Mem::at(Reg::rcx, first + static_cast<std::int32_t>(offsetof(PropertyEntry, shape))));
    a.j(Cond::NotEqual, miss);
    a.cmp8(Mem::at(Reg::rcx, first + static_cast<std::int32_t>(offsetof(PropertyEntry, kind))), static_cast<std::uint8_t>(CacheKind::OwnData));
    a.j(Cond::NotEqual, miss);
    a.test8(Mem::at(Reg::rdx, m_layout.shape_flags), m_layout.shape_dictionary);
    a.j(Cond::NotEqual, miss);
    a.add(Mem::at(interpreter_reg, m_layout.interpreter_ic_hits), 1);
    a.mov32(Reg::rdx, Mem::at(Reg::rcx, first + static_cast<std::int32_t>(offsetof(PropertyEntry, slot))));
    a.mov(object, Mem::at(object, m_layout.object_slots));
}

bool Writer::binary(std::uint32_t pc, Instruction const& ins)
{
    auto const op = static_cast<BinaryOp>(ins.a);
    bool compare = true;
    Cond condition = Cond::Equal;
    switch (op) {
    case BinaryOp::Less:
        condition = Cond::Less;
        break;
    case BinaryOp::LessEqual:
        condition = Cond::LessOrEqual;
        break;
    case BinaryOp::Greater:
        condition = Cond::Greater;
        break;
    case BinaryOp::GreaterEqual:
        condition = Cond::GreaterOrEqual;
        break;
    case BinaryOp::Equal:
    case BinaryOp::StrictEqual:
        condition = Cond::Equal;
        break;
    case BinaryOp::NotEqual:
    case BinaryOp::StrictNotEqual:
        condition = Cond::NotEqual;
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
        compare = false;
        break;
    default:
        return false;
    }
    Label const miss = cold(pc);
    a.mov(Reg::rax, operand(1));
    a.mov(Reg::rcx, operand(0));
    a.cmp(Reg::rax, tag_reg);
    a.j(Cond::Below, miss);
    a.cmp(Reg::rcx, tag_reg);
    a.j(Cond::Below, miss);
    if (ins.site != no_site && m_feedback != nullptr)
        record(&m_feedback->operands(ins.site), OperandInt32);
    if (compare) {
        // A comparison a conditional jump takes at once: the jump made
        // here, with no boolean between them (the operands popped first,
        // since the pop sets the flags). The jump's own code is still there
        // for an entry at it (after this one's step).
        if (Instruction const* const next = pc + 2 < m_count ? &m_block.code[pc + 1] : nullptr;
            next != nullptr && (next->op == Opcode::JumpIfFalse || next->op == Opcode::JumpIfTrue) && next->a < m_count) {
            a.sub(top_reg, 16);
            a.cmp32(Reg::rax, Reg::rcx);
            a.j(next->op == Opcode::JumpIfTrue ? condition : inverse(condition), m_at[next->a]);
            a.jmp(m_at[pc + 2]);
            return true;
        }
        a.cmp32(Reg::rax, Reg::rcx);
        a.set(condition, Reg::rax);
        a.movzx8(Reg::rax, Reg::rax);
        a.or_(Reg::rax, static_cast<std::int32_t>(Value::ValueFalse)); // false | 1 is true
        a.mov(operand(1), Reg::rax);
        a.sub(top_reg, 8);
        return true;
    }
    // int32 arithmetic as Interpreter::Impl::int32_binary does it: what is
    // not exactly an int32 (an overflow, a −0) leaves for the step.
    switch (op) {
    case BinaryOp::Add:
        a.add32(Reg::rax, Reg::rcx);
        a.j(Cond::Overflow, miss);
        break;
    case BinaryOp::Subtract:
        a.sub32(Reg::rax, Reg::rcx);
        a.j(Cond::Overflow, miss);
        break;
    case BinaryOp::Multiply: {
        // A zero product with a negative factor is −0, a double.
        Label const done = a.label();
        a.mov32(Reg::rdx, Reg::rax);
        a.imul32(Reg::rax, Reg::rcx);
        a.j(Cond::Overflow, miss);
        a.test32(Reg::rax, Reg::rax);
        a.j(Cond::NotEqual, done);
        a.or32(Reg::rdx, Reg::rcx);
        a.j(Cond::Sign, miss);
        a.bind(done);
        break;
    }
    case BinaryOp::Remainder:
        // A non-negative dividend by a positive divisor; the rest (a zero
        // divisor's NaN, a negative dividend's −0) by the step.
        a.test32(Reg::rcx, Reg::rcx);
        a.j(Cond::LessOrEqual, miss);
        a.test32(Reg::rax, Reg::rax);
        a.j(Cond::Sign, miss);
        a.cdq();
        a.idiv32(Reg::rcx);
        a.mov32(Reg::rax, Reg::rdx);
        break;
    case BinaryOp::BitwiseAnd:
        a.and32(Reg::rax, Reg::rcx);
        break;
    case BinaryOp::BitwiseOr:
        a.or32(Reg::rax, Reg::rcx);
        break;
    case BinaryOp::BitwiseXor:
        a.xor32(Reg::rax, Reg::rcx);
        break;
    case BinaryOp::LeftShift:
        a.shl32_cl(Reg::rax); // the count masked to five bits, as §13.9.1 masks it
        break;
    case BinaryOp::RightShift:
        a.sar32_cl(Reg::rax);
        break;
    default:
        break;
    }
    // A 32-bit result leaves the register's upper half clear: the tag
    // makes it a value.
    a.or_(Reg::rax, tag_reg);
    a.mov(operand(1), Reg::rax);
    a.sub(top_reg, 8);
    return true;
}

bool Writer::truth_jump(std::uint32_t pc, Instruction const& ins, bool jump_if_true, bool pop)
{
    if (ins.a >= m_count || pc + 1 >= m_count)
        return false;
    // ToBoolean of a boolean or an int32; anything else by the step.
    Label const miss = cold(pc);
    Label const truthy = a.label();
    Label const falsy = a.label();
    a.mov(Reg::rax, operand(0));
    a.cmp(Reg::rax, static_cast<std::int32_t>(Value::ValueTrue));
    a.j(Cond::Equal, truthy);
    a.cmp(Reg::rax, static_cast<std::int32_t>(Value::ValueFalse));
    a.j(Cond::Equal, falsy);
    a.cmp(Reg::rax, tag_reg);
    a.j(Cond::Below, miss);
    a.test32(Reg::rax, Reg::rax);
    a.j(Cond::Equal, falsy);
    a.jmp(truthy);
    // The side that jumps, then the side that goes on into the next
    // instruction's code.
    a.bind(jump_if_true ? truthy : falsy);
    if (pop)
        a.sub(top_reg, 8);
    a.jmp(m_at[ins.a]);
    a.bind(jump_if_true ? falsy : truthy);
    if (pop)
        a.sub(top_reg, 8);
    return true;
}

bool Writer::instruction(std::uint32_t pc)
{
    Instruction const& ins = m_block.code[pc];
    auto const push_bits = [&](std::uint64_t bits) {
        a.mov(Mem::at(top_reg), static_cast<std::int32_t>(bits));
        a.add(top_reg, 8);
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
        push_bits(Value::ValueEmpty);
        return true;
    case Opcode::PushInt:
        if (ins.a > 0x7FFFFFFFu)
            return false; // a double
        if (ins.a <= 0xFFFFu) {
            a.mov32(Reg::rax, ins.a);
        } else {
            a.mov_imm64(Reg::rax, reinterpret_cast<std::uint64_t>(&ins.a));
            a.mov32(Reg::rax, Mem::at(Reg::rax));
        }
        a.or_(Reg::rax, tag_reg);
        push(Reg::rax);
        return true;
    case Opcode::PushConstant: {
        if (ins.a >= m_block.constants.size())
            return false;
        Value const& constant = m_block.constants[ins.a];
        if (constant.is_int32() && small(constant.as_int32())) {
            a.mov32(Reg::rax, static_cast<std::uint32_t>(constant.as_int32()));
            a.or_(Reg::rax, tag_reg);
        } else {
            a.mov_imm64(Reg::rax, reinterpret_cast<std::uint64_t>(&constant));
            a.mov(Reg::rax, Mem::at(Reg::rax));
        }
        push(Reg::rax);
        return true;
    }
    case Opcode::Pop:
        a.sub(top_reg, 8);
        return true;
    case Opcode::Dup:
        a.mov(Reg::rax, operand(0));
        push(Reg::rax);
        return true;
    case Opcode::Over:
        a.mov(Reg::rax, operand(1));
        push(Reg::rax);
        return true;
    case Opcode::Swap:
        a.mov(Reg::rax, operand(0));
        a.mov(Reg::rcx, operand(1));
        a.mov(operand(0), Reg::rcx);
        a.mov(operand(1), Reg::rax);
        return true;
    case Opcode::LoadReg:
        if (ins.a >= m_block.register_count)
            return false;
        a.mov(Reg::rax, reg(ins.a));
        push(Reg::rax);
        return true;
    case Opcode::StoreReg:
        if (ins.a >= m_block.register_count)
            return false;
        a.mov(Reg::rax, operand(0));
        a.sub(top_reg, 8);
        a.mov(reg(ins.a), Reg::rax);
        return true;
    case Opcode::StoreRegKeep:
        if (ins.a >= m_block.register_count)
            return false;
        a.mov(Reg::rax, operand(0));
        a.mov(reg(ins.a), Reg::rax);
        return true;

    // ---- resolved bindings: the dead zone (the hole) by the step
    case Opcode::GetLocal: {
        if (ins.a >= m_block.register_count)
            return false;
        Label const miss = cold(pc);
        a.mov(Reg::rax, reg(ins.a));
        a.test(Reg::rax, Reg::rax);
        a.j(Cond::Equal, miss);
        push(Reg::rax);
        return true;
    }
    case Opcode::SetLocal: {
        if (ins.a >= m_block.register_count || (ins.flags & 1) != 0)
            return false; // a const's TypeError by the step
        Label const miss = cold(pc);
        a.mov(Reg::rax, reg(ins.a));
        a.test(Reg::rax, Reg::rax);
        a.j(Cond::Equal, miss);
        a.mov(Reg::rax, operand(0));
        a.mov(reg(ins.a), Reg::rax);
        return true;
    }

    case Opcode::GetScoped:
    case Opcode::SetScoped: {
        // A binding of an environment out from this one (a closure's): its
        // dead zone, and a const's or a function name's refusal, by the step.
        if (!m_layout.scopes || ins.a > 16 || ins.b >= (1u << 20))
            return false;
        Label const miss = cold(pc);
        scoped_binding(ins.a, ins.b, miss);
        std::int32_t const at = static_cast<std::int32_t>(ins.b) * m_layout.binding_size;
        a.cmp8(Mem::at(Reg::rcx, at + m_layout.binding_initialized), 0);
        a.j(Cond::Equal, miss);
        if (ins.op == Opcode::GetScoped) {
            a.mov(Reg::rax, Mem::at(Reg::rcx, at + m_layout.binding_value));
            push(Reg::rax);
        } else {
            a.cmp8(Mem::at(Reg::rcx, at + m_layout.binding_mutable), 0);
            a.j(Cond::Equal, miss);
            a.mov(Reg::rax, operand(0));
            a.mov(Mem::at(Reg::rcx, at + m_layout.binding_value), Reg::rax);
        }
        return true;
    }
    case Opcode::LoadThis: {
        // The frame's own `this`; one kept in the function's environment,
        // and a derived constructor's before super(), by the step.
        Label const miss = cold(pc);
        a.mov(Reg::rax, Mem::at(frame_reg, m_layout.frame_function_env));
        a.test(Reg::rax, Reg::rax);
        a.j(Cond::NotEqual, miss);
        a.mov(Reg::rax, Mem::at(frame_reg, m_layout.frame_this));
        a.test(Reg::rax, Reg::rax);
        a.j(Cond::Equal, miss);
        push(Reg::rax);
        return true;
    }
    case Opcode::LoadArgument: {
        // The call's argument, or undefined past the last.
        if (!m_layout.arguments || ins.a >= (1u << 24))
            return false;
        Label const done = a.label();
        a.mov32(Reg::rax, static_cast<std::uint32_t>(Value::ValueUndefined));
        a.mov(Reg::rcx, Mem::at(frame_reg, m_layout.frame_incoming_size));
        a.cmp(Reg::rcx, static_cast<std::int32_t>(ins.a));
        a.j(Cond::BelowOrEqual, done);
        a.mov(Reg::rcx, Mem::at(frame_reg, m_layout.frame_incoming_data));
        a.mov(Reg::rax, Mem::at(Reg::rcx, 8 * static_cast<std::int32_t>(ins.a)));
        a.bind(done);
        push(Reg::rax);
        return true;
    }

    // ---- operators
    case Opcode::Binary:
        return binary(pc, ins);
    case Opcode::Inc:
    case Opcode::Dec: {
        Label const miss = cold(pc);
        a.mov(Reg::rax, operand(0));
        a.cmp(Reg::rax, tag_reg);
        a.j(Cond::Below, miss);
        if (ins.op == Opcode::Inc)
            a.add32(Reg::rax, 1);
        else
            a.sub32(Reg::rax, 1);
        a.j(Cond::Overflow, miss);
        a.or_(Reg::rax, tag_reg);
        a.mov(operand(0), Reg::rax);
        return true;
    }
    case Opcode::ToNumeric: {
        Label const miss = cold(pc);
        a.mov(Reg::rax, operand(0));
        a.test(Reg::rax, tag_reg); // a number is its own
        a.j(Cond::Equal, miss);
        return true;
    }

    // ---- control
    case Opcode::Jump:
        if (ins.a >= m_count)
            return false;
        a.jmp(m_at[ins.a]);
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
    case Opcode::JumpIfEmpty: {
        if (ins.a >= m_count)
            return false;
        a.mov(Reg::rax, operand(0));
        if (ins.op != Opcode::JumpIfNotNullishKeep)
            a.sub(top_reg, 8);
        if (ins.op == Opcode::JumpIfNullish || ins.op == Opcode::JumpIfNotNullishKeep) {
            // null and undefined differ in the undefined tag alone
            a.and_(Reg::rax, ~static_cast<std::int32_t>(Value::UndefinedTag));
            a.cmp(Reg::rax, static_cast<std::int32_t>(Value::ValueNull));
            a.j(ins.op == Opcode::JumpIfNullish ? Cond::Equal : Cond::NotEqual, m_at[ins.a]);
        } else if (ins.op == Opcode::JumpIfNotUndefined) {
            a.cmp(Reg::rax, static_cast<std::int32_t>(Value::ValueUndefined));
            a.j(Cond::NotEqual, m_at[ins.a]);
        } else {
            a.test(Reg::rax, Reg::rax);
            a.j(Cond::Equal, m_at[ins.a]);
        }
        return true;
    }
    case Opcode::Step: {
        // The interrupt budget: spent here while more than one is left;
        // the last one, and the interrupt it brings, by the step.
        if (m_layout.budget == nullptr)
            return false;
        Label const miss = cold(pc);
        a.mov_imm64(Reg::rax, reinterpret_cast<std::uint64_t>(m_layout.budget));
        a.cmp32(Mem::at(Reg::rax), 1);
        a.j(Cond::LessOrEqual, miss);
        a.sub32(Mem::at(Reg::rax), 1);
        return true;
    }

    // ---- members: the site's one answer, an own data property
    case Opcode::GetMemberNamed: {
        if (ins.site == no_site || m_feedback == nullptr)
            return false;
        Label const miss = cold(pc);
        a.mov(Reg::rax, operand(0));
        property_hit(Reg::rax, m_feedback->property(ins.site), miss);
        a.mov(Reg::rax, Mem::at_index(Reg::rax, Reg::rdx, 8));
        a.mov(operand(0), Reg::rax);
        return true;
    }
    case Opcode::PutMemberNamed:
    case Opcode::PutMemberNamedKeep: {
        if (ins.site == no_site || m_feedback == nullptr)
            return false;
        Label const miss = cold(pc);
        a.mov(Reg::rax, operand(1));
        property_hit(Reg::rax, m_feedback->property(ins.site), miss);
        a.mov(Reg::rcx, operand(0));
        a.mov(Mem::at_index(Reg::rax, Reg::rdx, 8), Reg::rcx);
        if (ins.op == Opcode::PutMemberNamedKeep) {
            a.mov(operand(1), Reg::rcx);
            a.sub(top_reg, 8);
        } else {
            a.sub(top_reg, 16);
        }
        return true;
    }
    case Opcode::GetMember: {
        // An array read at an int32 index inside its dense storage, not a
        // hole: the element, as T0's fast path answers it.
        if (!m_layout.arrays)
            return false;
        Label const miss = cold(pc);
        a.mov(Reg::rcx, operand(0));
        a.cmp(Reg::rcx, tag_reg);
        a.j(Cond::Below, miss);
        a.test32(Reg::rcx, Reg::rcx);
        a.j(Cond::Sign, miss);
        a.mov(Reg::rax, operand(1));
        require_object(Reg::rax, Reg::rdx, miss);
        a.cmp8(Mem::at(Reg::rax, m_layout.object_class), m_layout.class_array);
        a.j(Cond::NotEqual, miss);
        a.mov32(Reg::rcx, Reg::rcx);
        a.mov32(Reg::rdx, Mem::at(Reg::rax, m_layout.array_size));
        a.cmp32(Reg::rcx, Reg::rdx);
        a.j(Cond::AboveOrEqual, miss);
        a.mov(Reg::rdx, Mem::at(Reg::rax, m_layout.array_data));
        a.mov(Reg::rax, Mem::at_index(Reg::rdx, Reg::rcx, 8));
        a.test(Reg::rax, Reg::rax);
        a.j(Cond::Equal, miss);
        if (ins.site != no_site && m_feedback != nullptr)
            record(&m_feedback->element(ins.site).kinds, ElementDense);
        a.mov(operand(1), Reg::rax);
        a.sub(top_reg, 8);
        return true;
    }
    default:
        return false;
    }
}

std::unique_ptr<Code> Writer::write(StepHelper step)
{
    // The prologue: a frame-pointer frame, the five callee-saved registers
    // the code keeps, and the stack 16-aligned for its calls (the return
    // address and six pushes are 56 bytes; the 8 more hold `next`).
    a.push(Reg::rbp);
    a.mov(Reg::rbp, Reg::rsp);
    a.push(interpreter_reg);
    a.push(frame_reg);
    a.push(tag_reg);
    a.push(registers_reg);
    a.push(top_reg);
    a.sub(Reg::rsp, 8);
    a.mov(interpreter_reg, Reg::rdi);
    a.mov(frame_reg, Reg::rsi);
    a.mov(Mem::at(Reg::rbp, next_slot), Reg::rdx);
    a.mov_imm64(tag_reg, Value::NumberTag);
    a.mov(registers_reg, Mem::at(frame_reg, m_layout.frame_registers));
    a.mov(top_reg, Mem::at(frame_reg, m_layout.frame_stack_top));

    // frame.pc to its instruction's code, or out when it names none.
    a.bind(m_dispatch);
    a.mov32(Reg::rax, Mem::at(frame_reg, m_layout.frame_pc));
    a.cmp32(Reg::rax, static_cast<std::int32_t>(m_count));
    a.j(Cond::AboveOrEqual, m_outside);
    a.lea(Reg::rcx, m_table);
    a.jmp(Mem::at_index(Reg::rcx, Reg::rax, 8));
    a.bind(m_outside);
    a.mov32(Reg::rax, bad_pc);
    a.bind(m_exit);
    a.add(Reg::rsp, 8);
    a.pop(top_reg);
    a.pop(registers_reg);
    a.pop(tag_reg);
    a.pop(frame_reg);
    a.pop(interpreter_reg);
    a.pop(Reg::rbp);
    a.ret();

    // Each instruction: its template, or the step and on — falling
    // through when the frame goes on at the next instruction, through the
    // table when it goes on elsewhere.
    std::size_t const start = a.size();
    for (std::uint32_t pc = 0; pc < m_count; ++pc) {
        a.bind(m_at[pc]);
        if (instruction(pc)) {
            ++m_inline;
            continue;
        }
        call_step(pc);
        if (pc + 1 < m_count) {
            a.cmp32(Mem::at(frame_reg, m_layout.frame_pc), static_cast<std::int32_t>(pc + 1));
            a.j(Cond::NotEqual, m_dispatch);
        }
    }
    // Nothing runs past the last instruction: the table says so.
    a.jmp(m_dispatch);
    // The paths out of line, each back at the next instruction's code or
    // through the table.
    for (Cold const& path : m_cold) {
        a.bind(path.label);
        call_step(path.pc);
        if (path.pc + 1 < m_count) {
            a.cmp32(Mem::at(frame_reg, m_layout.frame_pc), static_cast<std::int32_t>(path.pc + 1));
            a.j(Cond::Equal, m_at[path.pc + 1]);
        }
        a.jmp(m_dispatch);
    }
    std::size_t const code_bytes = a.size() - start;

    // The data: the step's address, and each instruction's absolute
    // address, written once the code's place is known.
    a.align(8);
    a.bind(m_step);
    a.emit_u64(reinterpret_cast<std::uint64_t>(step));
    a.bind(m_table);
    for (std::uint32_t pc = 0; pc < m_count; ++pc)
        a.emit_u64(0);
    if (!a.finish())
        return nullptr;

    std::optional<platform::ExecutableMemory> memory = platform::ExecutableMemory::allocate(a.size());
    if (!memory)
        return nullptr;
    std::byte* const base = memory->data();
    std::memcpy(base, a.bytes().data(), a.size());
    std::uint32_t const table_offset = a.offset_of(m_table);
    for (std::uint32_t pc = 0; pc < m_count; ++pc) {
        auto const address = reinterpret_cast<std::uint64_t>(base + a.offset_of(m_at[pc]));
        std::memcpy(base + table_offset + 8 * static_cast<std::size_t>(pc), &address, sizeof address);
    }
    if (!memory->seal())
        return nullptr;
    auto code = std::make_unique<Code>(std::move(*memory));
    code->entry = reinterpret_cast<Code::Entry>(const_cast<std::byte*>(code->memory.code()));
    code->instructions = m_count;
    code->inline_instructions = m_inline;
    code->code_bytes = code_bytes;
    return code;
}

}

std::unique_ptr<Code> compile(CodeBlock const& block, FeedbackVector* feedback, Layout const& layout, StepHelper step)
{
    if constexpr (!available) {
        static_cast<void>(block);
        static_cast<void>(feedback);
        static_cast<void>(layout);
        static_cast<void>(step);
        return nullptr;
    } else {
        return Writer(block, feedback, layout).write(step);
    }
}

}

namespace sashfold::js::jit {

namespace {

std::atomic<int> chosen_mode { -1 };
std::atomic<std::uint32_t> chosen_threshold { 0 };

// Calls and back-edges a block takes in T0 before it is given machine
// code (§5 "Tier-up": measured, not assumed; SpiderMonkey's baseline
// takes 100, JavaScriptCore's about 500).
constexpr std::uint32_t default_threshold = 200;

}

Mode mode()
{
    int value = chosen_mode.load(std::memory_order_relaxed);
    if (value < 0) {
        char const* const asked = std::getenv("SASHFOLD_JIT");
        Mode chosen = Mode::Off;
        if (asked != nullptr && std::strcmp(asked, "eager") == 0)
            chosen = Mode::Eager;
        else if (asked != nullptr && (std::strcmp(asked, "tiered") == 0 || std::strcmp(asked, "1") == 0))
            chosen = Mode::Tiered;
        value = static_cast<int>(chosen);
        chosen_mode.store(value, std::memory_order_relaxed);
    }
    return static_cast<Mode>(value);
}

void set_mode(Mode chosen)
{
    chosen_mode.store(static_cast<int>(chosen), std::memory_order_relaxed);
}

std::uint32_t threshold()
{
    std::uint32_t value = chosen_threshold.load(std::memory_order_relaxed);
    if (value == 0) {
        value = default_threshold;
        if (char const* const asked = std::getenv("SASHFOLD_JIT_THRESHOLD")) {
            long const parsed = std::strtol(asked, nullptr, 10);
            if (parsed > 0 && parsed < 1000000000)
                value = static_cast<std::uint32_t>(parsed);
        }
        chosen_threshold.store(value, std::memory_order_relaxed);
    }
    return value;
}

void set_threshold(std::uint32_t count)
{
    chosen_threshold.store(count, std::memory_order_relaxed);
}

}
