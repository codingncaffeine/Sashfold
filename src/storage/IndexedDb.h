#pragma once

// The Indexed Database API's storage (https://w3c.github.io/IndexedDB/):
// what an origin's databases hold, apart from any script. Keys and their
// order (section 7), key ranges, key paths as data, the object stores with
// their records and indexes, the undo log a read/write transaction keeps and
// the snapshot an upgrade keeps, the order transactions may start in, the
// connections to each database and the queue of open and delete requests;
// and the file a database is written to and read back from. The bindings
// (src/bindings/IndexedDb.cpp) evaluate key paths on script values and hand
// the keys and the serialized values here.
//
// One Storage is one origin's (and one container's), shared by every realm
// of that origin and by their workers, which may be on other threads: every
// member is used under its mutex. A value is kept as opaque bytes (the
// structured clone's serialization), and a record keeps the index keys the
// bindings extracted from it, so that deleting it touches exactly those.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::idb {

using Bytes = std::vector<std::uint8_t>;

// A key (section 7.1). The types in the order keys of different types
// compare: every number is less than every date, and so on to arrays.
struct Key {
    enum class Type : std::uint8_t { Number = 0, Date = 1, String = 2, Binary = 3, Array = 4 };
    Type type = Type::Number;
    double number = 0; // a number's value, a date's time value
    std::u16string string;
    Bytes binary;
    std::vector<Key> array;

    static Key from_number(double value);
    static Key from_date(double time_value);
    static Key from_string(std::u16string value);
    static Key from_binary(Bytes value);
    static Key from_array(std::vector<Key> value);
};

// Section 7.1, "compare two keys": negative, zero or positive.
int compare(Key const& a, Key const& b);
inline bool keys_equal(Key const& a, Key const& b) { return compare(a, b) == 0; }
struct KeyLess {
    bool operator()(Key const& a, Key const& b) const { return compare(a, b) < 0; }
};

// A key range (section 7.3): a bound on either side or none, each open or
// closed.
struct KeyRange {
    std::optional<Key> lower;
    std::optional<Key> upper;
    bool lower_open = false;
    bool upper_open = false;

    static KeyRange only(Key key);
    static KeyRange unbounded() { return {}; }
    bool includes(Key const& key) const;
    bool below_lower(Key const& key) const; // key is before the range starts
    bool above_upper(Key const& key) const; // key is past the range's end
};

// A key path (section 7.5): none, one string, or a list of strings.
struct KeyPath {
    enum class Kind : std::uint8_t { None, String, Array };
    Kind kind = Kind::None;
    std::vector<std::u16string> paths; // one for String

    bool operator==(KeyPath const&) const = default;
};

// A valid key path string: empty, or identifiers (ECMAScript IdentifierName)
// joined by periods.
bool is_valid_key_path_string(std::u16string_view);

// An index record: the index key, then the primary key, in the order the
// index keeps them (section 2.4), and a place in that order between records
// for a search: before (-1) or after (+1) every record of an index key, or
// exactly at a pair.
using IndexEntry = std::pair<Key, Key>;
struct IndexProbe {
    Key const* key = nullptr;
    Key const* primary = nullptr; // null: at `bias` among all of key's records
    int bias = 0;
};
struct IndexEntryLess {
    using is_transparent = void;
    bool operator()(IndexEntry const& a, IndexEntry const& b) const;
    bool operator()(IndexEntry const& a, IndexProbe const& b) const;
    bool operator()(IndexProbe const& a, IndexEntry const& b) const;
};

struct IndexState {
    std::uint64_t id = 0;
    std::u16string name;
    KeyPath key_path;
    bool unique = false;
    bool multi_entry = false;
    std::set<IndexEntry, IndexEntryLess> entries;
};

// The keys a record has in each index of its store, by the index's id.
using IndexKeys = std::vector<std::pair<std::uint64_t, std::vector<Key>>>;

struct Record {
    Bytes value;
    IndexKeys index_keys;
};

struct StoreState {
    std::uint64_t id = 0;
    std::u16string name;
    KeyPath key_path;
    bool auto_increment = false;
    double current_number = 1; // the key generator's (section 2.11)
    std::map<Key, Record, KeyLess> records;
    std::map<std::uint64_t, IndexState> indexes; // by id

    IndexState* index_named(std::u16string_view name);
    std::vector<std::u16string> index_names() const; // sorted, as a DOMStringList lists them
};

struct DatabaseState {
    std::u16string name;
    std::uint64_t version = 0;
    std::uint64_t next_id = 1; // for stores and indexes
    std::map<std::uint64_t, StoreState> stores; // by id

    StoreState* store_named(std::u16string_view name);
    StoreState* store(std::uint64_t id);
    std::vector<std::u16string> store_names() const; // sorted by code unit
};

// What a read/write transaction has done, undone in reverse on an abort.
class UndoLog {
public:
    void add(std::function<void()> step) { m_steps.push_back(std::move(step)); }
    void revert();
    void clear() { m_steps.clear(); }

private:
    std::vector<std::function<void()>> m_steps;
};

// The record operations, as the bindings' requests run them. Index keys come
// from the bindings; a unique index that already holds one of them for
// another record refuses the whole record, which is then not stored at all.
bool violates_unique_index(StoreState const&, Key const& primary, IndexKeys const&);
void put_record(StoreState&, Key const& primary, Record record, UndoLog*);
std::size_t delete_records(StoreState&, KeyRange const&, UndoLog*);
void clear_records(StoreState&, UndoLog*);
// The key generator (section 2.11): the next key, or none past 2^53; and a
// key a record was stored under moving it on.
std::optional<double> generate_key(StoreState&, UndoLog*);
void possibly_update_key_generator(StoreState&, Key const&, UndoLog*);
// An index made over a store that already has records: the records' keys for
// it come from the bindings, record by record; false when a unique index
// would hold one key twice (the index is left empty).
bool populate_index(StoreState&, IndexState&, std::vector<std::pair<Key, std::vector<Key>>> const& keys_by_record);

// The file a database is kept in: a magic line, the format's number, then
// the database length-prefixed field by field. Nothing outside it is needed
// to read it back.
Bytes encode_database(DatabaseState const&);
std::optional<DatabaseState> decode_database(std::span<std::uint8_t const>);
// A name as a file name: its UTF-8 with everything but letters, digits, '-'
// and '_' percent-encoded, cut short with a hash of the whole when it would
// be too long for a file system.
std::string file_name_for(std::u16string_view name);

enum class Mode : std::uint8_t { ReadOnly, ReadWrite, VersionChange };

// A transaction as the scheduler sees it (section 2.7.2): its scope and mode,
// the order it was made in, and what to call when it may start.
struct ScheduledTransaction {
    std::uint64_t serial = 0;
    std::uint64_t connection = 0;
    Mode mode = Mode::ReadOnly;
    std::vector<std::u16string> scope;
    bool started = false;
    bool finished = false;
    std::function<void()> on_start; // posts to the owner's loop; never runs script itself
};

// A connection (section 2.9) as the database sees it.
struct ConnectionEntry {
    std::uint64_t id = 0;
    std::uint64_t version = 0;
    bool close_pending = false;
    bool closed = false;
    int unfinished = 0; // transactions made on it and not finished
    // Posts a versionchange event to the connection's realm.
    std::function<void(std::uint64_t old_version, std::optional<std::uint64_t> new_version)> on_versionchange;
};

class Storage {
public:
    // An empty directory keeps everything in memory.
    explicit Storage(std::filesystem::path directory = {});
    Storage(Storage const&) = delete;
    Storage& operator=(Storage const&) = delete;

    std::mutex& mutex() { return m_mutex; }
    std::filesystem::path const& directory() const { return m_directory; }

    struct Database {
        DatabaseState state;
        bool exists = false;
        std::vector<std::shared_ptr<ConnectionEntry>> connections; // open, not closed
        std::vector<std::shared_ptr<ScheduledTransaction>> transactions; // unfinished, in creation order
        // The connection queue (section 2.8.1): open and delete requests in
        // order, each started when it reaches the front.
        struct Queued {
            std::uint64_t id = 0;
            std::function<void()> start;
            bool started = false;
        };
        std::deque<Queued> queue;
        // Called (posting a task) whenever a connection closes: the request
        // at the front may be waiting for that.
        std::map<std::uint64_t, std::function<void()>> close_watchers;
    };

    // All under the mutex.
    Database& database(std::u16string const& name); // read from its file the first time
    std::vector<std::pair<std::u16string, std::uint64_t>> databases(); // those that exist, by name
    void persist(Database&); // writes its file, when there is a directory
    void erase(Database&); // deleted: its state emptied and its file removed

    std::uint64_t next_serial() { return m_next_serial++; }

    // The connection queue.
    void enqueue(Database&, std::uint64_t id, std::function<void()> start);
    void dequeue(Database&, std::uint64_t id);

    // Connections.
    std::shared_ptr<ConnectionEntry> connect(Database&, std::uint64_t version);
    void request_close(Database&, ConnectionEntry&); // sets close pending; closes when its transactions are done
    bool others_open(Database const&, std::uint64_t except) const;

    // Transactions.
    void schedule(Database&, std::shared_ptr<ScheduledTransaction>);
    void finish(Database&, ScheduledTransaction&);

private:
    void start_ready(Database&);
    void close_if_done(Database&, ConnectionEntry&);
    std::filesystem::path file_of(std::u16string const& name) const;

    std::mutex m_mutex;
    std::filesystem::path m_directory;
    std::map<std::u16string, Database> m_databases;
    bool m_listed = false;
    std::uint64_t m_next_serial = 1;
};

}
