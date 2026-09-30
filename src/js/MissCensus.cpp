#include "js/MissCensus.h"

#include "js/Heap.h"
#include "js/Object.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

// The counts, for every thread that runs script, written as the process
// ends.
struct Census {
    std::string path;
    std::mutex mutex;
    std::unordered_map<std::string, std::uint64_t> counts; // "what\tname\thow"

    ~Census()
    {
        if (path.empty())
            return;
        std::vector<std::pair<std::string, std::uint64_t>> rows(counts.begin(), counts.end());
        std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        std::ofstream out(path, std::ios::binary);
        out << "hits\ton\tname\thow\n";
        for (auto const& [row, hits] : rows)
            out << hits << '\t' << row << '\n';
    }
};

Census* census()
{
    static Census* const taken = []() -> Census* {
        char const* const path = std::getenv("SASHFOLD_MISS_CENSUS");
        if (path == nullptr || *path == '\0')
            return nullptr;
        static Census the_census;
        the_census.path = path;
        return &the_census;
    }();
    return taken;
}

// What an object says it is: the global object, or the @@toStringTag an
// interface's prototype carries, read as data so that no script runs. An
// interface object itself — a constructor whose `prototype` says what it
// is — is that name with "(static)". Nothing for a plain object.
std::optional<std::string> name_of(Object const& object)
{
    Heap* const heap = object.heap();
    if (heap == nullptr)
        return std::nullopt;
    PropertyKey const tag = PropertyKey::symbol(heap->atoms().symbol_to_string_tag);
    auto const tag_of = [&tag](Object const& from) -> std::optional<std::string> {
        int depth = 0;
        for (Object const* link = &from; link != nullptr && depth < 64; link = link->prototype(), ++depth) {
            if (link->is_proxy())
                return std::nullopt;
            std::optional<PropertyDescriptor> const desc = link->get_own_property(tag);
            if (desc && desc->value && desc->value->is_string())
                return desc->value->as_string()->to_utf8();
        }
        return std::nullopt;
    };
    if (std::optional<std::string> named = tag_of(object))
        return named;
    if (object.class_id() == Object::Class::Global)
        return std::string("Window");
    if (object.is_callable()) {
        std::optional<PropertyDescriptor> const prototype = object.get_own_property(PropertyKey::atom(heap->atoms().prototype));
        if (prototype && prototype->value && prototype->value->is_object()) {
            if (std::optional<std::string> named = tag_of(*prototype->value->as_object()))
                return *named + " (static)";
        }
    }
    return std::nullopt;
}

}

bool miss_census_on() { return census() != nullptr; }

void note_miss(Object const& object, PropertyKey const& key, bool asked_in)
{
    Census* const taken = census();
    if (taken == nullptr || key.is_index())
        return;
    std::string name;
    if (key.is_symbol()) {
        Symbol const* const symbol = key.as_symbol();
        if (symbol == nullptr || symbol->is_private())
            return;
        name = "@@" + (symbol->description() != nullptr ? symbol->description()->to_utf8() : std::string("symbol"));
    } else {
        if (key.as_atom() == nullptr)
            return;
        name = key.as_atom()->to_utf8();
    }
    std::optional<std::string> const on = name_of(object);
    if (!on)
        return;
    std::string row = *on + '\t' + name + '\t' + (asked_in ? "in" : "get");
    std::lock_guard const lock(taken->mutex);
    ++taken->counts[std::move(row)];
}

}
