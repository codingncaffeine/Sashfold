#pragma once

// The bytecode a function body compiles to (Compiler.cpp) and the loop
// runs (Vm.cpp): a stack machine over Values with registers for state
// that outlives an expression, a reference stack for the assignment
// targets the specification evaluates before their values, and a static
// table of handlers for try/catch/finally. A body compiled this way runs
// on a Frame that is heap data, so a generator or an async function can
// suspend in the middle of anything and resume later.
//
// Every instruction is twelve bytes: the opcode, a flags byte, and two
// 32-bit operands whose meaning the opcode fixes — an index into one of
// the code block's pools, a jump target, a register, a count.

#include "js/Ast.h"
#include "js/Value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sashfold::js {

// The opcode list with each one's fixed stack effect (pushes minus pops),
// or Var for the ones whose effect depends on an operand (calls) and that
// the compiler accounts for by hand. The comments give the operands.
#define SASHFOLD_OPCODES(X)                                                                                        \
    /* stack */                                                                                                    \
    X(PushUndefined, 1) X(PushNull, 1) X(PushTrue, 1) X(PushFalse, 1) X(PushEmpty, 1)                              \
    X(PushConstant, 1) /* a: constant */ X(PushInt, 1) /* a: the number */ X(PushBigInt, 1) /* a: bigint */        \
    X(Pop, -1) X(Dup, 1) X(Over, 1) /* a copy of the second value */ X(Swap, 0)                                    \
    X(LoadReg, 1) /* a: register */ X(StoreReg, -1) /* a: register; pops */ X(StoreRegKeep, 0)                      \
    /* environments */                                                                                             \
    X(PushBlockEnv, 0) /* a: declarations */ X(PushNamesEnv, 0) /* a: name list; b: mutable */                      \
    X(PushWithEnv, -1) X(PopEnv, 0) X(CopyIterationEnv, 0) /* a: name list */                                      \
    X(InitializeBinding, -1) /* a: name */ X(AnnexBCopy, 0) /* a: name */ X(ResolveThis, 1) X(NewTarget, 1)        \
    /* references (the reference stack is accounted separately) */                                                 \
    X(RefName, 0) /* a: name */ X(RefMember, -2) X(RefMemberNamed, -1) /* a: name */                                \
    X(RefSuper, -1) X(RefSuperNamed, 0) /* a: name */ X(RefPrivate, -1) /* a: name */                               \
    X(RefGet, 1) X(RefPut, -1) X(RefPutKeep, 0) X(RefThis, 1) X(RefDrop, 0) X(RefDelete, 1)                          \
    X(GetName, 1) /* a: name */ X(TypeofName, 1) /* a: name */ X(GetMemberNamed, 0) /* a: name */ X(GetMember, -1)  \
    /* operators */                                                                                                \
    X(Binary, -1) /* a: BinaryOp */ X(Unary, 0) /* a: UnaryOp */ X(ToNumeric, 0) X(Inc, 0) X(Dec, 0)               \
    X(ToPropertyKey, 0) X(ToString, 0) X(StringConcat, -1) X(PrivateIn, 0) /* a: name */                            \
    X(RequireObjectCoercible, 0) X(ThrowTypeErrorConst, 0) /* a: constant (the message) */                         \
    /* control */                                                                                                  \
    X(Jump, 0) X(JumpIfTrue, -1) X(JumpIfFalse, -1) X(JumpIfTrueKeep, 0) X(JumpIfFalseKeep, 0)                      \
    X(JumpIfNullish, -1) X(JumpIfNotNullishKeep, 0) X(JumpIfNotUndefined, -1) X(JumpIfEmpty, -1)                    \
    X(Switch, 0) /* a: register; b: jump table */ X(JumpIfResumeNormal, 0) X(JumpIfResumeReturn, 0)                \
    X(Return, -1) X(Throw, -1) X(Step, 0)                                                                          \
    /* calls: [callee, this, args…] → result */                                                                    \
    X(Call, Var) /* a: argc; b: node */ X(CallArray, Var) /* b: node */ X(CallEval, Var) /* a: argc; b: node */     \
    X(CallEvalArray, Var) /* b: node */ X(New, Var) /* a: argc; b: node */ X(NewArray, Var) /* b: node */            \
    X(SuperCall, Var) /* a: argc */ X(SuperCallArray, Var)                                                         \
    /* literals */                                                                                                 \
    X(NewArrayLiteral, 1) X(ArrayPush, -1) X(ArrayHole, 0) X(ArraySpread, -1) X(NewObject, 1)                       \
    X(SetPrototype, -1) X(CopyDataProperties, -1) X(CopyDataPropertiesExcluding, -1) /* a: register (taken) */      \
    X(DefinePropertyNamed, -1) /* a: name */ X(DefinePropertyDyn, -2)                                              \
    X(DefineMethod, 0) /* a: function; b: name */ X(DefineMethodDyn, -1) /* a: function */                          \
    X(DefineAccessor, 0) /* a: function; b: name; flags 1 = setter */ X(DefineAccessorDyn, -1) /* a: function */    \
    X(NewRegExp, 1) /* a: regexp */ X(TemplateObject, 1) /* a: template */                                          \
    X(MakeClosure, 1) /* a: function; b: name or None */ X(MakeClosureNamedDyn, 0) /* a: function */                \
    X(MakeClass, 1) /* a: class; b: name or None */ X(MakeClassNamedDyn, 0) /* a: class */                           \
    X(AppendToReg, -1) /* a: register holding an array */                                                          \
    /* iteration */                                                                                                \
    X(GetIterator, 1) X(IteratorNext, 1) /* a: register */ X(IteratorResultDone, 0) X(IteratorResultValue, 0)       \
    X(IteratorStep, 1) /* a: register */ X(IteratorRestArray, 1) /* a: register */                                  \
    X(IteratorClose, 0) /* a: register */ X(IteratorCloseThrowing, 0) /* a: register */                             \
    X(ForInStart, 0) X(ForInNext, 1) /* a: register */                                                              \
    /* suspension */                                                                                               \
    X(Yield, 0) /* flags 1: the operand is already an iterator result */ X(Await, 0)                               \
    /* modules */                                                                                                  \
    X(ImportCall, -1) /* [specifier, options]: a promise of the namespace */ X(ImportMeta, 1)                       \
    X(Nop, 0)

enum class Opcode : std::uint8_t {
#define SASHFOLD_OPCODE_ENUM(name, effect) name,
    SASHFOLD_OPCODES(SASHFOLD_OPCODE_ENUM)
#undef SASHFOLD_OPCODE_ENUM
};

inline constexpr int Var = -1000; // "the compiler accounts for this one by hand"

inline constexpr int stack_effect(Opcode op)
{
    switch (op) {
#define SASHFOLD_OPCODE_EFFECT(name, effect) \
    case Opcode::name:                      \
        return effect;
        SASHFOLD_OPCODES(SASHFOLD_OPCODE_EFFECT)
#undef SASHFOLD_OPCODE_EFFECT
    }
    return 0;
}

inline constexpr char const* opcode_name(Opcode op)
{
    switch (op) {
#define SASHFOLD_OPCODE_NAME(name, effect) \
    case Opcode::name:                    \
        return #name;
        SASHFOLD_OPCODES(SASHFOLD_OPCODE_NAME)
#undef SASHFOLD_OPCODE_NAME
    }
    return "?";
}

struct Instruction {
    Opcode op = Opcode::Nop;
    std::uint8_t flags = 0;
    std::uint16_t unused = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

inline constexpr std::uint32_t None = 0xFFFFFFFFu; // "no name" and "no target" in an operand

// A protected range of instructions and where a throw inside it lands:
// the unwinder cuts the operand, reference and environment stacks back to
// the depths the range started at, pushes the thrown value, and jumps.
// Inner ranges come before the outer ones that contain them.
struct Handler {
    std::uint32_t start = 0; // first instruction covered
    std::uint32_t end = 0; // one past the last
    std::uint32_t target = 0;
    std::uint32_t stack_depth = 0;
    std::uint32_t ref_depth = 0;
    std::uint32_t env_depth = 0; // environments pushed above the frame's first
};

// One compiled function body. The pools hold what instructions refer to
// by index; the constants are atoms and numbers, which need no tracing,
// and the BigInt literals are kept as integers and made cells when pushed.
struct CodeBlock {
    FunctionNode const* function = nullptr;
    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<BigInteger> bigints;
    std::vector<JsString*> names;
    std::vector<FunctionNode const*> functions;
    std::vector<ClassNode const*> classes;
    std::vector<TemplateLiteral const*> templates;
    std::vector<RegExpLiteral const*> regexps;
    std::vector<Declarations const*> declarations;
    std::vector<std::vector<JsString*>> name_lists;
    std::vector<Expression const*> nodes; // for messages: "x is not a function"
    std::vector<Handler> handlers;
    std::vector<std::vector<std::uint32_t>> jump_tables;
    std::uint32_t register_count = 0;
    std::uint32_t max_stack = 0;
    bool strict = false;
    bool is_generator = false;
    bool is_async = false;
};

// How a run of a frame ended.
enum class RunStatus : std::uint8_t {
    Completed, // `return`, or the body's end: the value is in the frame's result
    Yielded, // a `yield`: the value is in the frame's result
    Awaiting, // an `await`: the operand is in the frame's result
    Threw, // an exception is pending in the interpreter
};

// What a suspended frame is resumed with (§27.5.3.3 GeneratorResume and
// §27.5.3.4 GeneratorResumeAbrupt): a value, a throw at the suspension
// point, or a return from it — which runs the enclosing finally blocks.
enum class ResumeKind : std::uint8_t { Normal, Throw, Return };

// A listing for the tests and the probe: one instruction per line, the
// pools, the handlers.
std::string disassemble(CodeBlock const&);

}
