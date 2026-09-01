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

    return sashfold::test::report("unicode");
}