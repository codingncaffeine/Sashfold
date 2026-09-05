#include "js/Heap.h"

#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

// ---------------------------------------------------------------- Tracer

void Tracer::visit(Cell* cell)
{
    // Marking here and tracing later (from the collector's drain loop)
    // keeps the walk iterative: a scope chain a script nests ten thousand
    // deep costs worklist entries, never stack frames.
    if (cell == nullptr || cell->marked())
        return;
    cell->set_marked(true);
    m_worklist.push_back(cell);
}

// -------------------------------------------------------------- JsString

std::string JsString::to_utf8() const
{
    return utf8_from_utf16(m_data);
}

std::optional<std::uint32_t> JsString::as_array_index() const
{
    if (!m_index_known) {
        auto const index = array_index_of(m_data);
        m_is_index = index.has_value();
        m_index = index.value_or(0);
        m_index_known = true;
    }
    if (m_is_index)
        return m_index;
    return std::nullopt;
}

std::size_t JsString::hash() const
{
    if (!m_hash_known) {
        // FNV-1a over the code units: it needs no table, and a property
        // map's buckets do not care about cryptographic strength.
        std::uint64_t h = 14695981039346656037ull;
        for (char16_t const unit : m_data) {
            h ^= static_cast<std::uint64_t>(unit);
            h *= 1099511628211ull;
        }
        m_hash = static_cast<std::size_t>(h);
        m_hash_known = true;
    }
    return m_hash;
}

// ------------------------------------------------------------------ Heap

Heap::Heap()
{
    intern_well_known();
}

Heap::~Heap()
{
    // A Persistent that outlives its heap must not touch the dead
    // registry from its destructor; it is told the heap is gone.
    for (Persistent* persistent : m_persistents)
        persistent->m_heap = nullptr;
    m_persistents.clear();
    // The atom table views the cells' data; drop it before the cells go.
    m_atoms.clear();
    m_cells.clear();
}

void Heap::adopt(std::unique_ptr<Cell> cell)
{
    cell->m_heap = this;
    m_bytes += cell->size_in_bytes();
    m_cells.push_back(std::move(cell));
}

void Heap::maybe_collect()
{
    if (m_no_collect > 0 || m_collecting)
        return;
    if (m_stress || m_bytes > m_threshold)
        collect();
}

void Heap::collect()
{
    // An explicit collect() honours the same guards as the automatic one:
    // a native that took a NoCollect scope is holding unrooted cells, and
    // a cell's trace() must never re-enter the collector.
    if (m_no_collect > 0 || m_collecting)
        return;
    m_collecting = true;

    for (auto const& cell : m_cells)
        cell->set_marked(false);

    // Roots: the atom table (atoms are permanent, so marking them is how
    // they survive rather than a special case in the sweep), the
    // well-known symbols, every RootProvider and every Persistent.
    Tracer tracer;
    for (auto const& entry : m_atoms)
        tracer.visit(entry.second);
    tracer.visit(m_well_known.symbol_to_primitive);
    tracer.visit(m_well_known.symbol_to_string_tag);
    tracer.visit(m_well_known.symbol_iterator);
    tracer.visit(m_well_known.symbol_has_instance);
    tracer.visit(m_well_known.symbol_is_concat_spreadable);
    tracer.visit(m_well_known.symbol_match);
    tracer.visit(m_well_known.symbol_match_all);
    tracer.visit(m_well_known.symbol_replace);
    tracer.visit(m_well_known.symbol_search);
    tracer.visit(m_well_known.symbol_split);
    tracer.visit(m_well_known.symbol_species);
    tracer.visit(m_well_known.symbol_unscopables);
    tracer.visit(m_well_known.symbol_async_iterator);
    for (RootProvider* provider : m_root_providers)
        provider->trace_roots(tracer);
    for (Persistent const* persistent : m_persistents)
        tracer.visit(persistent->m_value);

    // Drain: visit() marked and queued; trace() shows the tracer what
    // each cell keeps alive. Nothing recurses.
    while (!tracer.m_worklist.empty()) {
        Cell* cell = tracer.m_worklist.back();
        tracer.m_worklist.pop_back();
        cell->trace(tracer);
    }

    std::size_t live = 0;
    std::erase_if(m_cells, [&live](std::unique_ptr<Cell> const& cell) {
        if (cell->marked()) {
            live += cell->size_in_bytes();
            return false;
        }
        return true;
    });

    // The next collection is due once the garbage matches the live set,
    // never sooner than the floor: a small heap should not collect on
    // every few kilobytes.
    m_bytes = live;
    m_threshold = std::max<std::size_t>(8u * 1024u * 1024u, live * 2u);
    ++m_collections;
    m_collecting = false;
}

JsString* Heap::string(std::u16string data)
{
    return allocate<JsString>(std::move(data));
}

JsString* Heap::string(std::string_view utf8)
{
    return string(utf16_from_utf8(utf8));
}

JsString* Heap::string(char16_t code_unit)
{
    return string(std::u16string(1, code_unit));
}

JsString* Heap::atom(std::u16string_view contents)
{
    auto const found = m_atoms.find(contents);
    if (found != m_atoms.end())
        return found->second;
    // The copy is taken before allocate() can collect: `contents` may view
    // an unrooted string that the collection would sweep.
    std::u16string copy(contents);
    JsString* cell = allocate<JsString>(std::move(copy));
    cell->m_atom = true;
    // The key views the atom's own data; the cell never moves and the
    // data is immutable, so the view stays valid for the heap's life.
    m_atoms.emplace(cell->view(), cell);
    return cell;
}

JsString* Heap::atom(std::string_view utf8)
{
    std::u16string const contents = utf16_from_utf8(utf8);
    return atom(std::u16string_view(contents));
}

JsString* Heap::atom(JsString* existing)
{
    if (existing->is_atom())
        return existing;
    return atom(existing->view());
}

PropertyKey Heap::key(std::u16string_view name)
{
    if (auto const index = array_index_of(name))
        return PropertyKey::index(*index);
    return PropertyKey::atom(atom(name));
}

PropertyKey Heap::key(std::string_view utf8)
{
    std::u16string const name = utf16_from_utf8(utf8);
    return key(std::u16string_view(name));
}

PropertyKey Heap::key(JsString* name)
{
    if (auto const index = name->as_array_index())
        return PropertyKey::index(*index);
    return PropertyKey::atom(atom(name));
}

PropertyKey Heap::key(double number)
{
    // An integral Number in 0 … 2^32 − 2 is an array index (§6.1.7);
    // ToString(-0) is "0", so −0 lands on index 0 rather than an atom.
    // NaN fails both comparisons and Infinity the upper one, so each
    // falls through to its Number::toString spelling.
    if (number >= 0 && number <= 4294967294.0 && std::floor(number) == number)
        return PropertyKey::index(static_cast<std::uint32_t>(number));
    std::u16string const name = number_to_string(number);
    return PropertyKey::atom(atom(std::u16string_view(name)));
}

PropertyKey Heap::key(std::uint32_t index)
{
    // 2^32 − 1 is not an array index (§6.1.7); it is the string name
    // "4294967295", which is how `length`-sized loops never alias it.
    if (index == 0xFFFFFFFFu)
        return PropertyKey::atom(atom(std::u16string_view(u"4294967295")));
    return PropertyKey::index(index);
}

JsString* Heap::key_to_string(PropertyKey const& property_key)
{
    switch (property_key.kind()) {
    case PropertyKey::Kind::Atom:
        return property_key.as_atom();
    case PropertyKey::Kind::Index:
        return atom(std::string_view(std::to_string(property_key.as_index())));
    case PropertyKey::Kind::Symbol:
        return nullptr;
    }
    return nullptr;
}

Symbol* Heap::symbol(JsString* description)
{
    // The description is the caller's and may be unrooted; a collection
    // in allocate() would sweep it out from under the new symbol.
    NoCollect const guard(*this);
    return allocate<Symbol>(description);
}

void Heap::add_root_provider(RootProvider* provider)
{
    if (std::find(m_root_providers.begin(), m_root_providers.end(), provider) == m_root_providers.end())
        m_root_providers.push_back(provider);
}

void Heap::remove_root_provider(RootProvider* provider)
{
    std::erase(m_root_providers, provider);
}

void Heap::intern_well_known()
{
    WellKnownAtoms& a = m_well_known;
    a.empty = atom(std::string_view(""));
    a.length = atom(std::string_view("length"));
    a.prototype = atom(std::string_view("prototype"));
    a.constructor = atom(std::string_view("constructor"));
    a.name = atom(std::string_view("name"));
    a.message = atom(std::string_view("message"));
    a.cause = atom(std::string_view("cause"));
    a.stack = atom(std::string_view("stack"));
    a.value = atom(std::string_view("value"));
    a.get = atom(std::string_view("get"));
    a.set = atom(std::string_view("set"));
    a.writable = atom(std::string_view("writable"));
    a.enumerable = atom(std::string_view("enumerable"));
    a.configurable = atom(std::string_view("configurable"));
    a.to_string = atom(std::string_view("toString"));
    a.value_of = atom(std::string_view("valueOf"));
    a.to_json = atom(std::string_view("toJSON"));
    a.arguments = atom(std::string_view("arguments"));
    a.callee = atom(std::string_view("callee"));
    a.caller = atom(std::string_view("caller"));
    a.index = atom(std::string_view("index"));
    a.input = atom(std::string_view("input"));
    a.groups = atom(std::string_view("groups"));
    a.last_index = atom(std::string_view("lastIndex"));
    a.source = atom(std::string_view("source"));
    a.flags = atom(std::string_view("flags"));
    a.global = atom(std::string_view("global"));
    a.undefined = atom(std::string_view("undefined"));
    a.null = atom(std::string_view("null"));
    a.true_ = atom(std::string_view("true"));
    a.false_ = atom(std::string_view("false"));
    a.object = atom(std::string_view("object"));
    a.function = atom(std::string_view("function"));
    a.number = atom(std::string_view("number"));
    a.string = atom(std::string_view("string"));
    a.boolean = atom(std::string_view("boolean"));
    a.symbol = atom(std::string_view("symbol"));
    a.nan = atom(std::string_view("NaN"));
    a.infinity = atom(std::string_view("Infinity"));
    a.negative_infinity = atom(std::string_view("-Infinity"));
    a.zero = atom(std::string_view("0"));
    a.eval = atom(std::string_view("eval"));
    a.default_ = atom(std::string_view("default"));
    a.proto = atom(std::string_view("__proto__"));
    a.anonymous = atom(std::string_view("anonymous"));
    a.object_object = atom(std::string_view("[object Object]"));
    a.error = atom(std::string_view("Error"));
    a.done = atom(std::string_view("done"));
    a.next = atom(std::string_view("next"));
    a.description = atom(std::string_view("description"));
    a.raw = atom(std::string_view("raw"));
    a.then = atom(std::string_view("then"));
    a.comma = atom(std::string_view(","));

    // The well-known symbols (§6.1.5.1) are rooted by collect() itself;
    // their descriptions are atoms and so permanent on their own.
    a.symbol_to_primitive = symbol(atom(std::string_view("Symbol.toPrimitive")));
    a.symbol_to_string_tag = symbol(atom(std::string_view("Symbol.toStringTag")));
    a.symbol_iterator = symbol(atom(std::string_view("Symbol.iterator")));
    a.symbol_has_instance = symbol(atom(std::string_view("Symbol.hasInstance")));
    a.symbol_is_concat_spreadable = symbol(atom(std::string_view("Symbol.isConcatSpreadable")));
    a.symbol_match = symbol(atom(std::string_view("Symbol.match")));
    a.symbol_match_all = symbol(atom(std::string_view("Symbol.matchAll")));
    a.symbol_replace = symbol(atom(std::string_view("Symbol.replace")));
    a.symbol_search = symbol(atom(std::string_view("Symbol.search")));
    a.symbol_split = symbol(atom(std::string_view("Symbol.split")));
    a.symbol_species = symbol(atom(std::string_view("Symbol.species")));
    a.symbol_unscopables = symbol(atom(std::string_view("Symbol.unscopables")));
    a.symbol_async_iterator = symbol(atom(std::string_view("Symbol.asyncIterator")));
}

// ------------------------------------------------------------ Persistent

Persistent::Persistent(Heap& heap, Value value)
    : m_heap(&heap)
    , m_value(value)
{
    m_heap->m_persistents.insert(this);
}

Persistent::~Persistent()
{
    if (m_heap != nullptr)
        m_heap->m_persistents.erase(this);
}

Persistent::Persistent(Persistent&& other) noexcept
    : m_heap(other.m_heap)
    , m_value(other.m_value)
{
    // The registry keys on the handle's address, so a move re-registers
    // the new address and forgets the old one; the moved-from handle is
    // left holding nothing and no longer roots anything.
    if (m_heap != nullptr) {
        m_heap->m_persistents.erase(&other);
        m_heap->m_persistents.insert(this);
    }
    other.m_heap = nullptr;
    other.m_value = Value();
}

Persistent& Persistent::operator=(Persistent&& other) noexcept
{
    if (this == &other)
        return *this;
    if (m_heap != nullptr)
        m_heap->m_persistents.erase(this);
    m_heap = other.m_heap;
    m_value = other.m_value;
    if (m_heap != nullptr) {
        m_heap->m_persistents.erase(&other);
        m_heap->m_persistents.insert(this);
    }
    other.m_heap = nullptr;
    other.m_value = Value();
    return *this;
}

}
