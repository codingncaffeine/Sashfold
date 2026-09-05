#include "js/Runtime.h"

// String (§22.1) and RegExp (§22.2), which share a file because the
// string methods that take a pattern reach the RegExp side through the
// well-known symbols (@@match, @@replace, @@search, @@split), exactly as
// the specification routes them. The iterator-returning members
// (String.prototype[@@iterator], matchAll) wait for the iterator protocol.

#include "core/Unicode.h"
#include "js/Object.h"
#include "js/Regex.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {


Value make_string(Interpreter& in, std::u16string_view text)
{
    return Value::string(in.string(text));
}

// RequireObjectCoercible + ToString, the opening of every String
// prototype method, with the string rooted for the caller.
std::optional<JsString*> this_string(Interpreter& in, Value const& this_value, std::string_view method)
{
    std::optional<JsString*> const string = this_string_value(in, this_value, method);
    if (!string)
        return std::nullopt;
    in.root(Value::string(*string));
    return string;
}

// thisStringValue (§22.1.3.35.1): the primitive, or the wrapper's.
std::optional<JsString*> this_string_primitive(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (this_value.is_string())
        return this_value.as_string();
    if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::String)
        return static_cast<StringObject*>(this_value.as_object())->string();
    return in.throw_type_error("String.prototype." + std::string(method) + " requires that 'this' be a String");
}

// StringIndexOf (§6.1.4.1).
std::optional<std::size_t> string_index_of(std::u16string_view string, std::u16string_view search, std::size_t from)
{
    if (search.empty() && from <= string.size())
        return from;
    if (from >= string.size())
        return std::nullopt;
    std::size_t const found = string.find(search, from);
    if (found == std::u16string_view::npos)
        return std::nullopt;
    return found;
}

// AdvanceStringIndex (§22.2.7.3).
std::size_t advance_string_index(std::u16string_view string, std::size_t index, bool unicode)
{
    if (!unicode || index + 1 >= string.size())
        return index + 1;
    std::size_t units = 1;
    code_point_at(string, index, &units);
    return index + units;
}

// GetSubstitution (§22.1.3.19.1): the $-patterns of a replacement string.
std::optional<std::u16string> get_substitution(Interpreter& in, std::u16string_view matched, std::u16string_view string, std::size_t position,
    std::vector<Value> const& captures, Value const& named_captures, std::u16string_view replacement)
{
    std::u16string result;
    std::size_t const tail = std::min(position + matched.size(), string.size());
    std::size_t const capture_count = captures.size();
    for (std::size_t k = 0; k < replacement.size(); ++k) {
        char16_t const c = replacement[k];
        if (c != u'$' || k + 1 >= replacement.size()) {
            result += c;
            continue;
        }
        char16_t const next = replacement[k + 1];
        if (next == u'$') {
            result += u'$';
            ++k;
        } else if (next == u'&') {
            result += matched;
            ++k;
        } else if (next == u'`') {
            result += string.substr(0, position);
            ++k;
        } else if (next == u'\'') {
            result += string.substr(tail);
            ++k;
        } else if (next >= u'0' && next <= u'9') {
            // Two digits when they name a capture; else one digit; a
            // digit past the captures is literal text.
            std::size_t digits = 1;
            std::size_t index = static_cast<std::size_t>(next - u'0');
            if (k + 2 < replacement.size() && replacement[k + 2] >= u'0' && replacement[k + 2] <= u'9') {
                std::size_t const two = index * 10 + static_cast<std::size_t>(replacement[k + 2] - u'0');
                if (two >= 1 && two <= capture_count) {
                    index = two;
                    digits = 2;
                }
            }
            if (index >= 1 && index <= capture_count) {
                Value const capture = captures[index - 1];
                if (!capture.is_undefined()) {
                    std::optional<JsString*> const text = in.to_string(capture);
                    if (!text)
                        return std::nullopt;
                    result += (*text)->view();
                }
                k += digits;
            } else {
                result += u'$';
            }
        } else if (next == u'<') {
            if (named_captures.is_undefined()) {
                result += u'$';
                continue;
            }
            std::size_t const close = replacement.find(u'>', k + 2);
            if (close == std::u16string_view::npos) {
                result += u'$';
                continue;
            }
            std::u16string_view const group_name = replacement.substr(k + 2, close - (k + 2));
            std::optional<Value> const capture = in.get(named_captures, in.key(group_name));
            if (!capture)
                return std::nullopt;
            if (!capture->is_undefined()) {
                std::optional<JsString*> const text = in.to_string(*capture);
                if (!text)
                    return std::nullopt;
                result += (*text)->view();
            }
            k = close;
        } else {
            result += u'$';
        }
    }
    return result;
}

// ---------------------------------------------------------------- RegExp

// EscapeRegExpPattern (§22.2.6.13.1): what `source` shows.
std::u16string escape_pattern(std::u16string_view source)
{
    if (source.empty())
        return u"(?:)";
    std::u16string out;
    bool in_class = false;
    for (std::size_t k = 0; k < source.size(); ++k) {
        char16_t const c = source[k];
        if (c == u'\\' && k + 1 < source.size()) {
            out += c;
            out += source[++k];
            continue;
        }
        if (c == u'[')
            in_class = true;
        else if (c == u']')
            in_class = false;
        if (c == u'/' && !in_class) {
            out += u"\\/";
        } else if (c == u'\n') {
            out += u"\\n";
        } else if (c == u'\r') {
            out += u"\\r";
        } else if (c == 0x2028) {
            out += u"\\u2028";
        } else if (c == 0x2029) {
            out += u"\\u2029";
        } else {
            out += c;
        }
    }
    return out;
}

std::optional<RegExpObject*> this_regexp_object(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::RegExp)
        return in.throw_type_error("RegExp.prototype." + std::string(method) + " requires that 'this' be a RegExp object");
    return static_cast<RegExpObject*>(this_value.as_object());
}

std::optional<Object*> this_object(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!this_value.is_object())
        return in.throw_type_error("RegExp.prototype." + std::string(method) + " called on incompatible receiver " + in.describe(this_value));
    return this_value.as_object();
}

// RegExpInitialize (§22.2.3.3): the pattern and flags as strings, the
// flags checked, the pattern compiled, lastIndex reset.
struct Compiled {
    Regex regex;
    JsString* source = nullptr;
    JsString* flags = nullptr;
};

std::optional<Compiled> compile_regexp(Interpreter& in, Value const& pattern, Value const& flags)
{
    Interpreter::Roots const roots(in);
    JsString* source = in.atoms().empty;
    if (!pattern.is_undefined()) {
        std::optional<JsString*> const text = in.to_string(pattern);
        if (!text)
            return std::nullopt;
        source = *text;
    }
    in.root(Value::string(source));
    JsString* flag_string = in.atoms().empty;
    if (!flags.is_undefined()) {
        std::optional<JsString*> const text = in.to_string(flags);
        if (!text)
            return std::nullopt;
        flag_string = *text;
    }
    in.root(Value::string(flag_string));
    std::optional<RegexFlags> const parsed = RegexFlags::parse(flag_string->view());
    if (!parsed)
        return in.throw_syntax_error("Invalid regular expression flags '" + flag_string->to_utf8() + "'");
    Regex::CompileError error;
    std::optional<Regex> regex = Regex::compile(source->view(), *parsed, &error);
    if (!regex)
        return in.throw_syntax_error("Invalid regular expression: /" + source->to_utf8() + "/" + flag_string->to_utf8() + ": " + error.message);
    Compiled compiled;
    compiled.regex = std::move(*regex);
    compiled.source = source;
    compiled.flags = flag_string;
    return compiled;
}

std::optional<Value> regexp_create(Interpreter& in, Value const& pattern, Value const& flags, Object* prototype)
{
    std::optional<Compiled> compiled = compile_regexp(in, pattern, flags);
    if (!compiled)
        return std::nullopt;
    Heap::NoCollect const guard(in.heap());
    auto* object = in.heap().allocate<RegExpObject>(prototype, std::move(compiled->regex), compiled->source, compiled->flags);
    object->put(PropertyKey::atom(in.atoms().last_index), Value::number(0), Writable);
    return Value::object(object);
}

// RegExpBuiltinExec (§22.2.7.2).
std::optional<Value> regexp_builtin_exec(Interpreter& in, RegExpObject& regexp, JsString* input)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&regexp));
    in.root(Value::string(input));
    std::optional<Value> const last_index_value = in.get(regexp, PropertyKey::atom(in.atoms().last_index));
    if (!last_index_value)
        return std::nullopt;
    std::optional<double> const last_index = in.to_length(*last_index_value);
    if (!last_index)
        return std::nullopt;
    RegexFlags const flags = regexp.regex().flags();
    bool const global = flags.global;
    bool const sticky = flags.sticky;
    double start = (global || sticky) ? *last_index : 0;
    std::u16string_view const text = input->view();
    auto const reset = [&]() -> bool {
        return in.set(regexp, PropertyKey::atom(in.atoms().last_index), Value::number(0), true).has_value();
    };
    if (start > static_cast<double>(text.size())) {
        if (global || sticky) {
            if (!reset())
                return std::nullopt;
        }
        return Value::null();
    }
    bool exhausted = false;
    std::optional<Regex::Match> const match = regexp.regex().exec(text, static_cast<std::size_t>(start), &exhausted);
    if (exhausted)
        return in.throw_range_error("regular expression too complex");
    if (!match) {
        if (global || sticky) {
            if (!reset())
                return std::nullopt;
        }
        return Value::null();
    }
    auto const& whole = *match->groups[0];
    if (global || sticky) {
        if (!in.set(regexp, PropertyKey::atom(in.atoms().last_index), Value::number(static_cast<double>(whole.second)), true))
            return std::nullopt;
    }
    ArrayObject* result = in.new_array();
    in.root(Value::object(result));
    result->put(PropertyKey::atom(in.atoms().index), Value::number(static_cast<double>(whole.first)));
    result->put(PropertyKey::atom(in.atoms().input), Value::string(input));
    for (std::size_t k = 0; k < match->groups.size(); ++k) {
        Value value = Value::undefined();
        if (match->groups[k]) {
            auto const& [from, to] = *match->groups[k];
            value = make_string(in, text.substr(from, to - from));
        }
        result->set_element(static_cast<std::uint32_t>(k), value);
    }
    auto const& names = regexp.regex().group_names();
    if (names.empty()) {
        result->put(PropertyKey::atom(in.atoms().groups), Value::undefined());
    } else {
        Object* groups = in.new_object(nullptr);
        groups->set_prototype(nullptr);
        in.root(Value::object(groups));
        for (auto const& [name, index] : names)
            groups->put(in.key(std::u16string_view(name)), result->element(static_cast<std::uint32_t>(index)));
        result->put(PropertyKey::atom(in.atoms().groups), Value::object(groups));
    }
    if (flags.has_indices) {
        // MakeMatchIndicesIndexPairArray (§22.2.7.8): one [start, end] per
        // group, undefined where the group did not take part, and a
        // `groups` of the named ones.
        ArrayObject* indices = in.new_array();
        in.root(Value::object(indices));
        for (std::size_t k = 0; k < match->groups.size(); ++k) {
            Value pair = Value::undefined();
            if (match->groups[k]) {
                Value const bounds[2] = { Value::number(static_cast<double>(match->groups[k]->first)), Value::number(static_cast<double>(match->groups[k]->second)) };
                pair = Value::object(in.new_array(bounds));
            }
            indices->set_element(static_cast<std::uint32_t>(k), pair);
        }
        if (names.empty()) {
            indices->put(PropertyKey::atom(in.atoms().groups), Value::undefined());
        } else {
            Object* index_groups = in.new_object(nullptr);
            index_groups->set_prototype(nullptr);
            in.root(Value::object(index_groups));
            for (auto const& [name, index] : names)
                index_groups->put(in.key(std::u16string_view(name)), indices->element(static_cast<std::uint32_t>(index)));
            indices->put(PropertyKey::atom(in.atoms().groups), Value::object(index_groups));
        }
        result->put(in.key("indices"), Value::object(indices));
    }
    return Value::object(result);
}

// RegExpExec (§22.2.7.1): a script's own `exec` wins when there is one.
std::optional<Value> regexp_exec(Interpreter& in, Object& regexp, JsString* input)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&regexp));
    in.root(Value::string(input));
    std::optional<Value> const exec = in.get(regexp, in.key("exec"));
    if (!exec)
        return std::nullopt;
    if (Interpreter::is_callable(*exec)) {
        in.root(*exec);
        Value const arguments[1] = { Value::string(input) };
        std::optional<Value> const result = in.call(*exec, Value::object(&regexp), arguments);
        if (!result)
            return std::nullopt;
        if (!result->is_object() && !result->is_null())
            return in.throw_type_error("object null is not a function");
        return *result;
    }
    if (regexp.class_id() != Object::Class::RegExp)
        return in.throw_type_error("RegExp.prototype.exec requires that 'this' be a RegExp object");
    return regexp_builtin_exec(in, static_cast<RegExpObject&>(regexp), input);
}

std::optional<JsString*> flags_of(Interpreter& in, Object& regexp)
{
    std::optional<Value> const flags = in.get(regexp, PropertyKey::atom(in.atoms().flags));
    if (!flags)
        return std::nullopt;
    return in.to_string(*flags);
}

// RegExp.prototype[@@match] (§22.2.6.8).
std::optional<Value> regexp_symbol_match(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<Object*> const rx = this_object(in, this_value, "[Symbol.match]");
    if (!rx)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<JsString*> const string = in.to_string(argument(args, 0));
    if (!string)
        return std::nullopt;
    in.root(Value::string(*string));
    std::optional<JsString*> const flags = flags_of(in, **rx);
    if (!flags)
        return std::nullopt;
    if ((*flags)->view().find(u'g') == std::u16string_view::npos)
        return regexp_exec(in, **rx, *string);
    bool const unicode = (*flags)->view().find(u'u') != std::u16string_view::npos || (*flags)->view().find(u'v') != std::u16string_view::npos;
    if (!in.set(**rx, PropertyKey::atom(in.atoms().last_index), Value::number(0), true))
        return std::nullopt;
    ArrayObject* results = in.new_array();
    in.root(Value::object(results));
    std::size_t count = 0;
    while (true) {
        std::optional<Value> const result = regexp_exec(in, **rx, *string);
        if (!result)
            return std::nullopt;
        if (result->is_null())
            return count == 0 ? Value::null() : Value::object(results);
        Interpreter::Roots const match_roots(in);
        in.root(*result);
        std::optional<Value> const matched_value = in.get(*result->as_object(), PropertyKey::index(0));
        if (!matched_value)
            return std::nullopt;
        std::optional<JsString*> const matched = in.to_string(*matched_value);
        if (!matched)
            return std::nullopt;
        results->set_element(static_cast<std::uint32_t>(count++), Value::string(*matched));
        if ((*matched)->is_empty()) {
            std::optional<Value> const this_index_value = in.get(**rx, PropertyKey::atom(in.atoms().last_index));
            if (!this_index_value)
                return std::nullopt;
            std::optional<double> const this_index = in.to_length(*this_index_value);
            if (!this_index)
                return std::nullopt;
            double const next = static_cast<double>(advance_string_index((*string)->view(), static_cast<std::size_t>(*this_index), unicode));
            if (!in.set(**rx, PropertyKey::atom(in.atoms().last_index), Value::number(next), true))
                return std::nullopt;
        }
    }
}

// RegExp.prototype[@@replace] (§22.2.6.11).
std::optional<Value> regexp_symbol_replace(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<Object*> const rx = this_object(in, this_value, "[Symbol.replace]");
    if (!rx)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<JsString*> const string = in.to_string(argument(args, 0));
    if (!string)
        return std::nullopt;
    in.root(Value::string(*string));
    std::u16string_view const text = (*string)->view();
    Value replace_value = argument(args, 1);
    bool const functional = Interpreter::is_callable(replace_value);
    if (!functional) {
        std::optional<JsString*> const replacement = in.to_string(replace_value);
        if (!replacement)
            return std::nullopt;
        replace_value = Value::string(*replacement);
    }
    in.root(replace_value);
    std::optional<JsString*> const flags = flags_of(in, **rx);
    if (!flags)
        return std::nullopt;
    bool const global = (*flags)->view().find(u'g') != std::u16string_view::npos;
    bool unicode = false;
    if (global) {
        unicode = (*flags)->view().find(u'u') != std::u16string_view::npos || (*flags)->view().find(u'v') != std::u16string_view::npos;
        if (!in.set(**rx, PropertyKey::atom(in.atoms().last_index), Value::number(0), true))
            return std::nullopt;
    }
    std::vector<Value> results;
    while (true) {
        std::optional<Value> const result = regexp_exec(in, **rx, *string);
        if (!result)
            return std::nullopt;
        if (result->is_null())
            break;
        in.root(*result);
        results.push_back(*result);
        if (!global)
            break;
        std::optional<Value> const matched_value = in.get(*result->as_object(), PropertyKey::index(0));
        if (!matched_value)
            return std::nullopt;
        std::optional<JsString*> const matched = in.to_string(*matched_value);
        if (!matched)
            return std::nullopt;
        if ((*matched)->is_empty()) {
            std::optional<Value> const this_index_value = in.get(**rx, PropertyKey::atom(in.atoms().last_index));
            if (!this_index_value)
                return std::nullopt;
            std::optional<double> const this_index = in.to_length(*this_index_value);
            if (!this_index)
                return std::nullopt;
            double const next = static_cast<double>(advance_string_index(text, static_cast<std::size_t>(*this_index), unicode));
            if (!in.set(**rx, PropertyKey::atom(in.atoms().last_index), Value::number(next), true))
                return std::nullopt;
        }
    }
    std::u16string accumulated;
    std::size_t next_source_position = 0;
    for (Value const& result_value : results) {
        Interpreter::Roots const result_roots(in);
        Object& result = *result_value.as_object();
        std::optional<double> const result_length = in.length_of_array_like(result);
        if (!result_length)
            return std::nullopt;
        double const capture_count = std::max(*result_length - 1, 0.0);
        std::optional<Value> const matched_value = in.get(result, PropertyKey::index(0));
        if (!matched_value)
            return std::nullopt;
        std::optional<JsString*> const matched = in.to_string(*matched_value);
        if (!matched)
            return std::nullopt;
        in.root(Value::string(*matched));
        std::optional<Value> const position_value = in.get(result, PropertyKey::atom(in.atoms().index));
        if (!position_value)
            return std::nullopt;
        std::optional<double> const position_number = in.to_integer_or_infinity(*position_value);
        if (!position_number)
            return std::nullopt;
        auto const position = static_cast<std::size_t>(std::max(std::min(*position_number, static_cast<double>(text.size())), 0.0));
        std::vector<Value> captures;
        for (double n = 1; n <= capture_count; ++n) {
            std::optional<Value> capture = in.get(result, in.heap().key(n));
            if (!capture)
                return std::nullopt;
            if (!capture->is_undefined()) {
                std::optional<JsString*> const capture_text = in.to_string(*capture);
                if (!capture_text)
                    return std::nullopt;
                capture = Value::string(*capture_text);
            }
            in.root(*capture);
            captures.push_back(*capture);
        }
        std::optional<Value> named_captures = in.get(result, PropertyKey::atom(in.atoms().groups));
        if (!named_captures)
            return std::nullopt;
        in.root(*named_captures);
        std::u16string replacement;
        if (functional) {
            std::vector<Value> replacer_arguments;
            replacer_arguments.push_back(Value::string(*matched));
            replacer_arguments.insert(replacer_arguments.end(), captures.begin(), captures.end());
            replacer_arguments.push_back(Value::number(static_cast<double>(position)));
            replacer_arguments.push_back(Value::string(*string));
            if (!named_captures->is_undefined())
                replacer_arguments.push_back(*named_captures);
            std::optional<Value> const replaced = in.call(replace_value, Value::undefined(), replacer_arguments);
            if (!replaced)
                return std::nullopt;
            in.root(*replaced);
            std::optional<JsString*> const replaced_text = in.to_string(*replaced);
            if (!replaced_text)
                return std::nullopt;
            replacement = (*replaced_text)->data();
        } else {
            if (!named_captures->is_undefined()) {
                std::optional<Object*> const named = in.to_object(*named_captures);
                if (!named)
                    return std::nullopt;
                named_captures = Value::object(*named);
                in.root(*named_captures);
            }
            std::optional<std::u16string> const substituted = get_substitution(in, (*matched)->view(), text, position, captures, *named_captures, replace_value.as_string()->view());
            if (!substituted)
                return std::nullopt;
            replacement = *substituted;
        }
        if (position >= next_source_position) {
            accumulated += text.substr(next_source_position, position - next_source_position);
            accumulated += replacement;
            next_source_position = position + (*matched)->length();
        }
    }
    if (next_source_position < text.size())
        accumulated += text.substr(next_source_position);
    return make_string(in, accumulated);
}

// RegExp.prototype[@@search] (§22.2.6.12).
std::optional<Value> regexp_symbol_search(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<Object*> const rx = this_object(in, this_value, "[Symbol.search]");
    if (!rx)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<JsString*> const string = in.to_string(argument(args, 0));
    if (!string)
        return std::nullopt;
    in.root(Value::string(*string));
    std::optional<Value> const previous = in.get(**rx, PropertyKey::atom(in.atoms().last_index));
    if (!previous)
        return std::nullopt;
    in.root(*previous);
    if (!Interpreter::same_value(*previous, Value::number(0))) {
        if (!in.set(**rx, PropertyKey::atom(in.atoms().last_index), Value::number(0), true))
            return std::nullopt;
    }
    std::optional<Value> const result = regexp_exec(in, **rx, *string);
    if (!result)
        return std::nullopt;
    in.root(*result);
    std::optional<Value> const current = in.get(**rx, PropertyKey::atom(in.atoms().last_index));
    if (!current)
        return std::nullopt;
    if (!Interpreter::same_value(*current, *previous)) {
        if (!in.set(**rx, PropertyKey::atom(in.atoms().last_index), *previous, true))
            return std::nullopt;
    }
    if (result->is_null())
        return Value::number(-1);
    return in.get(*result->as_object(), PropertyKey::atom(in.atoms().index));
}

// RegExp.prototype[@@split] (§22.2.6.14): a sticky copy of the regexp
// tries each position in turn.
std::optional<Value> regexp_symbol_split(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<Object*> const rx = this_object(in, this_value, "[Symbol.split]");
    if (!rx)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<JsString*> const string = in.to_string(argument(args, 0));
    if (!string)
        return std::nullopt;
    in.root(Value::string(*string));
    std::u16string_view const text = (*string)->view();
    std::optional<Value> const constructor = in.species_constructor(**rx, in.intrinsics().regexp_constructor);
    if (!constructor)
        return std::nullopt;
    in.root(*constructor);
    std::optional<JsString*> const flags = flags_of(in, **rx);
    if (!flags)
        return std::nullopt;
    in.root(Value::string(*flags));
    bool const unicode = (*flags)->view().find(u'u') != std::u16string_view::npos || (*flags)->view().find(u'v') != std::u16string_view::npos;
    std::u16string new_flags = (*flags)->data();
    if (new_flags.find(u'y') == std::u16string::npos)
        new_flags += u'y';
    Value const construct_arguments[2] = { this_value, make_string(in, new_flags) };
    in.root(construct_arguments[1]);
    std::optional<Value> const splitter_value = in.construct(*constructor, construct_arguments);
    if (!splitter_value)
        return std::nullopt;
    in.root(*splitter_value);
    Object& splitter = *splitter_value->as_object();
    ArrayObject* array = in.new_array();
    in.root(Value::object(array));
    std::uint32_t length_a = 0;
    double limit = 4294967295.0;
    if (!argument(args, 1).is_undefined()) {
        std::optional<std::uint32_t> const given = in.to_uint32(argument(args, 1));
        if (!given)
            return std::nullopt;
        limit = static_cast<double>(*given);
    }
    if (limit == 0)
        return Value::object(array);
    std::size_t const size = text.size();
    if (size == 0) {
        std::optional<Value> const z = regexp_exec(in, splitter, *string);
        if (!z)
            return std::nullopt;
        if (!z->is_null())
            return Value::object(array);
        array->set_element(0, Value::string(*string));
        return Value::object(array);
    }
    std::size_t p = 0;
    std::size_t q = p;
    while (q < size) {
        if (!in.set(splitter, PropertyKey::atom(in.atoms().last_index), Value::number(static_cast<double>(q)), true))
            return std::nullopt;
        std::optional<Value> const z = regexp_exec(in, splitter, *string);
        if (!z)
            return std::nullopt;
        if (z->is_null()) {
            q = advance_string_index(text, q, unicode);
            continue;
        }
        Interpreter::Roots const match_roots(in);
        in.root(*z);
        std::optional<Value> const end_value = in.get(splitter, PropertyKey::atom(in.atoms().last_index));
        if (!end_value)
            return std::nullopt;
        std::optional<double> const end_number = in.to_length(*end_value);
        if (!end_number)
            return std::nullopt;
        std::size_t const e = std::min(static_cast<std::size_t>(*end_number), size);
        if (e == p) {
            q = advance_string_index(text, q, unicode);
            continue;
        }
        array->set_element(length_a++, make_string(in, text.substr(p, q - p)));
        if (static_cast<double>(length_a) == limit)
            return Value::object(array);
        p = e;
        std::optional<double> const z_length = in.length_of_array_like(*z->as_object());
        if (!z_length)
            return std::nullopt;
        double const capture_count = std::max(*z_length - 1, 0.0);
        for (double i = 1; i <= capture_count; ++i) {
            std::optional<Value> const capture = in.get(*z->as_object(), in.heap().key(i));
            if (!capture)
                return std::nullopt;
            array->set_element(length_a++, *capture);
            if (static_cast<double>(length_a) == limit)
                return Value::object(array);
        }
        q = p;
    }
    array->set_element(length_a, make_string(in, text.substr(p)));
    return Value::object(array);
}

// The RegExp constructor (§22.2.4.1).
std::optional<Value> construct_regexp(Interpreter& in, Args args, Object* new_target, bool called_as_function)
{
    Interpreter::Roots const roots(in);
    Value const pattern = argument(args, 0);
    Value const flags = argument(args, 1);
    in.root(pattern);
    in.root(flags);
    std::optional<bool> const pattern_is_regexp = in.is_regexp(pattern);
    if (!pattern_is_regexp)
        return std::nullopt;
    Function* regexp_constructor = in.intrinsics().regexp_constructor;
    if (called_as_function) {
        new_target = regexp_constructor;
        if (*pattern_is_regexp && flags.is_undefined()) {
            std::optional<Value> const constructor = in.get(pattern, PropertyKey::atom(in.atoms().constructor));
            if (!constructor)
                return std::nullopt;
            if (constructor->is_object() && constructor->as_object() == regexp_constructor)
                return pattern;
        }
    }
    Value p = pattern;
    Value f = flags;
    if (pattern.is_object() && pattern.as_object()->class_id() == Object::Class::RegExp) {
        auto const& source_regexp = *static_cast<RegExpObject*>(pattern.as_object());
        p = Value::string(source_regexp.source());
        if (flags.is_undefined())
            f = Value::string(source_regexp.flags());
    } else if (*pattern_is_regexp) {
        std::optional<Value> const source = in.get(pattern, PropertyKey::atom(in.atoms().source));
        if (!source)
            return std::nullopt;
        p = *source;
        in.root(p);
        if (flags.is_undefined()) {
            std::optional<Value> const pattern_flags = in.get(pattern, PropertyKey::atom(in.atoms().flags));
            if (!pattern_flags)
                return std::nullopt;
            f = *pattern_flags;
            in.root(f);
        }
    }
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target, in.intrinsics().regexp_prototype);
    if (!prototype)
        return std::nullopt;
    return regexp_create(in, p, f, *prototype);
}

// A flag getter (§22.2.6.5 and its siblings): the flag for a RegExp,
// undefined for RegExp.prototype itself, a TypeError otherwise.
void define_flag_getter(Interpreter& in, Object& prototype, std::string_view name, bool RegexFlags::* member)
{
    define_accessor(in, prototype, name, [member](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        if (!this_value.is_object())
            return interp.throw_type_error("RegExp flag getter called on incompatible receiver");
        Object* object = this_value.as_object();
        if (object->class_id() != Object::Class::RegExp) {
            if (object == interp.intrinsics().regexp_prototype)
                return Value::undefined();
            return interp.throw_type_error("RegExp flag getter called on incompatible receiver");
        }
        if (member == nullptr)
            return Value::boolean(false);
        return Value::boolean(static_cast<RegExpObject*>(object)->regex().flags().*member);
    });
}

void install_regexp_library(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object& prototype = *i.regexp_prototype;
    NativeFunction* constructor = in.new_native(
        "RegExp", 2,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> { return construct_regexp(interp, args, nullptr, true); },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> { return construct_regexp(interp, args, new_target, false); });
    i.regexp_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("RegExp"), Value::object(constructor), builtin_attributes);
    {
        NativeFunction* getter = in.new_native("get [Symbol.species]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> { return this_value; });
        constructor->put_accessor(PropertyKey::symbol(in.atoms().symbol_species), getter, nullptr, Configurable);
    }

    define_method(in, prototype, "exec", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<RegExpObject*> const regexp = this_regexp_object(interp, this_value, "exec");
        if (!regexp)
            return std::nullopt;
        std::optional<JsString*> const string = interp.to_string(argument(args, 0));
        if (!string)
            return std::nullopt;
        return regexp_builtin_exec(interp, **regexp, *string);
    });
    define_method(in, prototype, "test", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<Object*> const regexp = this_object(interp, this_value, "test");
        if (!regexp)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<JsString*> const string = interp.to_string(argument(args, 0));
        if (!string)
            return std::nullopt;
        std::optional<Value> const result = regexp_exec(interp, **regexp, *string);
        if (!result)
            return std::nullopt;
        return Value::boolean(!result->is_null());
    });
    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §22.2.6.17: "/" + source + "/" + flags, both read as properties.
        std::optional<Object*> const regexp = this_object(interp, this_value, "toString");
        if (!regexp)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<Value> const source = interp.get(**regexp, PropertyKey::atom(interp.atoms().source));
        if (!source)
            return std::nullopt;
        std::optional<JsString*> const source_text = interp.to_string(*source);
        if (!source_text)
            return std::nullopt;
        interp.root(Value::string(*source_text));
        std::optional<JsString*> const flags_text = flags_of(interp, **regexp);
        if (!flags_text)
            return std::nullopt;
        return make_string(interp, u"/" + (*source_text)->data() + u"/" + (*flags_text)->data());
    });
    define_method(in, prototype, "compile", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // B.2.4.1: the object is re-initialized in place.
        std::optional<RegExpObject*> const regexp = this_regexp_object(interp, this_value, "compile");
        if (!regexp)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        Value pattern = argument(args, 0);
        Value flags = argument(args, 1);
        if (pattern.is_object() && pattern.as_object()->class_id() == Object::Class::RegExp) {
            if (!flags.is_undefined())
                return interp.throw_type_error("Invalid flags supplied to RegExp constructor");
            auto const& source_regexp = *static_cast<RegExpObject*>(pattern.as_object());
            pattern = Value::string(source_regexp.source());
            flags = Value::string(source_regexp.flags());
        }
        std::optional<Compiled> compiled = compile_regexp(interp, pattern, flags);
        if (!compiled)
            return std::nullopt;
        (*regexp)->reset(std::move(compiled->regex), compiled->source, compiled->flags);
        if (!interp.set(**regexp, PropertyKey::atom(interp.atoms().last_index), Value::number(0), true))
            return std::nullopt;
        return this_value;
    });
    define_accessor(in, prototype, "source", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        if (!this_value.is_object())
            return interp.throw_type_error("RegExp.prototype.source getter called on incompatible receiver");
        Object* object = this_value.as_object();
        if (object->class_id() != Object::Class::RegExp) {
            if (object == interp.intrinsics().regexp_prototype)
                return make_string(interp, u"(?:)");
            return interp.throw_type_error("RegExp.prototype.source getter called on incompatible receiver");
        }
        return make_string(interp, escape_pattern(static_cast<RegExpObject*>(object)->source()->view()));
    });
    define_accessor(in, prototype, "flags", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §22.2.6.4: composed from the boolean getters, in "dgimsuvy" order.
        if (!this_value.is_object())
            return interp.throw_type_error("RegExp.prototype.flags getter called on non-object " + interp.describe(this_value));
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        struct Flag {
            char16_t letter;
            std::string_view name;
        };
        constexpr Flag flags[] = {
            { u'd', "hasIndices" }, { u'g', "global" }, { u'i', "ignoreCase" }, { u'm', "multiline" },
            { u's', "dotAll" }, { u'u', "unicode" }, { u'v', "unicodeSets" }, { u'y', "sticky" },
        };
        std::u16string out;
        for (Flag const& flag : flags) {
            std::optional<Value> const value = interp.get(*this_value.as_object(), interp.key(flag.name));
            if (!value)
                return std::nullopt;
            if (Interpreter::to_boolean(*value))
                out += flag.letter;
        }
        return make_string(interp, out);
    });
    define_flag_getter(in, prototype, "global", &RegexFlags::global);
    define_flag_getter(in, prototype, "ignoreCase", &RegexFlags::ignore_case);
    define_flag_getter(in, prototype, "multiline", &RegexFlags::multiline);
    define_flag_getter(in, prototype, "dotAll", &RegexFlags::dot_all);
    define_flag_getter(in, prototype, "unicode", &RegexFlags::unicode);
    define_flag_getter(in, prototype, "sticky", &RegexFlags::sticky);
    define_flag_getter(in, prototype, "hasIndices", &RegexFlags::has_indices);
    define_flag_getter(in, prototype, "unicodeSets", nullptr);
    {
        Heap::NoCollect const symbol_guard(in.heap());
        struct SymbolMethod {
            Symbol* symbol;
            std::string_view name;
            int length;
            NativeFunction::Callback callback;
        };
        SymbolMethod const methods[] = {
            { in.atoms().symbol_match, "[Symbol.match]", 1, regexp_symbol_match },
            { in.atoms().symbol_replace, "[Symbol.replace]", 2, regexp_symbol_replace },
            { in.atoms().symbol_search, "[Symbol.search]", 1, regexp_symbol_search },
            { in.atoms().symbol_split, "[Symbol.split]", 2, regexp_symbol_split },
        };
        for (SymbolMethod const& method : methods) {
            NativeFunction* function = in.new_native(method.name, method.length, method.callback);
            prototype.put(PropertyKey::symbol(method.symbol), Value::object(function), builtin_attributes);
        }
    }
}

// ---------------------------------------------------------------- String

// The String constructor (§22.1.1.1).
std::optional<Value> construct_string(Interpreter& in, Args args, Object* new_target, bool called_as_function)
{
    Interpreter::Roots const roots(in);
    JsString* string = in.atoms().empty;
    if (!args.empty()) {
        Value const value = args[0];
        if (called_as_function && value.is_symbol()) {
            std::u16string text = u"Symbol(";
            if (value.as_symbol()->description())
                text += value.as_symbol()->description()->view();
            text += u")";
            return make_string(in, text);
        }
        std::optional<JsString*> const converted = in.to_string(value);
        if (!converted)
            return std::nullopt;
        string = *converted;
    }
    if (called_as_function)
        return Value::string(string);
    in.root(Value::string(string));
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target, in.intrinsics().string_prototype);
    if (!prototype)
        return std::nullopt;
    return Value::object(in.heap().allocate<StringObject>(*prototype, string));
}

// The searchString argument of includes/startsWith/endsWith: a RegExp is
// refused (§22.1.3.8 step 4).
std::optional<JsString*> search_string(Interpreter& in, Value const& value, std::string_view method)
{
    std::optional<bool> const regexp = in.is_regexp(value);
    if (!regexp)
        return std::nullopt;
    if (*regexp)
        return in.throw_type_error("First argument to String.prototype." + std::string(method) + " must not be a regular expression");
    return in.to_string(value);
}

std::optional<double> clamped_position(Interpreter& in, Value const& value, double length, double fallback)
{
    if (value.is_undefined())
        return fallback;
    std::optional<double> const position = in.to_integer_or_infinity(value);
    if (!position)
        return std::nullopt;
    return std::min(std::max(*position, 0.0), length);
}

// The pad methods (§22.1.3.16, §22.1.3.17).
std::optional<Value> pad_string(Interpreter& in, Value const& this_value, Args args, bool at_start)
{
    Interpreter::Roots const roots(in);
    std::optional<JsString*> const string = this_string(in, this_value, at_start ? "padStart" : "padEnd");
    if (!string)
        return std::nullopt;
    std::optional<double> const max_length = in.to_length(argument(args, 0));
    if (!max_length)
        return std::nullopt;
    auto const length = static_cast<double>((*string)->length());
    if (*max_length <= length)
        return Value::string(*string);
    std::u16string fill = u" ";
    if (!argument(args, 1).is_undefined()) {
        std::optional<JsString*> const filler = in.to_string(argument(args, 1));
        if (!filler)
            return std::nullopt;
        fill = (*filler)->data();
    }
    if (fill.empty())
        return Value::string(*string);
    auto const fill_length = static_cast<std::size_t>(*max_length - length);
    if (fill_length > 0x3FFFFFFF)
        return in.throw_range_error("Invalid string length");
    std::u16string padding;
    padding.reserve(fill_length);
    while (padding.size() < fill_length)
        padding += fill;
    padding.resize(fill_length);
    return make_string(in, at_start ? padding + (*string)->data() : (*string)->data() + padding);
}

// The trim methods (§22.1.3.32 and B.2.2.16).
std::optional<Value> trim_method(Interpreter& in, Value const& this_value, bool start, bool end, std::string_view method)
{
    Interpreter::Roots const roots(in);
    std::optional<JsString*> const string = this_string(in, this_value, method);
    if (!string)
        return std::nullopt;
    return make_string(in, trim_string((*string)->view(), start, end));
}

std::optional<Value> case_method(Interpreter& in, Value const& this_value, bool upper, std::string_view method)
{
    Interpreter::Roots const roots(in);
    std::optional<JsString*> const string = this_string(in, this_value, method);
    if (!string)
        return std::nullopt;
    return make_string(in, upper ? to_upper((*string)->view()) : to_lower((*string)->view()));
}

// CreateHTML (B.2.2.2.1).
std::optional<Value> create_html(Interpreter& in, Value const& this_value, std::string_view tag, std::string_view attribute, Value const& attribute_value, std::string_view method)
{
    Interpreter::Roots const roots(in);
    std::optional<JsString*> const string = this_string(in, this_value, method);
    if (!string)
        return std::nullopt;
    std::u16string out = u"<" + utf16_from_utf8(tag);
    if (!attribute.empty()) {
        std::optional<JsString*> const value = in.to_string(attribute_value);
        if (!value)
            return std::nullopt;
        std::u16string escaped;
        for (char16_t const c : (*value)->view()) {
            if (c == u'"')
                escaped += u"&quot;";
            else
                escaped += c;
        }
        out += u" " + utf16_from_utf8(attribute) + u"=\"" + escaped + u"\"";
    }
    out += u">" + (*string)->data() + u"</" + utf16_from_utf8(tag) + u">";
    return make_string(in, out);
}

// Normalization (§22.1.3.15) over the engine's own tables: NFC and NFD,
// with the compatibility forms served by their canonical counterparts
// since the tables carry no compatibility decompositions.
std::u16string normalize_string(std::u16string_view input, bool compose)
{
    std::u32string points;
    for (std::size_t k = 0; k < input.size();) {
        std::size_t units = 1;
        points += code_point_at(input, k, &units);
        k += units;
    }
    std::u32string result;
    if (compose) {
        result = nfc(points);
    } else {
        std::u32string decomposed;
        for (char32_t const point : points) {
            if (point >= 0xAC00 && point <= 0xD7A3) {
                char32_t const s = point - 0xAC00;
                decomposed += static_cast<char32_t>(0x1100 + s / (21 * 28));
                decomposed += static_cast<char32_t>(0x1161 + (s % (21 * 28)) / 28);
                if (s % 28 != 0)
                    decomposed += static_cast<char32_t>(0x11A7 + s % 28);
                continue;
            }
            std::u32string_view const expansion = canonical_decomposition(point);
            if (expansion.empty())
                decomposed += point;
            else
                decomposed += expansion;
        }
        // Canonical ordering: runs of non-starters sort by combining class.
        for (std::size_t k = 0; k < decomposed.size();) {
            if (canonical_combining_class(decomposed[k]) == 0) {
                ++k;
                continue;
            }
            std::size_t end = k;
            while (end < decomposed.size() && canonical_combining_class(decomposed[end]) != 0)
                ++end;
            std::stable_sort(decomposed.begin() + static_cast<std::ptrdiff_t>(k), decomposed.begin() + static_cast<std::ptrdiff_t>(end),
                [](char32_t a, char32_t b) { return canonical_combining_class(a) < canonical_combining_class(b); });
            k = end;
        }
        result = decomposed;
    }
    std::u16string out;
    for (char32_t const point : result)
        append_code_point(out, point);
    return out;
}

void install_string_prototype(Interpreter& in, Object& prototype)
{
    define_method(in, prototype, "at", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "at");
        if (!string)
            return std::nullopt;
        std::optional<double> const relative = interp.to_integer_or_infinity(argument(args, 0));
        if (!relative)
            return std::nullopt;
        auto const length = static_cast<double>((*string)->length());
        double const k = *relative >= 0 ? *relative : length + *relative;
        if (k < 0 || k >= length)
            return Value::undefined();
        return Value::string(interp.heap().string((*string)->view()[static_cast<std::size_t>(k)]));
    });
    define_method(in, prototype, "charAt", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "charAt");
        if (!string)
            return std::nullopt;
        std::optional<double> const position = interp.to_integer_or_infinity(argument(args, 0));
        if (!position)
            return std::nullopt;
        if (*position < 0 || *position >= static_cast<double>((*string)->length()))
            return Value::string(interp.atoms().empty);
        return Value::string(interp.heap().string((*string)->view()[static_cast<std::size_t>(*position)]));
    });
    define_method(in, prototype, "charCodeAt", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "charCodeAt");
        if (!string)
            return std::nullopt;
        std::optional<double> const position = interp.to_integer_or_infinity(argument(args, 0));
        if (!position)
            return std::nullopt;
        if (*position < 0 || *position >= static_cast<double>((*string)->length()))
            return Value::number(std::numeric_limits<double>::quiet_NaN());
        return Value::number(static_cast<double>((*string)->view()[static_cast<std::size_t>(*position)]));
    });
    define_method(in, prototype, "codePointAt", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "codePointAt");
        if (!string)
            return std::nullopt;
        std::optional<double> const position = interp.to_integer_or_infinity(argument(args, 0));
        if (!position)
            return std::nullopt;
        if (*position < 0 || *position >= static_cast<double>((*string)->length()))
            return Value::undefined();
        return Value::number(static_cast<double>(code_point_at((*string)->view(), static_cast<std::size_t>(*position))));
    });
    define_method(in, prototype, "concat", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "concat");
        if (!string)
            return std::nullopt;
        std::u16string out = (*string)->data();
        for (Value const& arg : args) {
            std::optional<JsString*> const text = interp.to_string(arg);
            if (!text)
                return std::nullopt;
            out += (*text)->view();
        }
        return make_string(interp, out);
    });
    define_method(in, prototype, "endsWith", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "endsWith");
        if (!string)
            return std::nullopt;
        std::optional<JsString*> const search = search_string(interp, argument(args, 0), "endsWith");
        if (!search)
            return std::nullopt;
        interp.root(Value::string(*search));
        auto const length = static_cast<double>((*string)->length());
        std::optional<double> const end = clamped_position(interp, argument(args, 1), length, length);
        if (!end)
            return std::nullopt;
        auto const search_length = static_cast<double>((*search)->length());
        double const start = *end - search_length;
        if (start < 0)
            return Value::boolean(false);
        return Value::boolean((*string)->view().substr(static_cast<std::size_t>(start), static_cast<std::size_t>(search_length)) == (*search)->view());
    });
    define_method(in, prototype, "includes", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "includes");
        if (!string)
            return std::nullopt;
        std::optional<JsString*> const search = search_string(interp, argument(args, 0), "includes");
        if (!search)
            return std::nullopt;
        interp.root(Value::string(*search));
        std::optional<double> const start = clamped_position(interp, argument(args, 1), static_cast<double>((*string)->length()), 0);
        if (!start)
            return std::nullopt;
        return Value::boolean(string_index_of((*string)->view(), (*search)->view(), static_cast<std::size_t>(*start)).has_value());
    });
    define_method(in, prototype, "indexOf", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "indexOf");
        if (!string)
            return std::nullopt;
        std::optional<JsString*> const search = interp.to_string(argument(args, 0));
        if (!search)
            return std::nullopt;
        interp.root(Value::string(*search));
        std::optional<double> const start = clamped_position(interp, argument(args, 1), static_cast<double>((*string)->length()), 0);
        if (!start)
            return std::nullopt;
        std::optional<std::size_t> const found = string_index_of((*string)->view(), (*search)->view(), static_cast<std::size_t>(*start));
        return Value::number(found ? static_cast<double>(*found) : -1.0);
    });
    define_method(in, prototype, "isWellFormed", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "isWellFormed");
        if (!string)
            return std::nullopt;
        std::u16string_view const text = (*string)->view();
        for (std::size_t k = 0; k < text.size();) {
            std::size_t units = 1;
            char32_t const point = code_point_at(text, k, &units);
            if (point >= 0xD800 && point <= 0xDFFF)
                return Value::boolean(false);
            k += units;
        }
        return Value::boolean(true);
    });
    define_method(in, prototype, "lastIndexOf", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §22.1.3.11: a NaN position means the end.
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "lastIndexOf");
        if (!string)
            return std::nullopt;
        std::optional<JsString*> const search = interp.to_string(argument(args, 0));
        if (!search)
            return std::nullopt;
        interp.root(Value::string(*search));
        std::optional<double> const number = interp.to_number(argument(args, 1));
        if (!number)
            return std::nullopt;
        auto const length = static_cast<double>((*string)->length());
        double const position = std::isnan(*number) ? std::numeric_limits<double>::infinity() : Interpreter::to_integer_or_infinity(*number);
        auto const search_length = static_cast<double>((*search)->length());
        double const start = std::min(std::max(position, 0.0), length);
        for (double k = std::min(start, length - search_length); k >= 0; --k) {
            if ((*string)->view().substr(static_cast<std::size_t>(k), static_cast<std::size_t>(search_length)) == (*search)->view())
                return Value::number(k);
        }
        return Value::number(-1);
    });
    define_method(in, prototype, "localeCompare", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // Without locale data, code-unit order is the collation.
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "localeCompare");
        if (!string)
            return std::nullopt;
        std::optional<JsString*> const that = interp.to_string(argument(args, 0));
        if (!that)
            return std::nullopt;
        int const order = (*string)->view().compare((*that)->view());
        return Value::number(order < 0 ? -1.0 : order > 0 ? 1.0 : 0.0);
    });
    define_method(in, prototype, "match", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §22.1.3.13: a @@match on the argument, else a RegExp made of it.
        if (this_value.is_nullish())
            return interp.throw_type_error("String.prototype.match called on null or undefined");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        Value const regexp = argument(args, 0);
        interp.root(regexp);
        if (!regexp.is_nullish()) {
            std::optional<Value> const matcher = interp.get_method(regexp, PropertyKey::symbol(interp.atoms().symbol_match));
            if (!matcher)
                return std::nullopt;
            if (!matcher->is_undefined()) {
                interp.root(*matcher);
                Value const arguments[1] = { this_value };
                return interp.call(*matcher, regexp, arguments);
            }
        }
        std::optional<JsString*> const string = interp.to_string(this_value);
        if (!string)
            return std::nullopt;
        interp.root(Value::string(*string));
        std::optional<Value> const rx = regexp_create(interp, regexp, Value::undefined(), interp.intrinsics().regexp_prototype);
        if (!rx)
            return std::nullopt;
        interp.root(*rx);
        Value const arguments[1] = { Value::string(*string) };
        return interp.invoke(*rx, PropertyKey::symbol(interp.atoms().symbol_match), arguments);
    });
    define_method(in, prototype, "normalize", 0, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "normalize");
        if (!string)
            return std::nullopt;
        std::u16string form = u"NFC";
        if (!argument(args, 0).is_undefined()) {
            std::optional<JsString*> const text = interp.to_string(argument(args, 0));
            if (!text)
                return std::nullopt;
            form = (*text)->data();
        }
        if (form != u"NFC" && form != u"NFD" && form != u"NFKC" && form != u"NFKD")
            return interp.throw_range_error("The normalization form should be one of NFC, NFD, NFKC, NFKD.");
        bool const compose = form == u"NFC" || form == u"NFKC";
        return make_string(interp, normalize_string((*string)->view(), compose));
    });
    define_method(in, prototype, "padEnd", 1, [](Interpreter& interp, Value const& this_value, Args args) { return pad_string(interp, this_value, args, false); });
    define_method(in, prototype, "padStart", 1, [](Interpreter& interp, Value const& this_value, Args args) { return pad_string(interp, this_value, args, true); });
    define_method(in, prototype, "repeat", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "repeat");
        if (!string)
            return std::nullopt;
        std::optional<double> const count = interp.to_integer_or_infinity(argument(args, 0));
        if (!count)
            return std::nullopt;
        if (*count < 0 || std::isinf(*count))
            return interp.throw_range_error("Invalid count value: " + number_to_utf8(*count));
        if (*count == 0 || (*string)->is_empty())
            return Value::string(interp.atoms().empty);
        if (static_cast<double>((*string)->length()) * *count > 0x3FFFFFFF)
            return interp.throw_range_error("Invalid string length");
        std::u16string out;
        out.reserve(static_cast<std::size_t>(static_cast<double>((*string)->length()) * *count));
        for (double k = 0; k < *count; ++k)
            out += (*string)->view();
        return make_string(interp, out);
    });
    for (bool const all : { false, true }) {
        define_method(in, prototype, all ? "replaceAll" : "replace", 2, [all](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            // §22.1.3.19 and §22.1.3.20: a @@replace on the pattern takes
            // over; a string pattern is replaced at its first (or every)
            // occurrence.
            std::string_view const method = all ? "replaceAll" : "replace";
            if (this_value.is_nullish())
                return interp.throw_type_error("String.prototype." + std::string(method) + " called on null or undefined");
            Interpreter::Roots const roots(interp);
            interp.root(this_value);
            Value const search_value = argument(args, 0);
            Value replace_value = argument(args, 1);
            interp.root(search_value);
            interp.root(replace_value);
            if (!search_value.is_nullish()) {
                if (all) {
                    std::optional<bool> const is_regexp = interp.is_regexp(search_value);
                    if (!is_regexp)
                        return std::nullopt;
                    if (*is_regexp) {
                        std::optional<Value> const flags = interp.get(search_value, PropertyKey::atom(interp.atoms().flags));
                        if (!flags)
                            return std::nullopt;
                        if (flags->is_nullish())
                            return interp.throw_type_error("String.prototype.replaceAll called with a non-global RegExp argument");
                        std::optional<JsString*> const flag_text = interp.to_string(*flags);
                        if (!flag_text)
                            return std::nullopt;
                        if ((*flag_text)->view().find(u'g') == std::u16string_view::npos)
                            return interp.throw_type_error("replaceAll must be called with a global RegExp");
                    }
                }
                std::optional<Value> const replacer = interp.get_method(search_value, PropertyKey::symbol(interp.atoms().symbol_replace));
                if (!replacer)
                    return std::nullopt;
                if (!replacer->is_undefined()) {
                    interp.root(*replacer);
                    Value const arguments[2] = { this_value, replace_value };
                    return interp.call(*replacer, search_value, arguments);
                }
            }
            std::optional<JsString*> const string = interp.to_string(this_value);
            if (!string)
                return std::nullopt;
            interp.root(Value::string(*string));
            std::optional<JsString*> const search = interp.to_string(search_value);
            if (!search)
                return std::nullopt;
            interp.root(Value::string(*search));
            bool const functional = Interpreter::is_callable(replace_value);
            if (!functional) {
                std::optional<JsString*> const replacement = interp.to_string(replace_value);
                if (!replacement)
                    return std::nullopt;
                replace_value = Value::string(*replacement);
                interp.root(replace_value);
            }
            std::u16string_view const text = (*string)->view();
            std::u16string_view const pattern = (*search)->view();
            std::vector<std::size_t> positions;
            std::size_t const advance = std::max<std::size_t>(1, pattern.size());
            std::optional<std::size_t> position = string_index_of(text, pattern, 0);
            while (position) {
                positions.push_back(*position);
                if (!all)
                    break;
                position = string_index_of(text, pattern, *position + advance);
            }
            if (positions.empty())
                return Value::string(*string);
            std::u16string result;
            std::size_t end_of_last = 0;
            for (std::size_t const p : positions) {
                result += text.substr(end_of_last, p - end_of_last);
                std::u16string replacement;
                if (functional) {
                    Value const arguments[3] = { Value::string(*search), Value::number(static_cast<double>(p)), Value::string(*string) };
                    std::optional<Value> const replaced = interp.call(replace_value, Value::undefined(), arguments);
                    if (!replaced)
                        return std::nullopt;
                    Interpreter::Roots const call_roots(interp);
                    interp.root(*replaced);
                    std::optional<JsString*> const replaced_text = interp.to_string(*replaced);
                    if (!replaced_text)
                        return std::nullopt;
                    replacement = (*replaced_text)->data();
                } else {
                    std::optional<std::u16string> const substituted = get_substitution(interp, pattern, text, p, {}, Value::undefined(), replace_value.as_string()->view());
                    if (!substituted)
                        return std::nullopt;
                    replacement = *substituted;
                }
                result += replacement;
                end_of_last = p + pattern.size();
            }
            result += text.substr(end_of_last);
            return make_string(interp, result);
        });
    }
    define_method(in, prototype, "search", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §22.1.3.21.
        if (this_value.is_nullish())
            return interp.throw_type_error("String.prototype.search called on null or undefined");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        Value const regexp = argument(args, 0);
        interp.root(regexp);
        if (!regexp.is_nullish()) {
            std::optional<Value> const searcher = interp.get_method(regexp, PropertyKey::symbol(interp.atoms().symbol_search));
            if (!searcher)
                return std::nullopt;
            if (!searcher->is_undefined()) {
                interp.root(*searcher);
                Value const arguments[1] = { this_value };
                return interp.call(*searcher, regexp, arguments);
            }
        }
        std::optional<JsString*> const string = interp.to_string(this_value);
        if (!string)
            return std::nullopt;
        interp.root(Value::string(*string));
        std::optional<Value> const rx = regexp_create(interp, regexp, Value::undefined(), interp.intrinsics().regexp_prototype);
        if (!rx)
            return std::nullopt;
        interp.root(*rx);
        Value const arguments[1] = { Value::string(*string) };
        return interp.invoke(*rx, PropertyKey::symbol(interp.atoms().symbol_search), arguments);
    });
    define_method(in, prototype, "slice", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "slice");
        if (!string)
            return std::nullopt;
        auto const length = static_cast<double>((*string)->length());
        std::optional<double> const start = interp.to_integer_or_infinity(argument(args, 0));
        if (!start)
            return std::nullopt;
        double from = *start < 0 ? std::max(length + *start, 0.0) : std::min(*start, length);
        double to = length;
        if (!argument(args, 1).is_undefined()) {
            std::optional<double> const end = interp.to_integer_or_infinity(argument(args, 1));
            if (!end)
                return std::nullopt;
            to = *end < 0 ? std::max(length + *end, 0.0) : std::min(*end, length);
        }
        if (from >= to)
            return Value::string(interp.atoms().empty);
        return make_string(interp, (*string)->view().substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
    });
    define_method(in, prototype, "split", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §22.1.3.23.
        if (this_value.is_nullish())
            return interp.throw_type_error("String.prototype.split called on null or undefined");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        Value const separator = argument(args, 0);
        Value const limit = argument(args, 1);
        interp.root(separator);
        interp.root(limit);
        if (!separator.is_nullish()) {
            std::optional<Value> const splitter = interp.get_method(separator, PropertyKey::symbol(interp.atoms().symbol_split));
            if (!splitter)
                return std::nullopt;
            if (!splitter->is_undefined()) {
                interp.root(*splitter);
                Value const arguments[2] = { this_value, limit };
                return interp.call(*splitter, separator, arguments);
            }
        }
        std::optional<JsString*> const string = interp.to_string(this_value);
        if (!string)
            return std::nullopt;
        interp.root(Value::string(*string));
        double lim = 4294967295.0;
        if (!limit.is_undefined()) {
            std::optional<std::uint32_t> const given = interp.to_uint32(limit);
            if (!given)
                return std::nullopt;
            lim = static_cast<double>(*given);
        }
        std::optional<JsString*> const separator_string = interp.to_string(separator);
        if (!separator_string)
            return std::nullopt;
        interp.root(Value::string(*separator_string));
        ArrayObject* array = interp.new_array();
        interp.root(Value::object(array));
        if (lim == 0)
            return Value::object(array);
        if (separator.is_undefined()) {
            array->set_element(0, Value::string(*string));
            return Value::object(array);
        }
        std::u16string_view const text = (*string)->view();
        std::u16string_view const pattern = (*separator_string)->view();
        if (pattern.empty()) {
            std::size_t const count = std::min(static_cast<double>(text.size()), lim) > 0 ? static_cast<std::size_t>(std::min(static_cast<double>(text.size()), lim)) : 0;
            for (std::size_t k = 0; k < count; ++k)
                array->set_element(static_cast<std::uint32_t>(k), Value::string(interp.heap().string(text[k])));
            return Value::object(array);
        }
        if (text.empty()) {
            array->set_element(0, Value::string(*string));
            return Value::object(array);
        }
        std::uint32_t length_a = 0;
        std::size_t start = 0;
        std::optional<std::size_t> found = string_index_of(text, pattern, 0);
        while (found) {
            array->set_element(length_a++, make_string(interp, text.substr(start, *found - start)));
            if (static_cast<double>(length_a) >= lim)
                return Value::object(array);
            start = *found + pattern.size();
            found = string_index_of(text, pattern, start);
        }
        array->set_element(length_a, make_string(interp, text.substr(start)));
        return Value::object(array);
    });
    define_method(in, prototype, "startsWith", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "startsWith");
        if (!string)
            return std::nullopt;
        std::optional<JsString*> const search = search_string(interp, argument(args, 0), "startsWith");
        if (!search)
            return std::nullopt;
        interp.root(Value::string(*search));
        std::optional<double> const start = clamped_position(interp, argument(args, 1), static_cast<double>((*string)->length()), 0);
        if (!start)
            return std::nullopt;
        return Value::boolean((*string)->view().substr(static_cast<std::size_t>(*start), (*search)->length()) == (*search)->view());
    });
    define_method(in, prototype, "substring", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "substring");
        if (!string)
            return std::nullopt;
        auto const length = static_cast<double>((*string)->length());
        std::optional<double> const start = clamped_position(interp, argument(args, 0), length, 0);
        if (!start)
            return std::nullopt;
        std::optional<double> const end = clamped_position(interp, argument(args, 1), length, length);
        if (!end)
            return std::nullopt;
        double const from = std::min(*start, *end);
        double const to = std::max(*start, *end);
        return make_string(interp, (*string)->view().substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
    });
    define_method(in, prototype, "substr", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // B.2.2.1.
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "substr");
        if (!string)
            return std::nullopt;
        auto const size = static_cast<double>((*string)->length());
        std::optional<double> const start = interp.to_integer_or_infinity(argument(args, 0));
        if (!start)
            return std::nullopt;
        double from = *start;
        if (from == -std::numeric_limits<double>::infinity())
            from = 0;
        else if (from < 0)
            from = std::max(size + from, 0.0);
        else
            from = std::min(from, size);
        double length = size - from;
        if (!argument(args, 1).is_undefined()) {
            std::optional<double> const given = interp.to_integer_or_infinity(argument(args, 1));
            if (!given)
                return std::nullopt;
            length = std::min(std::max(*given, 0.0), size - from);
        }
        if (length <= 0)
            return Value::string(interp.atoms().empty);
        return make_string(interp, (*string)->view().substr(static_cast<std::size_t>(from), static_cast<std::size_t>(length)));
    });
    define_method(in, prototype, "toLocaleLowerCase", 0, [](Interpreter& interp, Value const& this_value, Args) { return case_method(interp, this_value, false, "toLocaleLowerCase"); });
    define_method(in, prototype, "toLocaleUpperCase", 0, [](Interpreter& interp, Value const& this_value, Args) { return case_method(interp, this_value, true, "toLocaleUpperCase"); });
    define_method(in, prototype, "toLowerCase", 0, [](Interpreter& interp, Value const& this_value, Args) { return case_method(interp, this_value, false, "toLowerCase"); });
    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<JsString*> const string = this_string_primitive(interp, this_value, "toString");
        if (!string)
            return std::nullopt;
        return Value::string(*string);
    });
    define_method(in, prototype, "toUpperCase", 0, [](Interpreter& interp, Value const& this_value, Args) { return case_method(interp, this_value, true, "toUpperCase"); });
    define_method(in, prototype, "toWellFormed", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const string = this_string(interp, this_value, "toWellFormed");
        if (!string)
            return std::nullopt;
        std::u16string out;
        std::u16string_view const text = (*string)->view();
        for (std::size_t k = 0; k < text.size();) {
            std::size_t units = 1;
            char32_t const point = code_point_at(text, k, &units);
            if (point >= 0xD800 && point <= 0xDFFF)
                out += static_cast<char16_t>(0xFFFD);
            else
                out += text.substr(k, units);
            k += units;
        }
        return make_string(interp, out);
    });
    define_method(in, prototype, "trim", 0, [](Interpreter& interp, Value const& this_value, Args) { return trim_method(interp, this_value, true, true, "trim"); });
    NativeFunction* trim_end = define_method(in, prototype, "trimEnd", 0, [](Interpreter& interp, Value const& this_value, Args) { return trim_method(interp, this_value, false, true, "trimEnd"); });
    NativeFunction* trim_start = define_method(in, prototype, "trimStart", 0, [](Interpreter& interp, Value const& this_value, Args) { return trim_method(interp, this_value, true, false, "trimStart"); });
    // B.2.2.15–16: the legacy names are the very same functions.
    prototype.put(in.key("trimLeft"), Value::object(trim_start), builtin_attributes);
    prototype.put(in.key("trimRight"), Value::object(trim_end), builtin_attributes);
    define_method(in, prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<JsString*> const string = this_string_primitive(interp, this_value, "valueOf");
        if (!string)
            return std::nullopt;
        return Value::string(*string);
    });
    // B.2.2.2–14: the HTML wrappers.
    struct Html {
        std::string_view method;
        std::string_view tag;
        std::string_view attribute;
    };
    constexpr Html html_methods[] = {
        { "anchor", "a", "name" }, { "big", "big", "" }, { "blink", "blink", "" }, { "bold", "b", "" },
        { "fixed", "tt", "" }, { "fontcolor", "font", "color" }, { "fontsize", "font", "size" }, { "italics", "i", "" },
        { "link", "a", "href" }, { "small", "small", "" }, { "strike", "strike", "" }, { "sub", "sub", "" }, { "sup", "sup", "" },
    };
    for (Html const& html : html_methods) {
        define_method(in, prototype, html.method, html.attribute.empty() ? 0 : 1, [html](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            return create_html(interp, this_value, html.tag, html.attribute, argument(args, 0), html.method);
        });
    }
}

void install_string_library(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object& prototype = *i.string_prototype;
    NativeFunction* constructor = in.new_native(
        "String", 1,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> { return construct_string(interp, args, nullptr, true); },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> { return construct_string(interp, args, new_target, false); });
    i.string_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("String"), Value::object(constructor), builtin_attributes);

    define_method(in, *constructor, "fromCharCode", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::u16string out;
        for (Value const& arg : args) {
            std::optional<std::uint32_t> const unit = interp.to_uint32(arg);
            if (!unit)
                return std::nullopt;
            out += static_cast<char16_t>(*unit & 0xFFFF);
        }
        return make_string(interp, out);
    });
    define_method(in, *constructor, "fromCodePoint", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::u16string out;
        for (Value const& arg : args) {
            std::optional<double> const number = interp.to_number(arg);
            if (!number)
                return std::nullopt;
            if (std::trunc(*number) != *number || *number < 0 || *number > 0x10FFFF)
                return interp.throw_range_error("Invalid code point " + interp.describe(arg));
            append_code_point(out, static_cast<char32_t>(*number));
        }
        return make_string(interp, out);
    });
    define_method(in, *constructor, "raw", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §22.1.2.4 over an object with a `raw` array-like.
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const cooked = interp.to_object(argument(args, 0));
        if (!cooked)
            return std::nullopt;
        interp.root(Value::object(*cooked));
        std::optional<Value> const raw_value = interp.get(**cooked, PropertyKey::atom(interp.atoms().raw));
        if (!raw_value)
            return std::nullopt;
        std::optional<Object*> const raw = interp.to_object(*raw_value);
        if (!raw)
            return std::nullopt;
        interp.root(Value::object(*raw));
        std::optional<double> const count = interp.length_of_array_like(**raw);
        if (!count)
            return std::nullopt;
        if (*count <= 0)
            return Value::string(interp.atoms().empty);
        std::u16string out;
        for (double k = 0; k < *count; ++k) {
            std::optional<Value> const segment = interp.get(**raw, interp.heap().key(k));
            if (!segment)
                return std::nullopt;
            std::optional<JsString*> const text = interp.to_string(*segment);
            if (!text)
                return std::nullopt;
            out += (*text)->view();
            if (k + 1 == *count)
                break;
            if (static_cast<std::size_t>(k) + 1 < args.size()) {
                std::optional<JsString*> const substitution = interp.to_string(args[static_cast<std::size_t>(k) + 1]);
                if (!substitution)
                    return std::nullopt;
                out += (*substitution)->view();
            }
        }
        return make_string(interp, out);
    });
    install_string_prototype(in, prototype);
}

} // namespace

void install_string(Interpreter& in)
{
    install_string_library(in);
}

void install_regexp(Interpreter& in)
{
    install_regexp_library(in);
}

}
