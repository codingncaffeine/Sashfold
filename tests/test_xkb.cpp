// The xkb keymap parser against keymaps libxkbcommon compiled (xkbcli
// compile-keymap --layout us / de / us,ru with grp:alt_shift_toggle), with
// every expectation taken from libxkbcommon's own answers for the same key,
// modifier mask and group: the compositor sends real-modifier bits only,
// so AltGr arrives as Mod5 and Num Lock as Mod2, and the parser has to
// resolve the keymap's virtual modifiers to those.
#include "Test.h"

#include "platform/linux/Xkb.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <string>

using namespace sashfold;
using namespace sashfold::platform;

namespace {

std::string slurp(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// evdev key numbers plus 8, as the compositor sends them
constexpr std::uint32_t key_ae02 = 11;
constexpr std::uint32_t key_tab = 23;
constexpr std::uint32_t key_ad01 = 24;
constexpr std::uint32_t key_return = 36;
constexpr std::uint32_t key_ac10 = 47;
constexpr std::uint32_t key_tilde = 49;
constexpr std::uint32_t key_space = 65;
constexpr std::uint32_t key_f5 = 71;
constexpr std::uint32_t key_kp7 = 79;
constexpr std::uint32_t key_kp_enter = 104;
constexpr std::uint32_t key_left = 113;

constexpr std::uint32_t keysym_return = 0xff0d;
constexpr std::uint32_t keysym_kp_enter = 0xff8d;
constexpr std::uint32_t keysym_left = 0xff51;
constexpr std::uint32_t keysym_f5 = 0xffc2;
constexpr std::uint32_t keysym_iso_left_tab = 0xfe20;
constexpr std::uint32_t keysym_tab = 0xff09;
constexpr std::uint32_t keysym_kp_7 = 0xffb7;
constexpr std::uint32_t keysym_kp_home = 0xff95;
constexpr std::uint32_t keysym_dead_circumflex = 0xfe52;
constexpr std::uint32_t keysym_cyrillic_shorti = 0x06ca;
constexpr std::uint32_t keysym_cyrillic_SHORTI = 0x06ea;

void test_vocabulary()
{
    CHECK_EQ(xkb::keysym_from_name("a").value_or(1), 0x61u);
    CHECK_EQ(xkb::keysym_from_name("Cyrillic_a").value_or(1), 0x6c1u);
    CHECK_EQ(xkb::keysym_from_name("Return").value_or(1), keysym_return);
    CHECK_EQ(xkb::keysym_from_name("U20AC").value_or(1), 0x010020acu); // 0x01000000 | the code point
    CHECK_EQ(xkb::keysym_from_name("U0041").value_or(1), 0x41u); // Latin-1 keysyms are their own code points
    CHECK_EQ(xkb::keysym_from_name("0x010020ac").value_or(1), 0x010020acu);
    CHECK_EQ(xkb::keysym_from_name("NoSymbol").value_or(1), 0u);
    CHECK(!xkb::keysym_from_name("XF86NotAKeysymWeKnow").has_value());
    CHECK(!xkb::keysym_from_name("").has_value());

    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(0x61)), 0x61u);
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(0xe9)), 0xe9u); // eacute
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(0x6c1)), 0x430u); // Cyrillic_a
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(0x20ac)), 0x20acu); // EuroSign
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(0x010020ac)), 0x20acu);
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(keysym_kp_7)), static_cast<std::uint32_t>('7'));
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(keysym_return)), 0u); // a function key types nothing
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(keysym_dead_circumflex)), 0u);
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(0xffe1)), 0u); // Shift_L

    CHECK(xkb::keysym_is_modifier(0xffe1)); // Shift_L
    CHECK(xkb::keysym_is_modifier(0xffe5)); // Caps_Lock
    CHECK(xkb::keysym_is_modifier(0xfe03)); // ISO_Level3_Shift
    CHECK(xkb::keysym_is_modifier(0xff7f)); // Num_Lock
    CHECK(!xkb::keysym_is_modifier(0x61));
    CHECK(!xkb::keysym_is_modifier(keysym_return));
}

void test_us(std::string const& directory)
{
    std::optional<xkb::Keymap> const keymap = xkb::Keymap::parse(slurp(directory + "/us.xkb"));
    CHECK(keymap.has_value());
    if (!keymap)
        return;
    CHECK(keymap->key_count() > 100);
    CHECK(keymap->type_count() > 10);

    CHECK_EQ(keymap->keysym(key_ad01, 0, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::shift_mask, 0), static_cast<std::uint32_t>('Q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::lock_mask, 0), static_cast<std::uint32_t>('Q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::shift_mask | xkb::lock_mask, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::control_mask, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::mod1_mask, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ae02, 0, 0), static_cast<std::uint32_t>('2'));
    CHECK_EQ(keymap->keysym(key_ae02, xkb::shift_mask, 0), static_cast<std::uint32_t>('@'));
    CHECK_EQ(keymap->keysym(key_ae02, xkb::lock_mask, 0), static_cast<std::uint32_t>('2')); // Caps Lock is for letters
    CHECK_EQ(keymap->keysym(key_return, 0, 0), keysym_return);
    CHECK_EQ(keymap->keysym(key_kp_enter, 0, 0), keysym_kp_enter);
    CHECK_EQ(keymap->keysym(key_left, 0, 0), keysym_left);
    CHECK_EQ(keymap->keysym(key_f5, 0, 0), keysym_f5);
    CHECK_EQ(keymap->keysym(key_space, 0, 0), static_cast<std::uint32_t>(' '));
    CHECK_EQ(keymap->keysym(key_tab, 0, 0), keysym_tab);
    CHECK_EQ(keymap->keysym(key_tab, xkb::shift_mask, 0), keysym_iso_left_tab);
    // The keypad follows Num Lock, which the compositor reports as Mod2;
    // this keymap's KEYPAD type consults nothing else, so Shift changes
    // nothing (libxkbcommon says KP_7 too).
    CHECK_EQ(keymap->keysym(key_kp7, 0, 0), keysym_kp_home);
    CHECK_EQ(keymap->keysym(key_kp7, xkb::mod2_mask, 0), keysym_kp_7);
    CHECK_EQ(keymap->keysym(key_kp7, xkb::mod2_mask | xkb::shift_mask, 0), keysym_kp_7);
    CHECK_EQ(keymap->keysym(key_kp7, xkb::shift_mask, 0), keysym_kp_home);
    // A key the map lacks, and a group past the end (groups wrap).
    CHECK_EQ(keymap->keysym(9999, 0, 0), xkb::no_symbol);
    CHECK_EQ(keymap->keysym(key_ad01, 0, 5), static_cast<std::uint32_t>('q'));

    // The virtual modifiers resolved to the real bits the compositor sends.
    CHECK_EQ(keymap->modifier_mask("Shift"), xkb::shift_mask);
    CHECK_EQ(keymap->modifier_mask("Alt"), xkb::mod1_mask);
    CHECK_EQ(keymap->modifier_mask("NumLock"), xkb::mod2_mask);
    CHECK_EQ(keymap->modifier_mask("Super"), xkb::mod4_mask);
    CHECK_EQ(keymap->modifier_mask("LevelThree"), xkb::mod5_mask);
    CHECK_EQ(keymap->modifier_mask("NotAModifier"), 0u);
}

void test_de(std::string const& directory)
{
    std::optional<xkb::Keymap> const keymap = xkb::Keymap::parse(slurp(directory + "/de.xkb"));
    CHECK(keymap.has_value());
    if (!keymap)
        return;
    // AltGr is the third level; libxkbcommon reports it as Mod5.
    CHECK_EQ(keymap->keysym(key_ad01, 0, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::mod5_mask, 0), static_cast<std::uint32_t>('@'));
    CHECK_EQ(keymap->keysym(key_ae02, xkb::mod5_mask, 0), 0xb2u); // twosuperior
    CHECK_EQ(keymap->keysym(key_ae02, xkb::mod5_mask | xkb::shift_mask, 0), 0x0ac3u); // oneeighth, a named keysym
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(keymap->keysym(key_ae02, xkb::mod5_mask | xkb::shift_mask, 0))), 0x215bu);
    // An umlaut key is alphabetic: Caps Lock uppercases it.
    CHECK_EQ(keymap->keysym(key_ac10, 0, 0), 0xf6u); // odiaeresis
    CHECK_EQ(keymap->keysym(key_ac10, xkb::shift_mask, 0), 0xd6u);
    CHECK_EQ(keymap->keysym(key_ac10, xkb::lock_mask, 0), 0xd6u);
    // A dead key types nothing; its AltGr level is a U+ keysym.
    CHECK_EQ(keymap->keysym(key_tilde, 0, 0), keysym_dead_circumflex);
    CHECK_EQ(keymap->keysym(key_tilde, xkb::mod5_mask, 0), 0x1002032u);
    CHECK_EQ(keymap->modifier_mask("LevelThree"), xkb::mod5_mask);
}

void test_us_ru(std::string const& directory)
{
    std::optional<xkb::Keymap> const keymap = xkb::Keymap::parse(slurp(directory + "/us-ru.xkb"));
    CHECK(keymap.has_value());
    if (!keymap)
        return;
    CHECK_EQ(keymap->keysym(key_ad01, 0, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ad01, 0, 1), keysym_cyrillic_shorti);
    CHECK_EQ(keymap->keysym(key_ad01, xkb::shift_mask, 1), keysym_cyrillic_SHORTI);
    CHECK_EQ(keymap->keysym(key_ad01, xkb::lock_mask, 1), keysym_cyrillic_SHORTI);
    CHECK_EQ(static_cast<std::uint32_t>(xkb::keysym_code_point(keysym_cyrillic_shorti)), 0x439u);
    CHECK_EQ(keymap->keysym(key_ad01, 0, 2), static_cast<std::uint32_t>('q')); // two groups: the third wraps
    CHECK_EQ(keymap->keysym(key_return, 0, 1), keysym_return); // one group: every group is it
}

// The keymap KWin (Plasma 6) hands a client: types it does not use are
// pruned, and the compatibility section spells keysyms as hex numbers.
void test_kwin(std::string const& directory)
{
    std::string error;
    std::optional<xkb::Keymap> const keymap = xkb::Keymap::parse(slurp(directory + "/kwin-us.xkb"), &error);
    CHECK(keymap.has_value());
    CHECK_EQ(error, std::string());
    if (!keymap)
        return;
    CHECK_EQ(keymap->keysym(key_ad01, 0, 0), static_cast<std::uint32_t>('q'));
    CHECK_EQ(keymap->keysym(key_ad01, xkb::shift_mask, 0), static_cast<std::uint32_t>('Q'));
    CHECK_EQ(keymap->keysym(key_kp7, xkb::mod2_mask, 0), keysym_kp_7);
    CHECK_EQ(keymap->keysym(key_kp_enter, 0, 0), keysym_kp_enter);
    CHECK_EQ(keymap->modifier_mask("Alt"), xkb::mod1_mask);
    CHECK_EQ(keymap->modifier_mask("NumLock"), xkb::mod2_mask); // the hex-spelled interpret resolved it
    CHECK_EQ(keymap->modifier_mask("LevelThree"), xkb::mod5_mask);
}

void test_malformed()
{
    std::string error;
    CHECK(!xkb::Keymap::parse("xkb_keymap { xkb_symbols { key <A> { [ a ] }; }; };", &error).has_value());
    CHECK(error.find("no keys") != std::string::npos); // <A> names no keycode
    CHECK(!xkb::Keymap::parse("").has_value());
    CHECK(!xkb::Keymap::parse("garbage {").has_value());
    CHECK(!xkb::Keymap::parse("xkb_keymap { xkb_keycodes { <A> = 9; }; };").has_value()); // no symbols
    // A keymap with no types at all still shifts.
    std::optional<xkb::Keymap> const bare = xkb::Keymap::parse(
        "xkb_keymap {\n"
        "xkb_keycodes \"x\" { <A> = 38; alias <B> = <A>; };\n"
        "xkb_symbols \"x\" { key <B> { [ a, A ] }; };\n"
        "};\n");
    CHECK(bare.has_value());
    if (bare) {
        CHECK_EQ(bare->keysym(38, 0, 0), static_cast<std::uint32_t>('a'));
        CHECK_EQ(bare->keysym(38, xkb::shift_mask, 0), static_cast<std::uint32_t>('A'));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_xkb <fixtures/xkb>\n");
        return 2;
    }
    std::string const directory = argv[1];
    test_vocabulary();
    test_us(directory);
    test_de(directory);
    test_us_ru(directory);
    test_kwin(directory);
    test_malformed();
    return test::report("test_xkb");
}
