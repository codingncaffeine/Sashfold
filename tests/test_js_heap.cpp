#include "Test.h"

#include "js/Heap.h"
#include "js/Strings.h"
#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace sashfold;
using namespace std::literals;

namespace {

// A root outside the heap: the shape of the interpreter's root stack and
// of the bindings' wrapper table as the collector sees them.
struct TestRoots : js::RootProvider {
    js::Value value;
    js::Cell* cell = nullptr;
    void trace_roots(js::Tracer& tracer) override
    {
        tracer.visit(value);
        tracer.visit(cell);
    }
};

// A cell that keeps another cell alive only through trace(): the shape
// of an object holding a string.
struct Box : js::Cell {
    js::JsString* inner = nullptr;
    void trace(js::Tracer& tracer) override { tracer.visit(inner); }
};

// A link in a chain, for the deep and the cyclic cases.
struct Link : js::Cell {
    js::Cell* next = nullptr;
    void trace(js::Tracer& tracer) override { tracer.visit(next); }
};

double nan()
{
    return std::numeric_limits<double>::quiet_NaN();
}

// A RootProvider that counts how often the collector asked it, so a
// removed provider is proven silent rather than merely harmless.
struct CountingRoots : js::RootProvider {
    int calls = 0;
    void trace_roots(js::Tracer&) override { ++calls; }
};

std::string ts(double x, int radix = 10)
{
    return js::utf8_from_utf16(js::number_to_string(x, radix));
}

std::string fixed(double x, int fraction_digits)
{
    return js::utf8_from_utf16(js::number_to_fixed(x, fraction_digits));
}

std::string exponential(double x, int fraction_digits)
{
    return js::utf8_from_utf16(js::number_to_exponential(x, fraction_digits));
}

std::string precision(double x, int digits)
{
    return js::utf8_from_utf16(js::number_to_precision(x, digits));
}

double pint(std::string_view utf8, int radix)
{
    return js::parse_int(js::utf16_from_utf8(utf8), radix);
}

double pfloat(std::string_view utf8)
{
    return js::parse_float(js::utf16_from_utf8(utf8));
}

double stn(std::string_view utf8)
{
    return js::string_to_number(js::utf16_from_utf8(utf8));
}

bool is_nan(double x)
{
    return x != x;
}

}

int main()
{
    // A fresh heap: every well-known atom and symbol is filled in, is an
    // atom, and survives a collection with no roots registered at all.
    {
        js::Heap heap;
        js::WellKnownAtoms const& a = heap.atoms();
        js::JsString* const strings[] = {
            a.empty, a.length, a.prototype, a.constructor, a.name, a.message, a.cause, a.stack, a.value,
            a.get, a.set, a.writable, a.enumerable, a.configurable, a.to_string, a.value_of, a.to_json,
            a.arguments, a.callee, a.caller, a.index, a.input, a.groups, a.last_index, a.source, a.flags,
            a.global, a.undefined, a.null, a.true_, a.false_, a.object, a.function, a.number, a.string,
            a.boolean, a.symbol, a.nan, a.infinity, a.negative_infinity, a.zero, a.eval, a.default_, a.proto,
            a.anonymous, a.object_object, a.error, a.done, a.next, a.description, a.raw, a.comma
        };
        for (js::JsString* const s : strings) {
            CHECK(s != nullptr);
            CHECK(s != nullptr && s->is_atom());
        }
        CHECK(a.empty->is_empty());
        CHECK(a.length->equals(u"length"));
        CHECK(a.to_string->equals(u"toString"));
        CHECK(a.value_of->equals(u"valueOf"));
        CHECK(a.to_json->equals(u"toJSON"));
        CHECK(a.last_index->equals(u"lastIndex"));
        CHECK(a.true_->equals(u"true"));
        CHECK(a.false_->equals(u"false"));
        CHECK(a.nan->equals(u"NaN"));
        CHECK(a.infinity->equals(u"Infinity"));
        CHECK(a.negative_infinity->equals(u"-Infinity"));
        CHECK(a.zero->equals(u"0"));
        CHECK(a.default_->equals(u"default"));
        CHECK(a.proto->equals(u"__proto__"));
        CHECK(a.object_object->equals(u"[object Object]"));
        CHECK(a.error->equals(u"Error"));
        CHECK(a.comma->equals(u","));

        js::Symbol* const symbols[] = {
            a.symbol_to_primitive, a.symbol_to_string_tag, a.symbol_iterator, a.symbol_has_instance,
            a.symbol_is_concat_spreadable
        };
        for (js::Symbol* const s : symbols) {
            CHECK(s != nullptr);
            CHECK(s != nullptr && s->description() != nullptr && s->description()->is_atom());
        }
        CHECK(a.symbol_to_primitive->description()->equals(u"Symbol.toPrimitive"));
        CHECK(a.symbol_to_string_tag->description()->equals(u"Symbol.toStringTag"));
        CHECK(a.symbol_iterator->description()->equals(u"Symbol.iterator"));
        CHECK(a.symbol_has_instance->description()->equals(u"Symbol.hasInstance"));
        CHECK(a.symbol_is_concat_spreadable->description()->equals(u"Symbol.isConcatSpreadable"));

        std::size_t const base = heap.cell_count();
        CHECK(base >= 57);
        CHECK_EQ(heap.collections(), std::size_t { 0 });
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
        CHECK_EQ(heap.collections(), std::size_t { 1 });
        CHECK(a.length->equals(u"length"));
        CHECK(a.symbol_iterator->description()->equals(u"Symbol.iterator"));
        // The well-known atoms are the atom table's: asking again is a hit.
        CHECK(heap.atom(u"length") == a.length);
        CHECK(heap.atom(std::string_view("Symbol.iterator")) == a.symbol_iterator->description());
        CHECK_EQ(heap.cell_count(), base);
    }

    // cell_count follows allocation; a collection frees what nothing
    // reaches.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        std::size_t const bytes = heap.bytes_allocated();
        js::JsString* s1 = heap.string(u"one"sv);
        js::JsString* s2 = heap.string(std::string_view("two"));
        js::JsString* s3 = heap.string(u'3');
        js::JsString* s4 = heap.string(std::u16string(u"four"));
        CHECK_EQ(heap.cell_count(), base + 4);
        CHECK(s1->equals(u"one"));
        CHECK(s2->equals(u"two"));
        CHECK(s3->equals(u"3"));
        CHECK(s4->equals(u"four"));
        CHECK_EQ(s4->length(), std::size_t { 4 });
        CHECK(!s1->is_atom());
        CHECK(heap.bytes_allocated() >= bytes + 4 * sizeof(js::JsString) + 2 * (3 + 3 + 1 + 4));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
        CHECK(heap.bytes_allocated() < bytes + sizeof(js::JsString));
    }

    // A RootProvider keeps what it traces; removing it stops that.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        TestRoots roots;
        heap.add_root_provider(&roots);
        heap.add_root_provider(&roots); // twice is once
        js::JsString* kept = heap.string(u"kept"sv);
        roots.value = js::Value::string(kept);
        heap.string(u"dropped"sv);
        CHECK_EQ(heap.cell_count(), base + 2);
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(kept->equals(u"kept"));
        roots.value = js::Value::number(1);
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
        heap.remove_root_provider(&roots);
        roots.value = js::Value::string(heap.string(u"unrooted now"sv));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
    }

    // A Persistent roots its value for as long as the handle exists, and
    // a move carries the registration to the new handle.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        js::Persistent handle(heap, js::Value::string(heap.string(u"persist"sv)));
        CHECK(&handle.heap() == &heap);
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(handle.value().as_string()->equals(u"persist"));

        js::Persistent moved(std::move(handle));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(moved.value().as_string()->equals(u"persist"));

        js::Persistent other(heap, js::Value::string(heap.string(u"other"sv)));
        CHECK_EQ(heap.cell_count(), base + 2);
        other = std::move(moved);
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(other.value().as_string()->equals(u"persist"));

        other.set(js::Value::boolean(true));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);

        // An empty handle roots nothing and is harmless.
        js::Persistent empty(heap);
        CHECK(empty.value().is_undefined());
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
    }
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        {
            js::Persistent scoped(heap, js::Value::string(heap.string(u"scoped"sv)));
            heap.collect();
            CHECK_EQ(heap.cell_count(), base + 1);
        }
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
    }

    // Atoms are permanent and unique: equal contents are one cell, and a
    // plain string with the same contents is a different cell.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        js::JsString* foo = heap.atom(u"foo");
        CHECK(foo->is_atom());
        CHECK_EQ(heap.cell_count(), base + 1);
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(foo->equals(u"foo"));
        CHECK(heap.atom(std::string_view("foo")) == foo);
        CHECK(heap.atom(std::u16string_view(u"foo")) == foo);
        CHECK_EQ(heap.cell_count(), base + 1);

        js::JsString* plain = heap.string(u"foo"sv);
        CHECK(plain != foo);
        CHECK(!plain->is_atom());
        CHECK(plain->equals(*foo));
        CHECK(foo->equals(*plain));
        CHECK(plain->hash() == foo->hash());
        CHECK(heap.atom(plain) == foo); // the atom for an existing string's contents
        CHECK(heap.atom(foo) == foo); // already an atom: the same pointer
        CHECK_EQ(heap.cell_count(), base + 2);

        js::JsString* bar = heap.atom(u"bar");
        CHECK(bar != foo);
        CHECK(!foo->equals(*bar));
        CHECK(!bar->equals(u"foo"));
        CHECK(heap.atom(u"") == heap.atoms().empty);

        // Non-ASCII and non-BMP contents intern by code units, whichever
        // encoding they arrive in.
        js::JsString* smile = heap.atom(u"\xD83D\xDE00");
        CHECK(heap.atom(std::string_view("\xF0\x9F\x98\x80")) == smile);
        CHECK_EQ(smile->length(), std::size_t { 2 });
        js::JsString* lone = heap.atom(u"\xD800");
        CHECK(heap.atom(std::string_view("\xED\xA0\x80")) == lone); // WTF-8 in
        CHECK(lone->to_utf8() == "\xED\xA0\x80"); // and out
        CHECK(smile->to_utf8() == "\xF0\x9F\x98\x80");

        heap.collect();
        CHECK(foo->equals(u"foo"));
        CHECK(heap.atom(u"foo") == foo);
    }

    // key(): an index when the name is a canonical array index, else the
    // atom; the numeric overloads agree with the string ones.
    {
        js::Heap heap;
        js::PropertyKey k = heap.key(u"0");
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 0 });
        k = heap.key(u"42");
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 42 });
        k = heap.key(u"4294967294");
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 4294967294u });
        k = heap.key(u"4294967295");
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"4294967295"));
        k = heap.key(u"01");
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"01"));
        k = heap.key(u"-1");
        CHECK(k.is_atom());
        k = heap.key(u"1.0");
        CHECK(k.is_atom());
        k = heap.key(u" 1");
        CHECK(k.is_atom());
        k = heap.key(u"");
        CHECK(k.is_atom());
        CHECK(k.as_atom() == heap.atoms().empty);
        k = heap.key(u"length");
        CHECK(k.is_atom());
        CHECK(k.as_atom() == heap.atoms().length);
        k = heap.key(std::string_view("7"));
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 7 });
        k = heap.key(std::string_view("name"));
        CHECK(k.is_atom());
        CHECK(k.as_atom() == heap.atoms().name);

        js::JsString* twelve = heap.string(u"12"sv);
        CHECK(twelve->as_array_index().has_value());
        CHECK_EQ(*twelve->as_array_index(), std::uint32_t { 12 });
        CHECK_EQ(*twelve->as_array_index(), std::uint32_t { 12 }); // cached answer is the same answer
        k = heap.key(twelve);
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 12 });
        js::JsString* name = heap.string(u"name"sv);
        CHECK(!name->as_array_index().has_value());
        CHECK(!name->as_array_index().has_value());
        k = heap.key(name);
        CHECK(k.is_atom());
        CHECK(k.as_atom() == heap.atoms().name);
        CHECK(!heap.string(u"4294967295"sv)->as_array_index().has_value());
        CHECK(!heap.string(u"01"sv)->as_array_index().has_value());

        k = heap.key(3.0);
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 3 });
        k = heap.key(-0.0);
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 0 });
        k = heap.key(4294967294.0);
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 4294967294u });
        k = heap.key(4294967295.0);
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"4294967295"));
        k = heap.key(1.5);
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"1.5"));
        k = heap.key(-1.0);
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"-1"));
        k = heap.key(nan());
        CHECK(k.is_atom());
        CHECK(k.as_atom() == heap.atoms().nan);
        k = heap.key(std::numeric_limits<double>::infinity());
        CHECK(k.is_atom());
        CHECK(k.as_atom() == heap.atoms().infinity);
        k = heap.key(-std::numeric_limits<double>::infinity());
        CHECK(k.as_atom() == heap.atoms().negative_infinity);
        k = heap.key(1e21);
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"1e+21"));
        k = heap.key(0.1 + 0.2);
        CHECK(k.as_atom()->equals(u"0.30000000000000004"));

        k = heap.key(std::uint32_t { 5 });
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 5 });
        k = heap.key(std::uint32_t { 0xFFFFFFFFu });
        CHECK(k.is_atom());
        CHECK(k.as_atom()->equals(u"4294967295"));
        CHECK(heap.key(std::uint32_t { 0xFFFFFFFFu }) == heap.key(u"4294967295"));

        // The same name reaches the same key by every road.
        CHECK(heap.key(u"42") == heap.key(42.0));
        CHECK(heap.key(u"42") == heap.key(std::uint32_t { 42 }));
        CHECK(heap.key(u"42").hash() == heap.key(42.0).hash());
        CHECK(heap.key(u"foo") == heap.key(std::string_view("foo")));
        CHECK(heap.key(u"foo") == heap.key(heap.string(u"foo"sv)));
        CHECK(!(heap.key(u"42") == heap.key(u"43")));
        CHECK(!(heap.key(u"foo") == heap.key(u"bar")));
        CHECK(!(heap.key(u"1") == heap.key(u"01")));

        // key_to_string: an index's decimal spelling as an atom, the atom
        // itself, nothing for a symbol.
        js::JsString* seven = heap.key_to_string(heap.key(7.0));
        CHECK(seven == heap.atom(u"7"));
        CHECK(seven->is_atom());
        CHECK(heap.key_to_string(heap.key(std::uint32_t { 4294967294u }))->equals(u"4294967294"));
        CHECK(heap.key_to_string(heap.key(u"4294967295"))->equals(u"4294967295"));
        js::JsString* foo = heap.atom(u"foo");
        CHECK(heap.key_to_string(js::PropertyKey::atom(foo)) == foo);
        CHECK(heap.key_to_string(heap.key(u"foo")) == foo);
        js::Symbol* sym = heap.symbol(nullptr);
        CHECK(sym->description() == nullptr);
        CHECK(heap.key_to_string(js::PropertyKey::symbol(sym)) == nullptr);
        CHECK(js::PropertyKey::symbol(sym).is_symbol());
        CHECK(!js::PropertyKey::symbol(sym).is_string());
    }

    // Stress mode: a collection at every allocation.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        CHECK(!heap.stress());
        heap.set_stress(true);
        CHECK(heap.stress());
        std::size_t const before = heap.collections();
        heap.string(u"a"sv);
        heap.string(u"b"sv);
        heap.string(u"c"sv);
        CHECK_EQ(heap.collections(), before + 3);
        // Each allocation swept the previous, unrooted string; the newest
        // one is never collected by its own allocation.
        CHECK_EQ(heap.cell_count(), base + 1);

        js::Persistent keep(heap, js::Value::string(heap.string(u"keep"sv)));
        for (int i = 0; i < 10; ++i)
            heap.string(u"garbage"sv);
        CHECK(keep.value().as_string()->equals(u"keep"));
        CHECK_EQ(heap.cell_count(), base + 2);

        // An atom made under stress is permanent like any other.
        js::JsString* atom = heap.atom(u"stressed");
        heap.string(u"more garbage"sv);
        CHECK(atom->equals(u"stressed"));
        CHECK(heap.atom(u"stressed") == atom);

        // symbol() keeps its description alive across its own allocation
        // even when the caller did not root it.
        js::Symbol* sym = heap.symbol(heap.string(u"described"sv));
        CHECK(sym->description()->equals(u"described"));

        heap.set_stress(false);
        std::size_t const after = heap.collections();
        heap.string(u"x"sv);
        heap.string(u"y"sv);
        CHECK_EQ(heap.collections(), after);
    }

    // NoCollect defers every collection, nests, and applies to an
    // explicit collect() too.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        heap.set_stress(true);
        std::size_t const before = heap.collections();
        {
            js::Heap::NoCollect const guard(heap);
            js::JsString* a = heap.string(u"a"sv);
            js::JsString* b = heap.string(u"b"sv);
            {
                js::Heap::NoCollect const nested(heap);
                heap.string(u"c"sv);
                heap.collect();
                CHECK_EQ(heap.collections(), before);
            }
            heap.string(u"d"sv);
            CHECK_EQ(heap.collections(), before);
            CHECK_EQ(heap.cell_count(), base + 4);
            CHECK(a->equals(u"a"));
            CHECK(b->equals(u"b"));
        }
        heap.string(u"e"sv);
        CHECK_EQ(heap.collections(), before + 1);
        CHECK_EQ(heap.cell_count(), base + 1);
    }

    // The tracer reaches through a cell's trace(): a box keeps its
    // string, a long chain is walked without recursion, a cycle ends.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        TestRoots roots;
        heap.add_root_provider(&roots);

        Box* box = heap.allocate<Box>();
        box->inner = heap.string(u"inner"sv);
        roots.cell = box;
        CHECK_EQ(heap.cell_count(), base + 2);
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 2);
        CHECK(box->inner->equals(u"inner"));
        roots.cell = nullptr;
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);

        // A Box's inner pointer cannot be nulled by a collection: the
        // string dies only with the box.
        Box* box2 = heap.allocate<Box>();
        roots.cell = box2;
        box2->inner = heap.string(u"kept by box"sv);
        heap.set_stress(true);
        heap.string(u"garbage"sv);
        heap.set_stress(false);
        CHECK(box2->inner->equals(u"kept by box"));
        roots.cell = nullptr;
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);

        constexpr std::size_t chain_length = 200000;
        Link* head = heap.allocate<Link>();
        roots.cell = head;
        Link* tail = head;
        for (std::size_t i = 1; i < chain_length; ++i) {
            Link* link = heap.allocate<Link>();
            tail->next = link;
            tail = link;
        }
        tail->next = heap.string(u"end of chain"sv);
        std::size_t const with_chain = heap.cell_count();
        heap.collect();
        CHECK_EQ(heap.cell_count(), with_chain);
        roots.cell = nullptr;
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);

        Link* a = heap.allocate<Link>();
        Link* b = heap.allocate<Link>();
        a->next = b;
        b->next = a;
        roots.cell = a;
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 2);
        CHECK(a->next == b && b->next == a);
        roots.cell = nullptr;
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
        heap.remove_root_provider(&roots);
    }

    // Symbol traces its description; a symbol with no description is fine.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        js::JsString* description = heap.string(u"desc"sv);
        js::Symbol* sym = heap.symbol(description);
        CHECK(sym->description() == description);
        CHECK(!description->is_atom());
        js::Persistent keep(heap, js::Value::symbol(sym));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 2);
        CHECK(sym->description()->equals(u"desc"));
        keep.set(js::Value::undefined());
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
        js::Symbol* anonymous = heap.symbol(nullptr);
        js::Persistent keep2(heap, js::Value::symbol(anonymous));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
    }

    // JsString: WTF-8 out, hashing, and equality by contents.
    {
        js::Heap heap;
        js::JsString* pair = heap.string(u"a\xD83D\xDE00z"sv);
        CHECK(pair->to_utf8() == "a\xF0\x9F\x98\x80z");
        CHECK_EQ(pair->length(), std::size_t { 4 });
        js::JsString* lone = heap.string(std::string_view("x\xED\xB0\x80y"));
        CHECK_EQ(lone->length(), std::size_t { 3 });
        CHECK(lone->data()[1] == u'\xDC00');
        CHECK(lone->to_utf8() == "x\xED\xB0\x80y");
        js::JsString* empty = heap.string(u""sv);
        CHECK(empty->is_empty());
        CHECK(empty->to_utf8().empty());
        CHECK(empty->hash() == heap.atoms().empty->hash());
        CHECK(heap.string(u"abc"sv)->hash() == heap.string(u"abc"sv)->hash());
        CHECK(heap.string(u"abc"sv)->hash() != heap.string(u"abd"sv)->hash());
        CHECK(heap.string(u"abc"sv)->hash() == heap.string(u"abc"sv)->hash());
        CHECK(heap.string(u"abc"sv)->view() == u"abc");
        CHECK(js::utf8_from_utf16(heap.string(std::string_view("caf\xC3\xA9"))->view()) == "caf\xC3\xA9");
    }

    // The threshold: 8 MiB to start, then twice the live set.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        std::size_t const c0 = heap.collections();
        std::u16string const mebibyte(512u * 1024u, u'm'); // 1 MiB of code units
        for (int i = 0; i < 8; ++i)
            heap.string(mebibyte);
        // The check runs before each allocation: 7 MiB was under the
        // threshold when the eighth was made.
        CHECK_EQ(heap.collections(), c0);
        CHECK(heap.bytes_allocated() > 8u * 1024u * 1024u);
        CHECK_EQ(heap.cell_count(), base + 8);
        heap.string(mebibyte); // the ninth finds 8 MiB and more behind it
        CHECK_EQ(heap.collections(), c0 + 1);
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(heap.bytes_allocated() >= 1024u * 1024u);
        CHECK(heap.bytes_allocated() < 2u * 1024u * 1024u);

        // With 10 MiB live the threshold becomes ~20 MiB: eight more
        // mebibytes of garbage do not trigger a collection.
        js::Persistent big(heap, js::Value::string(heap.string(std::u16string(5u * 1024u * 1024u, u'b'))));
        heap.collect();
        std::size_t const c1 = heap.collections();
        CHECK(heap.bytes_allocated() >= 10u * 1024u * 1024u);
        CHECK(heap.bytes_allocated() < 11u * 1024u * 1024u);
        CHECK_EQ(heap.cell_count(), base + 1);
        for (int i = 0; i < 8; ++i)
            heap.string(mebibyte);
        CHECK_EQ(heap.collections(), c1);
        CHECK(heap.bytes_allocated() >= 18u * 1024u * 1024u);
        CHECK_EQ(heap.cell_count(), base + 9);
        // Past twice the live set it collects again, and the live set is
        // what remains.
        for (int i = 0; i < 4; ++i)
            heap.string(mebibyte);
        CHECK(heap.collections() > c1);
        CHECK(heap.cell_count() < base + 9);
        CHECK(big.value().as_string()->length() == 5u * 1024u * 1024u);
    }

    // A Persistent may outlive its heap without touching it.
    {
        js::Persistent* orphan = nullptr;
        {
            js::Heap heap;
            static js::Persistent survivor(heap, js::Value::number(1));
            orphan = &survivor;
        }
        CHECK(orphan->value().is_number());
    }

    // ---- Review: the collector around a moved-from Persistent. A move
    // leaves the source registered nowhere, so a collection that walks
    // the registry must neither read it nor lose the value it carried;
    // re-arming the husk by move-assignment makes it a root again.
    {
        js::Heap heap;
        std::size_t const base = heap.cell_count();
        js::Persistent a(heap, js::Value::string(heap.string(u"carried"sv)));
        js::Persistent b(std::move(a));
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(b.value().as_string()->equals(u"carried"));
        CHECK(a.value().is_undefined()); // the husk holds nothing
        js::Persistent c(std::move(a)); // moving a husk is harmless
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(c.value().is_undefined());
        a = std::move(b); // the husk is a root again
        heap.collect();
        CHECK_EQ(heap.cell_count(), base + 1);
        CHECK(a.value().as_string()->equals(u"carried"));
        CHECK(b.value().is_undefined());
        a = std::move(c); // assigning a husk over a live handle unroots it
        heap.collect();
        CHECK_EQ(heap.cell_count(), base);
    }
    // A moved-to handle outlives its heap while the husk it came from
    // dies first: neither touches the dead registry.
    {
        std::optional<js::Persistent> moved_to;
        {
            js::Heap heap;
            js::Persistent source(heap, js::Value::number(2));
            moved_to.emplace(std::move(source));
            heap.collect();
        }
        CHECK(moved_to->value().is_number());
        CHECK_EQ(moved_to->value().as_number(), 2.0);
    }

    // ---- Review: a removed RootProvider is never asked again, not merely
    // ignored; and removing one that was never added is a no-op.
    {
        js::Heap heap;
        CountingRoots roots;
        heap.remove_root_provider(&roots);
        heap.collect();
        CHECK_EQ(roots.calls, 0);
        heap.add_root_provider(&roots);
        heap.collect();
        CHECK_EQ(roots.calls, 1);
        heap.remove_root_provider(&roots);
        heap.collect();
        CHECK_EQ(roots.calls, 1);
        heap.remove_root_provider(&roots); // twice is fine
        heap.collect();
        CHECK_EQ(roots.calls, 1);
    }

    // ---- Review: atom("7") is an atom cell even though "7" names an index;
    // key() of the same text is the index, and the two roads agree.
    {
        js::Heap heap;
        js::JsString* seven = heap.atom(u"7");
        CHECK(seven->is_atom());
        CHECK(seven->as_array_index().has_value());
        CHECK_EQ(*seven->as_array_index(), std::uint32_t { 7 });
        js::PropertyKey const k = heap.key(u"7");
        CHECK(k.is_index());
        CHECK_EQ(k.as_index(), std::uint32_t { 7 });
        CHECK(heap.key(seven).is_index()); // an atom that names an index is still the index
        CHECK(heap.key_to_string(k) == seven);
        CHECK(heap.atom(seven) == seven);
        CHECK(heap.atom(std::string_view("7")) == seven);

        // key(double) at the edges of the index range and at −0 and 1e21.
        CHECK(heap.key(4294967295.0) == heap.key(std::uint32_t { 0xFFFFFFFFu }));
        CHECK(heap.key(4294967295.0) == heap.key(u"4294967295"));
        CHECK(heap.key(4294967295.0).is_atom());
        CHECK(heap.key(4294967296.0).is_atom());
        CHECK(heap.key(4294967296.0).as_atom()->equals(u"4294967296"));
        CHECK(heap.key(4294967294.0).is_index());
        CHECK(heap.key(-0.0) == heap.key(u"0"));
        CHECK(heap.key(-0.0) == heap.key(std::uint32_t { 0 }));
        CHECK(heap.key(-0.0) == heap.key(0.0));
        CHECK(heap.key(-0.0).hash() == heap.key(0.0).hash());
        CHECK(heap.key_to_string(heap.key(-0.0)) == heap.atoms().zero);
        CHECK(heap.key(1e21) == heap.key(u"1e+21"));
        CHECK(heap.key(1e21).is_atom());
        CHECK(!heap.key(1e21).as_atom()->as_array_index().has_value());
        CHECK(heap.key(1e20).is_atom()); // integral, but past 2^32 − 2
        CHECK(heap.key(1e20).as_atom()->equals(u"100000000000000000000"));
        CHECK(heap.key(0.5).as_atom()->equals(u"0.5"));
        CHECK(heap.key(-1e21).as_atom()->equals(u"-1e+21"));
        CHECK(heap.key(1e-7).as_atom()->equals(u"1e-7"));
    }

    // ---- Review: Number::toString in other radices against V8's
    // DoubleToRadixCString. Radix 2 never rounds (a digit is a bit), so
    // the output is the double's exact binary expansion; above 2^53 the
    // low integer digits are zero-filled; the 36-digit cases replay the
    // same IEEE operations in exact arithmetic.
    {
        constexpr double two_53 = 9007199254740992.0;
        constexpr double two_64 = 18446744073709551616.0;
        constexpr double max_value = std::numeric_limits<double>::max();
        CHECK_EQ(ts(-0.1, 2), "-0.0001100110011001100110011001100110011001100110011001101");
        CHECK_EQ(ts(-0.5, 2), "-0.1");
        CHECK_EQ(ts(-3.75, 2), "-11.11");
        CHECK_EQ(ts(-0.5, 36), "-0.i");
        CHECK_EQ(ts(-255.5, 16), "-ff.8");
        CHECK_EQ(ts(two_53, 2), "1" + std::string(53, '0'));
        CHECK_EQ(ts(two_53, 16), "20000000000000");
        CHECK_EQ(ts(two_53, 10), "9007199254740992");
        CHECK_EQ(ts(two_53 + 2, 2), "1" + std::string(51, '0') + "10");
        CHECK_EQ(ts(two_64, 2), "1" + std::string(64, '0'));
        CHECK_EQ(ts(two_64, 16), "10000000000000000");
        CHECK_EQ(ts(two_64, 10), "18446744073709552000");
        CHECK_EQ(ts(two_64, 36), "3w5e11264sg00");
        CHECK_EQ(ts(1e21, 36), "5v1j4f4ds7c000");
        CHECK_EQ(ts(1e21, 2), "1101100011010111001001101011011100010111011110101000000000000000000000");
        CHECK_EQ(ts(1e-7, 2), "0.0000000000000000000000011010110101111111001010011010101111001010111101001");
        CHECK_EQ(ts(-1e-7, 2), "-0.0000000000000000000000011010110101111111001010011010101111001010111101001");
        CHECK_EQ(ts(max_value, 36), "1a1e4vngaiqo" + std::string(187, '0'));
        CHECK_EQ(ts(max_value, 2).size(), std::size_t { 1024 });
        CHECK_EQ(ts(5e-324, 2), "0." + std::string(1073, '0') + "1");
        CHECK_EQ(ts(0.75, 2), "0.11");
        CHECK_EQ(ts(1.0 / 3.0, 2), "0.010101010101010101010101010101010101010101010101010101");
        // The fraction loop replayed in IEEE arithmetic outside this
        // engine: the digit count stops at half an ulp, the last digit
        // rounds (0.3 in base 7 ends …205, not …204), and a value just
        // under 1 never carries into the integer part.
        CHECK_EQ(ts(0.1, 3), "0.0022002200220022002200220022002201");
        CHECK_EQ(ts(0.3, 7), "0.2046204620462046205");
        CHECK_EQ(ts(1.0 - 1.1102230246251565e-16, 3), "0.222222222222222222222222222222222"); // 1 − 2^−53
        CHECK_EQ(ts(0.9999999, 36), "0.zzzztybl0c");
    }

    // ---- Review: toFixed at its limits (§21.1.3.3). The exact expansion
    // must survive 100 places: 2^−100 has exactly 100 decimal places, so a
    // library that rounds early or prints garbage past 17 digits shows.
    {
        double two_minus_100 = 1;
        for (int i = 0; i < 100; ++i)
            two_minus_100 /= 2;
        CHECK_EQ(fixed(two_minus_100, 100), "0." + std::string(30, '0') + "7888609052210118054117285652827862296732064351090230047702789306640625");
        CHECK_EQ(fixed(1e-100, 100), "0." + std::string(99, '0') + "1");
        CHECK_EQ(fixed(1.5, 100), "1.5" + std::string(99, '0'));
        CHECK_EQ(fixed(-1.5, 100), "-1.5" + std::string(99, '0'));
        CHECK_EQ(fixed(-1.005, 2), "-1.00");
        CHECK_EQ(fixed(-0.5, 0), "-1"); // the tie goes to the larger magnitude
        CHECK_EQ(fixed(-0.004, 2), "-0.00");
        CHECK_EQ(fixed(-0.0, 0), "0");
        CHECK_EQ(fixed(-0.0, 100), "0." + std::string(100, '0'));
        CHECK_EQ(fixed(-0.0000001, 2), "-0.00");
        CHECK_EQ(fixed(999999999999999900000.0, 2), "999999999999999868928.00"); // the last double below 1e21
        CHECK_EQ(fixed(1e21, 0), "1e+21");
        CHECK_EQ(fixed(1.5e21, 3), "1.5e+21");
        CHECK_EQ(fixed(-1e21, 0), "-1e+21");
        CHECK_EQ(fixed(std::numeric_limits<double>::max(), 100), "1.7976931348623157e+308");
        CHECK_EQ(fixed(-std::numeric_limits<double>::infinity(), 5), "-Infinity");
        CHECK_EQ(fixed(0.5, 0), "1");
        CHECK_EQ(fixed(1e20, 100), "100000000000000000000." + std::string(100, '0'));
    }

    // ---- Review: toExponential ties (§21.1.3.2 step 10.a: the larger n)
    // on exact expansions, and toPrecision around 1e21 (§21.1.3.5).
    {
        CHECK_EQ(exponential(1.25, 1), "1.3e+0"); // 1.25 is exact: a true tie
        CHECK_EQ(exponential(-1.25, 1), "-1.3e+0");
        CHECK_EQ(exponential(1.35, 1), "1.4e+0"); // 1.35 is really 1.3500000000000000888…
        CHECK_EQ(exponential(1.45, 1), "1.4e+0"); // 1.45 is really 1.4499999999999999555…
        CHECK_EQ(exponential(9.5, 0), "1e+1"); // a tie that carries into the exponent
        CHECK_EQ(exponential(2.5, 0), "3e+0");
        CHECK_EQ(exponential(0.5, 0), "5e-1");
        CHECK_EQ(exponential(1.25, 100), "1.25" + std::string(98, '0') + "e+0");
        CHECK_EQ(precision(1e21, 3), "1.00e+21");
        CHECK_EQ(precision(1e21, 1), "1e+21");
        CHECK_EQ(precision(1e21, 22), "1000000000000000000000"); // e = p − 1: no point
        CHECK_EQ(precision(1e21, 23), "1000000000000000000000.0");
        CHECK_EQ(precision(-1e21, 2), "-1.0e+21");
        CHECK_EQ(precision(0.1, 100), "0.1000000000000000055511151231257827021181583404541015625" + std::string(45, '0'));
        CHECK_EQ(precision(1.25, 2), "1.3");
        CHECK_EQ(precision(-0.0, 2), "0.0");
    }

    // ---- Review: parseInt / parseFloat / StringToNumber at the seams
    // (§19.2.5, §19.2.4, §7.1.4.1.1).
    {
        constexpr double inf = std::numeric_limits<double>::infinity();
        CHECK(is_nan(pint("0x", 0)));
        CHECK(is_nan(pint("0x", 16)));
        CHECK(is_nan(pint("-0x", 16)));
        CHECK_EQ(pint("-0x10", 16), -16.0);
        CHECK_EQ(pint("-0x10", 0), -16.0);
        CHECK_EQ(pint("+0x10", 16), 16.0);
        CHECK(is_nan(pint("12", 1)));
        CHECK(is_nan(pint("12", 37)));
        CHECK(is_nan(pint("12", -1)));
        CHECK_EQ(pint("z", 36), 35.0);
        CHECK_EQ(pint("0x", 36), 33.0); // no prefix stripping outside radix 16: x is a digit
        CHECK_EQ(pint("0x10", 8), 0.0);
        CHECK_EQ(pint("0x1g", 16), 1.0);
        CHECK_EQ(pint("  -12  ", 10), -12.0); // only the start is trimmed
        CHECK_EQ(pfloat("  -.5e-1x"), -0.05);
        CHECK_EQ(pfloat("Infinityx"), inf);
        CHECK_EQ(pfloat("-Infinityx"), -inf);
        CHECK_EQ(pfloat("0x10"), 0.0);
        CHECK(is_nan(pfloat(".e1")));
        CHECK_EQ(pfloat("1e+"), 1.0);
        CHECK_EQ(pfloat("+.5"), 0.5);
        CHECK_EQ(pfloat("1.e-"), 1.0);
        CHECK(is_nan(stn("0x")));
        CHECK(is_nan(stn("0X")));
        CHECK(is_nan(stn("0b")));
        CHECK_EQ(stn("1."), 1.0);
        CHECK(is_nan(stn(".")));
        CHECK(is_nan(stn("+.")));
        CHECK(is_nan(stn("1e+")));
        CHECK(is_nan(stn("1e")));
        CHECK(is_nan(stn("-0x10")));
        CHECK(is_nan(stn("0x 10")));
        CHECK_EQ(stn("0x" + std::string(1000, '0') + "1"), 1.0); // leading zeros cost no precision
        CHECK_EQ(stn("0b1" + std::string(1100, '0')), inf); // 2^1100 overflows
        CHECK_EQ(stn("0b1" + std::string(1023, '0')), 8.98846567431158e307); // 2^1023 does not
        CHECK_EQ(stn("0o" + std::string(22, '7')), 73786976294838206464.0); // 2^66 − 1 rounds to 2^66
        CHECK_EQ(stn("0b1" + std::string(52, '0') + "1"), 9007199254740992.0); // 2^53 + 1 ties to even
        CHECK_EQ(stn("1" + std::string(400, '0')), inf);
        CHECK_EQ(stn("0." + std::string(400, '0') + "1e400"), 0.1);
    }

    // ---- Review: malformed WTF-8 becomes U+FFFD, never a crash, and the
    // count of replacements follows the maximal-subpart rule.
    {
        js::Heap heap;
        auto count_fffd = [](std::u16string const& s) {
            std::size_t n = 0;
            for (char16_t const c : s)
                if (c == u'\xFFFD')
                    ++n;
            return n;
        };
        CHECK(js::utf16_from_utf8("\xC0\xAF") == u"\xFFFD\xFFFD"); // overlong "/"
        CHECK(js::utf16_from_utf8("\xE0\x9F\xBF") == u"\xFFFD\xFFFD\xFFFD"); // overlong U+07FF
        CHECK(js::utf16_from_utf8("\xF0\x8F\xBF\xBF") == u"\xFFFD\xFFFD\xFFFD\xFFFD"); // overlong U+FFFF
        CHECK(js::utf16_from_utf8("\xF8\x88\x80\x80\x80") == u"\xFFFD\xFFFD\xFFFD\xFFFD\xFFFD"); // a five-byte lead
        CHECK(js::utf16_from_utf8("\xED\xA0") == u"\xFFFD"); // a truncated surrogate
        CHECK(js::utf16_from_utf8("\xF4") == u"\xFFFD");
        CHECK(js::utf16_from_utf8("\xC2") == u"\xFFFD");
        CHECK(js::utf16_from_utf8("\xF4\x90") == u"\xFFFD\xFFFD"); // F4 90 is past U+10FFFF: 90 restarts
        CHECK(js::utf16_from_utf8("\xE2\x82\xAC\xE2") == u"\x20AC\xFFFD");
        CHECK(js::utf16_from_utf8("\xED\xBF\xBF") == u"\xDFFF"); // WTF-8: the last low surrogate
        CHECK(js::utf16_from_utf8("\xFF\xFE") == u"\xFFFD\xFFFD");
        CHECK(js::utf16_from_utf8(std::string_view("a\0b", 3)) == std::u16string(u"a\0b", 3)); // NUL is a code point
        js::JsString* bad = heap.string(std::string_view("x\xF0\x8F\xBF\xBFy"));
        CHECK_EQ(bad->length(), std::size_t { 6 });
        CHECK_EQ(count_fffd(bad->data()), std::size_t { 4 });
        CHECK(bad->to_utf8() == "x\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDy");
        CHECK(heap.atom(std::string_view("\xE2\x82")) == heap.atom(u"\xFFFD")); // a truncated atom is the replacement atom
    }

    // ---- Review: code_point_at on a lone high surrogate at the very end,
    // and on two high surrogates in a row.
    {
        std::size_t units = 99;
        CHECK_EQ(js::code_point_at(u"ab\xD800", 2, &units), char32_t { 0xD800 });
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(u"\xD800\xD800", 0, &units), char32_t { 0xD800 });
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(u"\xD800\xD800", 1, &units), char32_t { 0xD800 });
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(u"\xD800\xDC00", 1, &units), char32_t { 0xDC00 }); // the low half alone
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(u"\xDBFF\xDFFF", 0, &units), char32_t { 0x10FFFF });
        CHECK_EQ(units, std::size_t { 2 });
        CHECK_EQ(js::code_point_at(u"", 0, &units), char32_t { 0 });
        CHECK_EQ(units, std::size_t { 0 });
        CHECK_EQ(js::code_point_at(u"\xD800", 0, nullptr), char32_t { 0xD800 }); // no units wanted
    }

    return sashfold::test::report("js_heap");
}
