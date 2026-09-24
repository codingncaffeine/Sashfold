#include "Test.h"

#include "bindings/Realm.h"
#include "dom/Dom.h"
#include "storage/IndexedDb.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The Indexed Database API: the order of keys (section 7.1), key path
// strings, the record store with its indexes, undo log and key generator,
// the file a database is kept in and read back from, and the API through a
// page's script: key paths evaluated on the values stored, the requests'
// events in order, a database that outlives its page in a file. The realm
// runs under heap stress, so an object the bindings forgot to root fails at
// once.

using namespace sashfold;
using idb::Key;

namespace {

Key number(double value) { return Key::from_number(value); }
Key date(double value) { return Key::from_date(value); }
Key string(std::u16string value) { return Key::from_string(std::move(value)); }
Key binary(std::vector<std::uint8_t> value) { return Key::from_binary(std::move(value)); }
Key array(std::vector<Key> value) { return Key::from_array(std::move(value)); }

void test_key_order()
{
    // Types first: number < date < string < binary < array.
    double const infinity = std::numeric_limits<double>::infinity();
    std::vector<Key> const ascending = {
        number(-infinity),
        number(-1),
        number(0),
        number(1e300),
        number(infinity),
        date(-5),
        date(0),
        string(u""),
        string(u"a"),
        string(u"b"),
        string(u"\xD800"), // code units compare as numbers: a lone surrogate before U+FFFF
        string(u"\xFFFF"),
        binary({}),
        binary({ 0x00 }),
        binary({ 0x00, 0x00 }),
        binary({ 0x01 }),
        binary({ 0xFF }),
        array({}),
        array({ number(1) }),
        array({ number(1), number(0) }),
        array({ date(0) }),
        array({ array({}) }),
    };
    for (std::size_t i = 0; i < ascending.size(); ++i) {
        CHECK_EQ(idb::compare(ascending[i], ascending[i]), 0);
        for (std::size_t j = i + 1; j < ascending.size(); ++j) {
            CHECK_EQ(idb::compare(ascending[i], ascending[j]), -1);
            CHECK_EQ(idb::compare(ascending[j], ascending[i]), 1);
        }
    }
    // A number and a date of the same value are still different keys.
    CHECK(!idb::keys_equal(number(0), date(0)));
    CHECK(idb::keys_equal(array({ string(u"x"), binary({ 1 }) }), array({ string(u"x"), binary({ 1 }) })));

    idb::KeyRange range;
    range.lower = number(1);
    range.upper = number(5);
    range.lower_open = true;
    CHECK(!range.includes(number(1)));
    CHECK(range.includes(number(1.5)));
    CHECK(range.includes(number(5)));
    CHECK(!range.includes(string(u"1")));
    CHECK(idb::KeyRange::only(string(u"k")).includes(string(u"k")));
}

void test_key_path_strings()
{
    for (std::u16string_view const valid : { u"", u"a", u"a.b.c", u"$x", u"_y1", u"\xE9t\xE9", u"length" })
        CHECK(idb::is_valid_key_path_string(valid));
    for (std::u16string_view const invalid : { u"a..b", u".a", u"a.", u"1a", u"a b", u"a-b", u"a[0]" })
        CHECK(!idb::is_valid_key_path_string(invalid));
}

idb::StoreState make_store()
{
    idb::StoreState store;
    store.id = 1;
    store.name = u"people";
    store.key_path = idb::KeyPath { idb::KeyPath::Kind::String, { u"id" } };
    store.auto_increment = true;
    idb::IndexState index;
    index.id = 2;
    index.name = u"by_name";
    index.key_path = idb::KeyPath { idb::KeyPath::Kind::String, { u"name" } };
    index.unique = true;
    store.indexes.emplace(2, std::move(index));
    return store;
}

idb::Record record(std::string_view text, std::u16string name)
{
    idb::Record out;
    out.value.assign(text.begin(), text.end());
    out.index_keys.emplace_back(2, std::vector<Key> { string(std::move(name)) });
    return out;
}

void test_record_store()
{
    idb::StoreState store = make_store();
    idb::UndoLog undo;
    std::optional<double> const first = idb::generate_key(store, &undo);
    CHECK(first && *first == 1);
    idb::put_record(store, number(1), record("ada", u"Ada"), &undo);
    idb::put_record(store, number(2), record("bo", u"Bo"), &undo);
    CHECK_EQ(store.records.size(), std::size_t(2));
    CHECK_EQ(store.indexes.at(2).entries.size(), std::size_t(2));

    // A unique index refuses a second record with its key, but not the same
    // record stored again.
    idb::IndexKeys const ada_again { { 2, { string(u"Ada") } } };
    CHECK(idb::violates_unique_index(store, number(3), ada_again));
    CHECK(!idb::violates_unique_index(store, number(1), ada_again));

    // Replacing a record takes its old index entries with it.
    idb::put_record(store, number(1), record("ada2", u"Ada Lovelace"), &undo);
    CHECK_EQ(store.indexes.at(2).entries.size(), std::size_t(2));
    CHECK(!idb::violates_unique_index(store, number(9), ada_again));

    // An explicit key moves the generator on; 2^53 and past exhaust it.
    idb::possibly_update_key_generator(store, number(10.5), &undo);
    CHECK_EQ(store.current_number, 11.0);
    idb::possibly_update_key_generator(store, number(9007199254740992.0), &undo);
    CHECK(!idb::generate_key(store, &undo));

    // A range deleted, then everything undone: as it was before the log.
    idb::KeyRange range;
    range.lower = number(2);
    CHECK_EQ(idb::delete_records(store, range, &undo), std::size_t(1));
    CHECK_EQ(store.records.size(), std::size_t(1));
    undo.revert();
    CHECK_EQ(store.records.size(), std::size_t(0));
    CHECK_EQ(store.indexes.at(2).entries.size(), std::size_t(0));
    CHECK_EQ(store.current_number, 1.0);

    // An index made over records that hold one key twice cannot be unique.
    idb::put_record(store, number(1), idb::Record { { 'x' }, {} }, nullptr);
    idb::put_record(store, number(2), idb::Record { { 'y' }, {} }, nullptr);
    idb::IndexState twice;
    twice.id = 3;
    twice.unique = true;
    CHECK(!idb::populate_index(store, twice, { { number(1), { string(u"same") } }, { number(2), { string(u"same") } } }));
    twice.unique = false;
    CHECK(idb::populate_index(store, twice, { { number(1), { string(u"same") } }, { number(2), { string(u"same") } } }));
    CHECK_EQ(twice.entries.size(), std::size_t(2));
}

void test_the_file_format()
{
    idb::DatabaseState database;
    database.name = u"library \xD83D\xDCDA";
    database.version = 7;
    database.next_id = 4;
    idb::StoreState store = make_store();
    store.current_number = 3;
    idb::put_record(store, number(1), record("one", u"Ada"), nullptr);
    idb::put_record(store, array({ date(5), binary({ 0, 255 }) }), record("two", u"Bo"), nullptr);
    database.stores.emplace(1, std::move(store));

    idb::Bytes const bytes = idb::encode_database(database);
    std::optional<idb::DatabaseState> const back = idb::decode_database(bytes);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->name == database.name);
        CHECK_EQ(back->version, std::uint64_t(7));
        CHECK_EQ(back->next_id, std::uint64_t(4));
        idb::StoreState const& read = back->stores.at(1);
        CHECK(read.name == u"people");
        CHECK(read.key_path == database.stores.at(1).key_path);
        CHECK(read.auto_increment);
        CHECK_EQ(read.current_number, 3.0);
        CHECK_EQ(read.records.size(), std::size_t(2));
        auto const found = read.records.find(array({ date(5), binary({ 0, 255 }) }));
        CHECK(found != read.records.end());
        if (found != read.records.end())
            CHECK(std::string(found->second.value.begin(), found->second.value.end()) == "two");
        CHECK_EQ(read.indexes.at(2).entries.size(), std::size_t(2));
        CHECK(read.indexes.at(2).unique);
        // The same database writes the same bytes again.
        CHECK(idb::encode_database(*back) == bytes);
    }
    // Anything cut short or changed at its head is not a database.
    CHECK(!idb::decode_database(std::span<std::uint8_t const>(bytes.data(), bytes.size() - 1)));
    idb::Bytes damaged = bytes;
    damaged[0] = 'X';
    CHECK(!idb::decode_database(damaged));
    idb::Bytes longer = bytes;
    longer.push_back(0);
    CHECK(!idb::decode_database(longer));

    // File names keep the name apart from the file system's own syntax.
    CHECK(idb::file_name_for(u"notes") == "notes.db");
    CHECK(idb::file_name_for(u"../x") == "%2E%2E%2Fx.db");
    CHECK(idb::file_name_for(u"") == "%.db");
    // A long name is cut short, with a hash, well inside a file system's 255.
    CHECK(idb::file_name_for(std::u16string(400, u'a')).size() <= 200);
    CHECK(idb::file_name_for(std::u16string(400, u'a')) != idb::file_name_for(std::u16string(401, u'a')));
}

void test_a_storage_reads_back_its_files()
{
    std::filesystem::path const folder = std::filesystem::temp_directory_path() / "sashfold-test-indexeddb";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    {
        idb::Storage storage(folder);
        std::lock_guard<std::mutex> const lock(storage.mutex());
        idb::Storage::Database& database = storage.database(u"kept");
        CHECK(!database.exists);
        database.exists = true;
        database.state.version = 3;
        idb::StoreState store = make_store();
        idb::put_record(store, number(1), record("value", u"Ada"), nullptr);
        database.state.stores.emplace(1, std::move(store));
        storage.persist(database);
        idb::Storage::Database& other = storage.database(u"gone");
        other.exists = true;
        storage.persist(other);
        storage.erase(other);
    }
    CHECK(std::filesystem::exists(folder / "kept.db"));
    CHECK(!std::filesystem::exists(folder / "gone.db"));
    {
        // A second storage over the same folder: a second run.
        idb::Storage storage(folder);
        std::lock_guard<std::mutex> const lock(storage.mutex());
        std::vector<std::pair<std::u16string, std::uint64_t>> const listed = storage.databases();
        CHECK_EQ(listed.size(), std::size_t(1));
        if (!listed.empty()) {
            CHECK(listed.front().first == u"kept");
            CHECK_EQ(listed.front().second, std::uint64_t(3));
        }
        idb::Storage::Database& database = storage.database(u"kept");
        CHECK(database.exists);
        CHECK_EQ(database.state.stores.at(1).records.size(), std::size_t(1));
        CHECK_EQ(database.state.stores.at(1).indexes.at(2).entries.size(), std::size_t(1));
    }
    {
        // With no folder nothing is written.
        idb::Storage storage;
        std::lock_guard<std::mutex> const lock(storage.mutex());
        CHECK(storage.databases().empty());
    }
    std::filesystem::remove_all(folder, error);
}

// A page's script against a realm of its own, pumped as the shell pumps it.
struct Page {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    double clock = 1000;

    explicit Page(bindings::HostHooks hooks = {})
    {
        hooks.now = [this] { return clock; };
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url("https://example.test/page.html"), std::move(hooks));
        realm->interpreter().heap().set_stress(true);
    }
    void run(std::string_view source) { CHECK(realm->run(source, "<test>").ok); }
    void settle()
    {
        for (int i = 0; i < 200 && realm->has_pending_timers(); ++i) {
            clock += 1;
            realm->run_pending();
        }
    }
    std::string text(std::string_view expression)
    {
        js::Outcome const outcome = realm->run(expression, "<test>");
        if (!outcome.ok || !outcome.value.is_string())
            return "<" + realm->interpreter().describe(outcome.value) + ">";
        return outcome.value.as_string()->to_utf8();
    }
};

void test_the_api_in_a_page()
{
    Page page;
    CHECK_EQ(page.text("typeof indexedDB + ' ' + (indexedDB === indexedDB) + ' ' + (indexedDB instanceof IDBFactory)"),
        std::string("object true true"));
    // cmp: the key order, and an invalid key is a DataError.
    CHECK_EQ(page.text("[indexedDB.cmp(1, new Date(0)), indexedDB.cmp('a', [0]), indexedDB.cmp([1, [2]], [1, [2]]),"
                       " indexedDB.cmp(new Uint8Array([1]).buffer, 'z')].join()"),
        std::string("-1,-1,0,1"));
    CHECK_EQ(page.text("try { indexedDB.cmp(NaN, 1); 'no' } catch (e) { e.name }"), std::string("DataError"));
    CHECK_EQ(page.text("try { indexedDB.cmp({}, 1); 'no' } catch (e) { e.name }"), std::string("DataError"));

    // Key paths: nested, arrays of paths, a generated key put in its place,
    // a multiEntry index; the events in order.
    page.run(R"JS(
        var log = [];
        var open = indexedDB.open('db', 2);
        open.onupgradeneeded = function (e) {
            log.push('upgrade ' + e.oldVersion + '>' + e.newVersion);
            var db = open.result;
            var people = db.createObjectStore('people', { keyPath: 'info.id', autoIncrement: true });
            people.createIndex('pair', ['first', 'last']);
            people.createIndex('tags', 'tags', { multiEntry: true });
            people.add({ first: 'Ada', last: 'L', tags: ['x', 'y', 'x'], info: {} });
            people.add({ first: 'Bo', last: 'M', tags: ['y'], info: { id: 10 } });
            people.add({ first: 'Cy', last: 'N', tags: [], info: {} });
            try { db.createObjectStore('people'); } catch (e) { log.push('again ' + e.name); }
            try { db.createObjectStore('bad', { keyPath: 'a..b' }); } catch (e) { log.push('path ' + e.name); }
        };
        open.onsuccess = function () {
            var db = open.result;
            log.push('open ' + db.version + ' ' + Array.from(db.objectStoreNames).join());
            var store = db.transaction('people').objectStore('people');
            store.getAllKeys().onsuccess = function (e) { log.push('keys ' + e.target.result.join()); };
            store.get(11).onsuccess = function (e) { log.push('generated ' + e.target.result.first + ' ' + e.target.result.info.id); };
            store.index('pair').getKey(['Bo', 'M']).onsuccess = function (e) { log.push('pair ' + e.target.result); };
            store.index('tags').count('y').onsuccess = function (e) { log.push('y ' + e.target.result); };
            store.index('tags').count().onsuccess = function (e) { log.push('tags ' + e.target.result); };
            store.openCursor(IDBKeyRange.lowerBound(10, true), 'prev').onsuccess = function (e) {
                var cursor = e.target.result;
                if (cursor) { log.push('cursor ' + cursor.key + ' ' + cursor.value.first); cursor.continue(); }
                else log.push('cursor end');
            };
            store.transaction.oncomplete = function () { log.push('complete'); db.close(); };
        };
    )JS");
    page.settle();
    CHECK_EQ(page.text("log.join('|')"),
        std::string("upgrade 0>2|again ConstraintError|path SyntaxError|open 2 people|keys 1,10,11|generated Cy 11|pair 10|y 2|tags 3"
                    "|cursor 11 Cy|cursor end|complete"));

    // A request error aborts its transaction unless it is prevented; the
    // abort undoes what the transaction did.
    page.run(R"JS(
        var log2 = [];
        var reopen = indexedDB.open('db');
        reopen.onsuccess = function () {
            var db = reopen.result;
            // An empty scope is judged before the mode.
            try { db.transaction([], 'versionchange'); } catch (e) { log2.push('empty ' + e.name); }
            var tx = db.transaction('people', 'readwrite');
            var store = tx.objectStore('people');
            store.put({ first: 'Di', info: { id: 20 } });
            store.add({ first: 'Ed', info: { id: 1 } }).onerror = function (e) { log2.push('add ' + e.target.error.name); };
            tx.onabort = function () {
                log2.push('abort ' + tx.error.name);
                db.transaction('people').objectStore('people').count().onsuccess = function (e) { log2.push('count ' + e.target.result); db.close(); };
            };
        };
    )JS");
    page.settle();
    CHECK_EQ(page.text("log2.join('|')"), std::string("empty InvalidAccessError|add ConstraintError|abort ConstraintError|count 3"));
}

void test_a_database_outlives_its_page()
{
    // Two pages, one after the other, each with a storage of its own over
    // the same folder: what the first committed, the second reads.
    std::filesystem::path const folder = std::filesystem::temp_directory_path() / "sashfold-test-indexeddb-pages";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    auto const hooks_over = [&folder] {
        bindings::HostHooks hooks;
        auto storage = std::make_shared<idb::Storage>(folder);
        hooks.indexed_db = [storage](std::string const&) { return storage; };
        return hooks;
    };
    {
        Page first(hooks_over());
        first.run(R"JS(
            var done = '';
            var open = indexedDB.open('kept', 1);
            open.onupgradeneeded = function () { open.result.createObjectStore('s').put({ when: new Date(5), list: [1, 'two'] }, 'k'); };
            open.onsuccess = function () { done = 'ok'; open.result.close(); };
        )JS");
        first.settle();
        CHECK_EQ(first.text("done"), std::string("ok"));
    }
    CHECK(std::filesystem::exists(folder / "kept.db"));
    {
        Page second(hooks_over());
        second.run(R"JS(
            var seen = '';
            indexedDB.databases().then(function (list) { seen += list.map(function (d) { return d.name + '@' + d.version; }).join() + ' '; });
            var open = indexedDB.open('kept');
            open.onupgradeneeded = function () { seen += 'upgraded?'; };
            open.onsuccess = function () {
                open.result.transaction('s').objectStore('s').get('k').onsuccess = function (e) {
                    var v = e.target.result;
                    seen += (v.when instanceof Date) + ' ' + v.when.getTime() + ' ' + v.list.join('/');
                    open.result.close();
                };
            };
        )JS");
        second.settle();
        CHECK_EQ(second.text("seen"), std::string("kept@1 true 5 1/two"));
    }
    std::filesystem::remove_all(folder, error);
}

}

int main()
{
    test_key_order();
    test_key_path_strings();
    test_record_store();
    test_the_file_format();
    test_a_storage_reads_back_its_files();
    test_the_api_in_a_page();
    test_a_database_outlives_its_page();
    return test::report("test_indexeddb");
}
