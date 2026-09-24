#include "storage/IndexedDb.h"

#include "js/Lexer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>

namespace sashfold::idb {

// --- Keys -------------------------------------------------------------------------------------

Key Key::from_number(double value)
{
    Key key;
    key.type = Type::Number;
    key.number = value;
    return key;
}

Key Key::from_date(double time_value)
{
    Key key;
    key.type = Type::Date;
    key.number = time_value;
    return key;
}

Key Key::from_string(std::u16string value)
{
    Key key;
    key.type = Type::String;
    key.string = std::move(value);
    return key;
}

Key Key::from_binary(Bytes value)
{
    Key key;
    key.type = Type::Binary;
    key.binary = std::move(value);
    return key;
}

Key Key::from_array(std::vector<Key> value)
{
    Key key;
    key.type = Type::Array;
    key.array = std::move(value);
    return key;
}

int compare(Key const& a, Key const& b)
{
    // Section 7.1: different types by their order; numbers and dates by
    // value; strings by code unit; binary keys byte by byte, a prefix first;
    // arrays item by item, a prefix first.
    if (a.type != b.type)
        return a.type < b.type ? -1 : 1;
    switch (a.type) {
    case Key::Type::Number:
    case Key::Type::Date:
        if (a.number < b.number)
            return -1;
        return a.number > b.number ? 1 : 0;
    case Key::Type::String: {
        int const order = a.string.compare(b.string);
        return order < 0 ? -1 : order > 0 ? 1 : 0;
    }
    case Key::Type::Binary: {
        std::size_t const common = std::min(a.binary.size(), b.binary.size());
        for (std::size_t i = 0; i < common; ++i) {
            if (a.binary[i] != b.binary[i])
                return a.binary[i] < b.binary[i] ? -1 : 1;
        }
        if (a.binary.size() == b.binary.size())
            return 0;
        return a.binary.size() < b.binary.size() ? -1 : 1;
    }
    case Key::Type::Array: {
        std::size_t const common = std::min(a.array.size(), b.array.size());
        for (std::size_t i = 0; i < common; ++i) {
            if (int const order = compare(a.array[i], b.array[i]); order != 0)
                return order;
        }
        if (a.array.size() == b.array.size())
            return 0;
        return a.array.size() < b.array.size() ? -1 : 1;
    }
    }
    return 0;
}

KeyRange KeyRange::only(Key key)
{
    KeyRange range;
    range.lower = key;
    range.upper = std::move(key);
    return range;
}

bool KeyRange::below_lower(Key const& key) const
{
    if (!lower)
        return false;
    int const order = compare(*lower, key);
    return order > 0 || (order == 0 && lower_open);
}

bool KeyRange::above_upper(Key const& key) const
{
    if (!upper)
        return false;
    int const order = compare(*upper, key);
    return order < 0 || (order == 0 && upper_open);
}

bool KeyRange::includes(Key const& key) const
{
    return !below_lower(key) && !above_upper(key);
}

bool is_valid_key_path_string(std::u16string_view path)
{
    if (path.empty())
        return true;
    // Identifiers joined by periods: none empty, each an IdentifierName.
    std::size_t start = 0;
    while (true) {
        std::size_t const dot = path.find(u'.', start);
        std::u16string_view const part = path.substr(start, dot == std::u16string_view::npos ? std::u16string_view::npos : dot - start);
        if (part.empty())
            return false;
        bool first = true;
        for (std::size_t i = 0; i < part.size();) {
            char32_t code_point = part[i];
            std::size_t width = 1;
            if (code_point >= 0xD800 && code_point <= 0xDBFF && i + 1 < part.size() && part[i + 1] >= 0xDC00 && part[i + 1] <= 0xDFFF) {
                code_point = 0x10000 + ((code_point - 0xD800) << 10) + (part[i + 1] - 0xDC00);
                width = 2;
            }
            bool const ok = first ? js::Lexer::is_identifier_start(code_point) : js::Lexer::is_identifier_part(code_point);
            if (!ok)
                return false;
            first = false;
            i += width;
        }
        if (dot == std::u16string_view::npos)
            return true;
        start = dot + 1;
    }
}

// --- Index order --------------------------------------------------------------------------------

namespace {

// Where an entry stands against a probe: negative when before it.
int compare_to_probe(IndexEntry const& entry, IndexProbe const& probe)
{
    int const by_key = compare(entry.first, *probe.key);
    if (by_key != 0)
        return by_key;
    if (probe.primary != nullptr) {
        int const by_primary = compare(entry.second, *probe.primary);
        if (by_primary != 0)
            return by_primary;
    }
    return -probe.bias;
}

}

bool IndexEntryLess::operator()(IndexEntry const& a, IndexEntry const& b) const
{
    int const by_key = compare(a.first, b.first);
    if (by_key != 0)
        return by_key < 0;
    return compare(a.second, b.second) < 0;
}

bool IndexEntryLess::operator()(IndexEntry const& a, IndexProbe const& b) const
{
    return compare_to_probe(a, b) < 0;
}

bool IndexEntryLess::operator()(IndexProbe const& a, IndexEntry const& b) const
{
    return compare_to_probe(b, a) > 0;
}

// --- Stores -------------------------------------------------------------------------------------

IndexState* StoreState::index_named(std::u16string_view wanted)
{
    for (auto& [index_id, index] : indexes) {
        if (index.name == wanted)
            return &index;
    }
    return nullptr;
}

std::vector<std::u16string> StoreState::index_names() const
{
    std::vector<std::u16string> names;
    for (auto const& [index_id, index] : indexes)
        names.push_back(index.name);
    std::sort(names.begin(), names.end());
    return names;
}

StoreState* DatabaseState::store_named(std::u16string_view wanted)
{
    for (auto& [id, store] : stores) {
        if (store.name == wanted)
            return &store;
    }
    return nullptr;
}

StoreState* DatabaseState::store(std::uint64_t id)
{
    auto const found = stores.find(id);
    return found == stores.end() ? nullptr : &found->second;
}

std::vector<std::u16string> DatabaseState::store_names() const
{
    std::vector<std::u16string> names;
    for (auto const& [id, store] : stores)
        names.push_back(store.name);
    std::sort(names.begin(), names.end());
    return names;
}

void UndoLog::revert()
{
    for (auto step = m_steps.rbegin(); step != m_steps.rend(); ++step)
        (*step)();
    m_steps.clear();
}

namespace {

void add_index_entries(StoreState& store, Key const& primary, IndexKeys const& keys)
{
    for (auto const& [index_id, index_keys] : keys) {
        auto const index = store.indexes.find(index_id);
        if (index == store.indexes.end())
            continue;
        for (Key const& key : index_keys)
            index->second.entries.insert(IndexEntry { key, primary });
    }
}

void remove_index_entries(StoreState& store, Key const& primary, IndexKeys const& keys)
{
    for (auto const& [index_id, index_keys] : keys) {
        auto const index = store.indexes.find(index_id);
        if (index == store.indexes.end())
            continue;
        for (Key const& key : index_keys)
            index->second.entries.erase(IndexEntry { key, primary });
    }
}

// Takes a record out, and returns it.
std::optional<Record> take_record(StoreState& store, Key const& primary)
{
    auto const found = store.records.find(primary);
    if (found == store.records.end())
        return std::nullopt;
    Record record = std::move(found->second);
    store.records.erase(found);
    remove_index_entries(store, primary, record.index_keys);
    return record;
}

void insert_record(StoreState& store, Key const& primary, Record record)
{
    add_index_entries(store, primary, record.index_keys);
    store.records.insert_or_assign(primary, std::move(record));
}

}

bool violates_unique_index(StoreState const& store, Key const& primary, IndexKeys const& keys)
{
    for (auto const& [index_id, index_keys] : keys) {
        auto const index = store.indexes.find(index_id);
        if (index == store.indexes.end() || !index->second.unique)
            continue;
        for (Key const& key : index_keys) {
            IndexProbe const probe { &key, nullptr, -1 };
            auto const at = index->second.entries.lower_bound(probe);
            // A record of this key other than the one being replaced.
            for (auto it = at; it != index->second.entries.end() && keys_equal(it->first, key); ++it) {
                if (!keys_equal(it->second, primary))
                    return true;
            }
        }
    }
    return false;
}

void put_record(StoreState& store, Key const& primary, Record record, UndoLog* undo)
{
    std::optional<Record> previous = take_record(store, primary);
    insert_record(store, primary, std::move(record));
    if (undo != nullptr) {
        StoreState* const at = &store;
        auto const old = std::make_shared<std::optional<Record>>(std::move(previous));
        undo->add([at, primary, old] {
            take_record(*at, primary);
            if (*old)
                insert_record(*at, primary, **old);
        });
    }
}

std::size_t delete_records(StoreState& store, KeyRange const& range, UndoLog* undo)
{
    std::vector<Key> doomed;
    auto it = range.lower ? store.records.lower_bound(*range.lower) : store.records.begin();
    for (; it != store.records.end(); ++it) {
        if (range.above_upper(it->first))
            break;
        if (range.includes(it->first))
            doomed.push_back(it->first);
    }
    for (Key const& key : doomed) {
        std::optional<Record> removed = take_record(store, key);
        if (undo != nullptr && removed) {
            StoreState* const at = &store;
            auto const old = std::make_shared<Record>(std::move(*removed));
            undo->add([at, key, old] { insert_record(*at, key, *old); });
        }
    }
    return doomed.size();
}

void clear_records(StoreState& store, UndoLog* undo)
{
    if (undo != nullptr) {
        StoreState* const at = &store;
        auto const old_records = std::make_shared<std::map<Key, Record, KeyLess>>(std::move(store.records));
        auto old_indexes = std::make_shared<std::vector<std::pair<std::uint64_t, std::set<IndexEntry, IndexEntryLess>>>>();
        for (auto& [id, index] : store.indexes)
            old_indexes->emplace_back(id, std::move(index.entries));
        undo->add([at, old_records, old_indexes] {
            at->records = *old_records;
            for (auto& [id, entries] : *old_indexes) {
                auto const index = at->indexes.find(id);
                if (index != at->indexes.end())
                    index->second.entries = entries;
            }
        });
    }
    store.records.clear();
    for (auto& [id, index] : store.indexes)
        index.entries.clear();
}

std::optional<double> generate_key(StoreState& store, UndoLog* undo)
{
    // Section 2.11: 2^53 is the last key a generator gives; past it the
    // current number is infinity (2^53 + 1 is no double).
    constexpr double last = 9007199254740992.0;
    if (store.current_number > last)
        return std::nullopt;
    double const key = store.current_number;
    if (undo != nullptr) {
        StoreState* const at = &store;
        undo->add([at, key] { at->current_number = key; });
    }
    store.current_number = key >= last ? std::numeric_limits<double>::infinity() : key + 1;
    return key;
}

void possibly_update_key_generator(StoreState& store, Key const& key, UndoLog* undo)
{
    if (key.type != Key::Type::Number)
        return;
    constexpr double last = 9007199254740992.0;
    double const value = std::floor(key.number);
    if (value >= store.current_number) {
        if (undo != nullptr) {
            StoreState* const at = &store;
            double const old = store.current_number;
            undo->add([at, old] { at->current_number = old; });
        }
        store.current_number = value >= last ? std::numeric_limits<double>::infinity() : value + 1;
    }
}

bool populate_index(StoreState& store, IndexState& index, std::vector<std::pair<Key, std::vector<Key>>> const& keys_by_record)
{
    std::set<IndexEntry, IndexEntryLess> entries;
    for (auto const& [primary, keys] : keys_by_record) {
        for (Key const& key : keys) {
            if (index.unique) {
                IndexProbe const probe { &key, nullptr, -1 };
                auto const at = entries.lower_bound(probe);
                if (at != entries.end() && keys_equal(at->first, key))
                    return false;
            }
            entries.insert(IndexEntry { key, primary });
        }
    }
    index.entries = std::move(entries);
    for (auto const& [primary, keys] : keys_by_record) {
        auto const record = store.records.find(primary);
        if (record != store.records.end() && !keys.empty())
            record->second.index_keys.emplace_back(index.id, keys);
    }
    return true;
}

// --- The file -----------------------------------------------------------------------------------

namespace {

constexpr std::string_view file_magic = "SASHFOLD-IDB\n";
constexpr std::uint32_t file_format = 1;

class Writer {
public:
    void u8(std::uint8_t value) { m_bytes.push_back(value); }
    void u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
            m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void f64(double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        u64(bits);
    }
    void text(std::u16string const& value)
    {
        u32(static_cast<std::uint32_t>(value.size()));
        for (char16_t const unit : value) {
            m_bytes.push_back(static_cast<std::uint8_t>(unit & 0xFF));
            m_bytes.push_back(static_cast<std::uint8_t>(unit >> 8));
        }
    }
    void bytes(Bytes const& value)
    {
        u64(value.size());
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    }
    void key(Key const& value)
    {
        u8(static_cast<std::uint8_t>(value.type));
        switch (value.type) {
        case Key::Type::Number:
        case Key::Type::Date:
            f64(value.number);
            break;
        case Key::Type::String:
            text(value.string);
            break;
        case Key::Type::Binary:
            bytes(value.binary);
            break;
        case Key::Type::Array:
            u32(static_cast<std::uint32_t>(value.array.size()));
            for (Key const& item : value.array)
                key(item);
            break;
        }
    }
    void key_path(KeyPath const& value)
    {
        u8(static_cast<std::uint8_t>(value.kind));
        u32(static_cast<std::uint32_t>(value.paths.size()));
        for (std::u16string const& path : value.paths)
            text(path);
    }
    Bytes take() { return std::move(m_bytes); }

private:
    Bytes m_bytes;
};

class Reader {
public:
    explicit Reader(std::span<std::uint8_t const> bytes)
        : m_bytes(bytes)
    {
    }
    bool ok() const { return m_ok; }
    bool at_end() const { return m_at == m_bytes.size(); }
    bool has(std::size_t count)
    {
        if (m_bytes.size() - m_at < count)
            m_ok = false;
        return m_ok;
    }
    std::uint8_t u8()
    {
        if (!has(1))
            return 0;
        return m_bytes[m_at++];
    }
    std::uint32_t u32()
    {
        if (!has(4))
            return 0;
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(m_bytes[m_at++]) << shift;
        return value;
    }
    std::uint64_t u64()
    {
        if (!has(8))
            return 0;
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(m_bytes[m_at++]) << shift;
        return value;
    }
    double f64()
    {
        std::uint64_t const bits = u64();
        double value = 0;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    std::u16string text()
    {
        std::uint32_t const length = u32();
        if (!has(std::size_t(length) * 2))
            return {};
        std::u16string value;
        value.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i) {
            value.push_back(static_cast<char16_t>(m_bytes[m_at] | (m_bytes[m_at + 1] << 8)));
            m_at += 2;
        }
        return value;
    }
    Bytes bytes()
    {
        std::uint64_t const length = u64();
        if (!has(static_cast<std::size_t>(length)))
            return {};
        Bytes value(m_bytes.begin() + static_cast<std::ptrdiff_t>(m_at), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_at + length));
        m_at += static_cast<std::size_t>(length);
        return value;
    }
    Key key(int depth = 0)
    {
        Key value;
        std::uint8_t const type = u8();
        if (type > static_cast<std::uint8_t>(Key::Type::Array) || depth > 10000) {
            m_ok = false;
            return value;
        }
        value.type = static_cast<Key::Type>(type);
        switch (value.type) {
        case Key::Type::Number:
        case Key::Type::Date:
            value.number = f64();
            break;
        case Key::Type::String:
            value.string = text();
            break;
        case Key::Type::Binary:
            value.binary = bytes();
            break;
        case Key::Type::Array: {
            std::uint32_t const count = u32();
            for (std::uint32_t i = 0; i < count && m_ok; ++i)
                value.array.push_back(key(depth + 1));
            break;
        }
        }
        return value;
    }
    KeyPath key_path()
    {
        KeyPath value;
        std::uint8_t const kind = u8();
        if (kind > static_cast<std::uint8_t>(KeyPath::Kind::Array)) {
            m_ok = false;
            return value;
        }
        value.kind = static_cast<KeyPath::Kind>(kind);
        std::uint32_t const count = u32();
        for (std::uint32_t i = 0; i < count && m_ok; ++i)
            value.paths.push_back(text());
        return value;
    }

private:
    std::span<std::uint8_t const> m_bytes;
    std::size_t m_at = 0;
    bool m_ok = true;
};

}

Bytes encode_database(DatabaseState const& database)
{
    Writer out;
    for (char const c : file_magic)
        out.u8(static_cast<std::uint8_t>(c));
    out.u32(file_format);
    out.text(database.name);
    out.u64(database.version);
    out.u64(database.next_id);
    out.u32(static_cast<std::uint32_t>(database.stores.size()));
    for (auto const& [id, store] : database.stores) {
        out.u64(id);
        out.text(store.name);
        out.key_path(store.key_path);
        out.u8(store.auto_increment ? 1 : 0);
        out.f64(store.current_number);
        out.u32(static_cast<std::uint32_t>(store.indexes.size()));
        for (auto const& [index_id, index] : store.indexes) {
            out.u64(index_id);
            out.text(index.name);
            out.key_path(index.key_path);
            out.u8(index.unique ? 1 : 0);
            out.u8(index.multi_entry ? 1 : 0);
        }
        out.u64(store.records.size());
        for (auto const& [key, record] : store.records) {
            out.key(key);
            out.bytes(record.value);
            out.u32(static_cast<std::uint32_t>(record.index_keys.size()));
            for (auto const& [index_id, keys] : record.index_keys) {
                out.u64(index_id);
                out.u32(static_cast<std::uint32_t>(keys.size()));
                for (Key const& index_key : keys)
                    out.key(index_key);
            }
        }
    }
    return out.take();
}

std::optional<DatabaseState> decode_database(std::span<std::uint8_t const> bytes)
{
    Reader in(bytes);
    for (char const c : file_magic) {
        if (in.u8() != static_cast<std::uint8_t>(c))
            return std::nullopt;
    }
    if (in.u32() != file_format)
        return std::nullopt;
    DatabaseState database;
    database.name = in.text();
    database.version = in.u64();
    database.next_id = in.u64();
    std::uint32_t const store_count = in.u32();
    for (std::uint32_t s = 0; s < store_count && in.ok(); ++s) {
        StoreState store;
        store.id = in.u64();
        store.name = in.text();
        store.key_path = in.key_path();
        store.auto_increment = in.u8() != 0;
        store.current_number = in.f64();
        std::uint32_t const index_count = in.u32();
        for (std::uint32_t i = 0; i < index_count && in.ok(); ++i) {
            IndexState index;
            index.id = in.u64();
            index.name = in.text();
            index.key_path = in.key_path();
            index.unique = in.u8() != 0;
            index.multi_entry = in.u8() != 0;
            std::uint64_t const index_id = index.id;
            store.indexes.emplace(index_id, std::move(index));
        }
        std::uint64_t const record_count = in.u64();
        for (std::uint64_t r = 0; r < record_count && in.ok(); ++r) {
            Key key = in.key();
            Record record;
            record.value = in.bytes();
            std::uint32_t const lists = in.u32();
            for (std::uint32_t l = 0; l < lists && in.ok(); ++l) {
                std::uint64_t const index_id = in.u64();
                std::uint32_t const count = in.u32();
                std::vector<Key> keys;
                for (std::uint32_t k = 0; k < count && in.ok(); ++k)
                    keys.push_back(in.key());
                record.index_keys.emplace_back(index_id, std::move(keys));
            }
            if (in.ok())
                insert_record(store, key, std::move(record));
        }
        std::uint64_t const store_id = store.id;
        database.stores.emplace(store_id, std::move(store));
    }
    if (!in.ok() || !in.at_end())
        return std::nullopt;
    return database;
}

std::string file_name_for(std::u16string_view name)
{
    // The name's UTF-8, a lone surrogate as U+FFFD.
    std::string utf8;
    for (std::size_t i = 0; i < name.size(); ++i) {
        char32_t code_point = name[i];
        if (code_point >= 0xD800 && code_point <= 0xDBFF && i + 1 < name.size() && name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF) {
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (name[i + 1] - 0xDC00);
            ++i;
        } else if (code_point >= 0xD800 && code_point <= 0xDFFF) {
            code_point = 0xFFFD;
        }
        if (code_point < 0x80) {
            utf8.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (char const c : utf8) {
        auto const byte = static_cast<unsigned char>(c);
        bool const plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (plain) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0xF]);
        }
    }
    // The empty name is a name too.
    if (out.empty())
        out = "%";
    constexpr std::size_t longest = 180;
    if (out.size() > longest) {
        // FNV-1a over the whole name tells two long names apart.
        std::uint64_t hash = 14695981039346656037ULL;
        for (char const c : utf8) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
        std::string tail = "~";
        for (int shift = 60; shift >= 0; shift -= 4)
            tail.push_back(hex[(hash >> shift) & 0xF]);
        out = out.substr(0, longest) + tail;
    }
    return out + ".db";
}

// --- The storage ----------------------------------------------------------------------------------

Storage::Storage(std::filesystem::path directory)
    : m_directory(std::move(directory))
{
}

std::filesystem::path Storage::file_of(std::u16string const& name) const
{
    return m_directory / file_name_for(name);
}

namespace {

std::optional<Bytes> read_bytes(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}

Storage::Database& Storage::database(std::u16string const& name)
{
    auto found = m_databases.find(name);
    if (found != m_databases.end())
        return found->second;
    Database& database = m_databases[name];
    database.state.name = name;
    if (!m_directory.empty()) {
        if (std::optional<Bytes> const bytes = read_bytes(file_of(name))) {
            if (std::optional<DatabaseState> decoded = decode_database(*bytes); decoded && decoded->name == name) {
                database.state = std::move(*decoded);
                database.exists = true;
            }
        }
    }
    return database;
}

std::vector<std::pair<std::u16string, std::uint64_t>> Storage::databases()
{
    if (!m_directory.empty() && !m_listed) {
        // Every database written here in an earlier run, read in once.
        m_listed = true;
        std::error_code error;
        std::filesystem::directory_iterator it(m_directory, error);
        for (std::filesystem::directory_iterator const end; !error && it != end; it.increment(error)) {
            if (it->path().extension() != ".db")
                continue;
            std::optional<Bytes> const bytes = read_bytes(it->path());
            if (!bytes)
                continue;
            std::optional<DatabaseState> decoded = decode_database(*bytes);
            if (!decoded || m_databases.contains(decoded->name))
                continue;
            std::u16string const name = decoded->name;
            Database& database = m_databases[name];
            database.state = std::move(*decoded);
            database.exists = true;
        }
    }
    std::vector<std::pair<std::u16string, std::uint64_t>> out;
    for (auto const& [name, database] : m_databases) {
        if (database.exists)
            out.emplace_back(name, database.state.version);
    }
    return out;
}

void Storage::persist(Database& database)
{
    if (m_directory.empty() || !database.exists)
        return;
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    // Whole or not at all: the new file under another name, then moved over
    // the old one.
    std::filesystem::path const path = file_of(database.state.name);
    std::filesystem::path temporary = path;
    temporary += ".new";
    Bytes const bytes = encode_database(database.state);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            return;
    }
    std::filesystem::rename(temporary, path, error);
}

void Storage::erase(Database& database)
{
    std::u16string const name = database.state.name;
    database.state = DatabaseState {};
    database.state.name = name;
    database.exists = false;
    if (!m_directory.empty()) {
        std::error_code error;
        std::filesystem::remove(file_of(name), error);
    }
}

void Storage::enqueue(Database& database, std::uint64_t id, std::function<void()> start)
{
    database.queue.push_back(Database::Queued { id, std::move(start), false });
    if (database.queue.size() == 1) {
        database.queue.front().started = true;
        database.queue.front().start();
    }
}

void Storage::dequeue(Database& database, std::uint64_t id)
{
    std::erase_if(database.queue, [id](Database::Queued const& queued) { return queued.id == id; });
    database.close_watchers.erase(id);
    if (!database.queue.empty() && !database.queue.front().started) {
        database.queue.front().started = true;
        database.queue.front().start();
    }
}

std::shared_ptr<ConnectionEntry> Storage::connect(Database& database, std::uint64_t version)
{
    auto connection = std::make_shared<ConnectionEntry>();
    connection->id = next_serial();
    connection->version = version;
    database.connections.push_back(connection);
    return connection;
}

void Storage::request_close(Database& database, ConnectionEntry& connection)
{
    connection.close_pending = true;
    close_if_done(database, connection);
}

void Storage::close_if_done(Database& database, ConnectionEntry& connection)
{
    if (connection.closed || !connection.close_pending || connection.unfinished > 0)
        return;
    connection.closed = true;
    std::erase_if(database.connections, [&connection](std::shared_ptr<ConnectionEntry> const& open) { return open.get() == &connection; });
    for (auto const& [id, watcher] : database.close_watchers)
        watcher();
}

bool Storage::others_open(Database const& database, std::uint64_t except) const
{
    for (auto const& connection : database.connections) {
        if (connection->id != except && !connection->closed)
            return true;
    }
    return false;
}

namespace {

bool scopes_overlap(ScheduledTransaction const& a, ScheduledTransaction const& b)
{
    // An upgrade's scope is every store, those it makes among them.
    if (a.mode == Mode::VersionChange || b.mode == Mode::VersionChange)
        return true;
    for (std::u16string const& name : a.scope) {
        if (std::find(b.scope.begin(), b.scope.end(), name) != b.scope.end())
            return true;
    }
    return false;
}

}

void Storage::schedule(Database& database, std::shared_ptr<ScheduledTransaction> transaction)
{
    for (auto const& connection : database.connections) {
        if (connection->id == transaction->connection)
            ++connection->unfinished;
    }
    database.transactions.push_back(std::move(transaction));
    start_ready(database);
}

void Storage::finish(Database& database, ScheduledTransaction& transaction)
{
    if (transaction.finished)
        return;
    transaction.finished = true;
    std::erase_if(database.transactions, [&transaction](std::shared_ptr<ScheduledTransaction> const& t) { return t.get() == &transaction; });
    std::shared_ptr<ConnectionEntry> owner;
    for (auto const& connection : database.connections) {
        if (connection->id == transaction.connection) {
            --connection->unfinished;
            owner = connection;
        }
    }
    if (owner)
        close_if_done(database, *owner);
    start_ready(database);
}

void Storage::start_ready(Database& database)
{
    // Section 2.7.2: a read-only transaction waits for the read/write ones
    // made before it over any of its stores; a read/write one waits for
    // every one made before it over any of its stores.
    for (std::size_t i = 0; i < database.transactions.size(); ++i) {
        ScheduledTransaction& candidate = *database.transactions[i];
        if (candidate.started)
            continue;
        bool blocked = false;
        for (std::size_t j = 0; j < i && !blocked; ++j) {
            ScheduledTransaction const& earlier = *database.transactions[j];
            if (earlier.finished || !scopes_overlap(earlier, candidate))
                continue;
            if (candidate.mode != Mode::ReadOnly || earlier.mode != Mode::ReadOnly)
                blocked = true;
        }
        if (blocked)
            continue;
        candidate.started = true;
        if (candidate.on_start)
            candidate.on_start();
    }
}

}
