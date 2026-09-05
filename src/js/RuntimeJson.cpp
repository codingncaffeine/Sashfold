#include "js/Runtime.h"

// JSON (§25.5): parse with its reviver, stringify with its replacer and
// gap. The parser is the JSON grammar of §25.5.1 exactly — the language's
// own literal grammar is wider, and the difference is what test262 checks.

#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

constexpr int max_depth = 1000;

// ------------------------------------------------------------- parsing

class JsonParser {
public:
    JsonParser(Interpreter& in, std::u16string_view text)
        : m_in(in)
        , m_text(text)
    {
    }

    std::optional<Value> parse()
    {
        skip_whitespace();
        std::optional<Value> value = parse_value(0);
        if (!value)
            return std::nullopt;
        m_in.root(*value);
        skip_whitespace();
        if (m_pos < m_text.size())
            return unexpected();
        return value;
    }

private:
    std::nullopt_t unexpected()
    {
        if (m_pos >= m_text.size())
            return m_in.throw_syntax_error("Unexpected end of JSON input");
        std::u16string one(1, m_text[m_pos]);
        return m_in.throw_syntax_error("Unexpected token " + utf8_from_utf16(one) + " in JSON at position " + std::to_string(m_pos));
    }

    void skip_whitespace()
    {
        while (m_pos < m_text.size()) {
            char16_t const c = m_text[m_pos];
            if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r')
                ++m_pos;
            else
                break;
        }
    }

    bool consume(std::u16string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word)
            return false;
        m_pos += word.size();
        return true;
    }

    std::optional<Value> parse_value(int depth)
    {
        if (depth > max_depth)
            return m_in.throw_range_error("Maximum call stack size exceeded");
        if (m_pos >= m_text.size())
            return unexpected();
        char16_t const c = m_text[m_pos];
        if (c == u'{')
            return parse_object(depth);
        if (c == u'[')
            return parse_array(depth);
        if (c == u'"') {
            std::optional<std::u16string> const text = parse_string();
            if (!text)
                return std::nullopt;
            return Value::string(m_in.string(std::u16string_view(*text)));
        }
        if (c == u'-' || (c >= u'0' && c <= u'9'))
            return parse_number();
        if (consume(u"true"))
            return Value::boolean(true);
        if (consume(u"false"))
            return Value::boolean(false);
        if (consume(u"null"))
            return Value::null();
        return unexpected();
    }

    std::optional<Value> parse_number()
    {
        // JSONNumber: -? int frac? exp?, with no leading zeros and no
        // leading + or dot.
        std::size_t const start = m_pos;
        if (m_text[m_pos] == u'-')
            ++m_pos;
        if (m_pos >= m_text.size())
            return unexpected();
        if (m_text[m_pos] == u'0') {
            ++m_pos;
        } else if (m_text[m_pos] >= u'1' && m_text[m_pos] <= u'9') {
            while (m_pos < m_text.size() && m_text[m_pos] >= u'0' && m_text[m_pos] <= u'9')
                ++m_pos;
        } else {
            return unexpected();
        }
        if (m_pos < m_text.size() && m_text[m_pos] == u'.') {
            ++m_pos;
            if (m_pos >= m_text.size() || m_text[m_pos] < u'0' || m_text[m_pos] > u'9')
                return unexpected();
            while (m_pos < m_text.size() && m_text[m_pos] >= u'0' && m_text[m_pos] <= u'9')
                ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == u'e' || m_text[m_pos] == u'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == u'+' || m_text[m_pos] == u'-'))
                ++m_pos;
            if (m_pos >= m_text.size() || m_text[m_pos] < u'0' || m_text[m_pos] > u'9')
                return unexpected();
            while (m_pos < m_text.size() && m_text[m_pos] >= u'0' && m_text[m_pos] <= u'9')
                ++m_pos;
        }
        return Value::number(string_to_number(m_text.substr(start, m_pos - start)));
    }

    std::optional<std::u16string> parse_string()
    {
        ++m_pos; // the opening quote
        std::u16string out;
        while (true) {
            if (m_pos >= m_text.size())
                return unexpected();
            char16_t const c = m_text[m_pos];
            if (c == u'"') {
                ++m_pos;
                return out;
            }
            if (c < 0x20)
                return unexpected();
            if (c != u'\\') {
                out += c;
                ++m_pos;
                continue;
            }
            ++m_pos;
            if (m_pos >= m_text.size())
                return unexpected();
            char16_t const escape = m_text[m_pos++];
            switch (escape) {
            case u'"': out += u'"'; break;
            case u'\\': out += u'\\'; break;
            case u'/': out += u'/'; break;
            case u'b': out += u'\b'; break;
            case u'f': out += u'\f'; break;
            case u'n': out += u'\n'; break;
            case u'r': out += u'\r'; break;
            case u't': out += u'\t'; break;
            case u'u': {
                if (m_pos + 4 > m_text.size())
                    return unexpected();
                char16_t unit = 0;
                for (int k = 0; k < 4; ++k) {
                    char16_t const h = m_text[m_pos];
                    int digit = -1;
                    if (h >= u'0' && h <= u'9')
                        digit = h - u'0';
                    else if (h >= u'a' && h <= u'f')
                        digit = 10 + h - u'a';
                    else if (h >= u'A' && h <= u'F')
                        digit = 10 + h - u'A';
                    if (digit < 0)
                        return unexpected();
                    unit = static_cast<char16_t>(unit * 16 + digit);
                    ++m_pos;
                }
                out += unit;
                break;
            }
            default:
                --m_pos;
                return unexpected();
            }
        }
    }

    std::optional<Value> parse_array(int depth)
    {
        ++m_pos; // [
        Interpreter::Roots const roots(m_in);
        ArrayObject* array = m_in.new_array();
        m_in.root(Value::object(array));
        skip_whitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == u']') {
            ++m_pos;
            return Value::object(array);
        }
        std::uint32_t index = 0;
        while (true) {
            skip_whitespace();
            std::optional<Value> const element = parse_value(depth + 1);
            if (!element)
                return std::nullopt;
            array->set_element(index++, *element);
            skip_whitespace();
            if (m_pos >= m_text.size())
                return unexpected();
            if (m_text[m_pos] == u',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == u']') {
                ++m_pos;
                return Value::object(array);
            }
            return unexpected();
        }
    }

    std::optional<Value> parse_object(int depth)
    {
        ++m_pos; // {
        Interpreter::Roots const roots(m_in);
        Object* object = m_in.new_object();
        m_in.root(Value::object(object));
        skip_whitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == u'}') {
            ++m_pos;
            return Value::object(object);
        }
        while (true) {
            skip_whitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != u'"')
                return unexpected();
            std::optional<std::u16string> const name = parse_string();
            if (!name)
                return std::nullopt;
            skip_whitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != u':')
                return unexpected();
            ++m_pos;
            skip_whitespace();
            std::optional<Value> const value = parse_value(depth + 1);
            if (!value)
                return std::nullopt;
            Interpreter::Roots const member_roots(m_in);
            m_in.root(*value);
            // §25.5.1 step 10: a repeated key replaces, and __proto__ is an
            // own property like any other.
            PropertyKey const key = m_in.key(std::u16string_view(*name));
            if (!m_in.create_data_property(*object, key, *value))
                return std::nullopt;
            skip_whitespace();
            if (m_pos >= m_text.size())
                return unexpected();
            if (m_text[m_pos] == u',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == u'}') {
                ++m_pos;
                return Value::object(object);
            }
            return unexpected();
        }
    }

    Interpreter& m_in;
    std::u16string_view m_text;
    std::size_t m_pos = 0;
};

// InternalizeJSONProperty (§25.5.1.1): the reviver walks the parsed
// tree bottom-up, deleting what it returns undefined for.
std::optional<Value> internalize(Interpreter& in, Object& holder, PropertyKey const& name, Value const& reviver, int depth)
{
    if (depth > max_depth)
        return in.throw_range_error("Maximum call stack size exceeded");
    Interpreter::Roots const roots(in);
    std::optional<Value> const value = in.get(holder, name);
    if (!value)
        return std::nullopt;
    in.root(*value);
    if (value->is_object()) {
        Object& object = *value->as_object();
        if (Interpreter::is_array(*value)) {
            std::optional<double> const length = in.length_of_array_like(object);
            if (!length)
                return std::nullopt;
            for (double k = 0; k < *length; ++k) {
                PropertyKey const key = in.heap().key(k);
                std::optional<Value> const element = internalize(in, object, key, reviver, depth + 1);
                if (!element)
                    return std::nullopt;
                if (element->is_undefined()) {
                    object.delete_property(key);
                } else if (!in.create_data_property(object, key, *element, false)) {
                    return std::nullopt;
                }
            }
        } else {
            std::vector<PropertyKey> keys;
            for (PropertyKey const& key : object.own_keys()) {
                if (key.is_symbol())
                    continue;
                std::optional<PropertyDescriptor> const desc = object.get_own_property(key);
                if (desc && desc->enumerable.value_or(false))
                    keys.push_back(key);
            }
            for (PropertyKey const& key : keys) {
                std::optional<Value> const element = internalize(in, object, key, reviver, depth + 1);
                if (!element)
                    return std::nullopt;
                if (element->is_undefined()) {
                    object.delete_property(key);
                } else if (!in.create_data_property(object, key, *element, false)) {
                    return std::nullopt;
                }
            }
        }
    }
    Value const arguments[2] = { Value::string(in.heap().key_to_string(name)), *value };
    return in.call(reviver, Value::object(&holder), arguments);
}

// ----------------------------------------------------------- stringify

struct Stringifier {
    Interpreter& in;
    Value replacer_function;
    std::vector<PropertyKey> property_list;
    bool has_property_list = false;
    std::u16string gap;
    std::u16string indent;
    std::vector<Object*> stack;

    // QuoteJSONString (§25.5.2.3): well-formed output, with a lone
    // surrogate written as its escape.
    static std::u16string quote(std::u16string_view text)
    {
        std::u16string out = u"\"";
        char const digits[] = "0123456789abcdef";
        for (std::size_t k = 0; k < text.size(); ++k) {
            char16_t const c = text[k];
            switch (c) {
            case u'\b': out += u"\\b"; break;
            case u'\t': out += u"\\t"; break;
            case u'\n': out += u"\\n"; break;
            case u'\f': out += u"\\f"; break;
            case u'\r': out += u"\\r"; break;
            case u'"': out += u"\\\""; break;
            case u'\\': out += u"\\\\"; break;
            default: {
                bool lone = false;
                if (c >= 0xD800 && c <= 0xDBFF)
                    lone = !(k + 1 < text.size() && text[k + 1] >= 0xDC00 && text[k + 1] <= 0xDFFF);
                else if (c >= 0xDC00 && c <= 0xDFFF)
                    lone = !(k > 0 && text[k - 1] >= 0xD800 && text[k - 1] <= 0xDBFF);
                if (c < 0x20 || lone) {
                    out += u"\\u";
                    out += static_cast<char16_t>(digits[(c >> 12) & 15]);
                    out += static_cast<char16_t>(digits[(c >> 8) & 15]);
                    out += static_cast<char16_t>(digits[(c >> 4) & 15]);
                    out += static_cast<char16_t>(digits[c & 15]);
                } else {
                    out += c;
                }
            }
            }
        }
        out += u'"';
        return out;
    }

    // SerializeJSONProperty (§25.5.2.2): nullopt-inner = undefined (the
    // property is left out); nullopt-outer = a throw.
    std::optional<std::optional<std::u16string>> serialize_property(PropertyKey const& key, Object& holder, int depth)
    {
        if (depth > max_depth)
            return in.throw_range_error("Maximum call stack size exceeded");
        Interpreter::Roots const roots(in);
        std::optional<Value> value = in.get(holder, key);
        if (!value)
            return std::nullopt;
        in.root(*value);
        if (value->is_object()) {
            std::optional<Value> const to_json = in.get_method(*value, PropertyKey::atom(in.atoms().to_json));
            if (!to_json)
                return std::nullopt;
            if (!to_json->is_undefined()) {
                in.root(*to_json);
                Value const arguments[1] = { Value::string(in.heap().key_to_string(key)) };
                value = in.call(*to_json, *value, arguments);
                if (!value)
                    return std::nullopt;
                in.root(*value);
            }
        }
        if (!replacer_function.is_undefined()) {
            Value const arguments[2] = { Value::string(in.heap().key_to_string(key)), *value };
            value = in.call(replacer_function, Value::object(&holder), arguments);
            if (!value)
                return std::nullopt;
            in.root(*value);
        }
        if (value->is_object()) {
            Object* object = value->as_object();
            switch (object->class_id()) {
            case Object::Class::Number: {
                std::optional<double> const number = in.to_number(*value);
                if (!number)
                    return std::nullopt;
                value = Value::number(*number);
                break;
            }
            case Object::Class::String: {
                std::optional<JsString*> const string = in.to_string(*value);
                if (!string)
                    return std::nullopt;
                value = Value::string(*string);
                in.root(*value);
                break;
            }
            case Object::Class::Boolean:
                value = static_cast<PrimitiveObject*>(object)->primitive();
                break;
            default:
                break;
            }
        }
        if (value->is_null())
            return std::optional<std::u16string>(u"null");
        if (value->is_boolean())
            return std::optional<std::u16string>(value->as_boolean() ? u"true" : u"false");
        if (value->is_string())
            return std::optional<std::u16string>(quote(value->as_string()->view()));
        if (value->is_number()) {
            if (!std::isfinite(value->as_number()))
                return std::optional<std::u16string>(u"null");
            return std::optional<std::u16string>(number_to_string(value->as_number()));
        }
        if (value->is_object() && !value->as_object()->is_callable()) {
            if (Interpreter::is_array(*value))
                return serialize_array(*value->as_object(), depth);
            return serialize_object(*value->as_object(), depth);
        }
        return std::optional<std::u16string>();
    }

    bool enter(Object& object)
    {
        if (std::find(stack.begin(), stack.end(), &object) != stack.end()) {
            in.throw_type_error("Converting circular structure to JSON");
            return false;
        }
        stack.push_back(&object);
        return true;
    }

    std::optional<std::optional<std::u16string>> serialize_object(Object& object, int depth)
    {
        // SerializeJSONObject (§25.5.2.5).
        if (!enter(object))
            return std::nullopt;
        std::u16string const stepback = indent;
        indent += gap;
        std::vector<PropertyKey> keys;
        if (has_property_list) {
            keys = property_list;
        } else {
            for (PropertyKey const& key : object.own_keys()) {
                if (key.is_symbol())
                    continue;
                std::optional<PropertyDescriptor> const desc = object.get_own_property(key);
                if (desc && desc->enumerable.value_or(false))
                    keys.push_back(key);
            }
        }
        std::vector<std::u16string> partial;
        for (PropertyKey const& key : keys) {
            std::optional<std::optional<std::u16string>> const text = serialize_property(key, object, depth + 1);
            if (!text)
                return std::nullopt;
            if (!*text)
                continue;
            std::u16string member = quote(in.heap().key_to_string(key)->view());
            member += u':';
            if (!gap.empty())
                member += u' ';
            member += **text;
            partial.push_back(std::move(member));
        }
        std::u16string result;
        if (partial.empty()) {
            result = u"{}";
        } else if (gap.empty()) {
            result = u"{";
            for (std::size_t k = 0; k < partial.size(); ++k) {
                if (k > 0)
                    result += u',';
                result += partial[k];
            }
            result += u"}";
        } else {
            result = u"{\n" + indent;
            for (std::size_t k = 0; k < partial.size(); ++k) {
                if (k > 0)
                    result += u",\n" + indent;
                result += partial[k];
            }
            result += u"\n" + stepback + u"}";
        }
        stack.pop_back();
        indent = stepback;
        return std::optional<std::u16string>(std::move(result));
    }

    std::optional<std::optional<std::u16string>> serialize_array(Object& array, int depth)
    {
        // SerializeJSONArray (§25.5.2.6): a hole and anything that would be
        // left out of an object both print as null.
        if (!enter(array))
            return std::nullopt;
        std::u16string const stepback = indent;
        indent += gap;
        std::optional<double> const length = in.length_of_array_like(array);
        if (!length)
            return std::nullopt;
        std::vector<std::u16string> partial;
        for (double k = 0; k < *length; ++k) {
            std::optional<std::optional<std::u16string>> const text = serialize_property(in.heap().key(k), array, depth + 1);
            if (!text)
                return std::nullopt;
            partial.push_back(*text ? **text : std::u16string(u"null"));
        }
        std::u16string result;
        if (partial.empty()) {
            result = u"[]";
        } else if (gap.empty()) {
            result = u"[";
            for (std::size_t k = 0; k < partial.size(); ++k) {
                if (k > 0)
                    result += u',';
                result += partial[k];
            }
            result += u"]";
        } else {
            result = u"[\n" + indent;
            for (std::size_t k = 0; k < partial.size(); ++k) {
                if (k > 0)
                    result += u",\n" + indent;
                result += partial[k];
            }
            result += u"\n" + stepback + u"]";
        }
        stack.pop_back();
        indent = stepback;
        return std::optional<std::u16string>(std::move(result));
    }
};

std::optional<Value> json_stringify(Interpreter& in, Args args)
{
    // §25.5.2.
    Interpreter::Roots const roots(in);
    Value const value = argument(args, 0);
    Value const replacer = argument(args, 1);
    Value space = argument(args, 2);
    in.root(value);
    in.root(replacer);
    in.root(space);
    Stringifier stringifier { in, Value::undefined(), {}, false, {}, {}, {} };
    if (Interpreter::is_callable(replacer)) {
        stringifier.replacer_function = replacer;
    } else if (Interpreter::is_array(replacer)) {
        // Step 4.b: the property list, strings and numbers only, in order
        // and without repeats.
        stringifier.has_property_list = true;
        Object& list = *replacer.as_object();
        std::optional<double> const length = in.length_of_array_like(list);
        if (!length)
            return std::nullopt;
        for (double k = 0; k < *length; ++k) {
            std::optional<Value> const element = in.get(list, in.heap().key(k));
            if (!element)
                return std::nullopt;
            std::optional<JsString*> item;
            if (element->is_string()) {
                item = element->as_string();
            } else if (element->is_number()) {
                item = in.to_string(*element);
            } else if (element->is_object() && (element->as_object()->class_id() == Object::Class::String || element->as_object()->class_id() == Object::Class::Number)) {
                in.root(*element);
                item = in.to_string(*element);
                if (!item)
                    return std::nullopt;
            }
            if (!item || !*item)
                continue;
            PropertyKey const key = in.heap().key(*item);
            if (std::find(stringifier.property_list.begin(), stringifier.property_list.end(), key) == stringifier.property_list.end())
                stringifier.property_list.push_back(key);
        }
    }
    if (space.is_object()) {
        if (space.as_object()->class_id() == Object::Class::Number) {
            std::optional<double> const number = in.to_number(space);
            if (!number)
                return std::nullopt;
            space = Value::number(*number);
        } else if (space.as_object()->class_id() == Object::Class::String) {
            std::optional<JsString*> const string = in.to_string(space);
            if (!string)
                return std::nullopt;
            space = Value::string(*string);
            in.root(space);
        }
    }
    if (space.is_number()) {
        double const count = std::min(10.0, Interpreter::to_integer_or_infinity(space.as_number()));
        if (count >= 1)
            stringifier.gap = std::u16string(static_cast<std::size_t>(count), u' ');
    } else if (space.is_string()) {
        stringifier.gap = std::u16string(space.as_string()->view().substr(0, 10));
    }
    Object* wrapper = in.new_object();
    in.root(Value::object(wrapper));
    wrapper->put(PropertyKey::atom(in.atoms().empty), value);
    std::optional<std::optional<std::u16string>> const text = stringifier.serialize_property(PropertyKey::atom(in.atoms().empty), *wrapper, 0);
    if (!text)
        return std::nullopt;
    if (!*text)
        return Value::undefined();
    return Value::string(in.string(std::u16string_view(**text)));
}

} // namespace

void install_json(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Object* json = in.heap().allocate<Object>(in.intrinsics().object_prototype, Object::Class::Json);
    in.intrinsics().json = json;
    in.global()->put(in.key("JSON"), Value::object(json), builtin_attributes);
    json->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), Value::string(in.atom("JSON")), Configurable);
    define_method(in, *json, "parse", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §25.5.1.
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const text = interp.to_string(argument(args, 0));
        if (!text)
            return std::nullopt;
        interp.root(Value::string(*text));
        JsonParser parser(interp, (*text)->view());
        std::optional<Value> const value = parser.parse();
        if (!value)
            return std::nullopt;
        interp.root(*value);
        Value const reviver = argument(args, 1);
        if (!Interpreter::is_callable(reviver))
            return *value;
        interp.root(reviver);
        Object* root = interp.new_object();
        interp.root(Value::object(root));
        root->put(PropertyKey::atom(interp.atoms().empty), *value);
        return internalize(interp, *root, PropertyKey::atom(interp.atoms().empty), reviver, 0);
    });
    define_method(in, *json, "stringify", 3, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        return json_stringify(interp, args);
    });
}

}
