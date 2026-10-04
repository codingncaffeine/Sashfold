// Import maps (HTML §8.1.5.2 and §8.1.5.3): what a `<script type=importmap>`
// says, parsed and normalized, merged into the document's one map, and the
// "resolve a module specifier" steps that read it — a bare specifier such
// as `lit` named to a URL, a prefix such as `app/` moved to another place,
// and both narrowed to the scripts under a scope.

#include "bindings/Internal.h"

#include <algorithm>

namespace sashfold::bindings {

namespace {

// "Resolve a URL-like module specifier": a relative reference that begins
// with "/", "./" or "../" is taken against the base, anything else is a
// URL by itself or nothing.
std::optional<net::Url> url_like(std::string_view specifier, net::Url const& base)
{
    if (specifier.starts_with("/") || specifier.starts_with("./") || specifier.starts_with("../"))
        return net::parse_url(specifier, &base);
    return net::parse_url(specifier, nullptr);
}

std::string key_text(js::PropertyKey const& key)
{
    if (key.is_index())
        return std::to_string(key.as_index());
    return key.as_atom()->to_utf8();
}

// The keys of a map are tried longest first: descending code unit order.
template<typename Entry>
void sort_descending(std::vector<Entry>& entries)
{
    std::stable_sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) {
        return js::utf16_from_utf8(a.first) > js::utf16_from_utf8(b.first);
    });
}

// "Sort and normalize a module specifier map".
std::optional<ImportMap::SpecifierMap> normalize_specifier_map(js::Interpreter& interp, js::Object& original, net::Url const& base,
    std::vector<std::string>& warnings)
{
    ImportMap::SpecifierMap normalized;
    std::optional<std::vector<js::PropertyKey>> const keys = interp.own_keys(original);
    if (!keys)
        return std::nullopt;
    for (js::PropertyKey const& key : *keys) {
        if (key.is_symbol())
            continue;
        std::string const specifier_key = key_text(key);
        if (specifier_key.empty()) {
            warnings.push_back("an import map's specifier key is the empty string");
            continue;
        }
        std::optional<net::Url> const key_url = url_like(specifier_key, base);
        std::string const normalized_key = key_url ? key_url->serialize() : specifier_key;
        std::optional<js::Value> const value = interp.get(original, key);
        if (!value)
            return std::nullopt;
        std::optional<net::Url> address;
        if (!value->is_string()) {
            warnings.push_back("the import map's address for '" + specifier_key + "' is not a string");
        } else {
            std::string const written = value->as_string()->to_utf8();
            address = url_like(written, base);
            if (!address) {
                warnings.push_back("the import map's address '" + written + "' for '" + specifier_key + "' is not a URL");
            } else if (specifier_key.ends_with("/") && !address->serialize().ends_with("/")) {
                warnings.push_back("the import map's address '" + written + "' for the prefix '" + specifier_key + "' does not end with '/'");
                address.reset();
            }
        }
        auto const existing = std::find_if(normalized.begin(), normalized.end(), [&](auto const& entry) { return entry.first == normalized_key; });
        if (existing != normalized.end())
            existing->second = std::move(address);
        else
            normalized.push_back({ normalized_key, std::move(address) });
    }
    sort_descending(normalized);
    return normalized;
}

// "Resolve an imports match": the map's answer for a specifier, nothing
// when the map does not name it, and an error when it names it to nothing.
std::optional<net::Url> imports_match(std::string const& normalized_specifier, std::optional<net::Url> const& as_url,
    ImportMap::SpecifierMap const& map, std::string& error)
{
    for (auto const& [specifier_key, address] : map) {
        if (specifier_key == normalized_specifier) {
            if (!address)
                error = "the import map names '" + normalized_specifier + "' to nothing";
            return address;
        }
        if (specifier_key.ends_with("/") && normalized_specifier.starts_with(specifier_key) && (!as_url || as_url->is_special())) {
            if (!address) {
                error = "the import map names the prefix '" + specifier_key + "' to nothing";
                return std::nullopt;
            }
            std::string const after_prefix = normalized_specifier.substr(specifier_key.size());
            std::optional<net::Url> const url = net::parse_url(after_prefix, &*address);
            if (!url) {
                error = "'" + after_prefix + "' is no URL under the import map's '" + address->serialize() + "'";
                return std::nullopt;
            }
            // What follows the prefix may not climb out of where the
            // prefix was sent.
            if (!url->serialize().starts_with(address->serialize())) {
                error = "'" + normalized_specifier + "' leaves the import map's '" + address->serialize() + "'";
                return std::nullopt;
            }
            return url;
        }
    }
    return std::nullopt;
}

}

std::optional<ImportMap> parse_import_map(Realm::Internals& in, std::string_view text, net::Url const& base, std::string& error,
    std::vector<std::string>& warnings)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    js::Value const json = js::Value::object(interp.intrinsics().json);
    std::optional<js::Value> const parse = interp.get(json, "parse");
    if (!parse)
        return std::nullopt;
    interp.root(*parse);
    js::Value const arguments[1] = { in.string(text) };
    interp.root(arguments[0]);
    std::optional<js::Value> const parsed = interp.call(*parse, json, arguments);
    if (!parsed) {
        error = interp.describe(interp.take_exception());
        return std::nullopt;
    }
    interp.root(*parsed);
    auto const is_map = [](js::Value const& value) { return value.is_object() && !value.as_object()->is_array(); };
    if (!is_map(*parsed)) {
        error = "TypeError: an import map is a JSON object";
        return std::nullopt;
    }
    ImportMap map;
    std::optional<js::Value> const imports = interp.get(*parsed, "imports");
    if (!imports)
        return std::nullopt;
    if (!imports->is_undefined()) {
        if (!is_map(*imports)) {
            error = "TypeError: an import map's \"imports\" is a JSON object";
            return std::nullopt;
        }
        std::optional<ImportMap::SpecifierMap> normalized = normalize_specifier_map(interp, *imports->as_object(), base, warnings);
        if (!normalized)
            return std::nullopt;
        map.imports = std::move(*normalized);
    }
    std::optional<js::Value> const scopes = interp.get(*parsed, "scopes");
    if (!scopes)
        return std::nullopt;
    if (!scopes->is_undefined()) {
        if (!is_map(*scopes)) {
            error = "TypeError: an import map's \"scopes\" is a JSON object";
            return std::nullopt;
        }
        std::optional<std::vector<js::PropertyKey>> const keys = interp.own_keys(*scopes->as_object());
        if (!keys)
            return std::nullopt;
        for (js::PropertyKey const& key : *keys) {
            if (key.is_symbol())
                continue;
            std::string const prefix = key_text(key);
            std::optional<js::Value> const scope = interp.get(*scopes, key);
            if (!scope)
                return std::nullopt;
            if (!is_map(*scope)) {
                error = "TypeError: the import map's scope '" + prefix + "' is a JSON object";
                return std::nullopt;
            }
            std::optional<net::Url> const prefix_url = net::parse_url(prefix, &base);
            if (!prefix_url) {
                warnings.push_back("the import map's scope '" + prefix + "' is not a URL");
                continue;
            }
            std::optional<ImportMap::SpecifierMap> normalized = normalize_specifier_map(interp, *scope->as_object(), base, warnings);
            if (!normalized)
                return std::nullopt;
            std::string const normalized_prefix = prefix_url->serialize();
            auto const existing = std::find_if(map.scopes.begin(), map.scopes.end(), [&](auto const& entry) { return entry.first == normalized_prefix; });
            if (existing != map.scopes.end())
                existing->second = std::move(*normalized);
            else
                map.scopes.push_back({ normalized_prefix, std::move(*normalized) });
        }
        sort_descending(map.scopes);
    }
    return map;
}

// "Merge existing and new import maps": what the document's map already
// says stands, and a later map adds only what it did not name.
void merge_import_map(ImportMap& into, ImportMap&& added, std::vector<std::string>& warnings)
{
    auto const merge_map = [&warnings](ImportMap::SpecifierMap& old_map, ImportMap::SpecifierMap& new_map) {
        for (auto& [specifier, address] : new_map) {
            bool const named = std::any_of(old_map.begin(), old_map.end(), [&](auto const& entry) { return entry.first == specifier; });
            if (named) {
                warnings.push_back("an import map's rule for '" + specifier + "' is ignored: an earlier map has one");
                continue;
            }
            old_map.push_back({ specifier, std::move(address) });
        }
        sort_descending(old_map);
    };
    for (auto& [prefix, scope_imports] : added.scopes) {
        auto const existing = std::find_if(into.scopes.begin(), into.scopes.end(), [&](auto const& entry) { return entry.first == prefix; });
        if (existing != into.scopes.end())
            merge_map(existing->second, scope_imports);
        else
            into.scopes.push_back({ prefix, std::move(scope_imports) });
    }
    sort_descending(into.scopes);
    merge_map(into.imports, added.imports);
}

std::optional<net::Url> resolve_module_specifier(ImportMap const& map, std::string_view specifier, net::Url const& base, std::string& error)
{
    std::optional<net::Url> const as_url = url_like(specifier, base);
    std::string const normalized_specifier = as_url ? as_url->serialize() : std::string(specifier);
    std::string const base_text = base.serialize();
    for (auto const& [scope_prefix, scope_imports] : map.scopes) {
        if (scope_prefix == base_text || (scope_prefix.ends_with("/") && base_text.starts_with(scope_prefix))) {
            std::optional<net::Url> match = imports_match(normalized_specifier, as_url, scope_imports, error);
            if (match || !error.empty())
                return match;
        }
    }
    std::optional<net::Url> match = imports_match(normalized_specifier, as_url, map.imports, error);
    if (match || !error.empty())
        return match;
    if (as_url)
        return as_url;
    error = "Failed to resolve module specifier '" + std::string(specifier)
        + "': a relative reference must start with \"/\", \"./\" or \"../\", and no import map names it";
    return std::nullopt;
}

}
