#include "pdf/Document.h"

#include "pdf/Filters.h"
#include "pdf/Parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string_view>

namespace sashfold::pdf {

namespace {

constexpr int k_max_depth = 32; // nested fetches: a stream's /Length in an object stream, and so on
constexpr int k_max_objects = 8 * 1024 * 1024;
constexpr int k_max_page_depth = 64;

std::span<std::uint8_t const> view(std::vector<std::uint8_t> const& bytes)
{
    return { bytes.data(), bytes.size() };
}

// The last occurrence of `needle` that ends at or before `end`.
std::optional<std::size_t> rfind(std::vector<std::uint8_t> const& bytes, std::string_view needle, std::size_t end)
{
    if (needle.size() > bytes.size())
        return std::nullopt;
    std::size_t at = std::min(end, bytes.size()) ;
    if (at < needle.size())
        return std::nullopt;
    at -= needle.size();
    for (;;) {
        if (std::memcmp(bytes.data() + at, needle.data(), needle.size()) == 0)
            return at;
        if (at == 0)
            return std::nullopt;
        --at;
    }
}

std::optional<std::size_t> find(std::vector<std::uint8_t> const& bytes, std::string_view needle, std::size_t from,
    std::size_t limit = SIZE_MAX)
{
    std::size_t const end = std::min(limit, bytes.size());
    if (needle.empty() || end < needle.size())
        return std::nullopt;
    for (std::size_t at = from; at + needle.size() <= end; ++at) {
        if (bytes[at] == static_cast<std::uint8_t>(needle[0]) && std::memcmp(bytes.data() + at, needle.data(), needle.size()) == 0)
            return at;
    }
    return std::nullopt;
}

Box box_of(Object const& object, Box fallback)
{
    Array const& a = object.as_array();
    if (a.size() < 4)
        return fallback;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!a[i].is_number())
            return fallback;
    }
    Box box { std::min(a[0].as_number(), a[2].as_number()), std::min(a[1].as_number(), a[3].as_number()),
        std::max(a[0].as_number(), a[2].as_number()), std::max(a[1].as_number(), a[3].as_number()) };
    if (box.width() <= 0 || box.height() <= 0 || !std::isfinite(box.width()) || !std::isfinite(box.height()))
        return fallback;
    return box;
}

thread_local std::set<int>* t_fetching = nullptr;

} // namespace

bool Document::looks_like_pdf(std::vector<std::uint8_t> const& bytes)
{
    return find(bytes, "%PDF-", 0, 1024).has_value();
}

std::shared_ptr<Document> Document::open(std::vector<std::uint8_t> bytes, std::string const& password, Status& status)
{
    status = Status::NotPdf;
    std::optional<std::size_t> const header = find(bytes, "%PDF-", 0, 1024);
    if (!header)
        return nullptr;
    std::shared_ptr<Document> doc(new Document());
    doc->m_header = *header;
    {
        std::size_t at = *header + 5;
        while (at < bytes.size() && at < *header + 12 && (std::isdigit(bytes[at]) || bytes[at] == '.'))
            doc->m_version.push_back(static_cast<char>(bytes[at++]));
    }
    doc->m_bytes = std::make_shared<std::vector<std::uint8_t> const>(std::move(bytes));
    std::vector<std::uint8_t> const& data = *doc->m_bytes;

    bool loaded = false;
    std::optional<std::size_t> const startxref = rfind(data, "startxref", data.size());
    if (startxref) {
        Lexer lexer(view(data), *startxref + 9);
        Token const offset = lexer.next();
        if (offset.kind == Token::Kind::Integer && offset.integer >= 0)
            loaded = doc->load_xref(static_cast<std::size_t>(offset.integer));
    }
    if (loaded && !doc->m_trailer.has("Root"))
        loaded = false;
    if (!loaded)
        doc->reconstruct();

    if (!doc->setup_security(password, status))
        return nullptr;

    if (!doc->load_catalog() && !doc->m_repaired) {
        // The cross-reference data read but led nowhere: rebuild and try again.
        {
            std::lock_guard const lock(doc->m_mutex);
            doc->m_cache.clear();
            doc->m_object_streams.clear();
        }
        doc->reconstruct();
        if (!doc->setup_security(password, status))
            return nullptr;
        doc->load_catalog();
    }
    if (doc->m_pages.empty()) {
        status = Status::Damaged;
        return doc;
    }
    status = Status::Ok;
    return doc;
}

void Document::set_entry(int num, Entry entry, bool replace)
{
    if (num < 0 || num >= k_max_objects)
        return;
    if (static_cast<std::size_t>(num) >= m_entries.size())
        m_entries.resize(static_cast<std::size_t>(num) + 1);
    Entry& slot = m_entries[static_cast<std::size_t>(num)];
    if (replace || slot.type == Entry::Type::Free)
        slot = entry;
}

bool Document::load_xref(std::size_t start_offset)
{
    std::vector<std::uint8_t> const& data = *m_bytes;
    std::vector<std::size_t> queue { start_offset };
    std::set<std::size_t> visited;
    bool first = true;
    while (!queue.empty()) {
        std::size_t offset = queue.back();
        queue.pop_back();
        if (!visited.insert(offset).second || visited.size() > 4096)
            continue;
        // Offsets counted from the file's start, or (in a file with bytes
        // before its header) from the header.
        auto const at_section = [&](std::size_t o) {
            if (o >= data.size())
                return false;
            Lexer lexer(view(data), o);
            Token const t = lexer.next();
            return (t.kind == Token::Kind::Keyword && t.text == "xref") || t.kind == Token::Kind::Integer;
        };
        if (!at_section(offset)) {
            if (m_header > 0 && at_section(offset + m_header))
                offset += m_header;
            else
                return false;
        }
        Dict trailer;
        Lexer lexer(view(data), offset);
        Token const t = lexer.next();
        bool ok = false;
        if (t.kind == Token::Kind::Keyword && t.text == "xref")
            ok = read_xref_table(offset, trailer);
        else
            ok = read_xref_stream(offset, trailer);
        if (!ok)
            return false;
        if (first) {
            m_trailer = trailer;
            first = false;
        } else {
            for (auto const& [key, value] : trailer.entries()) {
                if (!m_trailer.has(key) && key != "Prev" && key != "XRefStm")
                    m_trailer.set(key, value);
            }
        }
        Object const& prev = trailer.get("Prev");
        if (prev.is_number() && prev.as_int() >= 0)
            queue.push_back(static_cast<std::size_t>(prev.as_int()));
    }
    return !first;
}

bool Document::read_xref_table(std::size_t offset, Dict& trailer)
{
    std::vector<std::uint8_t> const& data = *m_bytes;
    Lexer lexer(view(data), offset);
    lexer.next(); // xref
    bool first_subsection = true;
    for (;;) {
        Token const start = lexer.next();
        if (start.kind == Token::Kind::Keyword && start.text == "trailer")
            break;
        if (start.kind != Token::Kind::Integer)
            return false;
        Token const count = lexer.next();
        if (count.kind != Token::Kind::Integer || count.integer < 0 || start.integer < 0
            || start.integer + count.integer > k_max_objects)
            return false;
        std::int64_t base = start.integer;
        for (std::int64_t i = 0; i < count.integer; ++i) {
            Token const entry_offset = lexer.next();
            Token const entry_gen = lexer.next();
            Token const kind = lexer.next();
            if (entry_offset.kind != Token::Kind::Integer || entry_gen.kind != Token::Kind::Integer
                || kind.kind != Token::Kind::Keyword || (kind.text != "n" && kind.text != "f"))
                return false;
            // A table that starts at 1 with object 0's free head is off by one (a known writer bug).
            if (first_subsection && i == 0 && base == 1 && kind.text == "f" && entry_offset.integer == 0
                && entry_gen.integer == 65535)
                base = 0;
            int const num = static_cast<int>(base + i);
            if (kind.text == "n" && entry_offset.integer > 0) {
                set_entry(num, Entry { Entry::Type::Offset, static_cast<std::uint64_t>(entry_offset.integer),
                                   static_cast<int>(entry_gen.integer) },
                    false);
            }
        }
        first_subsection = false;
    }
    Parser parser(lexer);
    Object const dict = parser.parse();
    if (!dict.is_dict())
        return false;
    trailer = dict.as_dict();
    // A hybrid file: the stream beside the table holds the objects in object streams.
    Object const& xref_stream = trailer.get("XRefStm");
    if (xref_stream.is_number() && xref_stream.as_int() > 0) {
        Dict ignored;
        read_xref_stream(static_cast<std::size_t>(xref_stream.as_int()), ignored);
    }
    return true;
}

bool Document::read_xref_stream(std::size_t offset, Dict& trailer)
{
    bool matched = false;
    Object const object = parse_at(offset, Ref { -1, 0 }, &matched);
    Stream const* const stream = object.as_stream();
    if (!stream || !stream->dict.get("Type").is_name("XRef"))
        return false;
    trailer = stream->dict;
    Array const& w = stream->dict.get("W").as_array();
    if (w.size() < 3)
        return false;
    int widths[3];
    for (int i = 0; i < 3; ++i) {
        widths[i] = static_cast<int>(w[static_cast<std::size_t>(i)].as_int(-1));
        if (widths[i] < 0 || widths[i] > 8)
            return false;
    }
    std::int64_t const size = stream->dict.get("Size").as_int(0);
    std::vector<std::int64_t> index;
    for (Object const& value : stream->dict.get("Index").as_array())
        index.push_back(value.as_int(-1));
    if (index.empty())
        index = { 0, size };
    std::vector<std::uint8_t> const rows = decode_through(*stream, false, nullptr, nullptr, 0);
    std::size_t const row_size = static_cast<std::size_t>(widths[0] + widths[1] + widths[2]);
    if (row_size == 0)
        return false;
    std::size_t at = 0;
    auto const field = [&](int width, std::uint64_t fallback) {
        if (width == 0)
            return fallback;
        std::uint64_t value = 0;
        for (int k = 0; k < width; ++k)
            value = (value << 8) | rows[at++];
        return value;
    };
    for (std::size_t s = 0; s + 1 < index.size(); s += 2) {
        std::int64_t const start = index[s];
        std::int64_t const count = index[s + 1];
        if (start < 0 || count < 0 || start + count > k_max_objects)
            return false;
        for (std::int64_t i = 0; i < count; ++i) {
            if (at + row_size > rows.size())
                return true;
            std::uint64_t const type = field(widths[0], 1);
            std::uint64_t const second = field(widths[1], 0);
            std::uint64_t const third = field(widths[2], 0);
            int const num = static_cast<int>(start + i);
            if (type == 1 && second > 0)
                set_entry(num, Entry { Entry::Type::Offset, second, static_cast<int>(third) }, false);
            else if (type == 2)
                set_entry(num, Entry { Entry::Type::Compressed, second, static_cast<int>(third) }, false);
        }
    }
    return true;
}

void Document::reconstruct()
{
    // Every "N G obj" in the file, the last of a number standing (an
    // incremental update's), and every trailer: what readers rebuild from.
    m_repaired = true;
    m_entries.clear();
    m_scanned.clear();
    std::vector<std::uint8_t> const& data = *m_bytes;
    Dict merged_trailer;
    std::vector<int> object_streams;
    std::optional<Ref> catalog;
    for (std::size_t at = 0; at + 3 <= data.size(); ++at) {
        if (data[at] == 't' && at + 7 <= data.size() && std::memcmp(data.data() + at, "trailer", 7) == 0) {
            Lexer lexer(view(data), at + 7);
            Parser parser(lexer);
            Object const dict = parser.parse();
            for (auto const& [key, value] : dict.as_dict().entries())
                merged_trailer.set(key, value); // later trailers are newer
            continue;
        }
        if (data[at] != 'o' || data[at + 1] != 'b' || data[at + 2] != 'j')
            continue;
        if (at + 3 < data.size() && !is_pdf_whitespace(data[at + 3]) && !is_pdf_delimiter(data[at + 3]))
            continue;
        // Back over whitespace, the generation, whitespace, the number.
        std::size_t p = at;
        auto const back_ws = [&] {
            std::size_t n = 0;
            while (p > 0 && is_pdf_whitespace(data[p - 1])) {
                --p;
                ++n;
            }
            return n;
        };
        auto const back_digits = [&] {
            std::size_t const end = p;
            while (p > 0 && std::isdigit(data[p - 1]) && end - p < 10)
                --p;
            return end - p;
        };
        if (back_ws() == 0)
            continue;
        std::size_t const gen_end = p;
        if (back_digits() == 0)
            continue;
        std::size_t const gen_start = p;
        if (back_ws() == 0)
            continue;
        std::size_t const num_end = p;
        if (back_digits() == 0)
            continue;
        std::size_t const num_start = p;
        if (p > 0 && !is_pdf_whitespace(data[p - 1]) && !is_pdf_delimiter(data[p - 1]))
            continue;
        int const num = std::atoi(std::string(data.begin() + static_cast<std::ptrdiff_t>(num_start),
            data.begin() + static_cast<std::ptrdiff_t>(num_end)).c_str());
        int const gen = std::atoi(std::string(data.begin() + static_cast<std::ptrdiff_t>(gen_start),
            data.begin() + static_cast<std::ptrdiff_t>(gen_end)).c_str());
        if (num <= 0 || num >= k_max_objects)
            continue;
        set_entry(num, Entry { Entry::Type::Offset, num_start, gen }, true);
        m_scanned[Ref { num, gen }] = num_start;
        // A peek at the object: catalogs, object streams, cross-reference streams' trailers.
        Lexer lexer(view(data), at + 3);
        Parser parser(lexer);
        Object const head = parser.parse();
        Dict const& dict = head.as_dict();
        if (dict.get("Type").is_name("Catalog"))
            catalog = Ref { num, gen };
        else if (dict.get("Type").is_name("ObjStm"))
            object_streams.push_back(num);
        else if (dict.get("Type").is_name("XRef")) {
            for (auto const& [key, value] : dict.entries()) {
                if (key == "Root" || key == "Info" || key == "Encrypt" || key == "ID")
                    merged_trailer.set(key, value);
            }
        }
    }
    m_trailer = merged_trailer;
    if (catalog) {
        // The trailer's root when it names a catalog; else the last catalog found.
        bool root_ok = false;
        if (m_trailer.get("Root").is_ref()) {
            Ref const root = m_trailer.get("Root").as_ref();
            root_ok = static_cast<std::size_t>(root.num) < m_entries.size()
                && m_entries[static_cast<std::size_t>(root.num)].type != Entry::Type::Free;
        }
        if (!root_ok)
            m_trailer.set("Root", Object::ref(*catalog));
    }
    // Objects inside object streams, where the scan found no direct one.
    for (int const stream_num : object_streams) {
        Object const stream_object = fetch_depth(Ref { stream_num, 0 }, 0);
        Stream const* const stream = stream_object.as_stream();
        if (!stream)
            continue;
        std::vector<std::uint8_t> const body = decode_through(*stream, false, nullptr, nullptr, 0);
        std::int64_t const n = stream->dict.get("N").as_int(0);
        Lexer lexer(view(body));
        for (std::int64_t i = 0; i < n && i < 1000000; ++i) {
            Token const num = lexer.next();
            Token const off = lexer.next();
            if (num.kind != Token::Kind::Integer || off.kind != Token::Kind::Integer)
                break;
            int const object_num = static_cast<int>(num.integer);
            if (object_num > 0 && object_num < k_max_objects
                && (static_cast<std::size_t>(object_num) >= m_entries.size()
                    || m_entries[static_cast<std::size_t>(object_num)].type == Entry::Type::Free))
                set_entry(object_num, Entry { Entry::Type::Compressed, static_cast<std::uint64_t>(stream_num), static_cast<int>(i) },
                    true);
        }
    }
    if (!m_trailer.get("Root").is_ref()) {
        for (std::size_t num = 1; num < m_entries.size(); ++num) {
            if (m_entries[num].type != Entry::Type::Compressed)
                continue;
            Object const object = fetch_depth(Ref { static_cast<int>(num), 0 }, 0);
            if (object.as_dict().get("Type").is_name("Catalog")) {
                m_trailer.set("Root", Object::ref(Ref { static_cast<int>(num), 0 }));
                break;
            }
        }
    }
    std::lock_guard const lock(m_mutex);
    m_cache.clear();
    m_object_streams.clear();
}

bool Document::setup_security(std::string const& password, Status& status)
{
    m_security.reset();
    Object const& encrypt_entry = m_trailer.get("Encrypt");
    if (encrypt_entry.is_null())
        return true;
    if (encrypt_entry.is_ref())
        m_encrypt_ref = encrypt_entry.as_ref();
    Object const encrypt = resolve(encrypt_entry);
    if (!encrypt.is_dict()) {
        status = Status::Unsupported;
        return false;
    }
    Array const& id = m_trailer.get("ID").as_array();
    std::string const id0 = id.empty() ? std::string {} : resolve(id[0]).as_string();
    SecurityHandler::Status security_status = SecurityHandler::Status::Unsupported;
    m_security = SecurityHandler::open(encrypt.as_dict(), id0, password, security_status);
    if (!m_security && !password.empty()) {
        // A wrong password for a file that needs none: it opens as it would have.
        m_security = SecurityHandler::open(encrypt.as_dict(), id0, std::string {}, security_status);
    }
    if (!m_security) {
        status = security_status == SecurityHandler::Status::NeedsPassword ? Status::NeedsPassword : Status::Unsupported;
        return false;
    }
    // Strings parsed before the key was known (the trailer's are never encrypted) are dropped from the cache.
    std::lock_guard const lock(m_mutex);
    m_cache.clear();
    m_object_streams.clear();
    return true;
}

bool Document::load_catalog()
{
    Object const root = resolve(m_trailer.get("Root"));
    if (!root.is_dict() && !m_repaired)
        return false;
    m_catalog = root.as_dict();
    m_pages.clear();
    std::vector<Ref> seen;
    walk_pages(m_catalog.get("Pages"), Object {}, Box { 0, 0, 612, 792 }, std::nullopt, 0, 1, 0, seen);
    if (m_pages.empty() && m_repaired) {
        // A broken tree, or no catalog left (a file cut short): the pages
        // in the order the file holds them, as MuPDF rebuilds them.
        std::vector<std::pair<std::size_t, Ref>> found;
        for (auto const& [ref, offset] : m_scanned)
            found.emplace_back(offset, ref);
        std::sort(found.begin(), found.end());
        for (auto const& [offset, ref] : found) {
            Object const page = fetch(ref);
            if (page.as_dict().get("Type").is_name("Page"))
                walk_pages(Object::ref(ref), Object {}, Box { 0, 0, 612, 792 }, std::nullopt, 0, 1, k_max_page_depth - 1, seen);
        }
    }
    return !m_pages.empty();
}

void Document::walk_pages(Object const& node_object, Object const& resources_in, Box media, std::optional<Box> crop,
    int rotate, double user_unit, int depth, std::vector<Ref>& seen)
{
    if (depth > k_max_page_depth || m_pages.size() > 1000000)
        return;
    Ref const ref = node_object.as_ref();
    if (node_object.is_ref()) {
        if (std::find(seen.begin(), seen.end(), ref) != seen.end())
            return;
        seen.push_back(ref);
    }
    Object const node = resolve(node_object);
    if (!node.is_dict())
        return;
    Dict const& dict = node.as_dict();
    Object resources = resources_in;
    if (dict.has("Resources"))
        resources = dict.get("Resources");
    media = box_of(resolve(dict.get("MediaBox")), media);
    if (dict.has("CropBox"))
        crop = box_of(resolve(dict.get("CropBox")), crop.value_or(media));
    if (dict.has("Rotate"))
        rotate = static_cast<int>(resolve(dict.get("Rotate")).as_int(0));
    if (dict.has("UserUnit"))
        user_unit = resolve(dict.get("UserUnit")).as_number(1);
    Object const kids = resolve(dict.get("Kids"));
    bool const is_page = dict.get("Type").is_name("Page") || (!dict.get("Type").is_name("Pages") && !kids.is_array());
    if (!is_page) {
        for (Object const& kid : kids.as_array())
            walk_pages(kid, resources, media, crop, rotate, user_unit, depth + 1, seen);
        return;
    }
    Page page;
    page.ref = ref;
    page.dict = std::make_shared<Dict const>(dict);
    page.media_box = media;
    Box c = crop.value_or(media);
    c = Box { std::max(c.x0, media.x0), std::max(c.y0, media.y0), std::min(c.x1, media.x1), std::min(c.y1, media.y1) };
    if (c.width() <= 0 || c.height() <= 0)
        c = media;
    page.crop_box = c;
    rotate %= 360;
    if (rotate < 0)
        rotate += 360;
    page.rotate = rotate / 90 * 90;
    page.user_unit = user_unit > 0 && std::isfinite(user_unit) ? user_unit : 1;
    page.resources = resources;
    m_pages.push_back(std::move(page));
}

std::optional<std::size_t> Document::page_index(Ref ref) const
{
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
        if (m_pages[i].ref == ref)
            return i;
    }
    return std::nullopt;
}

std::string Document::decrypt_string(Ref owner, std::string const& data) const
{
    return m_security ? m_security->decrypt_string(owner, data) : data;
}

Object Document::parse_at(std::size_t offset, Ref expected, bool* matched) const
{
    std::vector<std::uint8_t> const& data = *m_bytes;
    if (matched)
        *matched = false;
    if (offset >= data.size())
        return {};
    Lexer lexer(view(data), offset);
    Token const num = lexer.next();
    Token const gen = lexer.next();
    Token const obj = lexer.next();
    if (num.kind != Token::Kind::Integer || gen.kind != Token::Kind::Integer || obj.kind != Token::Kind::Keyword
        || obj.text != "obj")
        return {};
    Ref const ref { static_cast<int>(num.integer), static_cast<int>(gen.integer) };
    if (expected.num >= 0 && ref.num != expected.num)
        return {};
    if (matched)
        *matched = true;
    Parser parser(lexer);
    if (m_security && !(ref == m_encrypt_ref))
        parser.set_string_filter([this, ref](std::string s) { return m_security->decrypt_string(ref, s); });
    Object object = parser.parse();
    if (!object.is_dict())
        return object;
    std::size_t const after_dict = lexer.position();
    Token const keyword = lexer.next();
    if (keyword.kind != Token::Kind::Keyword || keyword.text != "stream") {
        lexer.seek(after_dict);
        return object;
    }
    std::size_t start = lexer.position();
    if (start < data.size() && data[start] == '\r')
        ++start;
    if (start < data.size() && data[start] == '\n')
        ++start;
    Dict const& dict = object.as_dict();
    // /Length, resolved — fetched without this object's lock held.
    std::int64_t length = -1;
    Object const& length_entry = dict.get("Length");
    if (length_entry.is_number())
        length = length_entry.as_int(-1);
    else if (length_entry.is_ref() && !(length_entry.as_ref() == ref))
        length = fetch(length_entry.as_ref()).as_int(-1);
    auto const ends_at = [&](std::size_t end) {
        if (end > data.size())
            return false;
        Lexer check(view(data), end);
        Token const t = check.next();
        return t.kind == Token::Kind::Keyword && (t.text == "endstream" || t.text.rfind("endstream", 0) == 0);
    };
    std::size_t stream_length = 0;
    if (length >= 0 && static_cast<std::uint64_t>(length) <= data.size() - start && ends_at(start + static_cast<std::size_t>(length))) {
        stream_length = static_cast<std::size_t>(length);
    } else {
        // A wrong length: the stream runs to its "endstream", less the end of line before it.
        std::optional<std::size_t> const end = find(data, "endstream", start);
        std::size_t stop = end.value_or(data.size());
        if (stop > start && data[stop - 1] == '\n')
            --stop;
        if (stop > start && data[stop - 1] == '\r')
            --stop;
        stream_length = stop - start;
    }
    Stream stream;
    stream.dict = dict;
    stream.source = m_bytes;
    stream.start = start;
    stream.length = stream_length;
    stream.owner = ref;
    return Object::stream(std::move(stream));
}

Object Document::fetch(Ref ref) const
{
    return fetch_depth(ref, 0);
}

Object Document::fetch_depth(Ref ref, int depth) const
{
    if (ref.num <= 0 || static_cast<std::size_t>(ref.num) >= m_entries.size() || depth > k_max_depth)
        return {};
    {
        std::lock_guard const lock(m_mutex);
        auto const it = m_cache.find(ref.num);
        if (it != m_cache.end())
            return it->second;
    }
    // A fetch that comes back to an object being fetched (a /Length that
    // names its own stream, an object stream inside itself) is null.
    std::set<int> local;
    bool const outermost = t_fetching == nullptr;
    if (outermost)
        t_fetching = &local;
    if (!t_fetching->insert(ref.num).second) {
        if (outermost)
            t_fetching = nullptr;
        return {};
    }
    Object object = fetch_uncached(ref, depth);
    t_fetching->erase(ref.num);
    if (outermost)
        t_fetching = nullptr;
    std::lock_guard const lock(m_mutex);
    m_cache.emplace(ref.num, object);
    return object;
}

Object Document::fetch_uncached(Ref ref, int depth) const
{
    Entry const entry = m_entries[static_cast<std::size_t>(ref.num)];
    if (entry.type == Entry::Type::Compressed)
        return from_object_stream(static_cast<int>(entry.offset), entry.gen, ref.num, depth);
    if (entry.type != Entry::Type::Offset)
        return {};
    bool matched = false;
    Object object = parse_at(static_cast<std::size_t>(entry.offset), ref, &matched);
    if (!matched && m_header > 0)
        object = parse_at(static_cast<std::size_t>(entry.offset) + m_header, ref, &matched);
    if (!matched) {
        // The table points somewhere else: where the scan found it, if the file was scanned.
        for (auto const& [scanned, offset] : m_scanned) {
            if (scanned.num == ref.num) {
                object = parse_at(offset, ref, &matched);
                break;
            }
        }
    }
    return object;
}

Object Document::from_object_stream(int stream_num, int index, int num, int depth) const
{
    std::shared_ptr<ObjectStream const> stream;
    {
        std::lock_guard const lock(m_mutex);
        auto const it = m_object_streams.find(stream_num);
        if (it != m_object_streams.end())
            stream = it->second;
    }
    if (!stream) {
        Object const container = fetch_depth(Ref { stream_num, 0 }, depth + 1);
        Stream const* const s = container.as_stream();
        if (!s)
            return {};
        auto built = std::make_shared<ObjectStream>();
        built->data = decode_through(*s, false, nullptr, nullptr, depth + 1);
        std::int64_t const n = s->dict.get("N").as_int(0);
        std::int64_t const first = s->dict.get("First").as_int(0);
        Lexer lexer(view(built->data));
        for (std::int64_t i = 0; i < n && i < 1000000; ++i) {
            Token const object_num = lexer.next();
            Token const offset = lexer.next();
            if (object_num.kind != Token::Kind::Integer || offset.kind != Token::Kind::Integer)
                break;
            built->offsets.emplace_back(static_cast<int>(object_num.integer),
                static_cast<std::size_t>(std::max<std::int64_t>(0, first + offset.integer)));
        }
        stream = built;
        std::lock_guard const lock(m_mutex);
        m_object_streams.emplace(stream_num, stream);
    }
    // By index, checked against the number; else by number.
    std::size_t offset = SIZE_MAX;
    if (index >= 0 && static_cast<std::size_t>(index) < stream->offsets.size() && stream->offsets[static_cast<std::size_t>(index)].first == num)
        offset = stream->offsets[static_cast<std::size_t>(index)].second;
    for (std::size_t i = 0; offset == SIZE_MAX && i < stream->offsets.size(); ++i) {
        if (stream->offsets[i].first == num)
            offset = stream->offsets[i].second;
    }
    if (offset >= stream->data.size())
        return {};
    // Strings in an object stream are decrypted with the stream: never again.
    Lexer lexer(view(stream->data), offset);
    Parser parser(lexer);
    return parser.parse();
}

Object Document::resolve(Object const& object) const
{
    return resolve_depth(object, 0);
}

Object Document::resolve_depth(Object const& object, int depth) const
{
    Object current = object;
    for (int i = 0; current.is_ref() && i < 32; ++i)
        current = fetch_depth(current.as_ref(), depth);
    return current.is_ref() ? Object {} : current;
}

std::vector<std::uint8_t> Document::decode_through(Stream const& stream, bool stop_at_image, std::string* codec,
    Dict* codec_params, int depth) const
{
    std::vector<std::uint8_t> data = stream.raw();
    Object const filter_object = resolve_depth(stream.dict.get("Filter"), depth);
    Object const params_object = resolve_depth(stream.dict.get("DecodeParms").is_null() ? stream.dict.get("DP") : stream.dict.get("DecodeParms"), depth);
    std::vector<std::string> filters;
    std::vector<Dict> params;
    if (filter_object.is_name()) {
        filters.push_back(filter_object.as_name());
        params.push_back(params_object.as_dict());
    } else {
        Array const& parameter_list = params_object.as_array();
        Array const& filter_list = filter_object.as_array();
        for (std::size_t i = 0; i < filter_list.size(); ++i) {
            filters.push_back(resolve_depth(filter_list[i], depth).as_name());
            params.push_back(i < parameter_list.size() ? resolve_depth(parameter_list[i], depth).as_dict() : Dict {});
        }
    }
    // Resolve the parameters' own references (a JBIG2 /JBIG2Globals stays a reference for the codec).
    for (Dict& p : params) {
        Dict resolved;
        for (auto const& [key, value] : p.entries())
            resolved.set(key, key == "JBIG2Globals" ? value : resolve_depth(value, depth));
        p = std::move(resolved);
    }
    if (m_security && !stream.dict.get("Type").is_name("XRef")
        && !(stream.dict.get("Type").is_name("Metadata") && !m_security->encrypt_metadata())) {
        std::string crypt_filter;
        if (!filters.empty() && filters[0] == "Crypt")
            crypt_filter = params[0].get("Name").is_name() ? params[0].get("Name").as_name() : "Identity";
        data = m_security->decrypt_stream(stream.owner, data, crypt_filter);
    }
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (is_image_filter(filters[i])) {
            if (stop_at_image || codec) {
                if (codec)
                    *codec = expand_filter_name(filters[i]);
                if (codec_params)
                    *codec_params = params[i];
                return data;
            }
            return data;
        }
        data = apply_filter(filters[i], data, params[i]);
    }
    return data;
}

std::vector<std::uint8_t> Document::decode(Stream const& stream) const
{
    return decode_through(stream, true, nullptr, nullptr, 0);
}

ImageBytes Document::decode_image(Stream const& stream) const
{
    ImageBytes out;
    out.bytes = decode_through(stream, true, &out.codec, &out.codec_params, 0);
    return out;
}

std::string Document::info_text(char const* key) const
{
    Object const info = resolve(m_trailer.get("Info"));
    Object const value = resolve(info.as_dict().get(key));
    return value.is_string() ? text_string_to_utf8(value.as_string()) : std::string {};
}

std::string Document::title() const
{
    return info_text("Title");
}

}
