#include "pdf/Function.h"

#include "pdf/Document.h"
#include "pdf/Parser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace sashfold::pdf {

namespace {

constexpr int k_max_depth = 8; // stitching functions inside stitching functions
constexpr int k_max_inputs = 8;
constexpr int k_max_outputs = 32;

std::vector<float> numbers(Document const& doc, Object const& object)
{
    std::vector<float> out;
    for (Object const& item : doc.resolve(object).as_array())
        out.push_back(static_cast<float>(doc.resolve(item).as_number()));
    return out;
}

float interpolate(float x, float x0, float x1, float y0, float y1)
{
    return x1 == x0 ? y0 : y0 + (x - x0) * (y1 - y0) / (x1 - x0);
}

class Sampled final : public Function {
public:
    std::vector<int> size;
    std::vector<float> encode;
    std::vector<float> decode;
    std::vector<float> samples; // every sample, normalized to [0, 1]

    void evaluate(float const* in, float* out) const override
    {
        int const m = inputs();
        // Each input to a (fractional) sample index, then multilinear interpolation over the 2^m corners.
        std::array<int, k_max_inputs> lower {};
        std::array<float, k_max_inputs> fraction {};
        for (int i = 0; i < m; ++i) {
            float const x = clip_input(in[i], i);
            float e = interpolate(x, m_domain[2 * i], m_domain[2 * i + 1], encode[2 * i], encode[2 * i + 1]);
            e = std::clamp(e, 0.0f, static_cast<float>(size[i] - 1));
            lower[i] = std::min(static_cast<int>(e), std::max(0, size[i] - 2));
            fraction[i] = size[i] > 1 ? e - static_cast<float>(lower[i]) : 0;
        }
        int const n = m_outputs;
        std::array<float, k_max_outputs> sum {};
        for (int corner = 0; corner < (1 << m); ++corner) {
            float weight = 1;
            std::size_t index = 0;
            std::size_t stride = 1;
            for (int i = 0; i < m; ++i) {
                bool const high = (corner >> i) & 1;
                weight *= high ? fraction[i] : 1 - fraction[i];
                int const at = std::min(lower[i] + (high ? 1 : 0), size[i] - 1);
                index += static_cast<std::size_t>(at) * stride;
                stride *= static_cast<std::size_t>(size[i]);
            }
            if (weight == 0)
                continue;
            for (int j = 0; j < n; ++j)
                sum[j] += weight * samples[index * static_cast<std::size_t>(n) + static_cast<std::size_t>(j)];
        }
        for (int j = 0; j < n; ++j)
            out[j] = clip_output(decode[2 * j] + sum[j] * (decode[2 * j + 1] - decode[2 * j]), j);
    }
};

class Exponential final : public Function {
public:
    std::vector<float> c0;
    std::vector<float> c1;
    float exponent = 1;

    void evaluate(float const* in, float* out) const override
    {
        float const x = clip_input(in[0], 0);
        float const p = exponent == 1 ? x : std::pow(x, exponent);
        for (int j = 0; j < m_outputs; ++j)
            out[j] = clip_output(c0[j] + p * (c1[j] - c0[j]), j);
    }
};

class Stitching final : public Function {
public:
    std::vector<std::shared_ptr<Function const>> functions;
    std::vector<float> bounds;
    std::vector<float> encode;

    void evaluate(float const* in, float* out) const override
    {
        float const x = clip_input(in[0], 0);
        std::size_t k = 0;
        while (k < bounds.size() && x >= bounds[k])
            ++k;
        float const lo = k == 0 ? m_domain[0] : bounds[k - 1];
        float const hi = k == bounds.size() ? m_domain[1] : bounds[k];
        float const t = interpolate(x, lo, hi, encode[2 * k], encode[2 * k + 1]);
        functions[k]->evaluate(&t, out);
        for (int j = 0; j < m_outputs; ++j)
            out[j] = clip_output(out[j], j);
    }
};

// PostScript calculator: the program compiled to a list of operations,
// "if"/"ifelse" as jumps.
class Calculator final : public Function {
public:
    enum class Op : std::uint8_t {
        Push, Abs, Add, Atan, Ceiling, Cos, Cvi, Cvr, Div, Exp, Floor, Idiv, Ln, Log, Mod, Mul, Neg, Round, Sin,
        Sqrt, Sub, Truncate, And, Bitshift, Eq, False, Ge, Gt, Le, Lt, Ne, Not, Or, True, Xor, Copy, Dup, Exch,
        Index, Pop, Roll, JumpIfFalse, Jump,
    };
    struct Instruction {
        Op op;
        double value = 0; // Push: the number; jumps: the target
    };
    std::vector<Instruction> program;

    void evaluate(float const* in, float* out) const override
    {
        constexpr int k_stack = 100;
        std::array<double, k_stack> stack {};
        int top = 0;
        for (int i = 0; i < inputs() && top < k_stack; ++i)
            stack[static_cast<std::size_t>(top++)] = static_cast<double>(clip_input(in[i], i));
        auto const pop = [&]() -> double { return top > 0 ? stack[static_cast<std::size_t>(--top)] : 0.0; };
        auto const push = [&](double v) {
            if (top < k_stack)
                stack[static_cast<std::size_t>(top++)] = v;
        };
        std::size_t steps = 0;
        for (std::size_t pc = 0; pc < program.size() && steps < 100000; ++pc, ++steps) {
            Instruction const& ins = program[pc];
            switch (ins.op) {
            case Op::Push: push(ins.value); break;
            case Op::Abs: push(std::fabs(pop())); break;
            case Op::Add: { double const b = pop(); push(pop() + b); break; }
            case Op::Atan: {
                double const den = pop();
                double const num = pop();
                double angle = std::atan2(num, den) * 180.0 / 3.14159265358979323846;
                if (angle < 0)
                    angle += 360;
                push(angle);
                break;
            }
            case Op::Ceiling: push(std::ceil(pop())); break;
            case Op::Cos: push(std::cos(pop() * 3.14159265358979323846 / 180.0)); break;
            case Op::Cvi: push(std::trunc(pop())); break;
            case Op::Cvr: break;
            case Op::Div: { double const b = pop(); double const a = pop(); push(b == 0 ? 0 : a / b); break; }
            case Op::Exp: { double const e = pop(); push(std::pow(pop(), e)); break; }
            case Op::Floor: push(std::floor(pop())); break;
            case Op::Idiv: {
                auto const b = static_cast<std::int64_t>(pop());
                auto const a = static_cast<std::int64_t>(pop());
                push(b == 0 ? 0.0 : static_cast<double>(a / b));
                break;
            }
            case Op::Ln: push(std::log(pop())); break;
            case Op::Log: push(std::log10(pop())); break;
            case Op::Mod: {
                auto const b = static_cast<std::int64_t>(pop());
                auto const a = static_cast<std::int64_t>(pop());
                push(b == 0 ? 0.0 : static_cast<double>(a % b));
                break;
            }
            case Op::Mul: { double const b = pop(); push(pop() * b); break; }
            case Op::Neg: push(-pop()); break;
            case Op::Round: push(std::floor(pop() + 0.5)); break;
            case Op::Sin: push(std::sin(pop() * 3.14159265358979323846 / 180.0)); break;
            case Op::Sqrt: push(std::sqrt(std::max(0.0, pop()))); break;
            case Op::Sub: { double const b = pop(); push(pop() - b); break; }
            case Op::Truncate: push(std::trunc(pop())); break;
            case Op::And: {
                auto const b = static_cast<std::int64_t>(pop());
                push(static_cast<double>(static_cast<std::int64_t>(pop()) & b));
                break;
            }
            case Op::Bitshift: {
                auto const shift = static_cast<std::int64_t>(pop());
                auto const value = static_cast<std::int64_t>(pop());
                if (shift > 0 && shift < 63)
                    push(static_cast<double>(value << shift));
                else if (shift < 0 && shift > -63)
                    push(static_cast<double>(value >> -shift));
                else
                    push(static_cast<double>(value));
                break;
            }
            case Op::Eq: { double const b = pop(); push(pop() == b ? 1 : 0); break; }
            case Op::False: push(0); break;
            case Op::Ge: { double const b = pop(); push(pop() >= b ? 1 : 0); break; }
            case Op::Gt: { double const b = pop(); push(pop() > b ? 1 : 0); break; }
            case Op::Le: { double const b = pop(); push(pop() <= b ? 1 : 0); break; }
            case Op::Lt: { double const b = pop(); push(pop() < b ? 1 : 0); break; }
            case Op::Ne: { double const b = pop(); push(pop() != b ? 1 : 0); break; }
            case Op::Not: {
                // Booleans are 0/1 here; an integer's bitwise not otherwise.
                double const v = pop();
                push(v == 0 ? 1 : v == 1 ? 0 : static_cast<double>(~static_cast<std::int64_t>(v)));
                break;
            }
            case Op::Or: {
                auto const b = static_cast<std::int64_t>(pop());
                push(static_cast<double>(static_cast<std::int64_t>(pop()) | b));
                break;
            }
            case Op::True: push(1); break;
            case Op::Xor: {
                auto const b = static_cast<std::int64_t>(pop());
                push(static_cast<double>(static_cast<std::int64_t>(pop()) ^ b));
                break;
            }
            case Op::Copy: {
                int const n = static_cast<int>(pop());
                if (n > 0 && n <= top && top + n <= k_stack) {
                    for (int k = 0; k < n; ++k)
                        stack[static_cast<std::size_t>(top + k)] = stack[static_cast<std::size_t>(top - n + k)];
                    top += n;
                }
                break;
            }
            case Op::Dup: {
                double const v = pop();
                push(v);
                push(v);
                break;
            }
            case Op::Exch: {
                double const b = pop();
                double const a = pop();
                push(b);
                push(a);
                break;
            }
            case Op::Index: {
                int const n = static_cast<int>(pop());
                push(n >= 0 && n < top ? stack[static_cast<std::size_t>(top - 1 - n)] : 0.0);
                break;
            }
            case Op::Pop: pop(); break;
            case Op::Roll: {
                int j = static_cast<int>(pop());
                int const n = static_cast<int>(pop());
                if (n > 0 && n <= top) {
                    j %= n;
                    if (j < 0)
                        j += n;
                    std::rotate(stack.begin() + (top - n), stack.begin() + (top - j), stack.begin() + top);
                }
                break;
            }
            case Op::JumpIfFalse:
                if (pop() == 0)
                    pc = static_cast<std::size_t>(ins.value) - 1;
                break;
            case Op::Jump: pc = static_cast<std::size_t>(ins.value) - 1; break;
            }
        }
        // The results are the top outputs() values, the first deepest.
        int const n = m_outputs;
        for (int j = n - 1; j >= 0; --j)
            out[j] = clip_output(static_cast<float>(pop()), j);
    }
};

bool compile_calculator(Lexer& lexer, Calculator& calc, int depth)
{
    if (depth > 64)
        return false;
    static constexpr std::pair<char const*, Calculator::Op> k_names[] = {
        { "abs", Calculator::Op::Abs }, { "add", Calculator::Op::Add }, { "atan", Calculator::Op::Atan },
        { "ceiling", Calculator::Op::Ceiling }, { "cos", Calculator::Op::Cos }, { "cvi", Calculator::Op::Cvi },
        { "cvr", Calculator::Op::Cvr }, { "div", Calculator::Op::Div }, { "exp", Calculator::Op::Exp },
        { "floor", Calculator::Op::Floor }, { "idiv", Calculator::Op::Idiv }, { "ln", Calculator::Op::Ln },
        { "log", Calculator::Op::Log }, { "mod", Calculator::Op::Mod }, { "mul", Calculator::Op::Mul },
        { "neg", Calculator::Op::Neg }, { "round", Calculator::Op::Round }, { "sin", Calculator::Op::Sin },
        { "sqrt", Calculator::Op::Sqrt }, { "sub", Calculator::Op::Sub }, { "truncate", Calculator::Op::Truncate },
        { "and", Calculator::Op::And }, { "bitshift", Calculator::Op::Bitshift }, { "eq", Calculator::Op::Eq },
        { "false", Calculator::Op::False }, { "ge", Calculator::Op::Ge }, { "gt", Calculator::Op::Gt },
        { "le", Calculator::Op::Le }, { "lt", Calculator::Op::Lt }, { "ne", Calculator::Op::Ne },
        { "not", Calculator::Op::Not }, { "or", Calculator::Op::Or }, { "true", Calculator::Op::True },
        { "xor", Calculator::Op::Xor }, { "copy", Calculator::Op::Copy }, { "dup", Calculator::Op::Dup },
        { "exch", Calculator::Op::Exch }, { "index", Calculator::Op::Index }, { "pop", Calculator::Op::Pop },
        { "roll", Calculator::Op::Roll },
    };
    // Blocks met but not yet used by if/ifelse: (start, end) in the program.
    std::vector<std::pair<std::size_t, std::size_t>> blocks;
    for (;;) {
        Token const token = lexer.next();
        if (token.kind == Token::Kind::Eof)
            return depth == 0;
        if (token.kind == Token::Kind::Integer || token.kind == Token::Kind::Real) {
            calc.program.push_back({ Calculator::Op::Push, token.kind == Token::Kind::Integer ? static_cast<double>(token.integer) : token.real });
            blocks.clear();
            continue;
        }
        if (token.kind != Token::Kind::Keyword)
            continue;
        if (token.text == "{") {
            // A block: compiled in place, preceded by a jump over it that
            // if/ifelse rewrite. Its start is after the jump.
            std::size_t const jump = calc.program.size();
            calc.program.push_back({ Calculator::Op::Jump, 0 });
            if (!compile_calculator(lexer, calc, depth + 1))
                return false;
            calc.program[jump].value = static_cast<double>(calc.program.size());
            blocks.emplace_back(jump, calc.program.size());
            continue;
        }
        if (token.text == "}")
            return depth > 0;
        if (token.text == "if" && !blocks.empty()) {
            // [jump-over B][B]: the jump becomes "jump past B if false".
            auto const [jump, end] = blocks.back();
            blocks.pop_back();
            calc.program[jump] = { Calculator::Op::JumpIfFalse, static_cast<double>(end) };
            // The condition was pushed before the block: fine, it is popped at the jump.
            continue;
        }
        if (token.text == "ifelse" && blocks.size() >= 2) {
            auto const [jump_b, end_b] = blocks.back();
            blocks.pop_back();
            auto const [jump_a, end_a] = blocks.back();
            blocks.pop_back();
            // [JA][A][JB][B]: JA = if false go to B's start; at A's end jump past B (JB already is a jump past B).
            calc.program[jump_a] = { Calculator::Op::JumpIfFalse, static_cast<double>(jump_b + 1) };
            calc.program[jump_b] = { Calculator::Op::Jump, static_cast<double>(end_b) };
            (void)end_a;
            continue;
        }
        bool found = false;
        for (auto const& [name, op] : k_names) {
            if (token.text == name) {
                calc.program.push_back({ op, 0 });
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        blocks.clear();
    }
}

} // namespace

class FunctionParser {
public:
    static std::shared_ptr<Function const> parse(Document const& doc, Object const& object_in, int depth)
    {
        if (depth > k_max_depth)
            return nullptr;
        Object const object = doc.resolve(object_in);
        if (object.is_array()) {
            // n one-output functions as one n-output function.
            std::vector<std::shared_ptr<Function const>> parts;
            for (Object const& part : object.as_array()) {
                auto f = parse(doc, part, depth + 1);
                if (!f || f->outputs() != 1)
                    return nullptr;
                parts.push_back(std::move(f));
            }
            if (parts.empty() || parts.size() > k_max_outputs)
                return nullptr;
            class Combined final : public Function {
            public:
                std::vector<std::shared_ptr<Function const>> parts;
                void evaluate(float const* in, float* out) const override
                {
                    for (std::size_t i = 0; i < parts.size(); ++i)
                        parts[i]->evaluate(in, out + i);
                }
            };
            auto combined = std::make_shared<Combined>();
            combined->m_domain = parts[0]->m_domain;
            combined->m_outputs = static_cast<int>(parts.size());
            combined->parts = std::move(parts);
            return combined;
        }
        Dict const& dict = object.as_dict();
        int const type = static_cast<int>(doc.resolve(dict.get("FunctionType")).as_int(-1));
        std::vector<float> const domain = numbers(doc, dict.get("Domain"));
        std::vector<float> const range = numbers(doc, dict.get("Range"));
        if (domain.size() < 2 || domain.size() % 2 != 0 || domain.size() / 2 > k_max_inputs)
            return nullptr;
        if (range.size() % 2 != 0 || range.size() / 2 > k_max_outputs)
            return nullptr;
        std::shared_ptr<Function> result;
        if (type == 0) {
            Stream const* const stream = object.as_stream();
            if (!stream || range.empty())
                return nullptr;
            auto f = std::make_shared<Sampled>();
            int const m = static_cast<int>(domain.size() / 2);
            int const n = static_cast<int>(range.size() / 2);
            for (float const s : numbers(doc, dict.get("Size")))
                f->size.push_back(std::max(1, static_cast<int>(s)));
            if (static_cast<int>(f->size.size()) != m)
                return nullptr;
            int const bps = static_cast<int>(doc.resolve(dict.get("BitsPerSample")).as_int(8));
            if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 12 && bps != 16 && bps != 24 && bps != 32)
                return nullptr;
            f->encode = numbers(doc, dict.get("Encode"));
            if (static_cast<int>(f->encode.size()) != 2 * m) {
                f->encode.clear();
                for (int i = 0; i < m; ++i) {
                    f->encode.push_back(0);
                    f->encode.push_back(static_cast<float>(f->size[static_cast<std::size_t>(i)] - 1));
                }
            }
            f->decode = numbers(doc, dict.get("Decode"));
            if (static_cast<int>(f->decode.size()) != 2 * n)
                f->decode = range;
            std::size_t count = static_cast<std::size_t>(n);
            for (int const s : f->size) {
                count *= static_cast<std::size_t>(s);
                if (count > 16u * 1024u * 1024u)
                    return nullptr;
            }
            std::vector<std::uint8_t> const data = doc.decode(*stream);
            f->samples.resize(count, 0);
            double const max = std::pow(2.0, bps) - 1;
            std::size_t bit = 0;
            for (std::size_t i = 0; i < count; ++i) {
                std::uint64_t value = 0;
                for (int b = 0; b < bps; ++b, ++bit) {
                    std::size_t const byte = bit / 8;
                    int const v = byte < data.size() ? (data[byte] >> (7 - bit % 8)) & 1 : 0;
                    value = (value << 1) | static_cast<std::uint64_t>(v);
                }
                f->samples[i] = static_cast<float>(static_cast<double>(value) / max);
            }
            f->m_outputs = n;
            result = f;
        } else if (type == 2) {
            auto f = std::make_shared<Exponential>();
            f->c0 = numbers(doc, dict.get("C0"));
            f->c1 = numbers(doc, dict.get("C1"));
            if (f->c0.empty())
                f->c0 = { 0 };
            if (f->c1.empty())
                f->c1 = { 1 };
            if (f->c0.size() != f->c1.size())
                return nullptr;
            f->exponent = static_cast<float>(doc.resolve(dict.get("N")).as_number(1));
            f->m_outputs = static_cast<int>(f->c0.size());
            result = f;
        } else if (type == 3) {
            auto f = std::make_shared<Stitching>();
            for (Object const& part : doc.resolve(dict.get("Functions")).as_array()) {
                auto sub = parse(doc, part, depth + 1);
                if (!sub)
                    return nullptr;
                f->functions.push_back(std::move(sub));
            }
            f->bounds = numbers(doc, dict.get("Bounds"));
            f->encode = numbers(doc, dict.get("Encode"));
            if (f->functions.empty() || f->bounds.size() + 1 != f->functions.size())
                return nullptr;
            if (f->encode.size() < 2 * f->functions.size()) {
                f->encode.clear();
                for (std::size_t i = 0; i < f->functions.size(); ++i) {
                    f->encode.push_back(0);
                    f->encode.push_back(1);
                }
            }
            f->m_outputs = f->functions[0]->outputs();
            for (auto const& sub : f->functions) {
                if (sub->outputs() != f->m_outputs)
                    return nullptr;
            }
            result = f;
        } else if (type == 4) {
            Stream const* const stream = object.as_stream();
            if (!stream || range.empty())
                return nullptr;
            auto f = std::make_shared<Calculator>();
            std::vector<std::uint8_t> const code = doc.decode(*stream);
            Lexer lexer(std::span<std::uint8_t const>(code.data(), code.size()));
            Token const open = lexer.next();
            if (open.kind != Token::Kind::Keyword || open.text != "{" || !compile_calculator(lexer, *f, 1))
                return nullptr;
            f->m_outputs = static_cast<int>(range.size() / 2);
            result = f;
        } else {
            return nullptr;
        }
        result->m_domain = domain;
        result->m_range = range;
        if (result->m_outputs <= 0 || result->m_outputs > k_max_outputs)
            return nullptr;
        if (type != 4 && type != 0 && result->inputs() != 1)
            return nullptr;
        return result;
    }
};

std::shared_ptr<Function const> Function::parse(Document const& doc, Object const& object, int depth)
{
    return FunctionParser::parse(doc, object, depth);
}

float Function::clip_input(float value, int index) const
{
    std::size_t const i = static_cast<std::size_t>(index) * 2;
    if (i + 1 >= m_domain.size())
        return value;
    return std::clamp(value, std::min(m_domain[i], m_domain[i + 1]), std::max(m_domain[i], m_domain[i + 1]));
}

float Function::clip_output(float value, int index) const
{
    std::size_t const i = static_cast<std::size_t>(index) * 2;
    if (i + 1 >= m_range.size())
        return value;
    return std::clamp(value, std::min(m_range[i], m_range[i + 1]), std::max(m_range[i], m_range[i + 1]));
}

SampledFunction1D::SampledFunction1D(Function const& function, float t0, float t1, int samples)
    : m_t0(t0)
    , m_t1(t1)
    , m_samples(std::max(2, samples))
    , m_outputs(function.outputs())
{
    m_values.resize(static_cast<std::size_t>(m_samples) * static_cast<std::size_t>(m_outputs));
    for (int i = 0; i < m_samples; ++i) {
        float const t = t0 + (t1 - t0) * static_cast<float>(i) / static_cast<float>(m_samples - 1);
        function.evaluate(&t, &m_values[static_cast<std::size_t>(i) * static_cast<std::size_t>(m_outputs)]);
    }
}

void SampledFunction1D::evaluate(float t, float* out) const
{
    float position = m_t1 == m_t0 ? 0 : (t - m_t0) / (m_t1 - m_t0) * static_cast<float>(m_samples - 1);
    position = std::clamp(position, 0.0f, static_cast<float>(m_samples - 1));
    int const i = std::min(static_cast<int>(position), m_samples - 2);
    float const f = position - static_cast<float>(i);
    float const* const a = &m_values[static_cast<std::size_t>(i) * static_cast<std::size_t>(m_outputs)];
    float const* const b = a + m_outputs;
    for (int j = 0; j < m_outputs; ++j)
        out[j] = a[j] + (b[j] - a[j]) * f;
}

}
