#include "Test.h"

#include "core/Inflate.h"
#include "crypto/Md5.h"
#include "pdf/Document.h"
#include "pdf/Filters.h"
#include "pdf/Parser.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// The PDF reader's file layer: MD5 and RC4 by their published vectors, the
// lexer's corner cases, every filter, and files written byte by byte here —
// classic and stream cross-references, object streams, hybrids,
// incremental updates, inherited page attributes, wrong lengths and broken
// offsets that must be rebuilt, loops that must end — and the encrypted
// fixtures written by tools/pdf-make-encrypted.py (RC4-40, AES-128,
// AES-256), which poppler opens the same way.

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytes(std::string const& s)
{
    return Bytes(s.begin(), s.end());
}

std::string str(Bytes const& b)
{
    return std::string(b.begin(), b.end());
}

std::string hex(Bytes const& b)
{
    std::string out;
    char buffer[3];
    for (std::uint8_t const c : b) {
        std::snprintf(buffer, sizeof buffer, "%02x", c);
        out += buffer;
    }
    return out;
}

// A file of numbered objects with a classic table, offsets computed here.
// `trailer_extra` goes into the trailer beside /Size and /Root 1 0 R.
std::string build_pdf(std::map<int, std::string> const& objects, std::string const& trailer_extra = {},
    std::string const& prefix = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n")
{
    std::string out = prefix;
    std::map<int, std::size_t> offsets;
    int max = 0;
    for (auto const& [num, body] : objects) {
        offsets[num] = out.size();
        out += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
        max = std::max(max, num);
    }
    std::size_t const xref = out.size();
    out += "xref\n0 " + std::to_string(max + 1) + "\n0000000000 65535 f \n";
    for (int num = 1; num <= max; ++num) {
        char line[32];
        if (offsets.count(num))
            std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[num]);
        else
            std::snprintf(line, sizeof line, "0000000000 00000 f \n");
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(max + 1) + " /Root 1 0 R " + trailer_extra + " >>\nstartxref\n"
        + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::shared_ptr<pdf::Document> open(std::string const& file, pdf::Document::Status& status, std::string const& password = {})
{
    return pdf::Document::open(bytes(file), password, status);
}

std::map<int, std::string> two_pages()
{
    return {
        { 1, "<< /Type /Catalog /Pages 2 0 R >>" },
        { 2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 600 800] /Rotate 90 /Resources << /Font << /F1 6 0 R >> >> >>" },
        { 3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>" },
        { 4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /CropBox [10 20 290 900] /Rotate -90 /Resources << >> >>" },
        { 5, "<< /Length 11 >>\nstream\nBT (Hi) ET\n\nendstream" },
        { 6, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>" },
    };
}

void test_md5_rc4()
{
    auto const md5 = [](std::string const& s) {
        auto const d = crypto::Md5::hash(bytes(s));
        return hex(Bytes(d.begin(), d.end()));
    };
    // RFC 1321 appendix A.5.
    CHECK_EQ(md5(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(md5("a"), std::string("0cc175b9c0f1b6a831c399e269772661"));
    CHECK_EQ(md5("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(md5("message digest"), std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    CHECK_EQ(md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
        std::string("57edf4a22be3c955ac49da2e2107b67a"));
    // Fed in pieces across the block boundary: the same digest.
    crypto::Md5 pieces;
    std::string const long_text(200, 'x');
    pieces.update(bytes(long_text.substr(0, 63)));
    pieces.update(bytes(long_text.substr(63, 2)));
    pieces.update(bytes(long_text.substr(65)));
    auto const d = pieces.finish();
    CHECK_EQ(hex(Bytes(d.begin(), d.end())), md5(long_text));
    // RC4's well-known vectors.
    CHECK_EQ(hex(crypto::rc4(bytes("Key"), bytes("Plaintext"))), std::string("bbf316e8d940af0ad3"));
    CHECK_EQ(hex(crypto::rc4(bytes("Wiki"), bytes("pedia"))), std::string("1021bf0420"));
    CHECK_EQ(hex(crypto::rc4(bytes("Secret"), bytes("Attack at dawn"))), std::string("45a01f645fc35b383552544b9bf5"));
}

void test_lexer()
{
    std::string const text = "(a\\(b\\)c (nested) \\101\\60\\0013 \\\nx\\r) <48656C6C6F2> /A#20B /#2F -.5 +3 4. --2 % comment\n"
                             "[1 2 R] << /K 7 0 R >> true null endobj";
    Bytes const data = bytes(text);
    pdf::Lexer lexer(std::span<std::uint8_t const>(data.data(), data.size()));
    pdf::Parser parser(lexer);
    pdf::Object const s = parser.parse();
    CHECK(s.is_string());
    CHECK_EQ(s.as_string(), std::string("a(b)c (nested) A0\001" "3 x\r"));
    pdf::Object const h = parser.parse();
    CHECK_EQ(h.as_string(), std::string("Hello ")); // an odd final digit is followed by 0
    CHECK(parser.parse().is_name("A B"));
    CHECK(parser.parse().is_name("/"));
    CHECK_EQ(parser.parse().as_number(), -0.5);
    CHECK_EQ(parser.parse().as_int(), std::int64_t(3));
    pdf::Object const four = parser.parse();
    CHECK(!four.is_int() && four.as_number() == 4.0);
    CHECK_EQ(parser.parse().as_int(), std::int64_t(-2)); // repeated signs, read leniently
    pdf::Object const array = parser.parse();
    CHECK_EQ(array.as_array().size(), std::size_t(1));
    CHECK(array.as_array()[0].is_ref() && array.as_array()[0].as_ref() == (pdf::Ref { 1, 2 }));
    pdf::Object const dict = parser.parse();
    CHECK(dict.as_dict().get("K").as_ref() == (pdf::Ref { 7, 0 }));
    CHECK(parser.parse().as_bool());
    CHECK(parser.parse().is_null());
    std::string keyword;
    parser.parse(&keyword);
    CHECK_EQ(keyword, std::string("endobj"));
    // Content streams have no references: "1 2 R" there is two numbers and an operator.
    Bytes const content = bytes("1 2 R");
    pdf::Lexer content_lexer(std::span<std::uint8_t const>(content.data(), content.size()));
    pdf::Parser content_parser(content_lexer, false);
    CHECK(content_parser.parse().is_int());
    CHECK(content_parser.parse().is_int());
    keyword.clear();
    content_parser.parse(&keyword);
    CHECK_EQ(keyword, std::string("R"));
    // Unterminated things end at the end of the data, and a deep nest is cut, not recursed into.
    Bytes const broken = bytes("[1 [2 (unterminated");
    pdf::Lexer broken_lexer(std::span<std::uint8_t const>(broken.data(), broken.size()));
    pdf::Parser broken_parser(broken_lexer);
    CHECK_EQ(broken_parser.parse().as_array().size(), std::size_t(2));
    Bytes const deep = bytes(std::string(100000, '['));
    pdf::Lexer deep_lexer(std::span<std::uint8_t const>(deep.data(), deep.size()));
    pdf::Parser deep_parser(deep_lexer);
    CHECK(deep_parser.parse().is_array());
    // Text strings: UTF-16BE with its mark, PDFDocEncoding's own characters.
    CHECK_EQ(pdf::text_string_to_utf8(std::string("\xFE\xFF\x00h\x00\xE9\xD8\x3D\xDE\x00", 10)), std::string("h\xC3\xA9\xF0\x9F\x98\x80"));
    // Annex D: 0x8D/0x8E are the curly quotes, 0x93 the fi ligature, 0xA0 the euro.
    CHECK_EQ(pdf::text_string_to_utf8("\x8Dquoted\x8E \x93 \xA0"), std::string("\xE2\x80\x9Cquoted\xE2\x80\x9D \xEF\xAC\x81 \xE2\x82\xAC"));
}

void test_filters()
{
    pdf::Dict none;
    CHECK_EQ(str(pdf::apply_filter("ASCIIHexDecode", bytes("48 65 6c\n6C 6f>ignored"), none)), std::string("Hello"));
    CHECK_EQ(str(pdf::apply_filter("AHx", bytes("414>"), none)), std::string("A@"));
    CHECK_EQ(str(pdf::apply_filter("ASCII85Decode", bytes("87cURD]j7BEbo8;+AbHq+T~>"), none)), std::string("Hello world, PDF!"));
    CHECK_EQ(pdf::apply_filter("A85", bytes("<~z@:E^~>"), none), (Bytes { 0, 0, 0, 0, 'a', 'b', 'c' }));
    CHECK_EQ(str(pdf::apply_filter("RunLengthDecode", Bytes { 2, 'a', 'b', 'c', 254, 'x', 128, 'z' }, none)), std::string("abcxxx"));
    // ISO 32000's own LZW example: "-----A---B".
    CHECK_EQ(str(pdf::apply_filter("LZWDecode", Bytes { 0x80, 0x0B, 0x60, 0x50, 0x22, 0x0C, 0x0C, 0x85, 0x01 }, none)),
        std::string("-----A---B"));
    // Flate with a PNG predictor (Up and Sub rows), the way cross-reference streams are written.
    Bytes const rows = { 2, 1, 2, 3, 2, 1, 1, 1, 0, 9, 9, 9, 1, 1, 1, 1 }; // Up, Up, None, Sub
    pdf::Dict params;
    params.set("Predictor", pdf::Object::integer(12));
    params.set("Columns", pdf::Object::integer(3));
    CHECK_EQ(pdf::apply_filter("FlateDecode", zlib_compress(rows), params), (Bytes { 1, 2, 3, 2, 3, 4, 9, 9, 9, 1, 2, 3 }));
    // A Flate stream cut short gives what came before the cut; no zlib header is needed.
    Bytes const long_text = bytes(std::string(5000, 'q') + "tail");
    Bytes compressed = zlib_compress(long_text);
    CHECK_EQ(pdf::apply_filter("Fl", compressed, none), long_text);
    compressed.resize(compressed.size() - 4); // the checksum gone
    CHECK_EQ(pdf::apply_filter("Fl", compressed, none), long_text);
    // TIFF predictor 2 over RGB.
    pdf::Dict tiff;
    tiff.set("Predictor", pdf::Object::integer(2));
    tiff.set("Colors", pdf::Object::integer(3));
    tiff.set("Columns", pdf::Object::integer(2));
    CHECK_EQ(pdf::unpredict(Bytes { 10, 20, 30, 1, 2, 3 }, tiff), (Bytes { 10, 20, 30, 11, 22, 33 }));
    CHECK(pdf::is_image_filter("DCTDecode") && pdf::is_image_filter("CCF") && !pdf::is_image_filter("FlateDecode"));
    CHECK(pdf::apply_filter("NoSuchFilter", bytes("x"), none).empty());
}

void test_classic_file()
{
    pdf::Document::Status status {};
    auto const doc = open(build_pdf(two_pages(), "/Info << /Title (Two \\(pages\\)) >>"), status);
    CHECK(doc && status == pdf::Document::Status::Ok);
    if (!doc)
        return;
    CHECK(!doc->repaired());
    CHECK_EQ(doc->page_count(), std::size_t(2));
    CHECK_EQ(doc->title(), std::string("Two (pages)"));
    // Inherited media box, rotation and resources; a crop box clipped to the media box.
    CHECK(doc->page(0).media_box == (pdf::Box { 0, 0, 600, 800 }));
    CHECK(doc->page(0).crop_box == (pdf::Box { 0, 0, 600, 800 }));
    CHECK_EQ(doc->page(0).rotate, 90);
    CHECK(doc->resolve(doc->page(0).resources).as_dict().has("Font"));
    CHECK(doc->page(1).media_box == (pdf::Box { 0, 0, 300, 400 }));
    CHECK(doc->page(1).crop_box == (pdf::Box { 10, 20, 290, 400 }));
    CHECK_EQ(doc->page(1).rotate, 270);
    CHECK(!doc->resolve(doc->page(1).resources).as_dict().has("Font"));
    // The content stream, by its /Length.
    pdf::Object const contents = doc->resolve(doc->page(0).dict->get("Contents"));
    CHECK(contents.is_stream());
    CHECK_EQ(str(doc->decode(*contents.as_stream())), std::string("BT (Hi) ET\n"));
    CHECK(doc->page_index(pdf::Ref { 4, 0 }) == std::optional<std::size_t>(1));
    CHECK(doc->fetch(pdf::Ref { 99, 0 }).is_null());
}

void test_damaged_files()
{
    // A wrong /Length: the stream runs to its endstream.
    std::map<int, std::string> objects = two_pages();
    objects[5] = "<< /Length 400 >>\nstream\nBT (Hi) ET\nendstream";
    pdf::Document::Status status {};
    auto doc = open(build_pdf(objects), status);
    CHECK(doc && status == pdf::Document::Status::Ok);
    if (doc) {
        pdf::Object const contents = doc->resolve(doc->page(0).dict->get("Contents"));
        CHECK_EQ(str(doc->decode(*contents.as_stream())), std::string("BT (Hi) ET"));
    }
    // A /Length that names its own stream ends instead of looping; an indirect one is followed.
    objects[5] = "<< /Length 5 0 R >>\nstream\nBT (Hi) ET\nendstream";
    doc = open(build_pdf(objects), status);
    CHECK(doc && doc->page_count() == 2);
    objects[5] = "<< /Length 7 0 R >>\nstream\nBT (Hi) ET\nendstream";
    objects[7] = "10";
    doc = open(build_pdf(objects), status);
    if (doc) {
        pdf::Object const contents = doc->resolve(doc->page(0).dict->get("Contents"));
        CHECK_EQ(str(doc->decode(*contents.as_stream())), std::string("BT (Hi) ET"));
    }
    // Bytes before the header and offsets that miss: rebuilt by the scan.
    std::string const good = build_pdf(two_pages());
    doc = open("garbage before the header\n" + good, status);
    CHECK(doc && doc->page_count() == 2);
    std::string shifted = good;
    shifted.insert(20, "% forty bytes of padding that move it\n");
    doc = open(shifted, status);
    CHECK(doc && doc->page_count() == 2 && doc->repaired());
    // No cross-reference data at all.
    std::string no_xref = good.substr(0, good.find("xref"));
    doc = open(no_xref, status);
    CHECK(doc && doc->page_count() == 2 && doc->repaired());
    // No catalog left: the pages the file still holds.
    std::map<int, std::string> orphan = two_pages();
    orphan.erase(1);
    doc = open(build_pdf(orphan).substr(0, build_pdf(orphan).find("xref")), status);
    CHECK(doc && doc->page_count() == 2);
    // A table that starts at 1 where it meant 0 (a writer bug readers allow for).
    std::string off_by_one = good;
    std::size_t const table = off_by_one.find("xref\n0 ");
    off_by_one.replace(table, 7, "xref\n1 ");
    doc = open(off_by_one, status);
    CHECK(doc && doc->page_count() == 2 && !doc->repaired());
    // A page tree that loops ends.
    std::map<int, std::string> loop = two_pages();
    loop[2] = "<< /Type /Pages /Kids [2 0 R 3 0 R] /Count 2 >>";
    doc = open(build_pdf(loop), status);
    CHECK(doc && doc->page_count() == 1);
    // Not a PDF, and every cut of a good one: no crash, an answer each time.
    doc = open("<html>not a pdf</html>", status);
    CHECK(!doc && status == pdf::Document::Status::NotPdf);
    int opened = 0;
    for (std::size_t cut = 0; cut < good.size(); cut += 7) {
        auto const partial = open(good.substr(0, cut), status);
        if (partial && partial->page_count() > 0)
            ++opened;
    }
    CHECK(opened > 0);
}

void test_streams_and_updates()
{
    // Objects 3-6 in an object stream; a cross-reference stream with a PNG predictor.
    std::string const objstm_body = "3 0 4 52 "; // two objects: offsets relative to /First
    std::string const obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 200] >>";
    std::string obj4 = "<< /Type /Page /Parent 2 0 R /Contents 7 0 R >>";
    std::string header = "3 0 4 " + std::to_string(obj3.size() + 1) + " ";
    std::string const objstm_data = header + obj3 + " " + obj4;
    Bytes const objstm_compressed = zlib_compress(bytes(objstm_data));

    std::string out = "%PDF-1.5\n";
    std::map<int, std::size_t> offsets;
    auto const add = [&](int num, std::string const& body) {
        offsets[num] = out.size();
        out += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
    };
    add(1, "<< /Type /Catalog /Pages 2 0 R >>");
    add(2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    add(5, "<< /Type /ObjStm /N 2 /First " + std::to_string(header.size()) + " /Filter /FlateDecode /Length "
            + std::to_string(objstm_compressed.size()) + " >>\nstream\n" + str(objstm_compressed) + "\nendstream");
    add(7, "<< /Length 4 >>\nstream\nq Q\n\nendstream");
    // Rows of W [1 2 1]: type, field 2, field 3.
    std::vector<std::array<int, 3>> const entries = {
        { 0, 0, 255 }, { 1, static_cast<int>(offsets[1]), 0 }, { 1, static_cast<int>(offsets[2]), 0 },
        { 2, 5, 0 }, { 2, 5, 1 }, { 1, static_cast<int>(offsets[5]), 0 }, { 0, 0, 0 },
        { 1, static_cast<int>(offsets[7]), 0 }, { 1, 0, 0 },
    };
    std::size_t const xref_offset = out.size();
    std::vector<std::array<int, 3>> rows = entries;
    rows[8] = { 1, static_cast<int>(xref_offset), 0 };
    Bytes raw;
    Bytes previous(4, 0);
    for (auto const& row : rows) {
        Bytes const line = { static_cast<std::uint8_t>(row[0]), static_cast<std::uint8_t>(row[1] >> 8),
            static_cast<std::uint8_t>(row[1]), static_cast<std::uint8_t>(row[2]) };
        raw.push_back(2); // PNG Up
        for (std::size_t i = 0; i < 4; ++i)
            raw.push_back(static_cast<std::uint8_t>(line[i] - previous[i]));
        previous = line;
    }
    Bytes const xref_data = zlib_compress(raw);
    out += "8 0 obj\n<< /Type /XRef /Size 9 /W [1 2 1] /Root 1 0 R /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 4 >> /Length "
        + std::to_string(xref_data.size()) + " >>\nstream\n" + str(xref_data) + "\nendstream\nendobj\nstartxref\n"
        + std::to_string(xref_offset) + "\n%%EOF\n";
    pdf::Document::Status status {};
    auto doc = open(out, status);
    CHECK(doc && status == pdf::Document::Status::Ok);
    if (doc) {
        CHECK(!doc->repaired());
        CHECK_EQ(doc->page_count(), std::size_t(2));
        if (doc->page_count() == 2) {
            CHECK(doc->page(0).media_box == (pdf::Box { 0, 0, 100, 200 }));
            CHECK(doc->page(1).media_box == (pdf::Box { 0, 0, 612, 792 })); // the default letter page
        }
    }
    // The same file with its cross-reference stream broken: the scan finds the object stream's objects too.
    std::string broken = out;
    broken.replace(broken.rfind("startxref\n") + 10, std::to_string(xref_offset).size(), "1");
    doc = open(broken, status);
    CHECK(doc && doc->repaired() && doc->page_count() == 2);

    // An incremental update: a newer section redefines the page; /Prev leads to the older.
    std::string const base = build_pdf(two_pages());
    std::size_t const base_xref = std::stoul(base.substr(base.rfind("startxref\n") + 10));
    std::string update = base;
    std::size_t const new_obj = update.size();
    update += "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 50 60] >>\nendobj\n";
    std::size_t const new_xref = update.size();
    char line[32];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", new_obj);
    update += "xref\n4 1\n" + std::string(line) + "trailer\n<< /Size 7 /Root 1 0 R /Prev " + std::to_string(base_xref)
        + " >>\nstartxref\n" + std::to_string(new_xref) + "\n%%EOF\n";
    doc = open(update, status);
    CHECK(doc && !doc->repaired() && doc->page_count() == 2);
    if (doc && doc->page_count() == 2)
        CHECK(doc->page(1).media_box == (pdf::Box { 0, 0, 50, 60 }));
}

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_encrypted(std::string const& fixtures)
{
    for (char const* revision : { "r2", "r4", "r6" }) {
        std::string const path = fixtures + "/encrypted-" + revision + ".pdf";
        Bytes const file = read_file(path);
        CHECK(!file.empty());
        for (char const* password : { "usr", "own" }) {
            pdf::Document::Status status {};
            auto const doc = pdf::Document::open(file, password, status);
            CHECK(doc && status == pdf::Document::Status::Ok);
            if (!doc)
                continue;
            CHECK(doc->encrypted());
            CHECK_EQ(doc->title(), std::string("Encrypted Title Check"));
            CHECK_EQ(doc->security()->opened_as_owner(), std::string(password) == "own");
            pdf::Object const contents = doc->resolve(doc->page(0).dict->get("Contents"));
            CHECK(contents.is_stream());
            if (contents.is_stream())
                CHECK_EQ(str(doc->decode(*contents.as_stream())), std::string("BT /F1 24 Tf 72 700 Td (Hello encrypted page) Tj ET"));
        }
        pdf::Document::Status status {};
        CHECK(!pdf::Document::open(file, "", status) && status == pdf::Document::Status::NeedsPassword);
        CHECK(!pdf::Document::open(file, "wrong", status) && status == pdf::Document::Status::NeedsPassword);
        // A file whose user password is empty opens with none, and with a wrong one.
        Bytes const open_file = read_file(fixtures + "/encrypted-" + revision + "-open.pdf");
        for (char const* password : { "", "wrong" }) {
            auto const doc = pdf::Document::open(open_file, password, status);
            CHECK(doc && status == pdf::Document::Status::Ok && doc->title() == "Encrypted Title Check");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    test_md5_rc4();
    test_lexer();
    test_filters();
    test_classic_file();
    test_damaged_files();
    test_streams_and_updates();
    if (argc > 1)
        test_encrypted(argv[1]);
    else
        CHECK(!"the fixtures folder is the first argument");
    return test::report("test_pdf");
}
