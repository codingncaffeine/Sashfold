#include "platform/linux/Xkb.h"

#include "core/Unicode.h"
#include "platform/linux/KeysymData.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace sashfold::platform::xkb {

namespace {

constexpr std::uint32_t unicode_keysym_flag = 0x01000000;
constexpr std::uint32_t first_virtual_bit = 8;

bool is_lower_keysym(std::uint32_t keysym)
{
    char32_t const cp = keysym_code_point(keysym);
    return cp != 0 && to_uppercase(cp) != cp && to_lowercase(cp) == cp;
}

bool is_upper_keysym(std::uint32_t keysym)
{
    char32_t const cp = keysym_code_point(keysym);
    return cp != 0 && to_lowercase(cp) != cp && to_uppercase(cp) == cp;
}

bool is_keypad_keysym(std::uint32_t keysym)
{
    return keysym >= 0xff80 && keysym <= 0xffbd; // KP_Space .. KP_Equal
}

std::uint32_t real_modifier_mask(std::string_view name)
{
    if (name == "Shift")
        return shift_mask;
    if (name == "Lock")
        return lock_mask;
    if (name == "Control")
        return control_mask;
    if (name == "Mod1")
        return mod1_mask;
    if (name == "Mod2")
        return mod2_mask;
    if (name == "Mod3")
        return mod3_mask;
    if (name == "Mod4")
        return mod4_mask;
    if (name == "Mod5")
        return mod5_mask;
    if (name == "all")
        return real_modifiers_mask;
    return 0;
}

std::optional<std::uint32_t> parse_hex(std::string_view digits)
{
    if (digits.empty() || digits.size() > 8)
        return std::nullopt;
    std::uint32_t value = 0;
    for (char const c : digits) {
        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value |= static_cast<std::uint32_t>(c - 'A' + 10);
        else
            return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> parse_number(std::string_view text)
{
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return parse_hex(text.substr(2));
    if (text.empty())
        return std::nullopt;
    std::uint32_t value = 0;
    for (char const c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

} // namespace

// --- Keysym vocabulary ------------------------------------------------------

std::optional<std::uint32_t> keysym_from_name(std::string_view name)
{
    if (name.empty())
        return std::nullopt;
    if (name == "NoSymbol")
        return no_symbol;
    auto const* const begin = std::begin(data::keysym_names);
    auto const* const end = std::end(data::keysym_names);
    auto const found = std::lower_bound(begin, end, name,
        [](data::KeysymName const& entry, std::string_view wanted) {
            return std::string_view(entry.name) < wanted;
        });
    if (found != end && std::string_view(found->name) == name)
        return found->keysym;
    if (name[0] == 'U' && name.size() > 1) {
        std::string_view digits = name.substr(1);
        if (digits[0] == '+')
            digits.remove_prefix(1);
        if (std::optional<std::uint32_t> const value = parse_hex(digits)) {
            if (*value < 0x20 || (*value > 0x7e && *value < 0xa0) || *value > 0x10ffff)
                return std::nullopt;
            return *value < 0x100 ? *value : (*value | unicode_keysym_flag);
        }
    }
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
        return parse_hex(name.substr(2));
    return std::nullopt;
}

char32_t keysym_code_point(std::uint32_t keysym)
{
    if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff))
        return keysym;
    if ((keysym & 0xff000000) == unicode_keysym_flag) {
        std::uint32_t const cp = keysym & 0x00ffffff;
        return cp <= 0x10ffff ? cp : 0;
    }
    if (keysym >= 0xffb0 && keysym <= 0xffb9) // KP_0 .. KP_9
        return U'0' + (keysym - 0xffb0);
    switch (keysym) {
    case 0xff80: return U' '; // KP_Space
    case 0xffaa: return U'*'; // KP_Multiply
    case 0xffab: return U'+'; // KP_Add
    case 0xffac: return U','; // KP_Separator
    case 0xffad: return U'-'; // KP_Subtract
    case 0xffae: return U'.'; // KP_Decimal
    case 0xffaf: return U'/'; // KP_Divide
    case 0xffbd: return U'='; // KP_Equal
    default: break;
    }
    if (keysym >= 0xff00) // the function keys: they type nothing
        return 0;
    auto const* const begin = std::begin(data::keysym_code_points);
    auto const* const end = std::end(data::keysym_code_points);
    auto const found = std::lower_bound(begin, end, keysym,
        [](data::KeysymCodePoint const& entry, std::uint32_t wanted) { return entry.keysym < wanted; });
    if (found != end && found->keysym == keysym)
        return found->code_point;
    return 0;
}

bool keysym_is_modifier(std::uint32_t keysym)
{
    if (keysym >= 0xffe1 && keysym <= 0xffee) // Shift_L .. Hyper_R, Caps_Lock among them
        return true;
    if (keysym >= 0xfe01 && keysym <= 0xfe13) // ISO_Lock .. ISO_Level5_Lock
        return true;
    return keysym == 0xff7e || keysym == 0xff7f || keysym == 0xff14; // Mode_switch, Num_Lock, Scroll_Lock
}

// --- The parser --------------------------------------------------------------

class Keymap::Parser {
public:
    Parser(std::string_view text, Keymap& keymap)
        : m_text(text)
        , m_keymap(keymap)
    {
    }

    bool parse();
    // Where parsing stopped: the line and the token, for a refused keymap.
    std::string position() const;

private:
    enum class Kind {
        End,
        Identifier,
        KeyName, // <AD01>, brackets included
        Number,
        String,
        Punct,
    };
    struct Token {
        Kind kind = Kind::End;
        std::string_view text;
        bool is(char punct) const { return kind == Kind::Punct && text.size() == 1 && text[0] == punct; }
        bool is(std::string_view identifier) const { return kind == Kind::Identifier && text == identifier; }
    };
    struct ModifierMapEntry {
        std::uint32_t mask;
        std::string key_name; // one of the two
        std::uint32_t keysym;
    };

    Token next();
    Token peek();
    bool expect(char punct);
    void skip_statement(); // through the next ';' at depth 0
    void skip_block(); // after a '{', through its matching '}'

    bool parse_keycodes();
    bool parse_types();
    bool parse_compatibility();
    bool parse_symbols();
    bool parse_key();
    void parse_virtual_modifiers(); // after the keyword, through ';'
    void parse_symbol_list(std::vector<std::uint32_t>& out); // after '[', through ']'
    std::uint32_t parse_modifier_list(); // stops before the token that ends it
    std::optional<std::uint32_t> resolve_keycode(std::string_view name) const;
    static std::optional<std::uint32_t> group_index(Token const& token);
    int virtual_modifier_index(std::string_view name) const;
    void apply_modifier_map();

    std::string_view m_text;
    std::size_t m_offset = 0;
    std::optional<Token> m_peeked;
    Keymap& m_keymap;
    std::unordered_map<std::string, std::uint32_t> m_keycodes;
    std::unordered_map<std::string, std::string> m_aliases;
    std::vector<ModifierMapEntry> m_modifier_map;
    bool m_interpret_level_one_only = false; // interpret.useModMapMods= level1 sets it
};

Keymap::Parser::Token Keymap::Parser::peek()
{
    if (!m_peeked)
        m_peeked = next();
    return *m_peeked;
}

Keymap::Parser::Token Keymap::Parser::next()
{
    if (m_peeked) {
        Token const token = *m_peeked;
        m_peeked.reset();
        return token;
    }
    for (;;) {
        while (m_offset < m_text.size() && (m_text[m_offset] == ' ' || m_text[m_offset] == '\t'
                   || m_text[m_offset] == '\n' || m_text[m_offset] == '\r'))
            ++m_offset;
        if (m_offset + 1 < m_text.size() && m_text[m_offset] == '/' && m_text[m_offset + 1] == '/') {
            while (m_offset < m_text.size() && m_text[m_offset] != '\n')
                ++m_offset;
            continue;
        }
        if (m_offset < m_text.size() && m_text[m_offset] == '#') {
            while (m_offset < m_text.size() && m_text[m_offset] != '\n')
                ++m_offset;
            continue;
        }
        break;
    }
    if (m_offset >= m_text.size())
        return {};
    char const c = m_text[m_offset];
    std::size_t const start = m_offset;
    auto is_word = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
    };
    if (c == '<') {
        while (m_offset < m_text.size() && m_text[m_offset] != '>')
            ++m_offset;
        if (m_offset < m_text.size())
            ++m_offset;
        return { Kind::KeyName, m_text.substr(start, m_offset - start) };
    }
    if (c == '"') {
        ++m_offset;
        while (m_offset < m_text.size() && m_text[m_offset] != '"') {
            if (m_text[m_offset] == '\\' && m_offset + 1 < m_text.size())
                ++m_offset;
            ++m_offset;
        }
        std::string_view const body = m_text.substr(start + 1, m_offset - start - 1);
        if (m_offset < m_text.size())
            ++m_offset;
        return { Kind::String, body };
    }
    if (c >= '0' && c <= '9') {
        while (m_offset < m_text.size() && is_word(m_text[m_offset]))
            ++m_offset;
        return { Kind::Number, m_text.substr(start, m_offset - start) };
    }
    if (is_word(c)) {
        while (m_offset < m_text.size() && (is_word(m_text[m_offset]) || m_text[m_offset] == '.'))
            ++m_offset;
        return { Kind::Identifier, m_text.substr(start, m_offset - start) };
    }
    ++m_offset;
    return { Kind::Punct, m_text.substr(start, 1) };
}

bool Keymap::Parser::expect(char punct)
{
    return next().is(punct);
}

void Keymap::Parser::skip_statement()
{
    int depth = 0;
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End)
            return;
        if (token.is('{') || token.is('[') || token.is('('))
            ++depth;
        else if (token.is('}') || token.is(']') || token.is(')'))
            --depth;
        else if (token.is(';') && depth <= 0)
            return;
        if (depth < 0)
            return;
    }
}

void Keymap::Parser::skip_block()
{
    int depth = 1;
    while (depth > 0) {
        Token const token = next();
        if (token.kind == Kind::End)
            return;
        if (token.is('{'))
            ++depth;
        else if (token.is('}'))
            --depth;
    }
}

bool Keymap::Parser::parse()
{
    if (!next().is("xkb_keymap") || !expect('{'))
        return false;
    for (;;) {
        Token const section = next();
        if (section.kind == Kind::End)
            return false;
        if (section.is('}'))
            break;
        if (section.kind != Kind::Identifier)
            return false;
        if (peek().kind == Kind::String)
            next(); // the section's name
        if (!expect('{'))
            return false;
        bool ok = true;
        if (section.text == "xkb_keycodes")
            ok = parse_keycodes();
        else if (section.text == "xkb_types")
            ok = parse_types();
        else if (section.text == "xkb_compatibility" || section.text == "xkb_compat")
            ok = parse_compatibility();
        else if (section.text == "xkb_symbols")
            ok = parse_symbols();
        else
            skip_block();
        if (!ok)
            return false;
        if (peek().is(';'))
            next();
    }
    apply_modifier_map();
    m_keymap.resolve_virtual_modifiers();
    // The types the symbols named are known now: settle every group's.
    for (auto& [code, key] : m_keymap.m_keys) {
        for (Group& group : key.groups) {
            if (group.type < 0)
                group.type = m_keymap.automatic_type(group);
        }
    }
    return true;
}

bool Keymap::Parser::parse_keycodes()
{
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End)
            return false;
        if (token.is('}'))
            return true;
        if (token.kind == Kind::KeyName) {
            if (!expect('='))
                return false;
            Token const value = next();
            if (value.kind != Kind::Number)
                return false;
            if (std::optional<std::uint32_t> const code = parse_number(value.text))
                m_keycodes[std::string(token.text)] = *code;
            skip_statement();
            continue;
        }
        if (token.is("alias")) {
            Token const alias = next();
            if (!expect('='))
                return false;
            Token const target = next();
            if (alias.kind == Kind::KeyName && target.kind == Kind::KeyName)
                m_aliases[std::string(alias.text)] = std::string(target.text);
            skip_statement();
            continue;
        }
        skip_statement(); // minimum, maximum, indicator ...
    }
}

int Keymap::Parser::virtual_modifier_index(std::string_view name) const
{
    for (std::size_t i = 0; i < m_keymap.m_virtual_modifiers.size(); ++i) {
        if (m_keymap.m_virtual_modifiers[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

void Keymap::Parser::parse_virtual_modifiers()
{
    for (;;) {
        Token const name = next();
        if (name.kind == Kind::End || name.is(';'))
            return;
        if (name.kind != Kind::Identifier)
            continue;
        // Declared once per keymap; the types and compatibility sections
        // both repeat the list.
        if (virtual_modifier_index(name.text) < 0)
            m_keymap.m_virtual_modifiers.push_back({ std::string(name.text), 0 });
        if (peek().is('=')) {
            next();
            Token const value = next(); // an explicit mapping
            if (value.kind == Kind::Number) {
                if (std::optional<std::uint32_t> const mapping = parse_number(value.text)) {
                    int const index = virtual_modifier_index(name.text);
                    m_keymap.m_virtual_modifiers[static_cast<std::size_t>(index)].mapping
                        = *mapping & real_modifiers_mask;
                }
            }
        }
    }
}

std::uint32_t Keymap::Parser::parse_modifier_list()
{
    std::uint32_t mask = 0;
    for (;;) {
        Token const token = peek();
        if (token.kind == Kind::Identifier) {
            next();
            if (token.text != "none" && token.text != "None")
                mask |= m_keymap.named_mask(token.text);
            continue;
        }
        if (token.is('+')) {
            next();
            continue;
        }
        return mask;
    }
}

bool Keymap::Parser::parse_types()
{
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End)
            return false;
        if (token.is('}'))
            return true;
        if (token.is("virtual_modifiers")) {
            parse_virtual_modifiers();
            continue;
        }
        if (token.is("type")) {
            Token const name = next();
            if (name.kind != Kind::String || !expect('{'))
                return false;
            Type type;
            type.name = std::string(name.text);
            for (;;) {
                Token const field = next();
                if (field.kind == Kind::End)
                    return false;
                if (field.is('}'))
                    break;
                if (field.is("modifiers")) {
                    if (!expect('='))
                        return false;
                    type.mask = parse_modifier_list();
                    skip_statement();
                } else if (field.is("map")) {
                    if (!expect('['))
                        return false;
                    std::uint32_t const mods = parse_modifier_list();
                    if (!expect(']') || !expect('='))
                        return false;
                    Token const level = next();
                    std::optional<std::uint32_t> number;
                    if (level.kind == Kind::Number)
                        number = parse_number(level.text);
                    else if (level.kind == Kind::Identifier && level.text.starts_with("Level"))
                        number = parse_number(level.text.substr(5));
                    if (number && *number >= 1)
                        type.map.emplace_back(mods, *number - 1);
                    skip_statement();
                } else {
                    skip_statement(); // preserve, level_name
                }
            }
            if (peek().is(';'))
                next();
            m_keymap.m_types.push_back(std::move(type));
            continue;
        }
        skip_statement();
    }
}

bool Keymap::Parser::parse_compatibility()
{
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End)
            return false;
        if (token.is('}'))
            return true;
        if (token.is("virtual_modifiers")) {
            parse_virtual_modifiers();
            continue;
        }
        if (token.is("interpret.useModMapMods")) {
            if (!expect('='))
                return false;
            Token const value = next();
            m_interpret_level_one_only = value.is("level1") || value.is("Level1");
            skip_statement();
            continue;
        }
        if (token.is("interpret")) {
            Interpret interpret;
            interpret.level_one_only = m_interpret_level_one_only;
            Token const symbol = next();
            // A name, or the hex number some writers spell a keysym with.
            if (symbol.kind != Kind::Identifier && symbol.kind != Kind::Number)
                return false;
            if (symbol.text == "Any" || symbol.text == "any") {
                interpret.any_keysym = true;
            } else if (std::optional<std::uint32_t> const keysym = keysym_from_name(symbol.text)) {
                interpret.keysym = *keysym;
            } else {
                interpret.known = false;
            }
            if (peek().is('+')) {
                next();
                Token const predicate = next();
                if (predicate.kind != Kind::Identifier)
                    return false;
                if (predicate.text == "NoneOf")
                    interpret.match = Interpret::Match::NoneOf;
                else if (predicate.text == "AnyOfOrNone")
                    interpret.match = Interpret::Match::AnyOfOrNone;
                else if (predicate.text == "AnyOf")
                    interpret.match = Interpret::Match::AnyOf;
                else if (predicate.text == "AllOf")
                    interpret.match = Interpret::Match::AllOf;
                else if (predicate.text == "Exactly")
                    interpret.match = Interpret::Match::Exactly;
                if (!expect('('))
                    return false;
                interpret.mods = parse_modifier_list() & real_modifiers_mask;
                if (!expect(')'))
                    return false;
            }
            if (!expect('{'))
                return false;
            for (;;) {
                Token const field = next();
                if (field.kind == Kind::End)
                    return false;
                if (field.is('}'))
                    break;
                if (field.is("virtualModifier") || field.is("virtualMod")) {
                    if (!expect('='))
                        return false;
                    Token const name = next();
                    if (name.kind == Kind::Identifier)
                        interpret.virtual_modifier = virtual_modifier_index(name.text);
                    skip_statement();
                } else if (field.is("useModMapMods")) {
                    if (!expect('='))
                        return false;
                    Token const value = next();
                    interpret.level_one_only = value.is("level1") || value.is("Level1");
                    skip_statement();
                } else {
                    skip_statement(); // action, repeat, locking
                }
            }
            if (peek().is(';'))
                next();
            m_keymap.m_interprets.push_back(interpret);
            continue;
        }
        skip_statement(); // indicator, group, interpret.repeat
    }
}

std::optional<std::uint32_t> Keymap::Parser::resolve_keycode(std::string_view name) const
{
    std::string current(name);
    for (int hops = 0; hops < 8; ++hops) {
        if (auto const found = m_keycodes.find(current); found != m_keycodes.end())
            return found->second;
        auto const alias = m_aliases.find(current);
        if (alias == m_aliases.end())
            return std::nullopt;
        current = alias->second;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Keymap::Parser::group_index(Token const& token)
{
    if (token.kind == Kind::Number) {
        std::optional<std::uint32_t> const number = parse_number(token.text);
        if (number && *number >= 1)
            return *number - 1;
        return std::nullopt;
    }
    if (token.kind == Kind::Identifier && token.text.starts_with("Group")) {
        std::optional<std::uint32_t> const number = parse_number(token.text.substr(5));
        if (number && *number >= 1)
            return *number - 1;
    }
    return std::nullopt;
}

void Keymap::Parser::parse_symbol_list(std::vector<std::uint32_t>& out)
{
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End || token.is(']'))
            return;
        if (token.is(','))
            continue;
        if (token.is('{')) {
            // Several keysyms on one level: the first is the key's.
            bool first = true;
            for (;;) {
                Token const inner = next();
                if (inner.kind == Kind::End || inner.is('}'))
                    break;
                if (inner.is(','))
                    continue;
                if (first) {
                    out.push_back(keysym_from_name(inner.text).value_or(no_symbol));
                    first = false;
                }
            }
            if (first)
                out.push_back(no_symbol);
            continue;
        }
        if (token.kind == Kind::Identifier || token.kind == Kind::Number)
            out.push_back(keysym_from_name(token.text).value_or(no_symbol));
    }
}

bool Keymap::Parser::parse_key()
{
    Token const name = next();
    if (name.kind != Kind::KeyName || !expect('{'))
        return false;
    Key key;
    std::string key_type; // a type named for every group
    std::vector<std::string> group_types; // or per group
    std::size_t positional = 0;
    auto group_at = [&key](std::size_t index) -> Group& {
        if (key.groups.size() <= index)
            key.groups.resize(index + 1);
        return key.groups[index];
    };
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End)
            return false;
        if (token.is('}'))
            break;
        if (token.is(','))
            continue;
        if (token.is('[')) {
            parse_symbol_list(group_at(positional++).keysyms);
            continue;
        }
        if (token.is("symbols")) {
            std::size_t index = positional;
            if (peek().is('[')) {
                next();
                if (std::optional<std::uint32_t> const group = group_index(next()))
                    index = *group;
                if (!expect(']'))
                    return false;
            }
            if (!expect('=') || !expect('['))
                return false;
            Group& group = group_at(index);
            group.keysyms.clear();
            parse_symbol_list(group.keysyms);
            if (index == positional)
                ++positional;
            continue;
        }
        if (token.is("type")) {
            std::optional<std::uint32_t> index;
            if (peek().is('[')) {
                next();
                index = group_index(next());
                if (!expect(']'))
                    return false;
            }
            if (!expect('='))
                return false;
            Token const value = next();
            if (value.kind != Kind::String)
                return false;
            if (index) {
                if (group_types.size() <= *index)
                    group_types.resize(*index + 1);
                group_types[*index] = std::string(value.text);
            } else {
                key_type = std::string(value.text);
            }
            continue;
        }
        if (token.is("virtualMods") || token.is("vmods")) {
            if (!expect('='))
                return false;
            key.explicit_vmodmap = parse_modifier_list() >> first_virtual_bit;
            continue;
        }
        // actions[...] = [ ... ], repeat= yes, locks= ...: nothing a symbol
        // lookup needs.
        if (token.kind == Kind::Identifier) {
            int depth = 0;
            for (;;) {
                Token const skip = peek();
                if (skip.kind == Kind::End)
                    return false;
                if (depth == 0 && (skip.is(',') || skip.is('}')))
                    break;
                next();
                if (skip.is('[') || skip.is('{') || skip.is('('))
                    ++depth;
                else if (skip.is(']') || skip.is('}') || skip.is(')'))
                    --depth;
            }
            continue;
        }
    }
    if (peek().is(';'))
        next();
    for (std::size_t g = 0; g < key.groups.size(); ++g) {
        std::string const& named = g < group_types.size() && !group_types[g].empty() ? group_types[g] : key_type;
        if (!named.empty())
            key.groups[g].type = m_keymap.type_index(named);
    }
    if (std::optional<std::uint32_t> const code = resolve_keycode(name.text))
        m_keymap.m_keys[*code] = std::move(key);
    return true;
}

bool Keymap::Parser::parse_symbols()
{
    for (;;) {
        Token const token = next();
        if (token.kind == Kind::End)
            return false;
        if (token.is('}'))
            return true;
        if (token.is("key")) {
            if (!parse_key())
                return false;
            continue;
        }
        if (token.is("modifier_map")) {
            Token const modifier = next();
            std::uint32_t const mask = modifier.kind == Kind::Identifier ? real_modifier_mask(modifier.text) : 0;
            if (!expect('{'))
                return false;
            for (;;) {
                Token const entry = next();
                if (entry.kind == Kind::End)
                    return false;
                if (entry.is('}'))
                    break;
                if (entry.kind == Kind::KeyName)
                    m_modifier_map.push_back({ mask, std::string(entry.text), no_symbol });
                else if (entry.kind == Kind::Identifier)
                    m_modifier_map.push_back({ mask, {}, keysym_from_name(entry.text).value_or(no_symbol) });
            }
            if (peek().is(';'))
                next();
            continue;
        }
        skip_statement(); // name[1]= "..."
    }
}

void Keymap::Parser::apply_modifier_map()
{
    for (ModifierMapEntry const& entry : m_modifier_map) {
        if (entry.mask == 0)
            continue;
        if (!entry.key_name.empty()) {
            if (std::optional<std::uint32_t> const code = resolve_keycode(entry.key_name)) {
                if (auto const found = m_keymap.m_keys.find(*code); found != m_keymap.m_keys.end())
                    found->second.modmap |= entry.mask;
            }
            continue;
        }
        if (entry.keysym == no_symbol)
            continue;
        // A keysym names every key whose base symbol it is.
        for (auto& [code, key] : m_keymap.m_keys) {
            if (!key.groups.empty() && !key.groups[0].keysyms.empty() && key.groups[0].keysyms[0] == entry.keysym)
                key.modmap |= entry.mask;
        }
    }
}

// --- Keymap ------------------------------------------------------------------

std::string Keymap::Parser::position() const
{
    std::size_t line = 1;
    for (std::size_t i = 0; i < m_offset && i < m_text.size(); ++i) {
        if (m_text[i] == '\n')
            ++line;
    }
    std::size_t const start = std::min(m_offset, m_text.size());
    std::size_t end = start;
    while (end < m_text.size() && end - start < 40 && m_text[end] != '\n')
        ++end;
    return "line " + std::to_string(line) + " near \"" + std::string(m_text.substr(start, end - start)) + "\"";
}

std::optional<Keymap> Keymap::parse(std::string_view text, std::string* error)
{
    Keymap keymap;
    Parser parser(text, keymap);
    if (!parser.parse()) {
        if (error)
            *error = "the keymap stopped parsing at " + parser.position();
        return std::nullopt;
    }
    if (keymap.m_keys.empty()) {
        if (error)
            *error = "the keymap defines no keys";
        return std::nullopt;
    }
    return keymap;
}

std::uint32_t Keymap::named_mask(std::string_view name) const
{
    if (std::uint32_t const real = real_modifier_mask(name))
        return real;
    for (std::size_t i = 0; i < m_virtual_modifiers.size(); ++i) {
        if (m_virtual_modifiers[i].name == name) {
            std::uint32_t const bit = first_virtual_bit + static_cast<std::uint32_t>(i);
            return bit < 32 ? (1u << bit) : 0u;
        }
    }
    return 0;
}

std::uint32_t Keymap::effective_mask(std::uint32_t named) const
{
    std::uint32_t mask = named & real_modifiers_mask;
    for (std::size_t i = 0; i < m_virtual_modifiers.size(); ++i) {
        std::uint32_t const bit = first_virtual_bit + static_cast<std::uint32_t>(i);
        if (bit < 32 && (named & (1u << bit)))
            mask |= m_virtual_modifiers[i].mapping;
    }
    return mask;
}

// libxkbcommon's UpdateDerivedKeymapFields: each key's symbols pick an
// interpret; the interpret's virtual modifier then maps to the key's real
// modifiers; the types' masks are rewritten in real terms.
void Keymap::resolve_virtual_modifiers()
{
    for (auto& [code, key] : m_keys) {
        std::uint32_t vmodmap = 0;
        for (std::size_t g = 0; g < key.groups.size(); ++g) {
            std::vector<std::uint32_t> const& syms = key.groups[g].keysyms;
            for (std::size_t level = 0; level < syms.size(); ++level) {
                std::uint32_t const sym = syms[level];
                if (sym == no_symbol)
                    continue;
                Interpret const* chosen = nullptr;
                for (Interpret const& interpret : m_interprets) {
                    if (!interpret.known)
                        continue;
                    if (!interpret.any_keysym && interpret.keysym != sym)
                        continue;
                    std::uint32_t const mods = (interpret.level_one_only && level != 0) ? 0 : key.modmap;
                    bool found = false;
                    switch (interpret.match) {
                    case Interpret::Match::NoneOf: found = (interpret.mods & mods) == 0; break;
                    case Interpret::Match::AnyOfOrNone: found = mods == 0 || (interpret.mods & mods) != 0; break;
                    case Interpret::Match::AnyOf: found = (interpret.mods & mods) != 0; break;
                    case Interpret::Match::AllOf: found = (interpret.mods & mods) == interpret.mods; break;
                    case Interpret::Match::Exactly: found = interpret.mods == mods; break;
                    }
                    if (found) {
                        chosen = &interpret;
                        break;
                    }
                }
                if (!chosen || chosen->virtual_modifier < 0)
                    continue;
                if ((g == 0 && level == 0) || !chosen->level_one_only)
                    vmodmap |= 1u << static_cast<std::uint32_t>(chosen->virtual_modifier);
            }
        }
        if (key.explicit_vmodmap)
            vmodmap = *key.explicit_vmodmap;
        for (std::size_t i = 0; i < m_virtual_modifiers.size() && i < 32; ++i) {
            if (vmodmap & (1u << i))
                m_virtual_modifiers[i].mapping |= key.modmap;
        }
    }
    for (Type& type : m_types) {
        type.mask = effective_mask(type.mask);
        for (auto& [mods, level] : type.map)
            mods = effective_mask(mods);
    }
}

std::uint32_t Keymap::modifier_mask(std::string_view name) const
{
    return effective_mask(named_mask(name));
}

int Keymap::type_index(std::string_view name) const
{
    for (std::size_t i = 0; i < m_types.size(); ++i) {
        if (m_types[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

// libxkbcommon's FindAutomaticType: a key that names no type gets one from
// the shape of its symbols.
int Keymap::automatic_type(Group const& group) const
{
    std::vector<std::uint32_t> const& syms = group.keysyms;
    std::size_t const width = syms.size();
    auto at = [&syms](std::size_t i) { return i < syms.size() ? syms[i] : no_symbol; };
    if (width <= 1)
        return type_index("ONE_LEVEL");
    if (width == 2) {
        if (is_lower_keysym(at(0)) && is_upper_keysym(at(1)))
            return type_index("ALPHABETIC");
        if (is_keypad_keysym(at(0)) || is_keypad_keysym(at(1)))
            return type_index("KEYPAD");
        return type_index("TWO_LEVEL");
    }
    if (width <= 4) {
        if (is_lower_keysym(at(0)) && is_upper_keysym(at(1))) {
            if (is_lower_keysym(at(2)) && is_upper_keysym(at(3)))
                return type_index("FOUR_LEVEL_ALPHABETIC");
            return type_index("FOUR_LEVEL_SEMIALPHABETIC");
        }
        if (is_keypad_keysym(at(0)) || is_keypad_keysym(at(1)))
            return type_index("FOUR_LEVEL_KEYPAD");
        return type_index("FOUR_LEVEL");
    }
    return -1;
}

std::uint32_t Keymap::keysym(std::uint32_t keycode, std::uint32_t mods, std::uint32_t group) const
{
    auto const found = m_keys.find(keycode);
    if (found == m_keys.end() || found->second.groups.empty())
        return no_symbol;
    std::vector<Group> const& groups = found->second.groups;
    Group const& chosen = groups[group % groups.size()]; // groups wrap
    std::uint32_t level = 0;
    if (chosen.type >= 0 && static_cast<std::size_t>(chosen.type) < m_types.size()) {
        Type const& type = m_types[static_cast<std::size_t>(chosen.type)];
        std::uint32_t const active = mods & type.mask;
        for (auto const& [entry_mods, entry_level] : type.map) {
            if (entry_mods == active) {
                level = entry_level;
                break;
            }
        }
    } else if (mods & shift_mask) {
        level = 1; // no types at all: Shift is the one level a keymap cannot lack
    }
    if (level >= chosen.keysyms.size())
        return no_symbol;
    return chosen.keysyms[level];
}

}
