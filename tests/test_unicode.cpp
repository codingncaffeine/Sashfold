#include "Test.h"

#include "core/Unicode.h"
#include "net/Idna.h"

using namespace sashfold;

int main()
{
    // NFC composition. Escapes throughout: a decomposed literal and its
    // composed form look identical in an editor, and a test whose two sides
    // hold the same bytes would pass vacuously.
    CHECK(nfc(U"e\u0301") == U"\u00E9"); // e + acute -> e-acute
    CHECK(nfc(U"A\u0308") == U"\u00C4"); // A + diaeresis -> A-diaeresis
    CHECK(nfc(U"abc") == U"abc");
    CHECK(nfc(U"\u00E9") == U"\u00E9"); // already composed stays

    // Canonical ordering: dot below (ccc 220) sorts before dot above (230);
    // neither composes with q.
    CHECK(nfc(U"q\u0307\u0323") == U"q\u0323\u0307");

    // The dot below composes with e; the acute then finds no composite with
    // the new starter and appends.
    CHECK(nfc(U"e\u0323\u0301") == U"\u1EB9\u0301");

    // a + diaeresis compose; the acute has no composite with a-diaeresis.
    CHECK(nfc(U"a\u0308\u0301") == U"\u00E4\u0301");

    // Singleton decompositions never recompose to themselves: ANGSTROM SIGN
    // decomposes through A + ring and composes to LATIN CAPITAL A WITH RING.
    CHECK(nfc(U"\u212B") == U"\u00C5");

    // Hangul, algorithmic in both directions (PHIEUPH + WI + RIEUL-HIEUH).
    CHECK(nfc(U"\u1111\u1171\u11B6") == U"\uD4DB"); // L+V+T -> syllable
    CHECK(nfc(U"\uD4DB") == U"\uD4DB"); // syllable round-trips
    CHECK(nfc(U"\u1111\u1171") == U"\uD4CC"); // L+V -> LV syllable
    CHECK(nfc(U"\uD4CC\u11B6") == U"\uD4DB"); // LV + T -> LVT

    CHECK(is_combining_mark(0x0301));
    CHECK(is_combining_mark(0x0BBE)); // Mc: TAMIL VOWEL SIGN AA
    CHECK(!is_combining_mark(U'a'));
    CHECK(!is_combining_mark(0x00E9));

    // Punycode decode: the RFC 3492 sample town, round trips, refusals.
    {
        auto const munich = net::punycode_decode("mnchen-3ya");
        CHECK(munich && *munich == U"m\u00FCnchen");
        auto const encoded = net::punycode_encode(U"m\u00FCnchen");
        CHECK(encoded && *encoded == "mnchen-3ya");
        CHECK(!net::punycode_decode("99999999").has_value()); // overflow
        CHECK(!net::punycode_decode("ab#c-x").has_value()); // bad digit
    }

    // UTS #46 domain-to-ASCII over the generated table.
    {
        auto const ascii = net::domain_to_ascii(to_utf8(U"B\u00FCcher.de"));
        CHECK(ascii && *ascii == "xn--bcher-kva.de");
    }
    {
        // Fullwidth compatibility characters map to their ASCII forms.
        auto const ascii = net::domain_to_ascii(to_utf8(U"\uFF27\uFF4F.com"));
        CHECK(ascii && *ascii == "go.com");
    }
    {
        // Soft hyphen is ignored; ideographic full stop maps to a dot.
        auto const ascii = net::domain_to_ascii(to_utf8(U"a\u00ADb\u3002example"));
        CHECK(ascii && *ascii == "ab.example");
    }
    {
        // Mapping (E-acute -> e-acute) happens before NFC and Punycode.
        auto const ascii = net::domain_to_ascii(to_utf8(U"\u00C9.fr"));
        CHECK(ascii && *ascii == "xn--9ca.fr");
    }
    {
        // Disallowed code points fail the whole domain.
        CHECK(!net::domain_to_ascii(to_utf8(U"a\uFFFDb")).has_value());
        CHECK(!net::domain_to_ascii(to_utf8(U"\uFDD0zyx.com")).has_value());
    }
    {
        // The WHATWG ASCII fast path: all-ASCII domains are lowercased and
        // never see UTS #46 — even xn-- labels whose decoded form would fail.
        auto const lowered = net::domain_to_ascii("EXAMPLE.CoM");
        CHECK(lowered && *lowered == "example.com");
        auto const kept = net::domain_to_ascii("XN--a.ru");
        CHECK(kept && *kept == "xn--a.ru");
    }
    {
        // In a non-ASCII domain, an ASCII xn-- label must decode and its
        // decoded form must validate.
        auto const good = net::domain_to_ascii(to_utf8(U"ü.xn--bcher-kva"));
        CHECK(good && *good == "xn--tda.xn--bcher-kva");
        CHECK(!net::domain_to_ascii(to_utf8(U"ü.xn--a")).has_value());
    }

    {
        // The simple case mappings, packed into runs by distance and step.
        // Escapes throughout, as above: a case pair is exactly the kind of
        // thing that looks right in an editor and is not.
        // ASCII is one run of stride 1.
        CHECK(to_uppercase(U'a') == U'A' && to_lowercase(U'Z') == U'z');
        CHECK(to_uppercase(U'à') == U'À'); // a-grave
        CHECK(to_lowercase(U'Æ') == U'æ'); // AE ligature
        // The mapping is not always to a near neighbour, or even to the same
        // script: the micro sign becomes a Greek capital mu, and y-diaeresis
        // jumps out of Latin-1 entirely.
        CHECK(to_uppercase(U'µ') == U'Μ');
        CHECK(to_uppercase(U'ÿ') == U'Ÿ');
        // Latin Extended-A alternates upper and lower, which is what a run
        // of stride two is for.
        CHECK(to_uppercase(U'ĳ') == U'Ĳ'); // ij ligature
        CHECK(to_lowercase(U'Ă') == U'ă'); // A-breve
        CHECK(to_uppercase(U'п') == U'П'); // Cyrillic pe
        CHECK(to_lowercase(U'Ω') == U'ω'); // Greek omega
        CHECK(to_uppercase(U'ա') == U'Ա'); // Armenian ayb
        // SIMPLE is the word that matters: sharp s does not become SS here.
        CHECK(to_uppercase(U'ß') == U'ß');
        // A code point with no mapping of that kind comes back as itself.
        CHECK(to_uppercase(U'5') == U'5' && to_lowercase(U'-') == U'-');
        CHECK(to_uppercase(U'漢') == U'漢'); // an uncased script is left alone
        // Titlecase is a mapping of its own, not uppercase: the digraphs with
        // a distinct title form are the reason field 14 is read at all.
        CHECK(to_titlecase(U'ǆ') == U'ǅ' && to_uppercase(U'ǆ') == U'Ǆ');
        CHECK(to_titlecase(U'a') == U'A'); // everything else titlecases as it uppercases
    }

    // --- The strong directions, and the first of them --------------------------
    {
        CHECK(strong_direction(U'a') == StrongDirection::Ltr);
        CHECK(strong_direction(U'漢') == StrongDirection::Ltr); // Han is left-to-right
        CHECK(strong_direction(U'א') == StrongDirection::Rtl); // Hebrew alef, class R
        CHECK(strong_direction(U'ا') == StrongDirection::Rtl); // Arabic alef, class AL
        // An UNASSIGNED code point inside a block reserved for a right-to-left
        // script is R, not the L everything else defaults to — which is why
        // the table comes from DerivedBidiClass and not UnicodeData field 4.
        CHECK(strong_direction(U'֐') == StrongDirection::Rtl);
        // Neither: the digits, the punctuation, the spaces, the marks.
        CHECK(strong_direction(U'5') == StrongDirection::None);
        CHECK(strong_direction(U'.') == StrongDirection::None);
        CHECK(strong_direction(U' ') == StrongDirection::None);
        CHECK(strong_direction(U'́') == StrongDirection::None); // combining acute

        CHECK(!first_strong_is_rtl(U"alpha"));
        CHECK(first_strong_is_rtl(U"אבג"));
        CHECK(!first_strong_is_rtl(U"123 — alpha")); // it steps over what is not strong
        CHECK(first_strong_is_rtl(U"123 — אב"));
        CHECK(!first_strong_is_rtl(U"")); // P3: nothing strong reads left-to-right
        CHECK(!first_strong_is_rtl(U"12.34"));
        // P2 steps over everything inside an isolate, so the Hebrew here does
        // not answer for the text around it. U+2066 is the left-to-right
        // isolate, U+2067 the right-to-left one, U+2069 the terminator.
        // Escaped, not written out: an unpaired control in a literal is a
        // warning in its own right, and it would reorder this source too.
        CHECK(!first_strong_is_rtl(U"\u2066א\u2069alpha"));
        CHECK(first_strong_is_rtl(U"\u2066alpha\u2069א"));
        // An isolate with no terminator swallows the rest of the paragraph.
        CHECK(!first_strong_is_rtl(U"\u2067א"));
        // An unmatched PDI ends nothing, and is itself not strong.
        CHECK(first_strong_is_rtl(U"\u2069א"));
    }

    return sashfold::test::report("unicode");
}