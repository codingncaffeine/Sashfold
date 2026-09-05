#include "Test.h"

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Object.h"
#include "js/Regex.h"
#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sashfold;
using namespace std::literals;

namespace {

// Everything a test makes is held here so that stress mode (a
// collection at every allocation) cannot sweep it between two lines.
struct Fixture : js::RootProvider {
    js::Heap heap;
    std::vector<js::Cell*> cells;

    Fixture()
    {
        heap.set_stress(true);
        heap.add_root_provider(this);
    }
    ~Fixture() override { heap.remove_root_provider(this); }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;

    void trace_roots(js::Tracer& tracer) override
    {
        for (js::Cell* cell : cells)
            tracer.visit(cell);
    }

    template<typename T, typename... Args>
    T* make(Args&&... args)
    {
        T* cell = heap.allocate<T>(std::forward<Args>(args)...);
        cells.push_back(cell);
        return cell;
    }
    js::Object* object(js::Object* prototype = nullptr) { return make<js::Object>(prototype); }
    js::ArrayObject* array(std::span<js::Value const> elements = {}, js::Object* prototype = nullptr)
    {
        return make<js::ArrayObject>(prototype, elements);
    }
    js::JsString* string_cell(std::string_view utf8)
    {
        js::JsString* cell = heap.string(utf8);
        cells.push_back(cell);
        return cell;
    }
    js::Value str(std::string_view utf8) { return js::Value::string(string_cell(utf8)); }
    js::PropertyKey key(std::string_view name) { return heap.key(name); }
    js::PropertyKey sym(std::string_view description)
    {
        js::Symbol* symbol = heap.symbol(heap.atom(description));
        cells.push_back(symbol);
        return js::PropertyKey::symbol(symbol);
    }
    js::PropertyKey length_key() { return js::PropertyKey::atom(heap.atoms().length); }
    js::NativeFunction* native()
    {
        return make<js::NativeFunction>(nullptr, js::NativeFunction::Callback(
                                                     [](js::Interpreter&, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
                                                         return js::Value::undefined();
                                                     }));
    }
};

// Object::get and Object::set take the interpreter only to run accessors
// (and, for `array.length = v`, ToNumber); on data properties they never
// touch it. This is a heap-only test by design — there is no interpreter
// to make yet — so the data paths run against a reference to storage that
// is never read, and the tests keep off every path that would read it.
alignas(js::Interpreter) std::byte g_no_interpreter_storage[sizeof(js::Interpreter)];

js::Interpreter& no_interpreter()
{
    return *reinterpret_cast<js::Interpreter*>(g_no_interpreter_storage);
}

js::Value num(double d)
{
    return js::Value::number(d);
}

js::PropertyKey idx(std::uint32_t i)
{
    return js::PropertyKey::index(i);
}

js::PropertyDescriptor value_desc(js::Value value)
{
    js::PropertyDescriptor d;
    d.value = value;
    return d;
}

js::PropertyDescriptor flags_desc(std::optional<bool> writable, std::optional<bool> enumerable, std::optional<bool> configurable)
{
    js::PropertyDescriptor d;
    d.writable = writable;
    d.enumerable = enumerable;
    d.configurable = configurable;
    return d;
}

js::PropertyDescriptor getter_desc(js::Object* getter)
{
    js::PropertyDescriptor d;
    d.get = getter;
    return d;
}

js::PropertyDescriptor setter_desc(js::Object* setter)
{
    js::PropertyDescriptor d;
    d.set = setter;
    return d;
}

bool has_attributes(std::optional<js::PropertyDescriptor> const& d, bool writable, bool enumerable, bool configurable)
{
    return d && d->writable.value_or(false) == writable && d->enumerable.value_or(false) == enumerable
        && d->configurable.value_or(false) == configurable;
}

bool is_data_value(std::optional<js::PropertyDescriptor> const& d, js::Value const& value)
{
    return d && d->is_data() && !d->is_accessor() && d->value.has_value() && *d->value == value;
}

bool string_equals(js::Value const& value, std::u16string_view expected)
{
    return value.is_string() && value.as_string()->view() == expected;
}

// ---------------------------------------------------------------------------
// The `length` key of an array's own_keys is recovered through the realm
// (Object.h gives no path to the heap): this runs first, before any test
// has let the exotic objects see a "length" atom, so it proves the realm
// path alone finds it.
void test_length_key_through_realm(Fixture& fx)
{
    js::Object* ctor = fx.object();
    ctor->put(fx.length_key(), num(1), js::Configurable);
    js::Object* proto = fx.object();
    proto->put(fx.key("constructor"), js::Value::object(ctor), js::builtin_attributes);
    js::ArrayObject* a = fx.array({}, proto);
    std::vector<js::PropertyKey> keys = a->own_keys();
    CHECK_EQ(keys.size(), 1u);
    CHECK(keys.size() == 1 && keys[0] == fx.length_key());

    // The same through a string wrapper.
    js::StringObject* s = fx.make<js::StringObject>(proto, fx.string_cell("ab"));
    keys = s->own_keys();
    CHECK_EQ(keys.size(), 3u);
    CHECK(keys.size() == 3 && keys[2] == fx.length_key());

    // With no realm in sight the atom still comes from the cell's own
    // heap (Cell::heap), so length is always listed.
    js::ArrayObject* bare = fx.array();
    keys = bare->own_keys();
    CHECK(keys.size() == 1 && keys[0] == fx.length_key());
}

// ---------------------------------------------------------------------------
void test_descriptors_round_trip(Fixture& fx)
{
    js::Object* o = fx.object();
    js::PropertyKey const a = fx.key("a");

    o->put(a, num(1));
    std::optional<js::PropertyDescriptor> d = o->get_own_property(a);
    CHECK(is_data_value(d, num(1)));
    CHECK(has_attributes(d, true, true, true));
    CHECK(d && !d->get && !d->set);
    CHECK_EQ(o->own_property_count(), 1u);

    // Every attribute combination survives put → get_own_property.
    for (std::uint8_t attributes = 0; attributes < 8; ++attributes) {
        js::PropertyKey const k = fx.key("attr" + std::to_string(attributes));
        o->put(k, num(attributes), attributes);
        d = o->get_own_property(k);
        CHECK(is_data_value(d, num(attributes)));
        CHECK(has_attributes(d, (attributes & js::Writable) != 0, (attributes & js::Enumerable) != 0,
            (attributes & js::Configurable) != 0));
        js::Property const* p = o->find_own(k);
        CHECK(p != nullptr && p->attributes == attributes && !p->accessor);
    }
    CHECK_EQ(o->own_property_count(), 9u);

    // Accessors: the getter and setter come back, and there is no value.
    js::Object* getter = fx.native();
    js::Object* setter = fx.native();
    js::PropertyKey const acc = fx.key("acc");
    o->put_accessor(acc, getter, setter, js::Enumerable | js::Configurable);
    d = o->get_own_property(acc);
    CHECK(d && d->is_accessor() && !d->is_data());
    CHECK(d && d->get.has_value() && *d->get == getter);
    CHECK(d && d->set.has_value() && *d->set == setter);
    CHECK(d && !d->value && !d->writable);
    CHECK(d && d->enumerable == true && d->configurable == true);

    // A missing getter is `undefined`: present in the descriptor, null inside.
    js::PropertyKey const half = fx.key("half");
    o->put_accessor(half, nullptr, setter);
    d = o->get_own_property(half);
    CHECK(d && d->get.has_value() && *d->get == nullptr);
    CHECK(d && d->set.has_value() && *d->set == setter);
    CHECK(d && d->enumerable == false && d->configurable == true);

    // put over an accessor makes a data property again.
    o->put(acc, num(7), js::frozen_attributes);
    d = o->get_own_property(acc);
    CHECK(is_data_value(d, num(7)));
    CHECK(has_attributes(d, false, false, false));
    CHECK(o->find_own(acc)->getter == nullptr && o->find_own(acc)->setter == nullptr);

    // String and object values are kept by identity.
    js::Value const text = fx.str("hello");
    o->put(fx.key("s"), text);
    CHECK(is_data_value(o->get_own_property(fx.key("s")), text));
    js::Object* inner = fx.object();
    o->put(fx.key("o"), js::Value::object(inner));
    CHECK(is_data_value(o->get_own_property(fx.key("o")), js::Value::object(inner)));

    // Missing keys.
    CHECK(!o->get_own_property(fx.key("nope")));
    CHECK(o->find_own(fx.key("nope")) == nullptr);
    CHECK(!o->get_own_property(idx(0)));
    CHECK(!o->get_own_property(fx.sym("nope")));

    // remove_own is unconditional and reports whether anything was there.
    CHECK(o->remove_own(acc));
    CHECK(!o->remove_own(acc));
    CHECK(!o->get_own_property(acc));

    // Index and symbol keys are stored like any other.
    js::PropertyKey const s1 = fx.sym("one");
    o->put(idx(3), num(3));
    o->put(s1, num(4));
    CHECK(is_data_value(o->get_own_property(idx(3)), num(3)));
    CHECK(is_data_value(o->get_own_property(s1), num(4)));
    CHECK(!o->get_own_property(idx(4)));
}

// ---------------------------------------------------------------------------
// ValidateAndApplyPropertyDescriptor (§10.1.6.3), rule by rule.
void test_define_own_property(Fixture& fx)
{
    js::Object* g1 = fx.native();
    js::Object* g2 = fx.native();

    // 1. An empty descriptor on a missing key creates {undefined, false, false, false}.
    {
        js::Object* o = fx.object();
        CHECK(o->define_own_property(fx.key("p"), js::PropertyDescriptor {}));
        std::optional<js::PropertyDescriptor> const d = o->get_own_property(fx.key("p"));
        CHECK(is_data_value(d, js::Value::undefined()));
        CHECK(has_attributes(d, false, false, false));
    }
    // 2. Value only: the other fields default to false.
    {
        js::Object* o = fx.object();
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(2))));
        std::optional<js::PropertyDescriptor> const d = o->get_own_property(fx.key("p"));
        CHECK(is_data_value(d, num(2)));
        CHECK(has_attributes(d, false, false, false));
    }
    // 3. Getter only: an accessor whose setter is undefined.
    {
        js::Object* o = fx.object();
        CHECK(o->define_own_property(fx.key("p"), getter_desc(g1)));
        std::optional<js::PropertyDescriptor> const d = o->get_own_property(fx.key("p"));
        CHECK(d && d->is_accessor());
        CHECK(d && d->get.has_value() && *d->get == g1);
        CHECK(d && d->set.has_value() && *d->set == nullptr);
        CHECK(d && d->enumerable == false && d->configurable == false);
        CHECK(d && !d->value && !d->writable);
    }
    // 4. A non-extensible object refuses a new key ...
    {
        js::Object* o = fx.object();
        o->prevent_extensions();
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(1))));
        CHECK(!o->define_own_property(fx.key("p"), js::PropertyDescriptor {}));
        CHECK(!o->define_own_property(idx(0), getter_desc(g1)));
        CHECK_EQ(o->own_property_count(), 0u);
    }
    // 5. ... but still lets an existing one change.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1));
        o->prevent_extensions();
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(2))));
        CHECK(is_data_value(o->get_own_property(fx.key("p")), num(2)));
    }
    // 6. An empty descriptor on an existing property changes nothing, even when locked.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::frozen_attributes);
        CHECK(o->define_own_property(fx.key("p"), js::PropertyDescriptor {}));
        CHECK(is_data_value(o->get_own_property(fx.key("p")), num(1)));
        CHECK(has_attributes(o->get_own_property(fx.key("p")), false, false, false));
    }
    // 7. Non-configurable refuses configurable: true.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::Writable | js::Enumerable);
        CHECK(!o->define_own_property(fx.key("p"), flags_desc({}, {}, true)));
        // 8. ... and an enumerable flip.
        CHECK(!o->define_own_property(fx.key("p"), flags_desc({}, false, {})));
        // 9. ... but the same enumerable is fine.
        CHECK(o->define_own_property(fx.key("p"), flags_desc({}, true, {})));
        // 10. configurable: false on a non-configurable property is fine too.
        CHECK(o->define_own_property(fx.key("p"), flags_desc({}, {}, false)));
        CHECK(has_attributes(o->get_own_property(fx.key("p")), true, true, false));
    }
    // 11. A non-configurable data property refuses to become an accessor.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::Writable | js::Enumerable);
        CHECK(!o->define_own_property(fx.key("p"), getter_desc(g1)));
        CHECK(!o->define_own_property(fx.key("p"), setter_desc(g1)));
        CHECK(is_data_value(o->get_own_property(fx.key("p")), num(1)));
    }
    // 12. A non-configurable accessor refuses to become data.
    {
        js::Object* o = fx.object();
        o->put_accessor(fx.key("p"), g1, g2, js::Enumerable);
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(1))));
        CHECK(!o->define_own_property(fx.key("p"), flags_desc(true, {}, {})));
        CHECK(o->get_own_property(fx.key("p"))->is_accessor());
    }
    // 13–16. A non-configurable accessor: a different getter or setter is refused, the same is fine.
    {
        js::Object* o = fx.object();
        o->put_accessor(fx.key("p"), g1, g2, js::Enumerable);
        CHECK(!o->define_own_property(fx.key("p"), getter_desc(g2)));
        CHECK(o->define_own_property(fx.key("p"), getter_desc(g1)));
        CHECK(!o->define_own_property(fx.key("p"), setter_desc(g1)));
        CHECK(o->define_own_property(fx.key("p"), setter_desc(g2)));
        CHECK(!o->define_own_property(fx.key("p"), getter_desc(nullptr)));
        js::PropertyDescriptor both;
        both.get = g1;
        both.set = g2;
        both.enumerable = true;
        CHECK(o->define_own_property(fx.key("p"), both));
        // 17. A generic descriptor with the same enumerable passes.
        CHECK(o->define_own_property(fx.key("p"), flags_desc({}, true, {})));
        CHECK(!o->define_own_property(fx.key("p"), flags_desc({}, false, {})));
        // A locked accessor with an undefined getter: the same undefined is fine.
        o->put_accessor(fx.key("q"), nullptr, g2, 0);
        CHECK(o->define_own_property(fx.key("q"), getter_desc(nullptr)));
        CHECK(!o->define_own_property(fx.key("q"), getter_desc(g1)));
    }
    // 18–21. A non-configurable, non-writable data property.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::Enumerable);
        CHECK(!o->define_own_property(fx.key("p"), flags_desc(true, {}, {})));
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(2))));
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(1))));
        CHECK(o->define_own_property(fx.key("p"), flags_desc(false, {}, {})));
        CHECK(is_data_value(o->get_own_property(fx.key("p")), num(1)));
    }
    // 22. SameValue: NaN matches NaN.
    {
        js::Object* o = fx.object();
        double const nan = std::numeric_limits<double>::quiet_NaN();
        o->put(fx.key("p"), num(nan), js::frozen_attributes);
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(-nan))));
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(0))));
    }
    // 23. SameValue: +0 and −0 differ.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(0.0), js::frozen_attributes);
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(-0.0))));
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(0.0))));
    }
    // 24. SameValue: strings compare by contents, not by cell.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), fx.str("same"), js::frozen_attributes);
        CHECK(o->define_own_property(fx.key("p"), value_desc(fx.str("same"))));
        CHECK(!o->define_own_property(fx.key("p"), value_desc(fx.str("other"))));
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(1))));
        // Objects by identity.
        js::Object* inner = fx.object();
        o->put(fx.key("q"), js::Value::object(inner), js::frozen_attributes);
        CHECK(o->define_own_property(fx.key("q"), value_desc(js::Value::object(inner))));
        CHECK(!o->define_own_property(fx.key("q"), value_desc(js::Value::object(fx.object()))));
    }
    // 25–27. A non-configurable but writable data property: the value may
    // change and writable may go false — once.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::Writable);
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(2))));
        CHECK(is_data_value(o->get_own_property(fx.key("p")), num(2)));
        CHECK(o->define_own_property(fx.key("p"), flags_desc(false, {}, {})));
        CHECK(has_attributes(o->get_own_property(fx.key("p")), false, false, false));
        CHECK(!o->define_own_property(fx.key("p"), flags_desc(true, {}, {})));
        CHECK(!o->define_own_property(fx.key("p"), value_desc(num(3))));
    }
    // 28. Data → accessor on a configurable property keeps enumerable and
    // configurable and drops value and writable.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::Writable | js::Configurable);
        CHECK(o->define_own_property(fx.key("p"), getter_desc(g1)));
        std::optional<js::PropertyDescriptor> const d = o->get_own_property(fx.key("p"));
        CHECK(d && d->is_accessor() && !d->is_data());
        CHECK(d && *d->get == g1 && *d->set == nullptr);
        CHECK(d && d->enumerable == false && d->configurable == true);
        js::Property const* p = o->find_own(fx.key("p"));
        CHECK(p != nullptr && p->accessor && !p->writable() && p->value.is_undefined());
    }
    // 29. Accessor → data: value undefined and writable false unless given.
    {
        js::Object* o = fx.object();
        o->put_accessor(fx.key("p"), g1, g2, js::Enumerable | js::Configurable);
        CHECK(o->define_own_property(fx.key("p"), flags_desc(false, {}, {})));
        std::optional<js::PropertyDescriptor> d = o->get_own_property(fx.key("p"));
        CHECK(is_data_value(d, js::Value::undefined()));
        CHECK(has_attributes(d, false, true, true));
        CHECK(o->find_own(fx.key("p"))->getter == nullptr && o->find_own(fx.key("p"))->setter == nullptr);
        // 30. Accessor → data with value and writable given.
        o->put_accessor(fx.key("q"), g1, g2, js::Configurable);
        js::PropertyDescriptor full = value_desc(num(9));
        full.writable = true;
        CHECK(o->define_own_property(fx.key("q"), full));
        d = o->get_own_property(fx.key("q"));
        CHECK(is_data_value(d, num(9)));
        CHECK(has_attributes(d, true, false, true));
    }
    // 31. A generic descriptor on a configurable property changes only what it names.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(5));
        CHECK(o->define_own_property(fx.key("p"), flags_desc({}, false, {})));
        std::optional<js::PropertyDescriptor> const d = o->get_own_property(fx.key("p"));
        CHECK(is_data_value(d, num(5)));
        CHECK(has_attributes(d, true, false, true));
    }
    // 32. A configurable, non-writable data property may still change value.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1), js::Configurable);
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(2))));
        CHECK(is_data_value(o->get_own_property(fx.key("p")), num(2)));
        CHECK(o->define_own_property(fx.key("p"), flags_desc(true, {}, {})));
        CHECK(has_attributes(o->get_own_property(fx.key("p")), true, false, true));
    }
    // 33. Locking: configurable: false takes, and afterwards the rules above bind.
    {
        js::Object* o = fx.object();
        o->put(fx.key("p"), num(1));
        CHECK(o->define_own_property(fx.key("p"), flags_desc({}, {}, false)));
        CHECK(has_attributes(o->get_own_property(fx.key("p")), true, true, false));
        CHECK(!o->define_own_property(fx.key("p"), flags_desc({}, {}, true)));
        CHECK(o->define_own_property(fx.key("p"), value_desc(num(2)))); // still writable
        CHECK(!o->define_own_property(fx.key("p"), getter_desc(g1)));
    }
    // 34. Every field at once on a fresh key, index and symbol keys included.
    {
        js::Object* o = fx.object();
        js::PropertyDescriptor full = value_desc(num(3));
        full.writable = true;
        full.enumerable = false;
        full.configurable = true;
        CHECK(o->define_own_property(idx(7), full));
        CHECK(is_data_value(o->get_own_property(idx(7)), num(3)));
        CHECK(has_attributes(o->get_own_property(idx(7)), true, false, true));
        js::PropertyKey const s = fx.sym("s");
        CHECK(o->define_own_property(s, full));
        CHECK(is_data_value(o->get_own_property(s), num(3)));
        CHECK_EQ(o->own_property_count(), 2u);
    }
}

// ---------------------------------------------------------------------------
void test_extensibility_and_prototype(Fixture& fx)
{
    js::Object* a = fx.object();
    js::Object* b = fx.object(a);
    js::Object* c = fx.object(b);
    CHECK(a->is_extensible());
    CHECK(c->prototype() == b && b->prototype() == a && a->prototype() == nullptr);

    // A cycle is refused at every length; the chain is unchanged.
    CHECK(!a->set_prototype(a));
    CHECK(!a->set_prototype(b));
    CHECK(!a->set_prototype(c));
    CHECK(!b->set_prototype(c));
    CHECK(a->prototype() == nullptr && b->prototype() == a);

    // A legal move, to null and back.
    CHECK(b->set_prototype(nullptr));
    CHECK(b->prototype() == nullptr);
    CHECK(b->set_prototype(a));
    CHECK(b->prototype() == a);
    js::Object* d = fx.object();
    CHECK(a->set_prototype(d));
    CHECK(a->prototype() == d);
    CHECK(!d->set_prototype(c)); // d → c → b → a → d would loop

    // Non-extensible: the same prototype is fine, any other is refused.
    c->prevent_extensions();
    CHECK(!c->is_extensible());
    CHECK(c->set_prototype(b));
    CHECK(!c->set_prototype(a));
    CHECK(!c->set_prototype(nullptr));
    CHECK(c->prototype() == b);

    // Non-extensible still allows put (the raw path) and refuses define.
    c->put(fx.key("raw"), num(1));
    CHECK(is_data_value(c->get_own_property(fx.key("raw")), num(1)));
    CHECK(!c->define_own_property(fx.key("defined"), value_desc(num(1))));
    CHECK(c->define_own_property(fx.key("raw"), value_desc(num(2))));

    // A long chain is walked, not recursed into.
    js::Object* head = fx.object();
    js::Object* tail = head;
    for (int i = 0; i < 20000; ++i)
        tail = fx.object(tail);
    CHECK(!head->set_prototype(tail));
    CHECK(head->set_prototype(nullptr));
    head->put(fx.key("deep"), num(1));
    CHECK(tail->has_property(fx.key("deep")));
    CHECK(!tail->has_property(fx.key("missing")));
}

// ---------------------------------------------------------------------------
void test_own_keys_order(Fixture& fx)
{
    js::Object* o = fx.object();
    std::vector<js::PropertyKey> atoms;
    std::vector<js::PropertyKey> symbols;
    auto put_atom = [&](std::string_view name) {
        js::PropertyKey const k = fx.key(name);
        atoms.push_back(k);
        o->put(k, num(1));
    };
    auto put_symbol = [&](std::string_view description) {
        js::PropertyKey const k = fx.sym(description);
        symbols.push_back(k);
        o->put(k, num(1));
    };
    put_atom("alpha");
    o->put(idx(30), num(30));
    put_symbol("sA");
    put_atom("beta");
    o->put(idx(2), num(2));
    put_atom("gamma");
    put_symbol("sB");
    o->put(idx(10), num(10));
    put_atom("delta");
    put_atom("epsilon");
    o->put(idx(7), num(7));
    for (int i = 0; i < 10; ++i)
        put_atom("more" + std::to_string(i));
    put_symbol("sC");
    o->put(idx(0), num(0));
    o->put(idx(4294967294u), num(1));
    CHECK_EQ(o->own_property_count(), 24u);

    std::vector<js::PropertyKey> const keys = o->own_keys();
    CHECK_EQ(keys.size(), 24u);
    std::uint32_t const expected_indices[] = { 0, 2, 7, 10, 30, 4294967294u };
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(keys[i] == idx(expected_indices[i]));
    for (std::size_t i = 0; i < atoms.size(); ++i)
        CHECK(keys[6 + i] == atoms[i]);
    for (std::size_t i = 0; i < symbols.size(); ++i)
        CHECK(keys[6 + atoms.size() + i] == symbols[i]);

    // properties() is the raw creation order.
    CHECK(o->properties()[0].key == atoms[0]);
    CHECK(o->properties()[1].key == idx(30));
    CHECK(o->properties()[2].key == symbols[0]);
}

// ---------------------------------------------------------------------------
void test_index_map(Fixture& fx)
{
    js::Object* o = fx.object();
    std::vector<js::PropertyKey> keys;
    for (int i = 0; i < 25; ++i) {
        keys.push_back(fx.key("k" + std::to_string(i)));
        o->put(keys.back(), num(i));
    }
    std::vector<bool> present(25, true);
    // `additional` counts keys stored beyond the 25 tracked ones.
    auto verify = [&](std::size_t additional = 0) {
        std::size_t count = additional;
        for (int i = 0; i < 25; ++i) {
            js::Property const* p = o->find_own(keys[static_cast<std::size_t>(i)]);
            if (present[static_cast<std::size_t>(i)]) {
                ++count;
                CHECK(p != nullptr && p->value == num(i));
                CHECK(is_data_value(o->get_own_property(keys[static_cast<std::size_t>(i)]), num(i)));
            } else {
                CHECK(p == nullptr);
                CHECK(!o->get_own_property(keys[static_cast<std::size_t>(i)]));
            }
        }
        CHECK_EQ(o->own_property_count(), count);
        // The storage itself agrees with the lookups.
        for (js::Property const& property : o->properties())
            CHECK(o->find_own(property.key) == &property);
    };
    verify();
    auto erase = [&](int i) {
        CHECK(o->remove_own(keys[static_cast<std::size_t>(i)]));
        present[static_cast<std::size_t>(i)] = false;
        verify();
    };
    erase(12); // the middle
    erase(0); // the front
    erase(24); // the back
    CHECK(!o->remove_own(keys[12]));
    // Insert after erase: a fresh key lands and the old ones stay right.
    js::PropertyKey const extra = fx.key("extra");
    o->put(extra, num(100));
    CHECK(o->find_own(extra) != nullptr && o->find_own(extra)->value == num(100));
    verify(1);
    CHECK(o->remove_own(extra));
    verify();
    // Down through the threshold (8) where the index is dropped, and below.
    for (int i = 1; i < 24; ++i) {
        if (present[static_cast<std::size_t>(i)])
            erase(i);
        if (o->own_property_count() == 3)
            break;
    }
    CHECK_EQ(o->own_property_count(), 3u);
    // And back up through it.
    for (int i = 0; i < 25; ++i) {
        if (present[static_cast<std::size_t>(i)])
            continue;
        o->put(keys[static_cast<std::size_t>(i)], num(i));
        present[static_cast<std::size_t>(i)] = true;
        verify();
    }
    CHECK_EQ(o->own_property_count(), 25u);

    // Exactly at the threshold and one past it, with index and symbol keys mixed in.
    js::Object* p = fx.object();
    std::vector<js::PropertyKey> mixed;
    for (std::uint32_t i = 0; i < 8; ++i) {
        mixed.push_back(i % 2 == 0 ? idx(i * 5) : fx.sym("m" + std::to_string(i)));
        p->put(mixed.back(), num(i));
    }
    for (std::uint32_t i = 0; i < 8; ++i)
        CHECK(p->find_own(mixed[i]) != nullptr && p->find_own(mixed[i])->value == num(i));
    mixed.push_back(fx.key("ninth"));
    p->put(mixed.back(), num(8));
    for (std::uint32_t i = 0; i < 9; ++i)
        CHECK(p->find_own(mixed[i]) != nullptr && p->find_own(mixed[i])->value == num(i));
    CHECK(p->find_own(idx(1)) == nullptr);
    CHECK(p->remove_own(mixed[0]));
    for (std::uint32_t i = 1; i < 9; ++i)
        CHECK(p->find_own(mixed[i]) != nullptr && p->find_own(mixed[i])->value == num(i));
    CHECK(p->find_own(mixed[0]) == nullptr);
}

// ---------------------------------------------------------------------------
void test_chain_get_set(Fixture& fx)
{
    js::Interpreter& in = no_interpreter();
    js::Object* root = fx.object();
    js::Object* proto = fx.object(root);
    js::Object* child = fx.object(proto);
    js::Value const child_value = js::Value::object(child);
    js::PropertyKey const x = fx.key("x");
    js::PropertyKey const deep = fx.key("deep");

    proto->put(x, num(1));
    root->put(deep, num(3));
    CHECK(child->has_property(x));
    CHECK(child->has_property(deep));
    CHECK(!child->has_property(fx.key("missing")));
    CHECK(child->find_own(x) == nullptr);

    // OrdinaryGet walks the chain; a miss is undefined.
    std::optional<js::Value> got = child->get(in, x, child_value);
    CHECK(got && *got == num(1));
    got = child->get(in, deep, child_value);
    CHECK(got && *got == num(3));
    got = child->get(in, fx.key("missing"), child_value);
    CHECK(got && got->is_undefined());

    // An accessor with an undefined getter reads as undefined, no call made.
    js::Object* setter = fx.native();
    proto->put_accessor(fx.key("half"), nullptr, setter, js::Configurable);
    got = child->get(in, fx.key("half"), child_value);
    CHECK(got && got->is_undefined());

    // OrdinarySet, inherited and writable: the receiver gets an own data
    // property with the default attributes; the prototype is untouched.
    std::optional<bool> ok = child->set(in, x, num(2), child_value);
    CHECK(ok && *ok);
    CHECK(is_data_value(child->get_own_property(x), num(2)));
    CHECK(has_attributes(child->get_own_property(x), true, true, true));
    CHECK(is_data_value(proto->get_own_property(x), num(1)));
    got = child->get(in, x, child_value);
    CHECK(got && *got == num(2));

    // An own writable property is updated in place, attributes kept.
    child->put(fx.key("own"), num(1), js::Writable);
    ok = child->set(in, fx.key("own"), num(5), child_value);
    CHECK(ok && *ok);
    CHECK(is_data_value(child->get_own_property(fx.key("own")), num(5)));
    CHECK(has_attributes(child->get_own_property(fx.key("own")), true, false, false));

    // Inherited non-writable refuses and creates nothing.
    proto->put(fx.key("ro"), num(5), js::Enumerable | js::Configurable);
    ok = child->set(in, fx.key("ro"), num(6), child_value);
    CHECK(ok && !*ok);
    CHECK(child->find_own(fx.key("ro")) == nullptr);

    // Own non-writable refuses.
    child->put(fx.key("mine"), num(1), 0);
    ok = child->set(in, fx.key("mine"), num(2), child_value);
    CHECK(ok && !*ok);
    CHECK(is_data_value(child->get_own_property(fx.key("mine")), num(1)));

    // A receiver that is not an object refuses.
    ok = proto->set(in, x, num(9), num(1));
    CHECK(ok && !*ok);
    ok = proto->set(in, x, num(9), js::Value::undefined());
    CHECK(ok && !*ok);
    CHECK(is_data_value(proto->get_own_property(x), num(1)));

    // A receiver whose own property for the key is an accessor refuses.
    js::Object* other = fx.object();
    other->put_accessor(x, nullptr, setter, js::Configurable);
    ok = proto->set(in, x, num(9), js::Value::object(other));
    CHECK(ok && !*ok);

    // A receiver whose own property is read-only refuses.
    js::Object* frozen = fx.object();
    frozen->put(x, num(0), js::frozen_attributes);
    ok = proto->set(in, x, num(9), js::Value::object(frozen));
    CHECK(ok && !*ok);
    CHECK(is_data_value(frozen->get_own_property(x), num(0)));

    // A non-extensible receiver cannot receive a new property.
    js::Object* sealed = fx.object(proto);
    sealed->prevent_extensions();
    ok = sealed->set(in, x, num(9), js::Value::object(sealed));
    CHECK(ok && !*ok);
    CHECK(sealed->find_own(x) == nullptr);

    // A key nowhere on the chain is created on the receiver.
    ok = child->set(in, fx.key("fresh"), num(4), child_value);
    CHECK(ok && *ok);
    CHECK(is_data_value(child->get_own_property(fx.key("fresh")), num(4)));
    CHECK(has_attributes(child->get_own_property(fx.key("fresh")), true, true, true));

    // The receiver need not be the object the set started on.
    js::Object* elsewhere = fx.object();
    ok = proto->set(in, fx.key("y"), num(7), js::Value::object(elsewhere));
    CHECK(ok && *ok);
    CHECK(is_data_value(elsewhere->get_own_property(fx.key("y")), num(7)));
    CHECK(proto->find_own(fx.key("y")) == nullptr);

    // An accessor with no setter refuses (no call is made).
    js::Object* getter = fx.native();
    proto->put_accessor(fx.key("getonly"), getter, nullptr, js::Configurable);
    ok = child->set(in, fx.key("getonly"), num(1), child_value);
    CHECK(ok && !*ok);
    CHECK(child->find_own(fx.key("getonly")) == nullptr);

    // Symbol keys walk the chain like any other.
    js::PropertyKey const s = fx.sym("s");
    root->put(s, num(11));
    got = child->get(in, s, child_value);
    CHECK(got && *got == num(11));
    CHECK(child->has_property(s));
}

// ---------------------------------------------------------------------------
void test_delete(Fixture& fx)
{
    js::Object* o = fx.object();
    o->put(fx.key("c"), num(1));
    o->put(fx.key("nc"), num(2), js::Writable | js::Enumerable);
    CHECK(o->delete_property(fx.key("c")));
    CHECK(!o->get_own_property(fx.key("c")));
    CHECK(!o->delete_property(fx.key("nc")));
    CHECK(is_data_value(o->get_own_property(fx.key("nc")), num(2)));
    CHECK(o->delete_property(fx.key("missing")));
    CHECK(o->delete_property(idx(5)));
    CHECK_EQ(o->own_property_count(), 1u);
    // Deleting through the index regime keeps the map right.
    for (int i = 0; i < 20; ++i)
        o->put(fx.key("d" + std::to_string(i)), num(i));
    for (int i = 0; i < 20; i += 3)
        CHECK(o->delete_property(fx.key("d" + std::to_string(i))));
    for (int i = 0; i < 20; ++i) {
        js::Property const* p = o->find_own(fx.key("d" + std::to_string(i)));
        if (i % 3 == 0)
            CHECK(p == nullptr);
        else
            CHECK(p != nullptr && p->value == num(i));
    }
}

// ---------------------------------------------------------------------------
void test_array_basics(Fixture& fx)
{
    js::Value const init[3] = { num(1), num(2), num(3) };
    js::ArrayObject* a = fx.array(init);
    CHECK(a->is_array());
    CHECK_EQ(a->length(), 3u);
    CHECK_EQ(a->dense_size(), 3u);
    CHECK(a->element(1) == num(2));
    CHECK(a->has_element(2));
    CHECK(!a->has_element(3));
    CHECK(a->element(7).is_empty());
    CHECK(a->is_simple_dense());

    a->push(num(4));
    CHECK_EQ(a->length(), 4u);
    CHECK_EQ(a->dense_size(), 4u);
    CHECK(a->element(3) == num(4));

    // The virtual length.
    std::optional<js::PropertyDescriptor> d = a->get_own_property(fx.length_key());
    CHECK(is_data_value(d, num(4)));
    CHECK(has_attributes(d, true, false, false));
    CHECK(a->find_own(fx.length_key()) == nullptr);
    CHECK(a->has_property(fx.length_key()));
    CHECK_EQ(a->own_property_count(), 0u);

    // Elements are plain data properties with the default attributes.
    d = a->get_own_property(idx(1));
    CHECK(is_data_value(d, num(2)));
    CHECK(has_attributes(d, true, true, true));
    CHECK(!a->get_own_property(idx(4)));
    CHECK(a->has_property(idx(0)));
    CHECK(!a->has_property(idx(4)));

    // own_keys: indices, then length.
    std::vector<js::PropertyKey> keys = a->own_keys();
    CHECK_EQ(keys.size(), 5u);
    for (std::uint32_t i = 0; i < 4; ++i)
        CHECK(keys[i] == idx(i));
    CHECK(keys[4] == fx.length_key());

    // Growing the length makes holes past the dense storage.
    CHECK(a->set_length(6));
    CHECK_EQ(a->length(), 6u);
    CHECK_EQ(a->dense_size(), 4u);
    CHECK(!a->is_simple_dense());
    CHECK(!a->has_element(5));
    CHECK(a->element(5).is_empty());
    CHECK_EQ(a->own_keys().size(), 5u);
    // Filling the end grows the dense storage with holes in between.
    a->push(num(7));
    CHECK_EQ(a->length(), 7u);
    CHECK_EQ(a->dense_size(), 7u);
    CHECK(a->element(4).is_empty() && a->element(5).is_empty() && a->element(6) == num(7));
    CHECK(!a->is_simple_dense());
    CHECK(!a->get_own_property(idx(5)));

    // Truncating drops the dense tail.
    CHECK(a->set_length(2));
    CHECK_EQ(a->length(), 2u);
    CHECK_EQ(a->dense_size(), 2u);
    CHECK(a->element(2).is_empty());
    CHECK(a->is_simple_dense());
    CHECK(a->set_length(2));

    // delete makes a hole; the length does not move.
    CHECK(a->delete_property(idx(1)));
    CHECK_EQ(a->length(), 2u);
    CHECK(a->element(1).is_empty());
    CHECK(!a->has_element(1));
    CHECK(!a->get_own_property(idx(1)));
    CHECK(!a->has_property(idx(1)));
    CHECK(!a->is_simple_dense());
    CHECK_EQ(a->own_keys().size(), 2u);
    CHECK(a->delete_property(idx(1))); // a hole deletes fine
    CHECK(!a->delete_property(fx.length_key()));

    // Filling the hole back in place.
    CHECK(a->define_own_property(idx(1), js::PropertyDescriptor::data(num(2), js::default_attributes)));
    CHECK_EQ(a->dense_size(), 2u);
    CHECK(a->element(1) == num(2));
    CHECK(a->is_simple_dense());
    CHECK_EQ(a->own_property_count(), 0u);

    // set_element and an index past the length.
    a->set_element(1, num(20));
    CHECK(a->element(1) == num(20));
    a->set_element(5, num(50));
    CHECK_EQ(a->length(), 6u);
    CHECK_EQ(a->dense_size(), 6u);
    CHECK(a->element(5) == num(50));

    // Defining an index past the length extends it.
    js::ArrayObject* b = fx.array();
    CHECK_EQ(b->length(), 0u);
    CHECK(b->define_own_property(idx(20), js::PropertyDescriptor::data(num(1), js::default_attributes)));
    CHECK_EQ(b->length(), 21u);
    CHECK_EQ(b->dense_size(), 21u);
    CHECK(b->element(20) == num(1));
    CHECK(b->element(19).is_empty());

    // Named and symbol keys after length, in creation order.
    js::PropertyKey const s = fx.sym("tag");
    b->put(fx.key("name"), num(1));
    b->put(s, num(2));
    keys = b->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(keys[0] == idx(20) && keys[1] == fx.length_key() && keys[2] == fx.key("name") && keys[3] == s);
    CHECK(b->has_property(fx.key("name")));
    CHECK(b->delete_property(fx.key("name")));

    // Elements from a span of zero, and the largest index.
    js::ArrayObject* c = fx.array();
    CHECK(c->own_keys().size() == 1 && c->own_keys()[0] == fx.length_key());
    CHECK(c->define_own_property(idx(4294967294u), value_desc(num(1))));
    CHECK_EQ(c->length(), 4294967295u);
    CHECK_EQ(c->dense_size(), 0u);
    CHECK(c->has_element(4294967294u));
    c->push(num(1)); // nowhere left to push: unchanged
    CHECK_EQ(c->length(), 4294967295u);
}

void test_array_sparse(Fixture& fx)
{
    js::Value const init[2] = { num(0), num(1) };
    js::ArrayObject* a = fx.array(init);

    // Far away: an ordinary property, no dense growth.
    CHECK(a->define_own_property(idx(5000), js::PropertyDescriptor::data(num(5), js::default_attributes)));
    CHECK_EQ(a->own_property_count(), 1u);
    CHECK_EQ(a->dense_size(), 2u);
    CHECK_EQ(a->length(), 5001u);
    CHECK(a->element(5000) == num(5));
    CHECK(a->has_element(5000));
    CHECK(a->has_property(idx(5000)));
    CHECK(is_data_value(a->get_own_property(idx(5000)), num(5)));
    CHECK(!a->is_simple_dense());
    std::vector<js::PropertyKey> keys = a->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(5000) && keys[3] == fx.length_key());

    // Near: the dense storage grows with holes.
    CHECK(a->define_own_property(idx(100), js::PropertyDescriptor::data(num(100), js::default_attributes)));
    CHECK_EQ(a->dense_size(), 101u);
    CHECK(a->element(50).is_empty());
    CHECK(a->element(100) == num(100));
    CHECK_EQ(a->own_property_count(), 1u);
    // Exactly 1024 past the end is too far, 1023 is not.
    CHECK(a->define_own_property(idx(101 + 1024), js::PropertyDescriptor::data(num(1), js::default_attributes)));
    CHECK_EQ(a->dense_size(), 101u);
    CHECK_EQ(a->own_property_count(), 2u);
    CHECK(a->define_own_property(idx(101 + 1023), js::PropertyDescriptor::data(num(2), js::default_attributes)));
    CHECK_EQ(a->dense_size(), 101u + 1024u);
    CHECK_EQ(a->own_property_count(), 2u);
    keys = a->own_keys();
    CHECK_EQ(keys.size(), 7u);
    CHECK(keys[2] == idx(100) && keys[3] == idx(1124) && keys[4] == idx(1125) && keys[5] == idx(5000));
    CHECK(keys[6] == fx.length_key());

    // An ordinary index inside the would-be growth range blocks growth.
    js::ArrayObject* b = fx.array(init);
    CHECK(b->define_own_property(idx(5), js::PropertyDescriptor::data(num(5), js::Writable | js::Enumerable)));
    CHECK_EQ(b->own_property_count(), 1u);
    CHECK_EQ(b->dense_size(), 2u);
    CHECK(b->define_own_property(idx(9), js::PropertyDescriptor::data(num(9), js::default_attributes)));
    CHECK_EQ(b->own_property_count(), 2u);
    CHECK_EQ(b->dense_size(), 2u);
    CHECK_EQ(b->length(), 10u);
    keys = b->own_keys();
    CHECK_EQ(keys.size(), 5u);
    CHECK(keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(5) && keys[3] == idx(9) && keys[4] == fx.length_key());
    b->set_element(3, num(3)); // below 5: grows fine
    CHECK_EQ(b->dense_size(), 4u);
    b->set_element(7, num(7)); // over 5: ordinary
    CHECK_EQ(b->dense_size(), 4u);
    CHECK(b->element(7) == num(7));
    b->set_element(7, num(8)); // an existing ordinary index is updated there
    CHECK(b->element(7) == num(8));
    CHECK_EQ(b->own_property_count(), 3u);
    CHECK(has_attributes(b->get_own_property(idx(7)), true, true, true));

    // A non-default descriptor on a dense element moves it and everything
    // above it out of the dense storage; the values are unchanged.
    js::Value const four[4] = { num(1), num(2), num(3), num(4) };
    js::ArrayObject* h = fx.array(four);
    CHECK(h->define_own_property(idx(1), flags_desc(false, {}, {})));
    CHECK_EQ(h->dense_size(), 1u);
    CHECK_EQ(h->own_property_count(), 3u);
    CHECK_EQ(h->length(), 4u);
    std::optional<js::PropertyDescriptor> d = h->get_own_property(idx(1));
    CHECK(is_data_value(d, num(2)));
    CHECK(has_attributes(d, false, true, true));
    d = h->get_own_property(idx(3));
    CHECK(is_data_value(d, num(4)));
    CHECK(has_attributes(d, true, true, true));
    CHECK(h->element(3) == num(4));
    CHECK(h->has_element(2));
    CHECK(!h->is_simple_dense());
    keys = h->own_keys();
    CHECK_EQ(keys.size(), 5u);
    CHECK(keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(2) && keys[3] == idx(3) && keys[4] == fx.length_key());
    CHECK(h->define_own_property(idx(1), value_desc(num(9)))); // configurable: a value change is allowed
    CHECK(h->element(1) == num(9));
    // Deleting a moved element removes it; a non-configurable one refuses.
    CHECK(h->define_own_property(idx(2), flags_desc({}, {}, false)));
    CHECK(!h->delete_property(idx(2)));
    CHECK(h->delete_property(idx(3)));
    CHECK(!h->has_element(3));
    CHECK_EQ(h->length(), 4u);

    // An accessor on a hole inside the dense range.
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::ArrayObject* i = fx.array(three);
    CHECK(i->delete_property(idx(1)));
    js::Object* getter = fx.native();
    CHECK(i->define_own_property(idx(1), getter_desc(getter)));
    CHECK_EQ(i->dense_size(), 1u);
    CHECK_EQ(i->own_property_count(), 2u);
    d = i->get_own_property(idx(1));
    CHECK(d && d->is_accessor() && *d->get == getter);
    CHECK(i->has_element(1));
    CHECK(i->element(1).is_empty());
    CHECK(i->element(2) == num(3));
    keys = i->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(2) && keys[3] == fx.length_key());

    // A new element with non-default attributes past the dense end.
    js::ArrayObject* j = fx.array(three);
    CHECK(j->define_own_property(idx(3), js::PropertyDescriptor::data(num(4), js::frozen_attributes)));
    CHECK_EQ(j->dense_size(), 3u);
    CHECK_EQ(j->own_property_count(), 1u);
    CHECK_EQ(j->length(), 4u);
    CHECK(has_attributes(j->get_own_property(idx(3)), false, false, false));
    CHECK(!j->define_own_property(idx(3), value_desc(num(5))));
    CHECK(!j->delete_property(idx(3)));
    // Truncation stops at it, having removed what lies above.
    j->push(num(6)); // index 4: ordinary because 3 is ordinary
    CHECK_EQ(j->length(), 5u);
    CHECK(!j->set_length(0));
    CHECK_EQ(j->length(), 4u);
    CHECK(!j->has_element(4));
    CHECK(j->has_element(3));
    CHECK_EQ(j->dense_size(), 3u);
    CHECK(j->element(2) == num(3));
}

void test_array_length(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::Interpreter& in = no_interpreter();

    // Making length read-only, and what it refuses afterwards.
    js::ArrayObject* c = fx.array(three);
    CHECK(c->define_own_property(fx.length_key(), flags_desc(false, {}, {})));
    CHECK(has_attributes(c->get_own_property(fx.length_key()), false, false, false));
    CHECK(!c->set_length(5));
    CHECK(!c->set_length(1));
    CHECK(c->set_length(3));
    CHECK_EQ(c->length(), 3u);
    CHECK(!c->define_own_property(idx(3), value_desc(num(4))));
    CHECK(!c->has_element(3));
    CHECK(c->define_own_property(idx(1), value_desc(num(9))));
    CHECK(c->element(1) == num(9));
    CHECK(c->define_own_property(fx.length_key(), value_desc(num(3))));
    CHECK(!c->define_own_property(fx.length_key(), value_desc(num(4))));
    CHECK(!c->define_own_property(fx.length_key(), value_desc(num(2))));
    CHECK(!c->define_own_property(fx.length_key(), flags_desc(true, {}, {})));
    CHECK(c->define_own_property(fx.length_key(), flags_desc(false, {}, {})));
    CHECK(c->define_own_property(fx.length_key(), js::PropertyDescriptor {}));
    CHECK(!c->delete_property(fx.length_key()));
    // The set path refuses too, and creates nothing.
    std::optional<bool> ok = c->set(in, idx(5), num(1), js::Value::object(c));
    CHECK(ok && !*ok);
    CHECK(!c->has_element(5));
    CHECK_EQ(c->length(), 3u);
    ok = c->set(in, idx(0), num(7), js::Value::object(c)); // below the length: fine
    CHECK(ok && *ok);
    CHECK(c->element(0) == num(7));

    // length is never enumerable or configurable.
    js::ArrayObject* d = fx.array(three);
    CHECK(!d->define_own_property(fx.length_key(), flags_desc({}, true, {})));
    CHECK(!d->define_own_property(fx.length_key(), flags_desc({}, {}, true)));
    CHECK(d->define_own_property(fx.length_key(), flags_desc({}, false, false)));
    CHECK(!d->define_own_property(fx.length_key(), getter_desc(fx.native())));

    // ArraySetLength through define: truncate, grow, and grow read-only.
    CHECK(d->define_own_property(fx.length_key(), value_desc(num(2))));
    CHECK_EQ(d->length(), 2u);
    CHECK_EQ(d->dense_size(), 2u);
    js::PropertyDescriptor grow_locked = value_desc(num(10));
    grow_locked.writable = false;
    CHECK(d->define_own_property(fx.length_key(), grow_locked));
    CHECK_EQ(d->length(), 10u);
    CHECK(has_attributes(d->get_own_property(fx.length_key()), false, false, false));
    CHECK(!d->define_own_property(fx.length_key(), value_desc(num(3))));
    CHECK(!d->set_length(11));

    // writable: false on a truncation is sticky.
    js::ArrayObject* e = fx.array(three);
    js::PropertyDescriptor shrink_locked = value_desc(num(1));
    shrink_locked.writable = false;
    CHECK(e->define_own_property(fx.length_key(), shrink_locked));
    CHECK_EQ(e->length(), 1u);
    CHECK(has_attributes(e->get_own_property(fx.length_key()), false, false, false));
    CHECK(!e->set_length(0));

    // What is not a valid length is refused, never truncated to.
    js::ArrayObject* f = fx.array(three);
    CHECK(!f->define_own_property(fx.length_key(), value_desc(num(1.5))));
    CHECK(!f->define_own_property(fx.length_key(), value_desc(num(-1))));
    CHECK(!f->define_own_property(fx.length_key(), value_desc(num(4294967296.0))));
    CHECK(!f->define_own_property(fx.length_key(), value_desc(num(std::numeric_limits<double>::quiet_NaN()))));
    CHECK(!f->define_own_property(fx.length_key(), value_desc(num(std::numeric_limits<double>::infinity()))));
    CHECK(!f->define_own_property(fx.length_key(), value_desc(fx.str("2"))));
    CHECK(!f->define_own_property(fx.length_key(), value_desc(js::Value::undefined())));
    CHECK_EQ(f->length(), 3u);
    CHECK(f->define_own_property(fx.length_key(), value_desc(num(4294967295.0))));
    CHECK_EQ(f->length(), 4294967295u);
    CHECK(f->define_own_property(fx.length_key(), value_desc(num(-0.0))));
    CHECK_EQ(f->length(), 0u);

    // A truncation blocked by a non-configurable element stops just above
    // it, keeps what lies below, and reports failure.
    js::Value const five[5] = { num(0), num(1), num(2), num(3), num(4) };
    js::ArrayObject* g = fx.array(five);
    CHECK(g->define_own_property(idx(7), js::PropertyDescriptor::data(num(7), js::Writable | js::Enumerable)));
    // 9 and 12 carry the default attributes but land as ordinary
    // properties, since the non-configurable 7 sits below them; both delete.
    CHECK(g->define_own_property(idx(9), js::PropertyDescriptor::data(num(9), js::default_attributes)));
    CHECK(g->define_own_property(idx(12), js::PropertyDescriptor::data(num(12), js::default_attributes)));
    CHECK_EQ(g->length(), 13u);
    CHECK_EQ(g->own_property_count(), 3u);
    CHECK_EQ(g->dense_size(), 5u);
    CHECK(!g->set_length(2));
    CHECK_EQ(g->length(), 8u);
    CHECK(!g->has_element(9) && !g->has_element(12));
    CHECK(g->has_element(7));
    CHECK_EQ(g->dense_size(), 5u);
    CHECK(g->element(4) == num(4));
    CHECK(has_attributes(g->get_own_property(fx.length_key()), true, false, false));
    // Through define with writable: false the lock lands even though the
    // truncation fails (§10.4.2.4 step 16.d).
    CHECK(!g->define_own_property(fx.length_key(), shrink_locked));
    CHECK_EQ(g->length(), 8u);
    CHECK(has_attributes(g->get_own_property(fx.length_key()), false, false, false));
    CHECK(!g->set_length(8u + 1u));
    CHECK(g->set_length(8));
    // Only a non-configurable element blocks: a writable one goes quietly.
    js::ArrayObject* k = fx.array(five);
    CHECK(k->define_own_property(idx(6), js::PropertyDescriptor::data(num(6), js::Enumerable | js::Configurable)));
    CHECK(k->set_length(1));
    CHECK_EQ(k->length(), 1u);
    CHECK_EQ(k->own_property_count(), 0u);
    CHECK_EQ(k->dense_size(), 1u);
    // A blocker below the new length is not in the way at all.
    js::ArrayObject* l = fx.array(five);
    CHECK(l->define_own_property(idx(1), flags_desc({}, {}, false)));
    CHECK(l->set_length(3));
    CHECK_EQ(l->length(), 3u);
    CHECK(l->has_element(2) && l->has_element(1));
    CHECK(!l->has_element(3));

    // Setting through the ordinary path: a new index extends the length,
    // a dense one is updated in place, and the receiver rules hold.
    js::ArrayObject* j = fx.array();
    j->push(num(1));
    ok = j->set(in, idx(5), num(9), js::Value::object(j));
    CHECK(ok && *ok);
    CHECK_EQ(j->length(), 6u);
    CHECK(j->element(5) == num(9));
    CHECK(has_attributes(j->get_own_property(idx(5)), true, true, true));
    ok = j->set(in, idx(0), num(2), js::Value::object(j));
    CHECK(ok && *ok);
    CHECK(j->element(0) == num(2));
    std::optional<js::Value> got = j->get(in, idx(0), js::Value::object(j));
    CHECK(got && *got == num(2));
    got = j->get(in, fx.length_key(), js::Value::object(j));
    CHECK(got && *got == num(6));
    got = j->get(in, idx(3), js::Value::object(j));
    CHECK(got && got->is_undefined());
    // An array as a prototype: a hole reads through it, a set lands on the receiver.
    js::Object* child = fx.object(j);
    ok = child->set(in, idx(0), num(7), js::Value::object(child));
    CHECK(ok && *ok);
    CHECK(j->element(0) == num(2));
    CHECK(is_data_value(child->get_own_property(idx(0)), num(7)));
    got = child->get(in, idx(5), js::Value::object(child));
    CHECK(got && *got == num(9));
    CHECK(child->has_property(idx(5)));
    CHECK(child->has_property(fx.length_key()));
    js::Object* proto = fx.object();
    proto->put(idx(3), num(33));
    CHECK(j->set_prototype(proto));
    got = j->get(in, idx(3), js::Value::object(j));
    CHECK(got && *got == num(33));
    CHECK(j->has_property(idx(3)));
    CHECK(!j->has_element(3));

    // A non-extensible array takes no new element.
    js::ArrayObject* m = fx.array(three);
    m->prevent_extensions();
    CHECK(!m->define_own_property(idx(3), value_desc(num(4))));
    ok = m->set(in, idx(9), num(1), js::Value::object(m));
    CHECK(ok && !*ok);
    CHECK_EQ(m->length(), 3u);
    CHECK(m->define_own_property(idx(0), value_desc(num(0))));
    CHECK(m->set_length(1));
    CHECK_EQ(m->length(), 1u);
}

void test_array_simple_dense(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::Object* plain = fx.object();
    js::ArrayObject* a = fx.array(three, plain);
    CHECK(a->is_simple_dense());
    // A hole, a sparse element, or an attribute change breaks it.
    CHECK(a->delete_property(idx(1)));
    CHECK(!a->is_simple_dense());
    a->set_element(1, num(2));
    CHECK(a->is_simple_dense());
    CHECK(a->define_own_property(idx(2), flags_desc({}, false, {})));
    CHECK(!a->is_simple_dense());

    // A prototype with an index property breaks it, anywhere up the chain.
    js::ArrayObject* b = fx.array(three, plain);
    CHECK(b->is_simple_dense());
    js::Object* far = fx.object();
    CHECK(plain->set_prototype(far));
    CHECK(b->is_simple_dense());
    far->put(idx(0), num(0));
    CHECK(!b->is_simple_dense());
    CHECK(far->remove_own(idx(0)));
    CHECK(b->is_simple_dense());

    // An array prototype: fine while it has no elements.
    js::ArrayObject* proto_array = fx.array();
    js::ArrayObject* c = fx.array(three, proto_array);
    CHECK(c->is_simple_dense());
    proto_array->push(num(1));
    CHECK(!c->is_simple_dense());
    CHECK(proto_array->set_length(0));
    CHECK(c->is_simple_dense());
    CHECK(proto_array->define_own_property(idx(0), value_desc(num(1))));
    CHECK(!c->is_simple_dense());

    // A string prototype: fine while it is empty.
    js::StringObject* empty_string = fx.make<js::StringObject>(nullptr, fx.string_cell(""));
    js::ArrayObject* d = fx.array(three, empty_string);
    CHECK(d->is_simple_dense());
    js::StringObject* text = fx.make<js::StringObject>(nullptr, fx.string_cell("x"));
    CHECK(d->set_prototype(text));
    CHECK(!d->is_simple_dense());

    // A host object may answer indices the storage does not show.
    js::Object* host = fx.make<js::Object>(nullptr, js::Object::Class::Host);
    js::ArrayObject* e = fx.array(three, host);
    CHECK(!e->is_simple_dense());

    // A length past the storage is holes.
    js::ArrayObject* f = fx.array(three);
    CHECK(f->set_length(4));
    CHECK(!f->is_simple_dense());
    CHECK(f->set_length(3));
    CHECK(f->is_simple_dense());
    js::ArrayObject* g = fx.array();
    CHECK(g->is_simple_dense());
}

// ---------------------------------------------------------------------------
void test_string_object(Fixture& fx)
{
    js::Interpreter& in = no_interpreter();
    js::StringObject* s = fx.make<js::StringObject>(nullptr, fx.string_cell("abc"));
    CHECK(s->class_id() == js::Object::Class::String);
    CHECK(s->string()->view() == u"abc");
    CHECK(s->primitive().is_string());

    // Code units: read-only, enumerable, non-configurable, one-unit strings.
    std::optional<js::PropertyDescriptor> d = s->get_own_property(idx(0));
    CHECK(d && d->is_data() && d->value && string_equals(*d->value, u"a"));
    CHECK(has_attributes(d, false, true, false));
    d = s->get_own_property(idx(2));
    CHECK(d && d->value && string_equals(*d->value, u"c"));
    CHECK(!s->get_own_property(idx(3)));
    // The same unit is the same cell, across wrappers.
    js::StringObject* t = fx.make<js::StringObject>(nullptr, fx.string_cell("cab"));
    CHECK(s->get_own_property(idx(0))->value->as_string() == t->get_own_property(idx(1))->value->as_string());
    CHECK(s->get_own_property(idx(0))->value->as_string()->equals(*t->get_own_property(idx(1))->value->as_string()));

    // length: read-only, non-enumerable, non-configurable.
    d = s->get_own_property(fx.length_key());
    CHECK(is_data_value(d, num(3)));
    CHECK(has_attributes(d, false, false, false));
    CHECK(s->find_own(fx.length_key()) == nullptr);
    CHECK_EQ(s->own_property_count(), 0u);

    CHECK(s->has_property(idx(2)));
    CHECK(!s->has_property(idx(3)));
    CHECK(s->has_property(fx.length_key()));
    CHECK(!s->has_property(fx.key("x")));

    // define accepts only what already holds.
    CHECK(s->define_own_property(idx(0), value_desc(fx.str("a"))));
    CHECK(!s->define_own_property(idx(0), value_desc(fx.str("b"))));
    CHECK(s->define_own_property(idx(0), flags_desc({}, true, {})));
    CHECK(!s->define_own_property(idx(0), flags_desc({}, false, {})));
    CHECK(!s->define_own_property(idx(0), flags_desc(true, {}, {})));
    CHECK(s->define_own_property(idx(0), flags_desc(false, true, false)));
    CHECK(!s->define_own_property(idx(0), flags_desc({}, {}, true)));
    CHECK(!s->define_own_property(idx(0), getter_desc(fx.native())));
    CHECK(s->define_own_property(idx(0), js::PropertyDescriptor {}));
    CHECK(s->define_own_property(fx.length_key(), value_desc(num(3))));
    CHECK(!s->define_own_property(fx.length_key(), value_desc(num(4))));
    CHECK(!s->define_own_property(fx.length_key(), flags_desc(true, {}, {})));
    CHECK(s->define_own_property(fx.length_key(), flags_desc(false, false, false)));
    CHECK_EQ(s->own_property_count(), 0u);

    // delete refuses the units and the length; a missing key deletes fine.
    CHECK(!s->delete_property(idx(0)));
    CHECK(!s->delete_property(fx.length_key()));
    CHECK(s->delete_property(idx(5)));

    // Ordinary properties beside them, and the key order.
    s->put(idx(5), num(5));
    js::PropertyKey const tag = fx.sym("tag");
    s->put(fx.key("name"), num(1));
    s->put(tag, num(2));
    CHECK(s->define_own_property(idx(7), value_desc(num(7))));
    CHECK(is_data_value(s->get_own_property(idx(7)), num(7)));
    CHECK(s->has_property(idx(7)));
    std::vector<js::PropertyKey> const keys = s->own_keys();
    CHECK_EQ(keys.size(), 8u);
    CHECK(keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(2));
    CHECK(keys[3] == idx(5) && keys[4] == idx(7));
    CHECK(keys[5] == fx.length_key());
    CHECK(keys[6] == fx.key("name") && keys[7] == tag);
    CHECK(s->delete_property(idx(5)));
    CHECK(!s->has_property(idx(5)));

    // get and set over the wrapper: a unit reads, refuses a write, and a
    // free index takes one.
    std::optional<js::Value> got = s->get(in, idx(1), js::Value::object(s));
    CHECK(got && string_equals(*got, u"b"));
    got = s->get(in, fx.length_key(), js::Value::object(s));
    CHECK(got && *got == num(3));
    std::optional<bool> ok = s->set(in, idx(0), fx.str("z"), js::Value::object(s));
    CHECK(ok && !*ok);
    ok = s->set(in, fx.length_key(), num(9), js::Value::object(s));
    CHECK(ok && !*ok);
    ok = s->set(in, idx(9), num(9), js::Value::object(s));
    CHECK(ok && *ok);
    CHECK(is_data_value(s->get_own_property(idx(9)), num(9)));
    // Through a child: the unit is inherited read-only.
    js::Object* child = fx.object(s);
    ok = child->set(in, idx(0), num(1), js::Value::object(child));
    CHECK(ok && !*ok);
    got = child->get(in, idx(2), js::Value::object(child));
    CHECK(got && string_equals(*got, u"c"));
    CHECK(child->has_property(fx.length_key()));

    // The empty wrapper.
    js::StringObject* e = fx.make<js::StringObject>(nullptr, fx.string_cell(""));
    CHECK(!e->get_own_property(idx(0)));
    CHECK(is_data_value(e->get_own_property(fx.length_key()), num(0)));
    CHECK(e->own_keys().size() == 1 && e->own_keys()[0] == fx.length_key());
    CHECK(e->define_own_property(idx(0), value_desc(num(1))));
    CHECK(is_data_value(e->get_own_property(idx(0)), num(1)));

    // A lone surrogate is a unit like any other.
    js::JsString* lone = fx.heap.string(std::u16string(u"x\xD800y"));
    fx.cells.push_back(lone);
    js::StringObject* l = fx.make<js::StringObject>(nullptr, lone);
    d = l->get_own_property(idx(1));
    CHECK(d && d->value && d->value->as_string()->length() == 1 && d->value->as_string()->data()[0] == 0xD800);
    CHECK(is_data_value(l->get_own_property(fx.length_key()), num(3)));
}

// ---------------------------------------------------------------------------
void test_environment(Fixture& fx)
{
    js::JsString* a = fx.heap.atom("a"sv);
    js::JsString* b = fx.heap.atom("b"sv);
    js::JsString* c = fx.heap.atom("c"sv);
    js::Environment* outer = fx.make<js::Environment>(nullptr);
    js::Environment* inner = fx.make<js::Environment>(outer);
    CHECK(inner->outer() == outer && outer->outer() == nullptr);
    CHECK(!inner->is_object_environment() && !inner->is_with_environment() && !inner->has_this());

    js::Environment::Binding& outer_a = outer->declare(a, num(1));
    CHECK(outer_a.name == a && outer_a.value == num(1));
    CHECK(outer_a.mutable_ && outer_a.initialized && !outer_a.deletable);
    CHECK(outer->find(a) == &outer_a);
    CHECK(outer->find(b) == nullptr);

    // Shadowing: each scope answers for itself; the chain is the caller's to walk.
    inner->declare(a, num(2));
    CHECK(inner->find(a) != nullptr && inner->find(a)->value == num(2));
    CHECK(outer->find(a)->value == num(1));
    CHECK(inner->find(b) == nullptr);
    js::Environment const* const_inner = inner;
    CHECK(const_inner->find(a) != nullptr && const_inner->find(a)->value == num(2));
    CHECK(const_inner->find(c) == nullptr);

    // Declaring again returns the existing binding untouched.
    js::Environment::Binding& again = outer->declare(a, num(9), false, false, true);
    CHECK(&again == outer->find(a));
    CHECK(again.value == num(1) && again.mutable_ && again.initialized && !again.deletable);
    CHECK_EQ(outer->bindings().size(), 1u);

    // const, the temporal dead zone, and a deletable var.
    js::Environment::Binding& konst = outer->declare(b, num(3), false);
    CHECK(!konst.mutable_ && konst.initialized);
    js::Environment::Binding& tdz = outer->declare(c, js::Value::empty(), true, false);
    CHECK(!tdz.initialized && tdz.value.is_empty());
    js::JsString* d = fx.heap.atom("d"sv);
    outer->declare(d, num(4), true, true, true);
    CHECK_EQ(outer->bindings().size(), 4u);

    // remove takes only deletable bindings.
    CHECK(!outer->remove(a));
    CHECK(!outer->remove(b));
    CHECK(outer->find(a) != nullptr);
    CHECK(outer->remove(d));
    CHECK(outer->find(d) == nullptr);
    CHECK(!outer->remove(d));
    CHECK(!inner->remove(a));
    CHECK_EQ(outer->bindings().size(), 3u);
    // Names compare by atom identity: a different cell with the same text is another name.
    js::JsString* a_copy = fx.string_cell("a");
    CHECK(outer->find(a_copy) == nullptr);

    // A function environment's this, function and new.target; a with's object.
    js::Object* receiver = fx.object();
    js::NativeFunction* fn = fx.native();
    js::Environment* call = fx.make<js::Environment>(outer);
    call->set_this(js::Value::object(receiver));
    call->set_function(fn);
    call->set_new_target(fn);
    CHECK(call->has_this() && call->this_value() == js::Value::object(receiver));
    CHECK(call->function() == fn && call->new_target() == fn);
    js::Environment* with = fx.make<js::Environment>(call, receiver);
    with->set_with_environment(true);
    CHECK(with->is_object_environment() && with->object() == receiver && with->is_with_environment());
}

// ---------------------------------------------------------------------------
void test_functions(Fixture& fx)
{
    js::NativeFunction* fn = fx.native();
    CHECK(fn->is_callable());
    CHECK(!fn->is_constructor());
    CHECK(fn->class_id() == js::Object::Class::Function);
    js::NativeFunction* ctor = fx.make<js::NativeFunction>(nullptr,
        js::NativeFunction::Callback([](js::Interpreter&, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
            return js::Value::undefined();
        }),
        js::NativeFunction::ConstructCallback([](js::Interpreter&, std::span<js::Value const>, js::Object*) -> std::optional<js::Value> {
            return js::Value::undefined();
        }));
    CHECK(ctor->is_constructor());

    std::vector<js::Value> bound_arguments = { num(1), fx.str("two") };
    js::BoundFunction* bound = fx.make<js::BoundFunction>(nullptr, fn, fx.str("this"), bound_arguments);
    CHECK(bound->is_callable());
    CHECK(!bound->is_constructor());
    CHECK(bound->target() == fn);
    CHECK(bound->class_id() == js::Object::Class::BoundFunction);
    js::BoundFunction* bound_ctor = fx.make<js::BoundFunction>(nullptr, ctor, js::Value::undefined(), std::vector<js::Value> {});
    CHECK(bound_ctor->is_constructor());

    js::FunctionNode node;
    node.is_arrow = true;
    node.is_strict = true;
    js::Environment* scope = fx.make<js::Environment>(nullptr);
    js::ScriptFunction* arrow = fx.make<js::ScriptFunction>(nullptr, node, scope, false);
    CHECK(arrow->is_arrow() && arrow->is_strict() && !arrow->is_constructor() && arrow->is_callable());
    CHECK(arrow->scope() == scope && &arrow->node() == &node);
    js::FunctionNode plain;
    js::ScriptFunction* normal = fx.make<js::ScriptFunction>(nullptr, plain, scope, true);
    CHECK(!normal->is_arrow() && !normal->is_strict() && normal->is_constructor());

    js::PrimitiveObject* boxed = fx.make<js::PrimitiveObject>(nullptr, js::Object::Class::Number, num(3));
    CHECK(boxed->primitive() == num(3) && boxed->class_id() == js::Object::Class::Number);
    js::DateObject* date = fx.make<js::DateObject>(nullptr, 5.0);
    CHECK(date->time_value() == 5.0);
    js::ErrorObject* error = fx.make<js::ErrorObject>(nullptr);
    CHECK(error->is_error());
}

// ---------------------------------------------------------------------------
// Everything reachable from a single Persistent survives a collection
// under stress, through prototypes, keys, values, getters, setters,
// elements, bindings and the function objects' own links; and once the
// root lets go, all of it is reclaimed.
void test_trace_under_stress(Fixture& fx)
{
    js::Heap& heap = fx.heap;
    // Atoms are permanent: make them before counting.
    js::PropertyKey const k_value = fx.key("value");
    js::PropertyKey const k_acc = fx.key("acc");
    js::PropertyKey const k_env = fx.key("env");
    js::PropertyKey const k_with = fx.key("with");
    js::PropertyKey const k_arr = fx.key("arr");
    js::PropertyKey const k_bound = fx.key("bound");
    js::PropertyKey const k_bound_with_argument = fx.key("boundWithArgument");
    js::PropertyKey const k_script = fx.key("script");
    js::PropertyKey const k_boxed = fx.key("boxed");
    js::PropertyKey const k_regexp = fx.key("regexp");
    js::PropertyKey const k_string = fx.key("string");
    js::JsString* const name_a = heap.atom("a"sv);
    js::JsString* const name_b = heap.atom("b"sv);
    js::JsString* const regexp_source = heap.atom("a+"sv);
    js::JsString* const regexp_flags = heap.atom("g"sv);
    js::FunctionNode node;
    heap.collect();
    std::size_t const before = heap.cell_count();

    // Every cell is attached to the graph before the next allocation, so
    // the Persistent alone keeps it.
    std::vector<js::Cell*> made;
    js::Persistent root(heap);
    js::Object* top = heap.allocate<js::Object>(nullptr);
    root.set(js::Value::object(top));
    made.push_back(top);
    js::Object* proto = heap.allocate<js::Object>(nullptr);
    CHECK(top->set_prototype(proto));
    made.push_back(proto);
    js::Object* proto2 = heap.allocate<js::Object>(nullptr);
    CHECK(proto->set_prototype(proto2));
    made.push_back(proto2);
    js::JsString* text = heap.string("held"sv);
    proto2->put(k_value, js::Value::string(text));
    made.push_back(text);
    // A symbol key and an object value.
    js::Symbol* symbol = heap.symbol(nullptr);
    top->put(js::PropertyKey::symbol(symbol), js::Value::undefined());
    made.push_back(symbol);
    js::Object* via_symbol = heap.allocate<js::Object>(nullptr);
    top->put(js::PropertyKey::symbol(symbol), js::Value::object(via_symbol));
    made.push_back(via_symbol);
    // A getter and a setter.
    js::Object* getter = heap.allocate<js::Object>(nullptr);
    top->put_accessor(k_acc, getter, nullptr);
    made.push_back(getter);
    js::Object* setter = heap.allocate<js::Object>(nullptr);
    top->put_accessor(k_acc, getter, setter);
    made.push_back(setter);
    // Over the index threshold, so the hash index is in play as well.
    for (int i = 0; i < 12; ++i) {
        js::Object* filler = heap.allocate<js::Object>(nullptr);
        top->put(js::PropertyKey::index(static_cast<std::uint32_t>(i)), js::Value::object(filler));
        made.push_back(filler);
    }
    // An array with dense and sparse elements.
    js::ArrayObject* array = heap.allocate<js::ArrayObject>(nullptr);
    top->put(k_arr, js::Value::object(array));
    made.push_back(array);
    js::JsString* element = heap.string("element"sv);
    array->push(js::Value::string(element));
    made.push_back(element);
    js::Object* sparse = heap.allocate<js::Object>(nullptr);
    array->set_element(5000, js::Value::object(sparse));
    made.push_back(sparse);
    // Environments are cells but not values, so each is reached through
    // the function that closes over it; the pair is made under NoCollect
    // because the scope is unrooted until the function holds it.
    js::Object* holder = heap.allocate<js::Object>(nullptr);
    top->put(k_env, js::Value::object(holder));
    made.push_back(holder);
    js::Environment* outer = nullptr;
    {
        js::Heap::NoCollect const guard(heap);
        outer = heap.allocate<js::Environment>(nullptr);
        js::ScriptFunction* script = heap.allocate<js::ScriptFunction>(nullptr, node, outer, true);
        holder->put(k_script, js::Value::object(script));
        made.push_back(script);
        made.push_back(outer);
    }
    js::JsString* bound_text = heap.string("binding"sv);
    outer->declare(name_a, js::Value::string(bound_text));
    made.push_back(bound_text);
    js::Environment* inner = nullptr;
    {
        js::Heap::NoCollect const guard(heap);
        inner = heap.allocate<js::Environment>(outer);
        js::ScriptFunction* inner_function = heap.allocate<js::ScriptFunction>(nullptr, node, inner, false);
        holder->put(k_value, js::Value::object(inner_function));
        made.push_back(inner);
        made.push_back(inner_function);
    }
    // A with-style environment over an object, with this, function and
    // new.target set: every link an Environment traces.
    js::Object* with_object = heap.allocate<js::Object>(nullptr);
    holder->put(k_with, js::Value::object(with_object));
    made.push_back(with_object);
    js::Environment* call = nullptr;
    js::ScriptFunction* call_function = nullptr;
    {
        js::Heap::NoCollect const guard(heap);
        call = heap.allocate<js::Environment>(inner, with_object);
        call_function = heap.allocate<js::ScriptFunction>(nullptr, node, call, false);
        inner->declare(name_b, js::Value::object(call_function));
        made.push_back(call);
        made.push_back(call_function);
    }
    holder->remove_own(k_with); // now held by the environment alone
    js::Object* this_object = heap.allocate<js::Object>(nullptr);
    call->set_this(js::Value::object(this_object));
    made.push_back(this_object);
    js::NativeFunction* native = heap.allocate<js::NativeFunction>(nullptr, js::NativeFunction::Callback {});
    call->set_function(native);
    made.push_back(native);
    js::Object* new_target = heap.allocate<js::Object>(nullptr);
    call->set_new_target(new_target);
    made.push_back(new_target);
    // Bound functions: the target, and then a bound argument that nothing
    // else holds once the temporary link on top is cut.
    js::BoundFunction* bound = heap.allocate<js::BoundFunction>(nullptr, native, js::Value::undefined(), std::vector<js::Value> {});
    top->put(k_bound, js::Value::object(bound));
    made.push_back(bound);
    js::JsString* argument_text = heap.string("argument"sv);
    top->put(k_value, js::Value::string(argument_text));
    js::BoundFunction* bound_with_argument = heap.allocate<js::BoundFunction>(nullptr, bound, js::Value::object(top), std::vector<js::Value> { js::Value::string(argument_text) });
    top->put(k_bound_with_argument, js::Value::object(bound_with_argument));
    top->put(k_value, js::Value::undefined());
    made.push_back(bound_with_argument);
    made.push_back(argument_text);
    // Wrappers: a boxed number, a string wrapper, a regexp.
    js::PrimitiveObject* boxed = heap.allocate<js::PrimitiveObject>(nullptr, js::Object::Class::Number, num(1));
    top->put(k_boxed, js::Value::object(boxed));
    made.push_back(boxed);
    js::JsString* wrapped = heap.string("wrapped"sv);
    top->put(k_string, js::Value::string(wrapped));
    js::StringObject* string_object = heap.allocate<js::StringObject>(nullptr, wrapped);
    top->put(k_string, js::Value::object(string_object));
    made.push_back(wrapped);
    made.push_back(string_object);
    std::optional<js::Regex> regex = js::Regex::compile(u"a+", js::RegexFlags {});
    CHECK(regex.has_value());
    js::RegExpObject* regexp = heap.allocate<js::RegExpObject>(nullptr, std::move(*regex), regexp_source, regexp_flags);
    top->put(k_regexp, js::Value::object(regexp));
    made.push_back(regexp);

    // Everything made is still there after a collection ...
    heap.collect();
    for (js::Cell const* cell : made)
        CHECK(cell->marked());
    CHECK_EQ(heap.cell_count(), before + made.size());
    CHECK(text->view() == u"held");
    CHECK(is_data_value(proto2->get_own_property(k_value), js::Value::string(text)));
    CHECK(array->element(0) == js::Value::string(element));
    CHECK(array->element(5000) == js::Value::object(sparse));
    CHECK(outer->find(name_a)->value == js::Value::string(bound_text));
    CHECK(inner->find(name_b)->value == js::Value::object(call_function));
    CHECK(string_object->string() == wrapped);
    CHECK(regexp->source() == regexp_source);

    // ... a dropped link frees its subgraph ...
    CHECK(top->remove_own(k_arr));
    heap.collect();
    CHECK_EQ(heap.cell_count(), before + made.size() - 3);
    CHECK(top->marked() && proto->marked() && bound_with_argument->marked() && argument_text->marked());

    // ... and letting go of the root frees the rest.
    root.set(js::Value::undefined());
    heap.collect();
    CHECK_EQ(heap.cell_count(), before);
}

// ===========================================================================
// The review cases: each pins one edge of §10.1.6.3, §10.4.2, §10.4.3 or
// §9.1 that the suites above only brush past.

bool same_descriptor(std::optional<js::PropertyDescriptor> const& a, std::optional<js::PropertyDescriptor> const& b)
{
    if (!a || !b)
        return !a && !b;
    return a->value == b->value && a->get == b->get && a->set == b->set && a->writable == b->writable
        && a->enumerable == b->enumerable && a->configurable == b->configurable;
}

// A generic descriptor over a non-configurable property (§10.1.6.3 steps
// 4–5): an empty one and one that repeats the current attributes pass and
// change nothing, one that flips an attribute is refused — the same for a
// data property and for an accessor.
void test_review_generic_on_locked(Fixture& fx)
{
    js::Object* g = fx.native();
    js::Object* o = fx.object();
    o->put(fx.key("d"), num(1), js::Writable | js::Enumerable);
    o->put_accessor(fx.key("a"), g, nullptr, js::Enumerable);
    for (js::PropertyKey const& key : { fx.key("d"), fx.key("a") }) {
        std::optional<js::PropertyDescriptor> const before = o->get_own_property(key);
        CHECK(o->define_own_property(key, js::PropertyDescriptor {}));
        CHECK(o->define_own_property(key, flags_desc({}, true, {})));
        CHECK(o->define_own_property(key, flags_desc({}, {}, false)));
        CHECK(o->define_own_property(key, flags_desc({}, true, false)));
        CHECK(!o->define_own_property(key, flags_desc({}, false, {})));
        CHECK(!o->define_own_property(key, flags_desc({}, {}, true)));
        CHECK(!o->define_own_property(key, flags_desc({}, false, true)));
        CHECK(!o->define_own_property(key, flags_desc({}, true, true)));
        CHECK(same_descriptor(o->get_own_property(key), before));
    }
    CHECK_EQ(o->own_property_count(), 2u);
    // A non-configurable, non-enumerable one: enumerable false repeats, true flips.
    o->put(fx.key("h"), num(1), js::Writable);
    CHECK(o->define_own_property(fx.key("h"), flags_desc({}, false, false)));
    CHECK(!o->define_own_property(fx.key("h"), flags_desc({}, true, {})));
}

// SameValue (§7.2.10) decides whether a value may be "changed" on a
// non-writable, non-configurable data property (step 5.e.ii): the same
// value, by that relation, is always accepted and nothing else is.
void test_review_same_value_on_frozen(Fixture& fx)
{
    js::Object* o = fx.object();
    js::Object* inner = fx.object();
    js::PropertyKey const sym = fx.sym("s");
    js::JsString* a1 = fx.string_cell("abc");
    js::JsString* a2 = fx.string_cell("abc");
    CHECK(a1 != a2);
    struct Case {
        js::Value value;
        js::Value same;
        js::Value other;
    };
    Case const cases[] = {
        { num(7), num(7.0), num(8) },
        { js::Value::string(a1), js::Value::string(a2), fx.str("abd") },
        { js::Value::object(inner), js::Value::object(inner), js::Value::object(o) },
        { js::Value::undefined(), js::Value::undefined(), js::Value::null() },
        { js::Value::null(), js::Value::null(), js::Value::undefined() },
        { js::Value::boolean(true), js::Value::boolean(true), js::Value::boolean(false) },
        { js::Value::symbol(sym.as_symbol()), js::Value::symbol(sym.as_symbol()), js::Value::symbol(fx.sym("t").as_symbol()) },
        { num(std::numeric_limits<double>::infinity()), num(std::numeric_limits<double>::infinity()), num(-std::numeric_limits<double>::infinity()) },
        { num(-0.0), num(-0.0), num(0.0) },
        { num(0.0), num(0.0), num(-0.0) },
    };
    std::uint32_t i = 0;
    for (Case const& c : cases) {
        js::PropertyKey const key = idx(i++);
        o->put(key, c.value, js::frozen_attributes);
        CHECK(o->define_own_property(key, value_desc(c.same)));
        CHECK(!o->define_own_property(key, value_desc(c.other)));
        // With the attributes restated alongside the value.
        js::PropertyDescriptor full = js::PropertyDescriptor::data(c.same, js::frozen_attributes);
        CHECK(o->define_own_property(key, full));
        full.value = c.other;
        CHECK(!o->define_own_property(key, full));
        // A different type is never the same value.
        CHECK(!o->define_own_property(key, value_desc(c.value.is_string() ? num(1) : fx.str("x"))));
        // What is stored is the same value by the language's reckoning:
        // step 6.c stores Desc.[[Value]], and a string with the same
        // contents in another cell is that same value.
        std::optional<js::PropertyDescriptor> const d = o->get_own_property(key);
        CHECK(d && d->value && d->value->type() == c.value.type());
        if (c.value.is_string())
            CHECK(d && d->value && d->value->as_string()->view() == c.value.as_string()->view());
        else if (!c.value.is_number())
            CHECK(d && d->value && *d->value == c.value);
        CHECK(has_attributes(d, false, false, false));
    }
    CHECK(o->get_own_property(idx(1))->value->as_string()->view() == u"abc");
    // NaN, whatever its bits.
    double const nan = std::numeric_limits<double>::quiet_NaN();
    o->put(fx.key("nan"), num(nan), js::frozen_attributes);
    CHECK(o->define_own_property(fx.key("nan"), value_desc(num(-nan))));
    CHECK(o->define_own_property(fx.key("nan"), value_desc(num(0.0 / 0.0))));
    CHECK(!o->define_own_property(fx.key("nan"), value_desc(num(0))));
    // Writable, non-configurable: any value goes (5.e applies only when
    // writable is false), and it lands.
    o->put(fx.key("w"), num(1), js::Writable);
    CHECK(o->define_own_property(fx.key("w"), value_desc(num(2))));
    CHECK(is_data_value(o->get_own_property(fx.key("w")), num(2)));
}

// A change of kind on a non-configurable property is refused whatever
// else the descriptor says (§10.1.6.3 step 5.c), and IsAccessorDescriptor
// counts a present-but-undefined getter as a field (§6.2.6.1).
void test_review_kind_change_on_locked(Fixture& fx)
{
    js::Object* g = fx.native();
    js::Object* o = fx.object();
    o->put(fx.key("d"), num(1), js::Writable | js::Enumerable);
    CHECK(!o->define_own_property(fx.key("d"), getter_desc(nullptr)));
    CHECK(!o->define_own_property(fx.key("d"), setter_desc(nullptr)));
    CHECK(!o->define_own_property(fx.key("d"), getter_desc(g)));
    js::PropertyDescriptor accessor_same_flags = getter_desc(g);
    accessor_same_flags.enumerable = true;
    accessor_same_flags.configurable = false;
    CHECK(!o->define_own_property(fx.key("d"), accessor_same_flags));
    CHECK(is_data_value(o->get_own_property(fx.key("d")), num(1)));
    CHECK(has_attributes(o->get_own_property(fx.key("d")), true, true, false));

    o->put_accessor(fx.key("a"), g, g, js::Enumerable);
    CHECK(!o->define_own_property(fx.key("a"), value_desc(js::Value::undefined())));
    CHECK(!o->define_own_property(fx.key("a"), flags_desc(false, {}, {})));
    js::PropertyDescriptor data_same_flags = js::PropertyDescriptor::data(num(1), js::Enumerable);
    CHECK(!o->define_own_property(fx.key("a"), data_same_flags));
    std::optional<js::PropertyDescriptor> const d = o->get_own_property(fx.key("a"));
    CHECK(d && d->is_accessor() && *d->get == g && *d->set == g);
    CHECK(d && !d->value && !d->writable);
    // The same getter and setter restated, with the flags, is accepted.
    js::PropertyDescriptor restated = js::PropertyDescriptor::accessor(g, g, js::Enumerable);
    CHECK(o->define_own_property(fx.key("a"), restated));
    // A present-but-undefined setter where the setter is defined is a change.
    CHECK(!o->define_own_property(fx.key("a"), setter_desc(nullptr)));
    // ... and where it is undefined, it is not.
    o->put_accessor(fx.key("b"), g, nullptr, js::Enumerable);
    CHECK(o->define_own_property(fx.key("b"), setter_desc(nullptr)));
    CHECK(!o->define_own_property(fx.key("b"), setter_desc(g)));
    // Configurable: the change of kind goes through, keeping enumerable.
    o->put_accessor(fx.key("c"), g, g, js::Enumerable | js::Configurable);
    CHECK(o->define_own_property(fx.key("c"), value_desc(num(3))));
    CHECK(is_data_value(o->get_own_property(fx.key("c")), num(3)));
    CHECK(has_attributes(o->get_own_property(fx.key("c")), false, true, true));
}

// A descriptor with only one attribute field on a missing key creates a
// data property whose other attributes take their defaults (§10.1.6.3
// step 2.d: undefined and false) — on an ordinary object, on an array
// past its end, and on a string wrapper past its units.
void test_review_partial_descriptor_creates(Fixture& fx)
{
    js::Object* o = fx.object();
    CHECK(o->define_own_property(fx.key("e"), flags_desc({}, true, {})));
    std::optional<js::PropertyDescriptor> d = o->get_own_property(fx.key("e"));
    CHECK(is_data_value(d, js::Value::undefined()));
    CHECK(d && !d->get && !d->set && !d->is_accessor());
    CHECK(has_attributes(d, false, true, false));
    CHECK(o->define_own_property(fx.key("c"), flags_desc({}, {}, true)));
    CHECK(has_attributes(o->get_own_property(fx.key("c")), false, false, true));
    CHECK(o->define_own_property(fx.key("w"), flags_desc(true, {}, {})));
    CHECK(has_attributes(o->get_own_property(fx.key("w")), true, false, false));
    CHECK(is_data_value(o->get_own_property(fx.key("w")), js::Value::undefined()));
    // The created property obeys the rules from then on: non-configurable,
    // non-writable, so its value is frozen at undefined.
    CHECK(!o->define_own_property(fx.key("e"), value_desc(num(1))));
    CHECK(o->define_own_property(fx.key("e"), value_desc(js::Value::undefined())));
    CHECK(!o->delete_property(fx.key("e")));
    CHECK(o->delete_property(fx.key("c")));

    js::Value const two[2] = { num(0), num(1) };
    js::ArrayObject* a = fx.array(two);
    CHECK(a->define_own_property(idx(4), flags_desc({}, true, {})));
    CHECK_EQ(a->length(), 5u);
    d = a->get_own_property(idx(4));
    CHECK(is_data_value(d, js::Value::undefined()));
    CHECK(has_attributes(d, false, true, false));
    // It is present, with the value undefined — not a hole.
    CHECK(a->has_element(4));
    CHECK(a->element(4).is_undefined());
    CHECK(!a->element(4).is_empty());
    CHECK_EQ(a->dense_size(), 2u);
    CHECK_EQ(a->own_property_count(), 1u);
    CHECK(!a->is_simple_dense());
    std::vector<js::PropertyKey> keys = a->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(keys.size() == 4 && keys[2] == idx(4) && keys[3] == fx.length_key());
    // In a hole inside the dense range the same happens: the hole is a
    // missing property, so the defaults apply and the element leaves
    // the dense storage.
    CHECK(a->delete_property(idx(0)));
    CHECK(a->define_own_property(idx(0), flags_desc({}, {}, true)));
    CHECK(has_attributes(a->get_own_property(idx(0)), false, false, true));
    CHECK(a->element(0).is_undefined());
    CHECK_EQ(a->dense_size(), 0u);
    CHECK_EQ(a->own_property_count(), 3u);

    js::StringObject* s = fx.make<js::StringObject>(nullptr, fx.string_cell("ab"));
    CHECK(s->define_own_property(idx(2), flags_desc({}, true, {})));
    CHECK(has_attributes(s->get_own_property(idx(2)), false, true, false));
    CHECK(is_data_value(s->get_own_property(idx(2)), js::Value::undefined()));
    CHECK(s->has_property(idx(2)));
    CHECK_EQ(s->own_property_count(), 1u);
}

// 2^32 − 2 is the largest array index; "4294967295" is a plain string
// name (§6.1.7). [[OwnPropertyKeys]] lists the one among the indices,
// ascending, and the other among the strings, in creation order.
void test_review_largest_index_versus_name(Fixture& fx)
{
    js::PropertyKey const last_index = fx.heap.key(4294967294.0);
    js::PropertyKey const not_index = fx.heap.key(static_cast<std::uint32_t>(4294967295u));
    CHECK(last_index.is_index() && last_index.as_index() == 4294967294u);
    CHECK(not_index.is_atom());
    CHECK(not_index == fx.key("4294967295"));
    CHECK(fx.heap.key(4294967295.0) == not_index);
    CHECK(fx.heap.key(std::u16string_view(u"4294967294")) == last_index);
    CHECK(fx.heap.key(4294967294u) == last_index);

    js::Object* o = fx.object();
    o->put(not_index, num(1));
    o->put(fx.key("b"), num(2));
    o->put(last_index, num(3));
    o->put(idx(0), num(4));
    std::vector<js::PropertyKey> keys = o->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(keys.size() == 4 && keys[0] == idx(0) && keys[1] == last_index && keys[2] == not_index && keys[3] == fx.key("b"));

    // On an array the name goes after `length`, the index before it, and
    // the length reaches its maximum.
    js::ArrayObject* a = fx.array();
    a->put(not_index, num(1));
    a->set_element(4294967294u, num(3));
    CHECK_EQ(a->length(), 4294967295u);
    CHECK_EQ(a->dense_size(), 0u);
    CHECK(a->has_element(4294967294u));
    CHECK(a->element(4294967294u) == num(3));
    CHECK(!a->has_property(not_index) || a->find_own(not_index) != nullptr);
    keys = a->own_keys();
    CHECK_EQ(keys.size(), 3u);
    CHECK(keys.size() == 3 && keys[0] == last_index && keys[1] == fx.length_key() && keys[2] == not_index);
    // The name is not an element.
    CHECK(is_data_value(a->get_own_property(not_index), num(1)));
    CHECK_EQ(a->own_property_count(), 2u);

    // On a string wrapper the units come first, then the ordinary indices
    // ascending, then length, then the names in creation order.
    js::StringObject* s = fx.make<js::StringObject>(nullptr, fx.string_cell("ab"));
    s->put(not_index, num(1));
    s->put(last_index, num(2));
    s->put(fx.key("x"), num(3));
    s->put(idx(3), num(4));
    js::PropertyKey const tag = fx.sym("tag");
    s->put(tag, num(5));
    keys = s->own_keys();
    CHECK_EQ(keys.size(), 8u);
    CHECK(keys.size() == 8 && keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(3) && keys[3] == last_index);
    CHECK(keys.size() == 8 && keys[4] == fx.length_key() && keys[5] == not_index && keys[6] == fx.key("x") && keys[7] == tag);
}

// A full array: push at length 2^32 − 1 has nowhere to go (§10.4.2.4
// would throw a RangeError on the length that follows) and must change
// nothing — no element, no "4294967295" name, no length.
void test_review_push_at_full_length(Fixture& fx)
{
    js::ArrayObject* a = fx.array();
    CHECK(a->define_own_property(idx(4294967294u), value_desc(num(1))));
    CHECK_EQ(a->length(), 4294967295u);
    CHECK_EQ(a->own_property_count(), 1u);
    a->push(num(2));
    a->push(num(3));
    CHECK_EQ(a->length(), 4294967295u);
    CHECK_EQ(a->own_property_count(), 1u);
    CHECK_EQ(a->dense_size(), 0u);
    CHECK(!a->get_own_property(fx.key("4294967295")));
    CHECK(is_data_value(a->get_own_property(idx(4294967294u)), num(1)));
    CHECK(a->own_keys().size() == 2);
    // The same through set_length to the top, then push.
    js::ArrayObject* b = fx.array();
    CHECK(b->set_length(4294967295u));
    b->push(num(1));
    CHECK_EQ(b->length(), 4294967295u);
    CHECK_EQ(b->own_property_count(), 0u);
    CHECK_EQ(b->dense_size(), 0u);
    CHECK(!b->has_element(0));
    CHECK(!b->has_element(4294967294u));
    // One below the top is the last index that can be pushed into.
    js::ArrayObject* c = fx.array();
    CHECK(c->set_length(4294967294u));
    c->push(num(9));
    CHECK_EQ(c->length(), 4294967295u);
    CHECK(c->element(4294967294u) == num(9));
    CHECK_EQ(c->dense_size(), 0u);
    c->push(num(10));
    CHECK_EQ(c->own_property_count(), 1u);
}

// ArraySetLength with the length read-only (§10.4.2.4): the same length
// is always accepted (step 11 → step 5.e.ii SameValue), whatever the
// route; any other is refused before an element is looked at.
void test_review_set_length_same_when_read_only(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::ArrayObject* a = fx.array(three);
    CHECK(a->define_own_property(idx(1), flags_desc({}, {}, false))); // a blocker, for later
    CHECK(a->define_own_property(fx.length_key(), flags_desc(false, {}, {})));
    CHECK(a->set_length(3));
    CHECK(!a->set_length(4));
    CHECK(!a->set_length(2));
    CHECK(!a->set_length(0));
    CHECK_EQ(a->length(), 3u);
    CHECK(a->define_own_property(fx.length_key(), value_desc(num(3))));
    CHECK(a->define_own_property(fx.length_key(), js::PropertyDescriptor::data(num(3), js::frozen_attributes)));
    js::PropertyDescriptor locked_same = value_desc(num(3));
    locked_same.writable = false;
    CHECK(a->define_own_property(fx.length_key(), locked_same));
    js::PropertyDescriptor unlock_same = value_desc(num(3));
    unlock_same.writable = true;
    CHECK(!a->define_own_property(fx.length_key(), unlock_same));
    CHECK(!a->define_own_property(fx.length_key(), value_desc(num(4))));
    CHECK(!a->define_own_property(fx.length_key(), value_desc(num(2))));
    CHECK(!a->define_own_property(fx.length_key(), value_desc(num(3.5))));
    CHECK_EQ(a->length(), 3u);
    CHECK(has_attributes(a->get_own_property(fx.length_key()), false, false, false));
    // Nothing was truncated on the way: 1 and 2 are still there.
    CHECK(a->has_element(1) && a->has_element(2) && a->has_element(0));
    // The set path with the length read-only refuses before touching the
    // value (OrdinarySet step 2.a), so no coercion runs.
    std::optional<bool> const ok = a->set(no_interpreter(), fx.length_key(), fx.str("junk"), js::Value::object(a));
    CHECK(ok && !*ok);
    CHECK_EQ(a->length(), 3u);
    // Also refused: a new element at or past the length, allowed: one below.
    CHECK(!a->define_own_property(idx(3), value_desc(num(4))));
    CHECK(!a->define_own_property(idx(100), js::PropertyDescriptor {}));
    CHECK(a->delete_property(idx(2)));
    CHECK(a->define_own_property(idx(2), value_desc(num(5))));
    CHECK_EQ(a->length(), 3u);
}

// A truncation blocked by non-configurable elements (§10.4.2.4 step 16):
// the elements go in descending order, the first that will not delete
// stops it, so the HIGHEST blocker decides — everything above it is gone,
// everything below it (blocker or not) stays, the length lands just
// above it, and false comes back.
void test_review_truncation_stops_at_highest_blocker(Fixture& fx)
{
    js::Value ten[10];
    for (std::uint32_t i = 0; i < 10; ++i)
        ten[i] = num(i);
    js::ArrayObject* a = fx.array(ten);
    CHECK(a->define_own_property(idx(3), flags_desc({}, {}, false)));
    js::Object* getter = fx.native();
    js::PropertyDescriptor locked_accessor = getter_desc(getter);
    locked_accessor.configurable = false;
    CHECK(a->define_own_property(idx(7), locked_accessor));
    CHECK_EQ(a->dense_size(), 3u);
    CHECK_EQ(a->own_property_count(), 7u);
    CHECK(!a->set_length(1));
    CHECK_EQ(a->length(), 8u);
    for (std::uint32_t i = 0; i < 8; ++i)
        CHECK(a->has_element(i));
    CHECK(!a->has_element(8) && !a->has_element(9));
    CHECK_EQ(a->own_property_count(), 5u);
    CHECK_EQ(a->dense_size(), 3u);
    CHECK(a->element(1) == num(1) && a->element(6) == num(6));
    CHECK(a->get_own_property(idx(7))->is_accessor());
    CHECK(has_attributes(a->get_own_property(fx.length_key()), true, false, false));
    // Below the highest blocker but above the lower one: still the highest.
    CHECK(!a->set_length(5));
    CHECK_EQ(a->length(), 8u);
    CHECK(a->has_element(5) && a->has_element(6));
    // Exactly at the highest blocker's index + 1 is the same length: fine.
    CHECK(a->set_length(8));
    CHECK(a->set_length(12));
    CHECK_EQ(a->length(), 12u);
    CHECK(!a->set_length(0));
    CHECK_EQ(a->length(), 8u);
    // Through define with writable: false the lock lands on the blocked
    // length (step 16.d), and from then on only 8 is accepted.
    js::PropertyDescriptor shrink_locked = value_desc(num(2));
    shrink_locked.writable = false;
    CHECK(!a->define_own_property(fx.length_key(), shrink_locked));
    CHECK_EQ(a->length(), 8u);
    CHECK(has_attributes(a->get_own_property(fx.length_key()), false, false, false));
    CHECK(a->set_length(8));
    CHECK(!a->set_length(9));
    CHECK(!a->set_length(7));
    CHECK(a->has_element(3) && a->has_element(7));
    // The keys of what remains, in order, with the length after them.
    std::vector<js::PropertyKey> const keys = a->own_keys();
    CHECK_EQ(keys.size(), 9u);
    for (std::uint32_t i = 0; i < 8; ++i)
        CHECK(keys.size() == 9 && keys[i] == idx(i));
    CHECK(keys.size() == 9 && keys[8] == fx.length_key());
}

// An element made read-only leaves the dense storage; set_element, the
// unchecked write, then finds it there: the value lands, and the
// attributes stay what they were — a fast path must not widen what the
// checked path could do (Object.isFrozen must not flip).
void test_review_set_element_over_read_only(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::ArrayObject* a = fx.array(three);
    CHECK(a->define_own_property(idx(1), flags_desc(false, {}, {})));
    CHECK_EQ(a->dense_size(), 1u);
    CHECK_EQ(a->own_property_count(), 2u);
    a->set_element(1, num(9));
    CHECK(a->element(1) == num(9));
    CHECK(is_data_value(a->get_own_property(idx(1)), num(9)));
    CHECK(has_attributes(a->get_own_property(idx(1)), false, true, true));
    CHECK_EQ(a->dense_size(), 1u);
    CHECK_EQ(a->own_property_count(), 2u);
    CHECK_EQ(a->length(), 3u);
    // The one moved along with it kept its default attributes and is
    // updated where it is too.
    a->set_element(2, num(8));
    CHECK(a->element(2) == num(8));
    CHECK(has_attributes(a->get_own_property(idx(2)), true, true, true));
    CHECK_EQ(a->own_property_count(), 2u);
    CHECK(!a->is_simple_dense());
    // The checked path still refuses the read-only one, and the value is
    // the one set_element stored.
    std::optional<bool> const ok = a->set(no_interpreter(), idx(1), num(5), js::Value::object(a));
    CHECK(ok && !*ok);
    CHECK(a->element(1) == num(9));
    // A frozen element (non-writable, non-configurable): same, and the
    // element stays undeletable afterwards.
    CHECK(a->define_own_property(idx(2), flags_desc(false, false, false)));
    a->set_element(2, num(7));
    CHECK(a->element(2) == num(7));
    CHECK(has_attributes(a->get_own_property(idx(2)), false, false, false));
    CHECK(!a->delete_property(idx(2)));
    // The keys are still each listed once.
    std::vector<js::PropertyKey> const keys = a->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(keys.size() == 4 && keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(2) && keys[3] == fx.length_key());
}

// A sparse (ordinary) index sitting just past the dense storage, and
// dense growth that reaches it: the dense storage may grow up to it but
// never over it, and every index is answered from exactly one place.
void test_review_dense_growth_meets_sparse(Fixture& fx)
{
    js::Value const two[2] = { num(0), num(1) };
    js::ArrayObject* a = fx.array(two);
    CHECK(a->define_own_property(idx(5), js::PropertyDescriptor::data(num(5), js::Enumerable | js::Configurable)));
    CHECK_EQ(a->own_property_count(), 1u);
    CHECK_EQ(a->dense_size(), 2u);
    CHECK_EQ(a->length(), 6u);
    a->set_element(2, num(2));
    a->set_element(3, num(3));
    a->set_element(4, num(4));
    CHECK_EQ(a->dense_size(), 5u);
    CHECK_EQ(a->own_property_count(), 1u);
    CHECK(has_attributes(a->get_own_property(idx(5)), false, true, true));
    CHECK(a->element(5) == num(5));
    // Past it: ordinary, since growing over it would shadow it.
    a->set_element(6, num(6));
    CHECK_EQ(a->dense_size(), 5u);
    CHECK_EQ(a->own_property_count(), 2u);
    CHECK_EQ(a->length(), 7u);
    CHECK(a->define_own_property(idx(7), js::PropertyDescriptor::data(num(7), js::default_attributes)));
    CHECK_EQ(a->dense_size(), 5u);
    CHECK_EQ(a->own_property_count(), 3u);
    std::vector<js::PropertyKey> keys = a->own_keys();
    CHECK_EQ(keys.size(), 9u);
    for (std::uint32_t i = 0; i < 8; ++i)
        CHECK(keys.size() == 9 && keys[i] == idx(i));
    for (std::uint32_t i = 0; i < 8; ++i) {
        CHECK(a->has_element(i));
        CHECK(a->element(i) == num(i));
        CHECK(is_data_value(a->get_own_property(idx(i)), num(i)));
    }
    CHECK(!a->is_simple_dense());
    // Once the sparse one is gone the dense storage may cross that index.
    CHECK(a->delete_property(idx(5)));
    CHECK_EQ(a->own_property_count(), 2u);
    a->set_element(5, num(50));
    CHECK_EQ(a->dense_size(), 6u);
    CHECK(a->element(5) == num(50));
    CHECK_EQ(a->own_property_count(), 2u);
    // ... but not over the next ordinary one (6).
    a->set_element(8, num(8));
    CHECK_EQ(a->dense_size(), 6u);
    CHECK_EQ(a->own_property_count(), 3u);
    CHECK(a->element(6) == num(6) && a->element(8) == num(8));
    // Truncation clears both stores and reports success: all configurable.
    CHECK(a->set_length(0));
    CHECK_EQ(a->dense_size(), 0u);
    CHECK_EQ(a->own_property_count(), 0u);
    CHECK(a->own_keys().size() == 1);
    CHECK(a->is_simple_dense());

    // The same crossing through define with default attributes.
    js::ArrayObject* b = fx.array(two);
    CHECK(b->define_own_property(idx(4), js::PropertyDescriptor::data(num(4), js::Writable | js::Enumerable)));
    CHECK(b->define_own_property(idx(3), js::PropertyDescriptor::data(num(3), js::default_attributes)));
    CHECK_EQ(b->dense_size(), 4u);
    CHECK(b->define_own_property(idx(4), js::PropertyDescriptor::data(num(40), js::Writable | js::Enumerable)));
    CHECK(b->element(4) == num(40));
    CHECK_EQ(b->dense_size(), 4u);
    CHECK_EQ(b->own_property_count(), 1u);
    CHECK(b->define_own_property(idx(6), js::PropertyDescriptor::data(num(6), js::default_attributes)));
    CHECK_EQ(b->dense_size(), 4u);
    CHECK_EQ(b->own_property_count(), 2u);
    // The non-configurable 4 blocks a truncation below it.
    CHECK(!b->set_length(2));
    CHECK_EQ(b->length(), 5u);
    CHECK_EQ(b->dense_size(), 4u);
    CHECK(!b->has_element(6));
}

// A dense element re-defined with a descriptor it already satisfies stays
// dense: no migration for a no-op.
void test_review_dense_stays_dense(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::ArrayObject* a = fx.array(three);
    CHECK(a->define_own_property(idx(1), value_desc(num(2))));
    CHECK(a->define_own_property(idx(1), js::PropertyDescriptor::data(num(2), js::default_attributes)));
    CHECK(a->define_own_property(idx(1), flags_desc(true, true, true)));
    CHECK(a->define_own_property(idx(1), js::PropertyDescriptor {}));
    CHECK(a->define_own_property(idx(1), value_desc(num(20))));
    CHECK_EQ(a->dense_size(), 3u);
    CHECK_EQ(a->own_property_count(), 0u);
    CHECK(a->element(1) == num(20));
    CHECK(a->is_simple_dense());
}

// String exotic objects (§10.4.3.2): a code unit accepts exactly the
// descriptors it already satisfies, and `length` likewise — SameValue
// on the value, so −0 is not the length of "".
void test_review_string_unit_define(Fixture& fx)
{
    js::Object* g = fx.native();
    js::StringObject* s = fx.make<js::StringObject>(nullptr, fx.string_cell("ab"));
    CHECK(s->define_own_property(idx(0), value_desc(fx.str("a"))));
    CHECK(s->define_own_property(idx(0), js::PropertyDescriptor::data(fx.str("a"), js::Enumerable)));
    CHECK(s->define_own_property(idx(0), flags_desc(false, true, false)));
    CHECK(s->define_own_property(idx(1), value_desc(fx.str("b"))));
    CHECK(!s->define_own_property(idx(0), value_desc(fx.str("b"))));
    CHECK(!s->define_own_property(idx(0), value_desc(fx.str("ab"))));
    CHECK(!s->define_own_property(idx(0), value_desc(num(0))));
    CHECK(!s->define_own_property(idx(0), js::PropertyDescriptor::data(fx.str("a"), js::Writable | js::Enumerable)));
    CHECK(!s->define_own_property(idx(0), js::PropertyDescriptor::data(fx.str("a"), js::Enumerable | js::Configurable)));
    CHECK(!s->define_own_property(idx(0), js::PropertyDescriptor::data(fx.str("a"), 0)));
    CHECK(!s->define_own_property(idx(0), getter_desc(g)));
    CHECK(!s->define_own_property(idx(0), getter_desc(nullptr)));
    CHECK(!s->define_own_property(idx(0), setter_desc(nullptr)));
    CHECK(!s->define_own_property(idx(1), value_desc(fx.str("a"))));
    CHECK_EQ(s->own_property_count(), 0u);
    CHECK(string_equals(*s->get_own_property(idx(0))->value, u"a"));
    // Non-extensible changes nothing for the units: they exist already.
    s->prevent_extensions();
    CHECK(s->define_own_property(idx(1), value_desc(fx.str("b"))));
    CHECK(!s->define_own_property(idx(2), value_desc(fx.str("c"))));
    CHECK(s->define_own_property(fx.length_key(), value_desc(num(2))));
    CHECK(s->define_own_property(fx.length_key(), js::PropertyDescriptor::data(num(2), js::frozen_attributes)));
    CHECK(!s->define_own_property(fx.length_key(), value_desc(num(2.5))));
    CHECK(!s->define_own_property(fx.length_key(), value_desc(fx.str("2"))));
    CHECK(!s->define_own_property(fx.length_key(), flags_desc({}, true, {})));
    CHECK(!s->define_own_property(fx.length_key(), getter_desc(g)));
    js::StringObject* e = fx.make<js::StringObject>(nullptr, fx.string_cell(""));
    CHECK(e->define_own_property(fx.length_key(), value_desc(num(0.0))));
    CHECK(!e->define_own_property(fx.length_key(), value_desc(num(-0.0))));
    // Index 0 of "" is not a unit: an ordinary property, defined as usual.
    CHECK(e->define_own_property(idx(0), value_desc(fx.str(""))));
    CHECK_EQ(e->own_property_count(), 1u);
    CHECK(has_attributes(e->get_own_property(idx(0)), false, false, false));
}

// Environment (§9.1.1.1): declaring a name twice yields the binding that
// is already there, untouched; a deletable binding removed and declared
// again is a fresh one with the new flags.
void test_review_environment_duplicate_declare(Fixture& fx)
{
    js::JsString* x = fx.heap.atom("x"sv);
    js::JsString* y = fx.heap.atom("y"sv);
    js::JsString* z = fx.heap.atom("z"sv);
    js::Environment* env = fx.make<js::Environment>(nullptr);
    js::Environment::Binding& first = env->declare(x, num(1));
    js::Environment::Binding& second = env->declare(x, num(2), false, false, true);
    CHECK(&first == &second);
    CHECK_EQ(env->bindings().size(), 1u);
    CHECK(second.value == num(1) && second.mutable_ && second.initialized && !second.deletable);
    CHECK(!env->remove(x));
    CHECK_EQ(env->bindings().size(), 1u);
    // A binding in its temporal dead zone declared again stays there.
    js::Environment::Binding& tdz = env->declare(y, js::Value::empty(), true, false);
    js::Environment::Binding& tdz_again = env->declare(y, num(5));
    CHECK(&tdz == &tdz_again || env->find(y)->value.is_empty());
    CHECK(env->find(y) != nullptr && !env->find(y)->initialized && env->find(y)->value.is_empty());
    // Deletable, removed, declared anew: the new flags take.
    env->declare(z, num(3), true, true, true);
    CHECK(env->remove(z));
    CHECK(env->find(z) == nullptr);
    js::Environment::Binding& fresh = env->declare(z, num(4), false, true, false);
    CHECK(fresh.value == num(4) && !fresh.mutable_ && fresh.initialized && !fresh.deletable);
    CHECK(!env->remove(z));
    CHECK_EQ(env->bindings().size(), 3u);
    // The order of declaration is kept.
    CHECK(env->bindings()[0].name == x && env->bindings()[1].name == y && env->bindings()[2].name == z);
}

// A prototype chain ten thousand links long: [[Get]], [[Set]] and
// [[HasProperty]] walk it without recursing (the stack would not take
// it), through an ordinary head and through an array at the root.
void test_review_deep_chain_get_set(Fixture& fx)
{
    js::Interpreter& in = no_interpreter();
    js::Value const one[1] = { num(42) };
    js::ArrayObject* root = fx.array(one);
    root->put(fx.key("deep"), num(1));
    root->put(fx.key("ro"), num(2), js::Enumerable | js::Configurable);
    root->put_accessor(fx.key("half"), nullptr, fx.native(), js::Configurable);
    js::Object* link = root;
    for (int i = 0; i < 10000; ++i)
        link = fx.object(link);
    js::Object* leaf = link;
    js::Value const leaf_value = js::Value::object(leaf);

    std::optional<js::Value> got = leaf->get(in, fx.key("deep"), leaf_value);
    CHECK(got && *got == num(1));
    got = leaf->get(in, idx(0), leaf_value);
    CHECK(got && *got == num(42));
    got = leaf->get(in, fx.length_key(), leaf_value);
    CHECK(got && *got == num(1));
    got = leaf->get(in, fx.key("half"), leaf_value);
    CHECK(got && got->is_undefined());
    got = leaf->get(in, fx.key("missing"), leaf_value);
    CHECK(got && got->is_undefined());
    CHECK(leaf->has_property(fx.key("deep")));
    CHECK(leaf->has_property(idx(0)));
    CHECK(leaf->has_property(fx.length_key()));
    CHECK(!leaf->has_property(idx(1)));
    CHECK(!leaf->has_property(fx.key("missing")));

    // A set walks down to the root's descriptor and back to the receiver.
    std::optional<bool> ok = leaf->set(in, fx.key("deep"), num(3), leaf_value);
    CHECK(ok && *ok);
    CHECK(is_data_value(leaf->get_own_property(fx.key("deep")), num(3)));
    CHECK(is_data_value(root->get_own_property(fx.key("deep")), num(1)));
    ok = leaf->set(in, fx.key("ro"), num(9), leaf_value);
    CHECK(ok && !*ok);
    CHECK(leaf->find_own(fx.key("ro")) == nullptr);
    ok = leaf->set(in, idx(0), num(7), leaf_value);
    CHECK(ok && *ok);
    CHECK(is_data_value(leaf->get_own_property(idx(0)), num(7)));
    CHECK(root->element(0) == num(42));
    ok = leaf->set(in, fx.key("missing"), num(8), leaf_value);
    CHECK(ok && *ok);
    // A setter-less accessor found at the far end refuses without a call.
    root->put_accessor(fx.key("getonly"), fx.native(), nullptr, js::Configurable);
    ok = leaf->set(in, fx.key("getonly"), num(1), leaf_value);
    CHECK(ok && !*ok);
    // The cycle check walks the whole chain too.
    CHECK(!root->set_prototype(leaf));
    CHECK(root->prototype() == nullptr);
    // And the fast-path precondition reads the whole chain.
    js::ArrayObject* array_leaf = fx.array(one, leaf);
    CHECK(!array_leaf->is_simple_dense()); // the root array has an element
    CHECK(root->set_length(0));
    CHECK(array_leaf->is_simple_dense() || leaf->find_own(idx(0)) != nullptr);
    CHECK(leaf->remove_own(idx(0)));
    CHECK(array_leaf->is_simple_dense());
}

// The collector reaches a getter or setter that nothing but an accessor
// property refers to — on an ordinary object, an array element that left
// the dense storage, a string wrapper, and a bound function's target.
void test_review_trace_through_accessor(Fixture& fx)
{
    js::Heap& heap = fx.heap;
    // Atoms are permanent and interning one allocates (a collection in
    // stress mode): every key is made before anything unrooted exists,
    // and before the count.
    js::PropertyKey const k = fx.key("acc");
    js::PropertyKey const k_arr = fx.key("arr");
    js::PropertyKey const k_text = fx.key("text");
    js::PropertyKey const k_tmp = fx.key("tmp");
    js::PropertyKey const k_bound = fx.key("bound");
    heap.collect();
    std::size_t const before = heap.cell_count();

    js::Persistent root(heap);
    js::Object* owner = heap.allocate<js::Object>(nullptr);
    root.set(js::Value::object(owner));
    // The getter is held by nothing but the accessor; stress mode collects
    // at every allocation, so each cell is attached before the next one.
    js::Object* getter = heap.allocate<js::Object>(nullptr);
    owner->put_accessor(k, getter, nullptr);
    heap.collect();
    CHECK(getter->marked());
    CHECK_EQ(heap.cell_count(), before + 2);
    // The same through define_own_property, with a setter.
    js::Object* setter = heap.allocate<js::Object>(nullptr);
    CHECK(owner->define_own_property(k, setter_desc(setter)));
    heap.collect();
    CHECK(getter->marked() && setter->marked());
    CHECK_EQ(heap.cell_count(), before + 3);
    // Replacing the getter lets the old one go.
    js::Object* getter2 = heap.allocate<js::Object>(nullptr);
    CHECK(owner->define_own_property(k, getter_desc(getter2)));
    heap.collect();
    CHECK_EQ(heap.cell_count(), before + 3);
    CHECK(getter2->marked() && setter->marked());

    // An array element made an accessor: it leaves the dense storage and
    // is traced as an ordinary property.
    js::ArrayObject* array = heap.allocate<js::ArrayObject>(nullptr);
    owner->put(k_arr, js::Value::object(array));
    array->push(num(1));
    array->push(num(2));
    js::Object* element_getter = heap.allocate<js::Object>(nullptr);
    CHECK(array->define_own_property(idx(0), getter_desc(element_getter)));
    CHECK_EQ(array->dense_size(), 0u);
    heap.collect();
    CHECK(element_getter->marked());
    CHECK_EQ(heap.cell_count(), before + 5);

    // A string wrapper with an accessor beside its units.
    js::JsString* text = heap.string("ab"sv);
    owner->put(k_text, js::Value::string(text));
    js::StringObject* wrapper = heap.allocate<js::StringObject>(nullptr, text);
    owner->put(k_text, js::Value::object(wrapper));
    js::Object* unit_setter = heap.allocate<js::Object>(nullptr);
    CHECK(wrapper->define_own_property(idx(5), setter_desc(unit_setter)));
    heap.collect();
    CHECK(unit_setter->marked() && text->marked());
    CHECK_EQ(heap.cell_count(), before + 8);

    // A bound function reached through an accessor, holding its target.
    js::NativeFunction* target = heap.allocate<js::NativeFunction>(nullptr, js::NativeFunction::Callback {});
    owner->put(k_tmp, js::Value::object(target));
    js::BoundFunction* bound = heap.allocate<js::BoundFunction>(nullptr, target, js::Value::undefined(), std::vector<js::Value> {});
    owner->put_accessor(k_bound, bound, nullptr);
    CHECK(owner->remove_own(k_tmp));
    heap.collect();
    CHECK(bound->marked() && target->marked());
    CHECK_EQ(heap.cell_count(), before + 10);

    // Cutting the accessor frees what only it reached.
    CHECK(owner->delete_property(k));
    heap.collect();
    CHECK_EQ(heap.cell_count(), before + 8);
    CHECK(owner->remove_own(k_bound));
    heap.collect();
    CHECK_EQ(heap.cell_count(), before + 6);
    root.set(js::Value::undefined());
    heap.collect();
    CHECK_EQ(heap.cell_count(), before);
}

// put() and remove_own() are not virtual, yet an array keeps its indices
// in the dense vector: an index put on an array must land where the
// array's [[DefineOwnProperty]] would put it (§10.4.2.1), never as an
// ordinary property shadowed by — or shadowing — a dense element.
void test_review_put_on_array_index(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::ArrayObject* a = fx.array(three);
    a->put(idx(1), num(9));
    CHECK(a->element(1) == num(9));
    CHECK_EQ(a->own_property_count(), 0u);
    CHECK_EQ(a->dense_size(), 3u);
    CHECK(is_data_value(a->get_own_property(idx(1)), num(9)));
    std::vector<js::PropertyKey> keys = a->own_keys();
    CHECK_EQ(keys.size(), 4u);
    CHECK(a->is_simple_dense());
    // Past the end: dense growth and the length follow.
    a->put(idx(5), num(5));
    CHECK_EQ(a->dense_size(), 6u);
    CHECK_EQ(a->length(), 6u);
    CHECK(a->element(5) == num(5));
    CHECK_EQ(a->own_property_count(), 0u);
    // With attributes the vector has no room for, the element and those
    // above it move to ordinary storage; the attributes land.
    a->put(idx(2), num(22), js::Enumerable);
    CHECK_EQ(a->dense_size(), 2u);
    CHECK(has_attributes(a->get_own_property(idx(2)), false, true, false));
    CHECK(a->element(2) == num(22));
    CHECK(a->element(5) == num(5));
    CHECK(has_attributes(a->get_own_property(idx(5)), true, true, true));
    CHECK(!a->has_element(3) && !a->has_element(4));
    CHECK_EQ(a->own_property_count(), 2u);
    keys = a->own_keys();
    CHECK_EQ(keys.size(), 5u);
    CHECK(keys.size() == 5 && keys[0] == idx(0) && keys[1] == idx(1) && keys[2] == idx(2) && keys[3] == idx(5));
    // Frozen and far: ordinary, length extended.
    a->put(idx(4000), num(1), js::frozen_attributes);
    CHECK_EQ(a->length(), 4001u);
    CHECK(has_attributes(a->get_own_property(idx(4000)), false, false, false));
    CHECK(!a->set_length(0));
    CHECK_EQ(a->length(), 4001u);
    // put over an existing ordinary index replaces value and attributes
    // outright, as put does.
    a->put(idx(2), num(23), js::default_attributes);
    CHECK(has_attributes(a->get_own_property(idx(2)), true, true, true));
    CHECK(a->element(2) == num(23));
    // put over an accessor element makes it data again.
    CHECK(a->define_own_property(idx(1), getter_desc(fx.native())));
    a->put(idx(1), num(11));
    CHECK(is_data_value(a->get_own_property(idx(1)), num(11)));
    CHECK(a->element(1) == num(11));
    // remove_own on a dense element makes a hole and says so; on a hole
    // it says nothing was there; the length is untouched.
    CHECK(a->remove_own(idx(0)));
    CHECK(!a->has_element(0));
    CHECK(!a->remove_own(idx(0)));
    CHECK_EQ(a->length(), 4001u);
    CHECK(a->remove_own(idx(4000)));
    CHECK(!a->has_element(4000));
    CHECK_EQ(a->length(), 4001u);
    // The named keys still go through the ordinary path.
    a->put(fx.key("name"), num(1));
    CHECK(is_data_value(a->get_own_property(fx.key("name")), num(1)));
    CHECK(a->remove_own(fx.key("name")));
}

// Loose ends: the delete rules on the exotic keys, and set_prototype's
// SameValue short-circuit on a non-extensible object.
void test_review_loose_ends(Fixture& fx)
{
    js::Value const three[3] = { num(1), num(2), num(3) };
    js::ArrayObject* a = fx.array(three);
    CHECK(!a->delete_property(fx.length_key()));
    CHECK(a->delete_property(idx(7)));
    CHECK_EQ(a->length(), 3u);
    CHECK(a->define_own_property(idx(2), flags_desc({}, {}, false)));
    CHECK(!a->delete_property(idx(2)));
    CHECK(a->delete_property(idx(1)));
    CHECK(a->has_element(2) && !a->has_element(1));
    // A non-configurable element cannot be made configurable again.
    CHECK(!a->define_own_property(idx(2), flags_desc({}, {}, true)));

    js::Object* o = fx.object();
    o->prevent_extensions();
    CHECK(o->set_prototype(nullptr));
    js::Object* p = fx.object(o);
    p->prevent_extensions();
    CHECK(p->set_prototype(o));
    CHECK(!p->set_prototype(nullptr));
    CHECK(p->prototype() == o);
}

}

int main()
{
    Fixture fx;
    test_length_key_through_realm(fx);
    test_descriptors_round_trip(fx);
    test_define_own_property(fx);
    test_extensibility_and_prototype(fx);
    test_own_keys_order(fx);
    test_index_map(fx);
    test_chain_get_set(fx);
    test_delete(fx);
    test_array_basics(fx);
    test_array_sparse(fx);
    test_array_length(fx);
    test_array_simple_dense(fx);
    test_string_object(fx);
    test_environment(fx);
    test_functions(fx);
    test_trace_under_stress(fx);
    test_review_generic_on_locked(fx);
    test_review_same_value_on_frozen(fx);
    test_review_kind_change_on_locked(fx);
    test_review_partial_descriptor_creates(fx);
    test_review_largest_index_versus_name(fx);
    test_review_push_at_full_length(fx);
    test_review_set_length_same_when_read_only(fx);
    test_review_truncation_stops_at_highest_blocker(fx);
    test_review_set_element_over_read_only(fx);
    test_review_dense_growth_meets_sparse(fx);
    test_review_dense_stays_dense(fx);
    test_review_string_unit_define(fx);
    test_review_environment_duplicate_declare(fx);
    test_review_deep_chain_get_set(fx);
    test_review_trace_through_accessor(fx);
    test_review_put_on_array_index(fx);
    test_review_loose_ends(fx);
    return sashfold::test::report("js_objects");
}
