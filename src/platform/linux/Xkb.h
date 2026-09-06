#pragma once

// The keyboard map a Wayland compositor hands its clients: an xkb keymap in
// the text form libxkbcommon writes (format xkb_v1), parsed here so the
// shell needs no keymap library. Four sections matter. xkb_keycodes names
// the key numbers; xkb_types says which modifiers pick which shift level;
// xkb_symbols lists each key's symbols per group and level, and maps keys
// to the eight real modifiers; xkb_compatibility ties key symbols to the
// keymap's virtual modifiers (LevelThree, NumLock, Alt ...). The
// compositor reports modifier state as real-modifier bits only, so the
// virtual modifiers the types speak of are resolved to real ones the way
// libxkbcommon resolves them: a virtual modifier maps to the real
// modifiers of every key whose symbol an `interpret` assigns it to.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::platform::xkb {

// The eight real modifiers have fixed indices; the compositor's masks use
// them.
constexpr std::uint32_t shift_mask = 1u << 0;
constexpr std::uint32_t lock_mask = 1u << 1;
constexpr std::uint32_t control_mask = 1u << 2;
constexpr std::uint32_t mod1_mask = 1u << 3;
constexpr std::uint32_t mod2_mask = 1u << 4;
constexpr std::uint32_t mod3_mask = 1u << 5;
constexpr std::uint32_t mod4_mask = 1u << 6;
constexpr std::uint32_t mod5_mask = 1u << 7;
constexpr std::uint32_t real_modifiers_mask = 0xffu;

constexpr std::uint32_t no_symbol = 0;

class Keymap {
public:
    // `error`, when given, says where a refused keymap stopped parsing.
    static std::optional<Keymap> parse(std::string_view text, std::string* error = nullptr);

    // The symbol a key produces under a modifier mask and group (both as
    // the compositor reports them); no_symbol when it produces none.
    std::uint32_t keysym(std::uint32_t keycode, std::uint32_t mods, std::uint32_t group) const;
    // The real-modifier bits a named modifier stands for: a real one
    // ("Shift", "Mod1") or one of this keymap's virtual ones ("Alt",
    // "LevelThree", "NumLock"), resolved; 0 for a name the keymap lacks.
    std::uint32_t modifier_mask(std::string_view name) const;
    std::size_t key_count() const { return m_keys.size(); }
    std::size_t type_count() const { return m_types.size(); }

private:
    struct Type {
        std::string name;
        std::uint32_t mask = 0; // the modifiers this type consults (real, once resolved)
        std::vector<std::pair<std::uint32_t, std::uint32_t>> map; // active mods -> level (0-based)
    };
    struct Group {
        std::vector<std::uint32_t> keysyms; // one per level
        int type = -1; // index into m_types; -1 = choose from the symbols
    };
    struct Key {
        std::vector<Group> groups;
        std::uint32_t modmap = 0; // the real modifiers modifier_map gives it
        std::optional<std::uint32_t> explicit_vmodmap; // virtualMods= in its definition
    };
    struct Interpret {
        std::uint32_t keysym = no_symbol;
        bool any_keysym = false; // "Any": matches every symbol
        bool known = true; // a symbol our vocabulary lacks matches nothing
        enum class Match { NoneOf, AnyOfOrNone, AnyOf, AllOf, Exactly } match = Match::AnyOfOrNone;
        std::uint32_t mods = real_modifiers_mask;
        bool level_one_only = false;
        int virtual_modifier = -1; // index into m_virtual_modifiers
    };
    struct VirtualModifier {
        std::string name;
        std::uint32_t mapping = 0; // the real modifiers it resolves to
    };

    class Parser;
    int automatic_type(Group const& group) const;
    int type_index(std::string_view name) const;
    // A modifier list's mask while parsing: real bits, and virtual
    // modifiers as bits from 8 by declaration order, resolved at the end.
    std::uint32_t named_mask(std::string_view name) const;
    std::uint32_t effective_mask(std::uint32_t named) const;
    void resolve_virtual_modifiers();

    std::unordered_map<std::uint32_t, Key> m_keys;
    std::vector<Type> m_types;
    std::vector<Interpret> m_interprets;
    std::vector<VirtualModifier> m_virtual_modifiers;
};

// The keysym a keymap names, "a", "Cyrillic_a", "U20AC" or "0x1000020ac";
// nullopt for a name the X keysym vocabulary lacks.
std::optional<std::uint32_t> keysym_from_name(std::string_view name);
// The character a keysym types; 0 when it types none (a function key, a
// modifier, a dead key).
char32_t keysym_code_point(std::uint32_t keysym);
// Shift, Control, Alt, Super, Caps Lock, Num Lock, the level shifts: keys
// that type nothing and never repeat.
bool keysym_is_modifier(std::uint32_t keysym);

}
