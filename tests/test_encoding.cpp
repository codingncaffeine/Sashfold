#include "Test.h"

#include "html/Encoding.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"

#include <string>
#include <string_view>

using namespace sashfold;
using html::Encoding;

namespace {

std::string sniff_name(std::string_view bytes)
{
    switch (html::sniff_encoding(bytes).encoding) {
    case Encoding::Utf8: return "utf-8";
    case Encoding::Utf16Le: return "utf-16le";
    case Encoding::Utf16Be: return "utf-16be";
    case Encoding::Windows1252: return "windows-1252";
    case Encoding::XUserDefined: return "x-user-defined";
    }
    return "?";
}

} // namespace

int main()
{
    // --- BOM sniffing outranks everything ------------------------------------
    CHECK_EQ(sniff_name("\xEF\xBB\xBFhello"), "utf-8");
    CHECK_EQ(html::sniff_encoding("\xEF\xBB\xBFhello").bom_length, std::size_t { 3 });
    CHECK_EQ(sniff_name(std::string_view("\xFE\xFF\x00h", 4)), "utf-16be");
    CHECK_EQ(sniff_name(std::string_view("\xFF\xFEh\x00", 4)), "utf-16le");
    CHECK_EQ(sniff_name("\xEF\xBB\xBF<meta charset=windows-1252>"), "utf-8"); // BOM wins over meta

    // --- Label table ---------------------------------------------------------
    CHECK(html::encoding_from_label("UTF-8") == Encoding::Utf8);
    CHECK(html::encoding_from_label("  utf8\t") == Encoding::Utf8);
    CHECK(html::encoding_from_label("Latin1") == Encoding::Windows1252);
    CHECK(html::encoding_from_label("ISO-8859-1") == Encoding::Windows1252);
    CHECK(html::encoding_from_label("us-ascii") == Encoding::Windows1252);
    CHECK(html::encoding_from_label("UTF-16") == Encoding::Utf16Le);
    CHECK(html::encoding_from_label("x-user-defined") == Encoding::XUserDefined);
    CHECK(!html::encoding_from_label("shift_jis").has_value()); // not implemented yet
    CHECK(!html::encoding_from_label("").has_value());

    // --- Meta prescan --------------------------------------------------------
    CHECK_EQ(sniff_name("<html><head><meta charset=\"windows-1252\"></head>"), "windows-1252");
    CHECK_EQ(sniff_name("<meta charset=utf-8>"), "utf-8");
    CHECK_EQ(sniff_name("<META CHARSET=UTF-8>"), "utf-8");
    CHECK_EQ(sniff_name("<meta charset = 'iso-8859-1'>"), "windows-1252");
    CHECK_EQ(sniff_name("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=windows-1252\">"),
        "windows-1252");
    // content= without the http-equiv pragma is inert (both directions).
    CHECK_EQ(sniff_name("<meta content=\"text/html; charset=utf-8\">x\x80"), "windows-1252");
    CHECK_EQ(sniff_name("<meta content=\"text/html; charset=windows-1252\">plain"), "utf-8");
    // A 16-bit family label from meta means UTF-8 (the bytes were ASCII-ish).
    CHECK_EQ(sniff_name("<meta charset=utf-16>\x80"), "utf-8");
    // x-user-defined from meta means windows-1252.
    CHECK_EQ(sniff_name("<meta charset=x-user-defined>"), "windows-1252");
    // Commented-out metas are skipped.
    CHECK_EQ(sniff_name("<!-- <meta charset=windows-1252> --><meta charset=utf-8>"), "utf-8");
    // Unknown label: prescan keeps going, later meta wins.
    CHECK_EQ(sniff_name("<meta charset=shift_jis><meta charset=utf-8>"), "utf-8");
    // A second charset attribute on the same meta is ignored.
    CHECK_EQ(sniff_name("<meta charset=utf-8 charset=windows-1252>"), "utf-8");
    // Only the first 1024 bytes are scanned.
    {
        std::string big(1100, 'a');
        big += "<meta charset=windows-1252>";
        CHECK_EQ(sniff_name(big), "utf-8"); // meta out of range; pure ASCII → utf-8
    }

    // --- Fallbacks -----------------------------------------------------------
    CHECK_EQ(sniff_name("plain ascii"), "utf-8");
    CHECK_EQ(sniff_name("caf\xC3\xA9"), "utf-8"); // valid UTF-8 stays UTF-8
    CHECK_EQ(sniff_name("caf\xE9 au lait"), "windows-1252"); // invalid UTF-8 falls back

    // --- Decoders ------------------------------------------------------------
    CHECK(html::decode("caf\xE9", Encoding::Windows1252) == std::u32string(U"café"));
    CHECK(html::decode("\x80\x99", Encoding::Windows1252) == std::u32string(U"€™"));
    CHECK(html::decode(std::string { '\x8D', '\x9D' }, Encoding::Windows1252)
        == (std::u32string { 0x8D, 0x9D })); // C1 controls pass through
    CHECK(html::decode(std::string_view("h\x00i\x00", 4), Encoding::Utf16Le) == std::u32string(U"hi"));
    CHECK(html::decode(std::string_view("\x00h\x00i", 4), Encoding::Utf16Be) == std::u32string(U"hi"));
    // Surrogate pair: U+1F600 in UTF-16LE (D83D DE00).
    CHECK(html::decode(std::string_view("\x3D\xD8\x00\xDE", 4), Encoding::Utf16Le)
        == std::u32string(U"\U0001F600"));
    // Lone high surrogate and odd trailing byte become U+FFFD.
    CHECK(html::decode(std::string_view("\x3D\xD8", 2), Encoding::Utf16Le) == std::u32string(U"�"));
    CHECK(html::decode(std::string_view("h\x00i", 3), Encoding::Utf16Le) == std::u32string(U"h�"));

    // --- End to end: bytes in, tree out --------------------------------------
    {
        auto document = html::parse_document_bytes("<meta charset=windows-1252><p>caf\xE9");
        std::string const dump = html::dump_document(*document);
        CHECK(dump.find("caf\xC3\xA9") != std::string::npos); // é re-encoded as UTF-8 by the dump
    }
    {
        // UTF-16LE with BOM: "<p>hi"
        std::string bytes = "\xFF\xFE";
        for (char const c : std::string_view("<p>hi")) {
            bytes += c;
            bytes += '\0';
        }
        auto document = html::parse_document_bytes(bytes);
        std::string const dump = html::dump_document(*document);
        CHECK(dump.find("\"hi\"") != std::string::npos);
    }

    return sashfold::test::report("encoding");
}
