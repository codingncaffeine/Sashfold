#include "bindings/Internal.h"
#include "bindings/Fetching.h"

// fetch() and what goes with it (the Fetch Standard): Headers, Request,
// Response and the Body mixin, FormData, and the fetching itself — through
// the host's loader, on the event loop's next task, with the CORS checks a
// browser makes: a cross-origin response reaches the page only when the
// server allowed it, a preflight goes first when the request is not a
// simple one, and a no-cors request gets an opaque response. The network
// round trip is synchronous inside that task until the process split.
// XMLHttpRequest (Xhr.cpp) fetches through the same core, perform_fetch.

#include "core/Ascii.h"
#include "html/Encoding.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// ---- header names and values (Fetch §2.2)

bool is_token_char(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool is_header_name(std::string_view name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), is_token_char);
}

bool is_http_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// "Normalize" a header value: HTTP whitespace stripped from both ends.
std::string normalize_header_value(std::string_view value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && is_http_whitespace(value[begin]))
        ++begin;
    while (end > begin && is_http_whitespace(value[end - 1]))
        --end;
    return std::string(value.substr(begin, end - begin));
}

bool is_header_value(std::string_view value)
{
    return value.find_first_of(std::string_view("\0\r\n", 3)) == std::string_view::npos;
}

std::string lowercase(std::string_view text)
{
    return ascii_lower(text);
}

bool is_forbidden_request_header(std::string_view lowered)
{
    static constexpr std::string_view names[] = { "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host",
        "keep-alive", "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via" };
    for (std::string_view const name : names) {
        if (lowered == name)
            return true;
    }
    return lowered.starts_with("proxy-") || lowered.starts_with("sec-");
}

bool is_forbidden_response_header(std::string_view lowered)
{
    return lowered == "set-cookie" || lowered == "set-cookie2";
}

} // namespace

// The MIME essence of a Content-Type value: the type/subtype, lowercased.
// Shared with the realm, which holds a module script to a JavaScript type.
std::string mime_essence(std::string_view value)
{
    std::size_t const semicolon = value.find(';');
    return lowercase(normalize_header_value(value.substr(0, semicolon)));
}

namespace {

// A CORS-safelisted request header (Fetch §2.2.2): one of the four names
// with a value of at most 128 bytes and, for Content-Type, one of the
// three form and text types.
bool is_cors_safelisted_request_header(std::string_view lowered, std::string_view value)
{
    if (value.size() > 128)
        return false;
    if (lowered == "accept" || lowered == "accept-language" || lowered == "content-language")
        return true;
    if (lowered == "content-type") {
        std::string const essence = mime_essence(value);
        return essence == "application/x-www-form-urlencoded" || essence == "multipart/form-data" || essence == "text/plain";
    }
    return false;
}

bool is_cors_safelisted_response_header(std::string_view lowered)
{
    static constexpr std::string_view names[] = { "cache-control", "content-language", "content-length", "content-type", "expires",
        "last-modified", "pragma" };
    for (std::string_view const name : names) {
        if (lowered == name)
            return true;
    }
    return false;
}

std::vector<std::string> split_list(std::string_view value)
{
    // A comma-separated header list, each item trimmed and lowercased.
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t const comma = value.find(',', start);
        std::string_view const item = value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        std::string const trimmed = lowercase(normalize_header_value(item));
        if (!trimmed.empty())
            items.push_back(trimmed);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return items;
}

// ---- application/x-www-form-urlencoded

std::string form_urlencode(std::vector<std::pair<std::string, std::string>> const& pairs)
{
    // The URL Standard's serializer: letters, digits and *-._ as they are,
    // a space as +, every other byte as %XX.
    auto const encode = [](std::string_view text, std::string& out) {
        static constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char const c : text) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') {
                out += static_cast<char>(c);
            } else if (c == ' ') {
                out += '+';
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
    };
    std::string out;
    for (auto const& [name, value] : pairs) {
        if (!out.empty())
            out += '&';
        encode(name, out);
        out += '=';
        encode(value, out);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> form_urldecode(std::string_view text)
{
    auto const decode = [](std::string_view piece) {
        std::string out;
        for (std::size_t i = 0; i < piece.size(); ++i) {
            char const c = piece[i];
            if (c == '+') {
                out += ' ';
            } else if (c == '%' && i + 2 < piece.size() && is_ascii_hex_digit(static_cast<unsigned char>(piece[i + 1]))
                && is_ascii_hex_digit(static_cast<unsigned char>(piece[i + 2]))) {
                out += static_cast<char>(hex_digit_value(static_cast<unsigned char>(piece[i + 1])) * 16
                    + hex_digit_value(static_cast<unsigned char>(piece[i + 2])));
                i += 2;
            } else {
                out += c;
            }
        }
        return out;
    };
    std::vector<std::pair<std::string, std::string>> pairs;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t const amp = text.find('&', start);
        std::string_view const piece = text.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        if (!piece.empty()) {
            std::size_t const equals = piece.find('=');
            if (equals == std::string_view::npos)
                pairs.emplace_back(decode(piece), "");
            else
                pairs.emplace_back(decode(piece.substr(0, equals)), decode(piece.substr(equals + 1)));
        }
        if (amp == std::string_view::npos)
            break;
        start = amp + 1;
    }
    return pairs;
}

// ---- JSON through the realm's own JSON object

Native json_parse(js::Interpreter& interp, std::u16string_view text)
{
    js::Value const json = js::Value::object(interp.intrinsics().json);
    std::optional<js::Value> const parse = interp.get(json, "parse");
    if (!parse)
        return std::nullopt;
    js::Interpreter::Roots const roots(interp);
    interp.root(*parse);
    js::Value const arguments[1] = { js::Value::string(interp.string(text)) };
    interp.root(arguments[0]);
    return interp.call(*parse, json, arguments);
}

Native json_stringify(js::Interpreter& interp, js::Value const& value)
{
    js::Value const json = js::Value::object(interp.intrinsics().json);
    std::optional<js::Value> const stringify = interp.get(json, "stringify");
    if (!stringify)
        return std::nullopt;
    js::Interpreter::Roots const roots(interp);
    interp.root(*stringify);
    interp.root(value);
    js::Value const arguments[1] = { value };
    return interp.call(*stringify, json, arguments);
}

std::u16string utf8_to_string(std::span<std::uint8_t const> bytes)
{
    std::string_view const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    std::u16string out;
    for (char32_t const code_point : html::decode(text, html::Encoding::Utf8))
        js::append_code_point(out, code_point);
    return out;
}

// An iterator over these values: the array iterator of a fresh array, which
// is what Headers, FormData and their kin hand out.
Native iterator_over(js::Interpreter& interp, std::span<js::Value const> items)
{
    js::Interpreter::Roots const roots(interp);
    for (js::Value const& item : items)
        interp.root(item);
    js::ArrayObject* array = interp.new_array(items);
    interp.root(js::Value::object(array));
    std::optional<js::Value> const values = interp.get(*array, interp.key("values"));
    if (!values)
        return std::nullopt;
    return interp.call(*values, js::Value::object(array), {});
}

// ---- Headers

class HeadersObject final : public js::Object {
public:
    enum class Guard : std::uint8_t { None, Request, RequestNoCors, Response, Immutable };

    explicit HeadersObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }

    std::vector<std::pair<std::string, std::string>> list; // names lowercased, in order
    Guard guard = Guard::None;

    bool has(std::string_view name) const
    {
        return std::any_of(list.begin(), list.end(), [&](auto const& entry) { return entry.first == name; });
    }
    // "get": every value under the name, joined by ", ".
    std::optional<std::string> combined(std::string_view name) const
    {
        std::optional<std::string> combined;
        for (auto const& [key, value] : list) {
            if (key != name)
                continue;
            if (!combined)
                combined = value;
            else
                *combined += ", " + value;
        }
        return combined;
    }
    void remove(std::string_view name)
    {
        std::erase_if(list, [&](auto const& entry) { return entry.first == name; });
    }
    void append(std::string name, std::string value) { list.emplace_back(std::move(name), std::move(value)); }
    void replace(std::string name, std::string value)
    {
        auto const first = std::find_if(list.begin(), list.end(), [&](auto const& entry) { return entry.first == name; });
        if (first == list.end()) {
            list.emplace_back(std::move(name), std::move(value));
            return;
        }
        first->second = std::move(value);
        list.erase(std::remove_if(first + 1, list.end(), [&](auto const& entry) { return entry.first == name; }), list.end());
    }
    // "Sort and combine": one entry per name in sorted order, the values
    // joined, except Set-Cookie, whose values stay apart.
    std::vector<std::pair<std::string, std::string>> sorted_and_combined() const
    {
        std::vector<std::string> names;
        for (auto const& [key, value] : list) {
            if (std::find(names.begin(), names.end(), key) == names.end())
                names.push_back(key);
        }
        std::sort(names.begin(), names.end());
        std::vector<std::pair<std::string, std::string>> out;
        for (std::string const& name : names) {
            if (name == "set-cookie") {
                for (auto const& [key, value] : list) {
                    if (key == name)
                        out.emplace_back(name, value);
                }
            } else {
                out.emplace_back(name, *combined(name));
            }
        }
        return out;
    }
    std::vector<net::Header> as_list() const
    {
        std::vector<net::Header> out;
        for (auto const& [key, value] : list)
            out.push_back({ key, value });
        return out;
    }
};

std::optional<HeadersObject*> this_headers(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* headers = dynamic_cast<HeadersObject*>(this_value.as_object()))
            return headers;
    }
    return interp.throw_type_error("Illegal invocation");
}

HeadersObject* new_headers(Realm::Internals& in, HeadersObject::Guard guard)
{
    auto* headers = in.interpreter.heap().allocate<HeadersObject>(in.prototype("Headers"));
    headers->guard = guard;
    return headers;
}

// The validation before an append or set (Fetch §5.1 "append"): the name
// and value checked, the guard consulted. False = silently not added.
std::optional<bool> admit_header(js::Interpreter& interp, HeadersObject& headers, std::string& name, std::string& value, std::string_view method)
{
    value = normalize_header_value(value);
    if (!is_header_name(name) || !is_header_value(value))
        return interp.throw_type_error("Failed to execute '" + std::string(method) + "' on 'Headers': Invalid name or value");
    name = lowercase(name);
    switch (headers.guard) {
    case HeadersObject::Guard::Immutable:
        return interp.throw_type_error("Failed to execute '" + std::string(method) + "' on 'Headers': Headers are immutable");
    case HeadersObject::Guard::Request:
        return !is_forbidden_request_header(name);
    case HeadersObject::Guard::RequestNoCors: {
        std::string joined = value;
        if (std::optional<std::string> const existing = headers.combined(name))
            joined = *existing + ", " + value;
        return is_cors_safelisted_request_header(name, joined);
    }
    case HeadersObject::Guard::Response:
        return !is_forbidden_response_header(name);
    case HeadersObject::Guard::None:
        return true;
    }
    return true;
}

std::optional<bool> headers_append(js::Interpreter& interp, HeadersObject& headers, std::string name, std::string value, std::string_view method)
{
    std::optional<bool> const admitted = admit_header(interp, headers, name, value, method);
    if (!admitted)
        return std::nullopt;
    if (*admitted)
        headers.append(std::move(name), std::move(value));
    return true;
}

// "Fill" (Fetch §5.1): from another Headers, a sequence of pairs, or a
// record of its own enumerable string keys.
std::optional<bool> fill_headers(js::Interpreter& interp, HeadersObject& headers, js::Value const& init, std::string_view method)
{
    if (init.is_undefined() || init.is_null())
        return true;
    if (!init.is_object())
        return interp.throw_type_error("Failed to execute '" + std::string(method) + "': The provided value is not of type '(record<ByteString, ByteString> or sequence<sequence<ByteString>>)'");
    Realm::Internals& internals = internals_of(interp);
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&headers));
    interp.root(init);
    if (auto const* other = dynamic_cast<HeadersObject const*>(init.as_object())) {
        for (auto const& [name, value] : other->list) {
            if (!headers_append(interp, headers, name, value, method))
                return std::nullopt;
        }
        return true;
    }
    std::optional<js::Value> const iterator_method = interp.get_method(init, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
    if (!iterator_method)
        return std::nullopt;
    if (!iterator_method->is_undefined()) {
        std::optional<std::vector<js::Value>> const entries = interp.iterable_to_list(init);
        if (!entries)
            return std::nullopt;
        for (js::Value const& entry : *entries)
            interp.root(entry);
        for (js::Value const& entry : *entries) {
            std::optional<std::vector<js::Value>> const pair = interp.iterable_to_list(entry);
            if (!pair)
                return std::nullopt;
            if (pair->size() != 2)
                return interp.throw_type_error("Failed to construct 'Headers': Invalid value");
            std::optional<std::string> const name = internals.to_utf8((*pair)[0]);
            if (!name)
                return std::nullopt;
            std::optional<std::string> const value = internals.to_utf8((*pair)[1]);
            if (!value)
                return std::nullopt;
            if (!headers_append(interp, headers, *name, *value, method))
                return std::nullopt;
        }
        return true;
    }
    js::Object& record = *init.as_object();
    for (js::PropertyKey const& key : record.own_keys()) {
        if (!key.is_string())
            continue;
        std::optional<js::PropertyDescriptor> const descriptor = record.get_own_property(key);
        if (!descriptor || !descriptor->enumerable.value_or(false))
            continue;
        std::optional<js::Value> const value = interp.get(record, key);
        if (!value)
            return std::nullopt;
        std::optional<std::string> const text = internals.to_utf8(*value);
        if (!text)
            return std::nullopt;
        if (!headers_append(interp, headers, js::key_description(key), *text, method))
            return std::nullopt;
    }
    return true;
}

// ---- FormData

class FormDataObject final : public js::Object {
public:
    struct Entry {
        std::string name;
        std::string value; // a string entry
        BlobObject* file = nullptr; // a file entry
        std::string filename;
    };
    explicit FormDataObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<Entry> entries;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        for (Entry const& entry : entries)
            tracer.visit(entry.file);
    }
};

std::optional<FormDataObject*> this_form_data(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* data = dynamic_cast<FormDataObject*>(this_value.as_object()))
            return data;
    }
    return interp.throw_type_error("Illegal invocation");
}

// An entry's value as script sees it: the string, or a File.
js::Value entry_value(Realm::Internals& in, FormDataObject::Entry const& entry)
{
    if (entry.file != nullptr)
        return js::Value::object(entry.file);
    return in.string(entry.value);
}

// "Create an entry" (XHR §5.3): a Blob becomes a File named `filename`,
// "blob" by default.
std::optional<FormDataObject::Entry> make_entry(js::Interpreter& interp, std::string name, js::Value const& value, js::Value const& filename)
{
    Realm::Internals& internals = internals_of(interp);
    FormDataObject::Entry entry;
    entry.name = std::move(name);
    if (value.is_object()) {
        if (auto* blob = dynamic_cast<BlobObject*>(value.as_object())) {
            std::string file_name = blob->is_file ? blob->name : "blob";
            if (!filename.is_undefined()) {
                std::optional<std::string> const given = internals.to_utf8(filename);
                if (!given)
                    return std::nullopt;
                file_name = *given;
            }
            if (!blob->is_file || file_name != blob->name) {
                auto* file = internals.interpreter.heap().allocate<BlobObject>(internals.prototype("File"));
                file->bytes = blob->bytes;
                file->type = blob->type;
                file->is_file = true;
                file->name = file_name;
                file->last_modified = blob->is_file ? blob->last_modified : js::current_time_ms();
                blob = file;
            }
            entry.file = blob;
            entry.filename = file_name;
            return entry;
        }
    }
    std::optional<std::string> const text = internals.to_utf8(value);
    if (!text)
        return std::nullopt;
    entry.value = *text;
    return entry;
}

// The form's entries (HTML §4.10.21.4, the parts a page fills without
// files): named, enabled controls in tree order.
void collect_form_entries(Realm::Internals& in, dom::Element const& form, FormDataObject& data)
{
    // Depth first, in tree order.
    std::function<void(dom::Node&)> walk = [&](dom::Node& node) {
        if (auto* element = dynamic_cast<dom::Element*>(&node)) {
            std::string const tag = lowercase(element->local_name());
            std::string const name = attribute_or_empty(*element, "name");
            bool const disabled = element->has_attribute("disabled");
            if (!name.empty() && !disabled) {
                if (tag == "input") {
                    std::string const type = lowercase(attribute_or_empty(*element, "type"));
                    if (type == "checkbox" || type == "radio") {
                        if (control_checked_of(in, *element)) {
                            std::string value = attribute_or_empty(*element, "value");
                            if (!element->has_attribute("value"))
                                value = "on";
                            data.entries.push_back({ name, value, nullptr, "" });
                        }
                    } else if (type != "submit" && type != "button" && type != "reset" && type != "image" && type != "file") {
                        data.entries.push_back({ name, control_value_of(in, *element), nullptr, "" });
                    }
                } else if (tag == "textarea" || tag == "select") {
                    data.entries.push_back({ name, control_value_of(in, *element), nullptr, "" });
                }
            }
        }
        for (dom::Node* child : node.children())
            walk(*child);
    };
    for (dom::Node* child : form.children())
        walk(*child);
}

std::string random_boundary()
{
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device device;
    std::string boundary = "----SashfoldFormBoundary";
    for (int i = 0; i < 16; ++i)
        boundary += hex[device() & 0xF];
    return boundary;
}

std::string escape_disposition(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        if (c == '"')
            out += "%22";
        else if (c == '\r')
            out += "%0D";
        else if (c == '\n')
            out += "%0A";
        else
            out += c;
    }
    return out;
}

// multipart/form-data (HTML §4.10.21.8).
std::pair<std::vector<std::uint8_t>, std::string> multipart_encode(FormDataObject const& data)
{
    std::string const boundary = random_boundary();
    std::string out;
    for (FormDataObject::Entry const& entry : data.entries) {
        out += "--" + boundary + "\r\n";
        out += "Content-Disposition: form-data; name=\"" + escape_disposition(entry.name) + "\"";
        if (entry.file != nullptr) {
            out += "; filename=\"" + escape_disposition(entry.filename) + "\"\r\n";
            out += "Content-Type: " + (entry.file->type.empty() ? std::string("application/octet-stream") : entry.file->type) + "\r\n\r\n";
            out.append(entry.file->bytes.begin(), entry.file->bytes.end());
        } else {
            out += "\r\n\r\n";
            out += entry.value;
        }
        out += "\r\n";
    }
    out += "--" + boundary + "--\r\n";
    return { std::vector<std::uint8_t>(out.begin(), out.end()), "multipart/form-data; boundary=" + boundary };
}

// The reverse, for Body.formData(): the parts between the boundaries,
// their Content-Disposition read for the name and filename.
bool multipart_decode(Realm::Internals& in, std::span<std::uint8_t const> bytes, std::string_view boundary, FormDataObject& data)
{
    std::string_view const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    std::string const delimiter = "--" + std::string(boundary);
    std::size_t position = text.find(delimiter);
    if (position == std::string_view::npos)
        return false;
    position += delimiter.size();
    while (true) {
        if (text.substr(position, 2) == "--")
            return true;
        if (text.substr(position, 2) == "\r\n")
            position += 2;
        std::size_t const headers_end = text.find("\r\n\r\n", position);
        if (headers_end == std::string_view::npos)
            return false;
        std::string_view const head = text.substr(position, headers_end - position);
        std::size_t const body_start = headers_end + 4;
        std::size_t const next = text.find("\r\n" + delimiter, body_start);
        if (next == std::string_view::npos)
            return false;
        std::string_view const body = text.substr(body_start, next - body_start);
        std::string name;
        std::optional<std::string> filename;
        std::string type;
        std::size_t line_start = 0;
        while (line_start < head.size()) {
            std::size_t const line_end = head.find("\r\n", line_start);
            std::string_view const line = head.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
            std::size_t const colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string const key = lowercase(normalize_header_value(line.substr(0, colon)));
                std::string const value = normalize_header_value(line.substr(colon + 1));
                if (key == "content-disposition") {
                    auto const param = [&](std::string_view parameter) -> std::optional<std::string> {
                        std::size_t const at = value.find(std::string(parameter) + "=\"");
                        if (at == std::string::npos)
                            return std::nullopt;
                        std::size_t const from = at + parameter.size() + 2;
                        std::size_t const to = value.find('"', from);
                        return value.substr(from, to == std::string::npos ? std::string::npos : to - from);
                    };
                    name = param("name").value_or("");
                    filename = param("filename");
                } else if (key == "content-type") {
                    type = value;
                }
            }
            if (line_end == std::string_view::npos)
                break;
            line_start = line_end + 2;
        }
        FormDataObject::Entry entry;
        entry.name = name;
        if (filename) {
            auto* file = in.interpreter.heap().allocate<BlobObject>(in.prototype("File"));
            file->bytes.assign(body.begin(), body.end());
            file->type = type;
            file->is_file = true;
            file->name = *filename;
            file->last_modified = js::current_time_ms();
            entry.file = file;
            entry.filename = *filename;
        } else {
            entry.value = std::string(body);
        }
        data.entries.push_back(std::move(entry));
        position = next + 2 + delimiter.size();
    }
}

// ---- bodies

struct BodyInit {
    std::vector<std::uint8_t> bytes;
    std::string type; // the Content-Type the source implies; empty for none
};

// "Extract a body" (Fetch §5.2): a Blob, a BufferSource, FormData,
// URLSearchParams, or anything else as a string. The outer nullopt is a
// throw; the inner one is no body (null or undefined).
std::optional<std::optional<BodyInit>> extract_body(js::Interpreter& interp, js::Value const& value)
{
    if (value.is_null() || value.is_undefined())
        return std::optional<BodyInit>();
    BodyInit body;
    if (value.is_object()) {
        if (auto const* blob = dynamic_cast<BlobObject const*>(value.as_object())) {
            body.bytes = blob->bytes;
            body.type = blob->type;
            return body;
        }
        if (std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(value)) {
            body.bytes.assign(bytes->begin(), bytes->end());
            return body;
        }
        if (auto const* data = dynamic_cast<FormDataObject const*>(value.as_object())) {
            auto [bytes, type] = multipart_encode(*data);
            body.bytes = std::move(bytes);
            body.type = std::move(type);
            return body;
        }
        if (auto const* params = dynamic_cast<SearchParamsObject const*>(value.as_object())) {
            std::string const encoded = form_urlencode(params->pairs);
            body.bytes.assign(encoded.begin(), encoded.end());
            body.type = "application/x-www-form-urlencoded;charset=UTF-8";
            return body;
        }
    }
    std::optional<js::JsString*> const text = interp.to_string(value);
    if (!text)
        return std::nullopt;
    std::string const encoded = encode_utf8((*text)->view());
    body.bytes.assign(encoded.begin(), encoded.end());
    body.type = "text/plain;charset=UTF-8";
    return body;
}

// ---- Request and Response

class RequestObject final : public js::Object {
public:
    explicit RequestObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    net::Url url;
    std::string method = "GET";
    HeadersObject* headers = nullptr;
    std::optional<std::vector<std::uint8_t>> body;
    bool body_used = false;
    std::string mode = "cors";
    std::string credentials = "same-origin";
    std::string cache = "default";
    std::string redirect = "follow";
    std::string referrer = "about:client";
    std::string referrer_policy;
    bool keepalive = false;
    AbortSignalObject* signal = nullptr;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(headers);
        tracer.visit(signal);
    }
    std::size_t size_in_bytes() const override { return sizeof(*this) + (body ? body->capacity() : 0); }
};

class ResponseObject final : public js::Object {
public:
    explicit ResponseObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::string type = "default";
    net::Url url;
    bool has_url = false;
    bool redirected = false;
    int status = 200;
    std::string status_text;
    HeadersObject* headers = nullptr;
    std::optional<std::vector<std::uint8_t>> body;
    bool body_used = false;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(headers);
    }
    std::size_t size_in_bytes() const override { return sizeof(*this) + (body ? body->capacity() : 0); }
};

std::optional<RequestObject*> this_request(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* request = dynamic_cast<RequestObject*>(this_value.as_object()))
            return request;
    }
    return interp.throw_type_error("Illegal invocation");
}

std::optional<ResponseObject*> this_response(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* response = dynamic_cast<ResponseObject*>(this_value.as_object()))
            return response;
    }
    return interp.throw_type_error("Illegal invocation");
}

// The Body mixin's two views of an object: where the bytes are, whether
// they have been read, and the Content-Type that describes them.
struct BodyRef {
    std::optional<std::vector<std::uint8_t>>* body = nullptr;
    bool* used = nullptr;
    HeadersObject* headers = nullptr;
};

std::optional<BodyRef> body_of(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* request = dynamic_cast<RequestObject*>(this_value.as_object()))
            return BodyRef { &request->body, &request->body_used, request->headers };
        if (auto* response = dynamic_cast<ResponseObject*>(this_value.as_object()))
            return BodyRef { &response->body, &response->body_used, response->headers };
    }
    return interp.throw_type_error("Illegal invocation");
}

enum class Consume { ArrayBuffer, Blob, Bytes, FormData, Json, Text };

// "Consume body" (Fetch §5.3): once only, the bytes read as asked, the
// answer a promise — rejected when the body was read before or the JSON
// does not parse.
Native consume_body(js::Interpreter& interp, js::Value const& this_value, Consume kind)
{
    std::optional<BodyRef> const ref = body_of(interp, this_value);
    if (!ref)
        return std::nullopt;
    Realm::Internals& internals = internals_of(interp);
    js::Interpreter::Roots const roots(interp);
    interp.root(this_value);
    if (*ref->used) {
        js::Value const error = js::Value::object(interp.new_error(js::ErrorType::TypeError, "Failed to execute on body: body stream already read"));
        return rejected_promise(interp, error);
    }
    std::vector<std::uint8_t> bytes;
    if (ref->body->has_value()) {
        *ref->used = true;
        bytes = **ref->body;
    }
    std::string content_type;
    if (ref->headers != nullptr) {
        if (std::optional<std::string> const value = ref->headers->combined("content-type"))
            content_type = *value;
    }
    switch (kind) {
    case Consume::Text:
        return resolved_promise(interp, js::Value::string(interp.string(utf8_to_string(bytes))));
    case Consume::Json: {
        Native const parsed = json_parse(interp, utf8_to_string(bytes));
        if (!parsed) {
            js::Value const error = interp.take_exception();
            return rejected_promise(interp, error);
        }
        return resolved_promise(interp, *parsed);
    }
    case Consume::ArrayBuffer: {
        std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(bytes.size()), std::nullopt);
        if (!buffer)
            return std::nullopt;
        if (!bytes.empty())
            std::memcpy((*buffer)->data(), bytes.data(), bytes.size());
        return resolved_promise(interp, js::Value::object(*buffer));
    }
    case Consume::Bytes: {
        Native const array = uint8_array_of(interp, bytes);
        if (!array)
            return std::nullopt;
        return resolved_promise(interp, *array);
    }
    case Consume::Blob:
        return resolved_promise(interp, js::Value::object(new_blob(internals, std::move(bytes), lowercase(content_type))));
    case Consume::FormData: {
        std::string const essence = mime_essence(content_type);
        auto* data = interp.heap().allocate<FormDataObject>(internals.prototype("FormData"));
        interp.root(js::Value::object(data));
        if (essence == "application/x-www-form-urlencoded") {
            for (auto& [name, value] : form_urldecode(std::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size())))
                data->entries.push_back({ std::move(name), std::move(value), nullptr, "" });
            return resolved_promise(interp, js::Value::object(data));
        }
        if (essence == "multipart/form-data") {
            std::size_t const at = lowercase(content_type).find("boundary=");
            std::string boundary = at == std::string::npos ? "" : content_type.substr(at + 9);
            if (!boundary.empty() && boundary.front() == '"')
                boundary = boundary.substr(1, boundary.find('"', 1) - 1);
            if (std::size_t const semicolon = boundary.find(';'); semicolon != std::string::npos)
                boundary = boundary.substr(0, semicolon);
            if (!boundary.empty() && multipart_decode(internals, bytes, normalize_header_value(boundary), *data))
                return resolved_promise(interp, js::Value::object(data));
        }
        js::Value const error = js::Value::object(interp.new_error(js::ErrorType::TypeError, "Failed to fetch: could not parse the body as FormData"));
        return rejected_promise(interp, error);
    }
    }
    return js::Value::undefined();
}

void install_body_mixin(Realm::Internals& in, js::Object& prototype)
{
    js::Interpreter& interpreter = in.interpreter;
    define_getter(in, prototype, "body", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!body_of(interp, this_value))
            return std::nullopt;
        return js::Value::null(); // no streams
    });
    define_getter(in, prototype, "bodyUsed", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BodyRef> const ref = body_of(interp, this_value);
        if (!ref)
            return std::nullopt;
        return js::Value::boolean(*ref->used);
    });
    for (auto const& [name, kind] : { std::pair { "arrayBuffer", Consume::ArrayBuffer }, std::pair { "blob", Consume::Blob },
             std::pair { "bytes", Consume::Bytes }, std::pair { "formData", Consume::FormData }, std::pair { "json", Consume::Json },
             std::pair { "text", Consume::Text } }) {
        Consume const which = kind;
        js::define_method(interpreter, prototype, name, 0, [which](js::Interpreter& interp, js::Value const& this_value, Args) {
            return consume_body(interp, this_value, which);
        });
    }
}

// ---- the request's making (Fetch §5.4)

// "Normalize" a method: the six standard ones uppercased however written;
// a forbidden one is a TypeError, and so is anything that is not a token.
std::optional<std::string> normalize_method(js::Interpreter& interp, std::string_view method)
{
    if (!is_header_name(method))
        return interp.throw_type_error("Failed to construct 'Request': '" + std::string(method) + "' is not a valid HTTP method.");
    std::string const upper = ascii_upper(method);
    if (upper == "CONNECT" || upper == "TRACE" || upper == "TRACK")
        return interp.throw_type_error("Failed to construct 'Request': '" + std::string(method) + "' HTTP method is unsupported.");
    if (upper == "DELETE" || upper == "GET" || upper == "HEAD" || upper == "OPTIONS" || upper == "POST" || upper == "PUT")
        return upper;
    return std::string(method);
}

// A string member of an init dictionary, when present.
std::optional<std::optional<std::string>> init_member(js::Interpreter& interp, js::Value const& init, std::string_view name)
{
    if (!init.is_object())
        return std::optional<std::string>();
    std::optional<js::Value> const value = interp.get(*init.as_object(), interp.key(name));
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return std::optional<std::string>();
    std::optional<std::string> const text = internals_of(interp).to_utf8(*value);
    if (!text)
        return std::nullopt;
    return text;
}

std::optional<std::optional<js::Value>> init_field(js::Interpreter& interp, js::Value const& init, std::string_view name)
{
    if (!init.is_object())
        return std::optional<js::Value>();
    std::optional<js::Value> const value = interp.get(*init.as_object(), interp.key(name));
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return std::optional<js::Value>();
    return *value;
}

bool one_of(std::string_view value, std::span<std::string_view const> allowed)
{
    return std::any_of(allowed.begin(), allowed.end(), [&](std::string_view item) { return item == value; });
}

// The enumerated members of a RequestInit and the values each admits.
constexpr std::string_view request_modes[] = { "same-origin", "cors", "no-cors" };
constexpr std::string_view request_credentials[] = { "omit", "same-origin", "include" };
constexpr std::string_view request_caches[] = { "default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached" };
constexpr std::string_view request_redirects[] = { "follow", "error", "manual" };
constexpr std::string_view request_policies[] = { "", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin",
    "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url" };
struct EnumMember {
    char const* name;
    std::string RequestObject::* field;
    std::span<std::string_view const> allowed;
};

std::optional<RequestObject*> make_request(js::Interpreter& interp, js::Value const& input, js::Value const& init)
{
    // new Request(input, init): a Request copied or a URL parsed, then each
    // init member over it; the headers made last so the body's type can
    // join them.
    Realm::Internals& internals = internals_of(interp);
    js::Interpreter::Roots const roots(interp);
    interp.root(input);
    interp.root(init);
    if (!init.is_undefined() && !init.is_object())
        return interp.throw_type_error("Failed to construct 'Request': The provided value is not of type 'RequestInit'.");
    auto* request = interp.heap().allocate<RequestObject>(internals.prototype("Request"));
    interp.root(js::Value::object(request));
    RequestObject const* source = input.is_object() ? dynamic_cast<RequestObject const*>(input.as_object()) : nullptr;
    std::vector<std::pair<std::string, std::string>> header_list;
    if (source != nullptr) {
        if (source->body_used)
            return interp.throw_type_error("Failed to construct 'Request': Cannot construct a Request with a Request object that has already been used.");
        request->url = source->url;
        request->method = source->method;
        header_list = source->headers->list;
        request->body = source->body;
        request->mode = source->mode;
        request->credentials = source->credentials;
        request->cache = source->cache;
        request->redirect = source->redirect;
        request->referrer = source->referrer;
        request->referrer_policy = source->referrer_policy;
        request->keepalive = source->keepalive;
        request->signal = source->signal;
    } else {
        std::optional<std::string> const text = internals.to_utf8(input);
        if (!text)
            return std::nullopt;
        std::optional<net::Url> const parsed = net::parse_url(*text, &internals.url);
        if (!parsed || parsed->includes_credentials())
            return interp.throw_type_error("Failed to construct 'Request': Failed to parse URL from " + *text);
        request->url = *parsed;
        request->url.fragment.reset();
    }
    std::optional<std::optional<std::string>> member = init_member(interp, init, "method");
    if (!member)
        return std::nullopt;
    if (*member) {
        std::optional<std::string> const method = normalize_method(interp, **member);
        if (!method)
            return std::nullopt;
        request->method = *method;
    }
    EnumMember const members[] = {
        { "mode", &RequestObject::mode, request_modes },
        { "credentials", &RequestObject::credentials, request_credentials },
        { "cache", &RequestObject::cache, request_caches },
        { "redirect", &RequestObject::redirect, request_redirects },
        { "referrerPolicy", &RequestObject::referrer_policy, request_policies },
    };
    for (EnumMember const& enum_member : members) {
        member = init_member(interp, init, enum_member.name);
        if (!member)
            return std::nullopt;
        if (!*member)
            continue;
        if (!one_of(**member, enum_member.allowed))
            return interp.throw_type_error("Failed to construct 'Request': The provided value '" + **member + "' is not a valid enum value of type " + std::string(enum_member.name) + ".");
        request->*enum_member.field = **member;
    }
    member = init_member(interp, init, "referrer");
    if (!member)
        return std::nullopt;
    if (*member)
        request->referrer = (*member)->empty() ? "no-referrer" : **member;
    std::optional<std::optional<js::Value>> field = init_field(interp, init, "keepalive");
    if (!field)
        return std::nullopt;
    if (*field)
        request->keepalive = js::Interpreter::to_boolean(**field);
    field = init_field(interp, init, "signal");
    if (!field)
        return std::nullopt;
    if (*field) {
        if ((*field)->is_null()) {
            request->signal = nullptr;
        } else {
            AbortSignalObject* signal = (*field)->is_object() ? dynamic_cast<AbortSignalObject*>((*field)->as_object()) : nullptr;
            if (signal == nullptr)
                return interp.throw_type_error("Failed to construct 'Request': member signal is not of type AbortSignal.");
            request->signal = signal;
        }
    }
    if (request->signal == nullptr)
        request->signal = new_abort_signal(internals);
    // The headers: the init's when given, else the source's, under the
    // request guard (no-cors admits only the safelisted names).
    HeadersObject* headers = new_headers(internals, request->mode == "no-cors" ? HeadersObject::Guard::RequestNoCors : HeadersObject::Guard::Request);
    request->headers = headers;
    field = init_field(interp, init, "headers");
    if (!field)
        return std::nullopt;
    if (*field) {
        if (!fill_headers(interp, *headers, **field, "Request"))
            return std::nullopt;
    } else {
        for (auto const& [name, value] : header_list) {
            if (!headers_append(interp, *headers, name, value, "Request"))
                return std::nullopt;
        }
    }
    field = init_field(interp, init, "body");
    if (!field)
        return std::nullopt;
    if (*field && !(*field)->is_null()) {
        if (request->method == "GET" || request->method == "HEAD")
            return interp.throw_type_error("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
        std::optional<std::optional<BodyInit>> extracted = extract_body(interp, **field);
        if (!extracted)
            return std::nullopt;
        if (*extracted) {
            request->body = std::move((*extracted)->bytes);
            if (!(*extracted)->type.empty() && !headers->has("content-type")) {
                if (!headers_append(interp, *headers, "content-type", (*extracted)->type, "Request"))
                    return std::nullopt;
            }
        }
    } else if (*field && (*field)->is_null()) {
        request->body.reset();
    }
    return request;
}

// ---- the fetching (Fetch §4)

bool is_same_origin(Realm::Internals& in, net::Url const& url)
{
    if (url.scheme == "data" || url.scheme == "about" || url.scheme == "blob")
        return true;
    return url.serialize_origin() == in.url.serialize_origin();
}

// Does a cross-origin request need a preflight (Fetch §4.7)? A method
// beyond GET, HEAD and POST, or a header beyond the safelisted ones.
bool needs_preflight(PageRequest const& page_request)
{
    if (page_request.method != "GET" && page_request.method != "HEAD" && page_request.method != "POST")
        return true;
    for (net::Header const& header : page_request.headers) {
        if (!is_cors_safelisted_request_header(lowercase(header.name), header.value))
            return true;
    }
    return false;
}

// The CORS check (Fetch §4.10) on a response's headers.
bool cors_check(std::vector<net::Header> const& headers, std::string_view origin, bool credentials)
{
    std::string const* const allow_origin = net::find_header(headers, "access-control-allow-origin");
    if (allow_origin == nullptr)
        return false;
    std::string const allowed = normalize_header_value(*allow_origin);
    if (allowed == "*")
        return !credentials;
    if (allowed != origin)
        return false;
    if (!credentials)
        return true;
    std::string const* const allow_credentials = net::find_header(headers, "access-control-allow-credentials");
    return allow_credentials != nullptr && normalize_header_value(*allow_credentials) == "true";
}

// The response headers the page may see across origins: the safelisted
// ones and those the server exposed.
std::vector<net::Header> filter_cors_headers(std::vector<net::Header> const& headers, bool credentials)
{
    std::vector<std::string> exposed;
    bool all = false;
    if (std::string const* const expose = net::find_header(headers, "access-control-expose-headers")) {
        exposed = split_list(*expose);
        all = !credentials && std::find(exposed.begin(), exposed.end(), "*") != exposed.end();
    }
    std::vector<net::Header> out;
    for (net::Header const& header : headers) {
        std::string const name = lowercase(header.name);
        if (is_forbidden_response_header(name))
            continue;
        if (all || is_cors_safelisted_response_header(name) || std::find(exposed.begin(), exposed.end(), name) != exposed.end())
            out.push_back(header);
    }
    return out;
}

} // namespace

FetchOutcome perform_fetch(Realm::Internals& in, PageRequest const& page_request)
{
    FetchOutcome outcome;
    if (!in.hooks.fetch_resource) {
        outcome.error = "the host gave no loader";
        return outcome;
    }
    bool const same_origin = is_same_origin(in, page_request.url);
    if (!same_origin && page_request.mode == FetchMode::SameOrigin) {
        outcome.error = "a same-origin request to " + page_request.url.serialize_origin();
        return outcome;
    }
    std::string const origin = in.url.serialize_origin();
    bool const credentials = page_request.credentials == FetchCredentials::Include || (page_request.credentials == FetchCredentials::SameOrigin && same_origin);
    std::vector<net::Header> headers = page_request.headers;
    if (net::find_header(headers, "accept") == nullptr)
        headers.push_back({ "Accept", "*/*" });
    if (page_request.body && !page_request.body_type.empty() && net::find_header(headers, "content-type") == nullptr)
        headers.push_back({ "content-type", page_request.body_type });
    if (!same_origin || (page_request.method != "GET" && page_request.method != "HEAD"))
        headers.push_back({ "Origin", origin });
    bool const cors = !same_origin && page_request.mode == FetchMode::Cors;
    if (cors && needs_preflight(page_request)) {
        net::ResourceRequest preflight;
        preflight.method = "OPTIONS";
        preflight.credentials = false;
        preflight.follow_redirects = false;
        preflight.headers.push_back({ "Accept", "*/*" });
        preflight.headers.push_back({ "Origin", origin });
        preflight.headers.push_back({ "Access-Control-Request-Method", page_request.method });
        std::vector<std::string> names;
        for (net::Header const& header : page_request.headers) {
            std::string const name = lowercase(header.name);
            if (!is_cors_safelisted_request_header(name, header.value) && std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        std::string joined;
        for (std::string const& name : names)
            joined += (joined.empty() ? "" : ",") + name;
        if (!joined.empty())
            preflight.headers.push_back({ "Access-Control-Request-Headers", joined });
        net::FetchResult const result = in.hooks.fetch_resource(page_request.url, preflight);
        if (!result.response) {
            outcome.error = "preflight: " + result.error;
            return outcome;
        }
        net::FetchResponse const& answer = *result.response;
        bool ok = answer.status >= 200 && answer.status <= 299 && cors_check(answer.headers, origin, credentials);
        if (ok && page_request.method != "GET" && page_request.method != "HEAD" && page_request.method != "POST") {
            std::vector<std::string> methods;
            if (std::string const* const allow = net::find_header(answer.headers, "access-control-allow-methods"))
                methods = split_list(*allow);
            ok = std::find(methods.begin(), methods.end(), lowercase(page_request.method)) != methods.end()
                || (!credentials && std::find(methods.begin(), methods.end(), "*") != methods.end());
        }
        if (ok && !names.empty()) {
            std::vector<std::string> allowed;
            if (std::string const* const allow = net::find_header(answer.headers, "access-control-allow-headers"))
                allowed = split_list(*allow);
            bool const any = !credentials && std::find(allowed.begin(), allowed.end(), "*") != allowed.end();
            for (std::string const& name : names) {
                if (!any && std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                    ok = false;
            }
        }
        if (!ok) {
            outcome.error = "the preflight for " + page_request.url.serialize() + " was refused";
            return outcome;
        }
    }
    net::ResourceRequest request;
    request.method = page_request.method;
    request.headers = std::move(headers);
    if (page_request.body)
        request.body = *page_request.body;
    request.credentials = credentials;
    request.follow_redirects = page_request.redirect == FetchRedirect::Follow;
    request.destination = page_request.destination;
    net::FetchResult result = in.hooks.fetch_resource(page_request.url, request);
    if (!result.response) {
        outcome.error = result.error;
        return outcome;
    }
    net::FetchResponse& response = *result.response;
    if (response.status >= 300 && response.status <= 399 && net::find_header(response.headers, "location") != nullptr
        && page_request.redirect != FetchRedirect::Follow) {
        if (page_request.redirect == FetchRedirect::Error) {
            outcome.error = "a redirect the request refused";
            return outcome;
        }
        outcome.ok = true;
        outcome.type = "opaqueredirect";
        outcome.url = page_request.url;
        return outcome;
    }
    // A response from another origin — asked for, or reached through a
    // redirect — is subject to the CORS check, or opaque under no-cors.
    bool const tainted = !is_same_origin(in, response.final_url) || !same_origin;
    if (tainted && page_request.mode == FetchMode::SameOrigin) {
        outcome.error = "a redirect left the origin";
        return outcome;
    }
    outcome.ok = true;
    outcome.url = response.final_url;
    outcome.redirected = response.redirected;
    if (tainted && page_request.mode == FetchMode::NoCors) {
        outcome.type = "opaque";
        outcome.url = net::Url();
        outcome.redirected = false;
        return outcome;
    }
    if (tainted) {
        if (!cors_check(response.headers, origin, credentials)) {
            outcome.ok = false;
            outcome.error = "the CORS check on " + response.final_url.serialize() + " failed";
            return outcome;
        }
        outcome.type = "cors";
        outcome.headers = filter_cors_headers(response.headers, credentials);
    } else {
        outcome.type = "basic";
        for (net::Header const& header : response.headers) {
            if (!is_forbidden_response_header(lowercase(header.name)))
                outcome.headers.push_back(header);
        }
    }
    outcome.status = response.status;
    outcome.status_text = std::move(response.status_text);
    outcome.body = std::move(response.body);
    return outcome;
}

FetchMode fetch_mode_of(std::string_view mode)
{
    if (mode == "same-origin")
        return FetchMode::SameOrigin;
    if (mode == "no-cors")
        return FetchMode::NoCors;
    return FetchMode::Cors;
}

FetchCredentials fetch_credentials_of(std::string_view credentials)
{
    if (credentials == "omit")
        return FetchCredentials::Omit;
    if (credentials == "include")
        return FetchCredentials::Include;
    return FetchCredentials::SameOrigin;
}

namespace {

// A Response over what a fetch answered.
ResponseObject* response_from(Realm::Internals& in, FetchOutcome const& outcome)
{
    js::Heap::NoCollect const no_collect(in.interpreter.heap());
    auto* response = in.interpreter.heap().allocate<ResponseObject>(in.prototype("Response"));
    response->type = outcome.type;
    response->url = outcome.url;
    response->has_url = !outcome.url.scheme.empty();
    response->redirected = outcome.redirected;
    response->status = outcome.status;
    response->status_text = outcome.status_text;
    response->headers = new_headers(in, HeadersObject::Guard::Immutable);
    for (net::Header const& header : outcome.headers)
        response->headers->append(lowercase(header.name), normalize_header_value(header.value));
    if (outcome.type != "opaque" && outcome.type != "opaqueredirect")
        response->body = outcome.body;
    return response;
}

// fetch(input, init): the request made now, the fetch run on the next
// task, the promise settled from there — or rejected at once when the
// signal is already aborted.
Native fetch_function(js::Interpreter& interp, js::Value const&, Args args)
{
    Realm::Internals& internals = internals_of(interp);
    js::Interpreter::Roots const roots(interp);
    std::optional<RequestObject*> const made = make_request(interp, js::argument(args, 0), js::argument(args, 1));
    if (!made) {
        js::Value const error = interp.take_exception();
        return rejected_promise(interp, error);
    }
    RequestObject& request = **made;
    interp.root(js::Value::object(&request));
    if (request.signal != nullptr && request.signal->aborted)
        return rejected_promise(interp, request.signal->reason);
    std::optional<js::PromiseCapability> const capability
        = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    interp.root(capability->promise);
    auto held_request = std::make_shared<js::Persistent>(interp.heap(), js::Value::object(&request));
    auto resolve = std::make_shared<js::Persistent>(interp.heap(), capability->resolve);
    auto reject = std::make_shared<js::Persistent>(interp.heap(), capability->reject);
    internals.post_task([&internals, held_request, resolve, reject] {
        Realm::Internals::Entry const entry(internals);
        js::Interpreter& interp_inner = internals.interpreter;
        js::Interpreter::Roots const task_roots(interp_inner);
        auto& pending = *static_cast<RequestObject*>(held_request->value().as_object());
        auto const settle = [&](js::Value const& function, js::Value const& value) {
            js::Value const arguments[1] = { value };
            interp_inner.root(value);
            interp_inner.call(function, js::Value::undefined(), arguments);
            if (interp_inner.has_exception())
                internals.report_uncaught(interp_inner.take_exception(), "fetch");
        };
        if (pending.signal != nullptr && pending.signal->aborted) {
            settle(reject->value(), pending.signal->reason);
            return;
        }
        PageRequest page_request;
        page_request.url = pending.url;
        page_request.method = pending.method;
        page_request.headers = pending.headers->as_list();
        page_request.body = pending.body;
        page_request.mode = fetch_mode_of(pending.mode);
        page_request.credentials = fetch_credentials_of(pending.credentials);
        page_request.redirect = pending.redirect == "error" ? FetchRedirect::Error : pending.redirect == "manual" ? FetchRedirect::Manual : FetchRedirect::Follow;
        FetchOutcome const outcome = perform_fetch(internals, page_request);
        if (pending.signal != nullptr && pending.signal->aborted) {
            settle(reject->value(), pending.signal->reason);
            return;
        }
        if (!outcome.ok) {
            internals.console("warn", "fetch " + pending.url.serialize() + ": " + outcome.error);
            settle(reject->value(), js::Value::object(interp_inner.new_error(js::ErrorType::TypeError, "Failed to fetch")));
            return;
        }
        ResponseObject* response = response_from(internals, outcome);
        settle(resolve->value(), js::Value::object(response));
    });
    return capability->promise;
}

void install_headers(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* headers = define_interface(in, "Headers", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            HeadersObject* object = new_headers(internals, HeadersObject::Guard::None);
            interp.root(js::Value::object(object));
            if (!fill_headers(interp, *object, js::argument(args, 0), "Headers"))
                return std::nullopt;
            return js::Value::object(object);
        },
        0);
    auto const name_of = [](js::Interpreter& interp, js::Value const& value) -> std::optional<std::string> {
        std::optional<std::string> const text = internals_of(interp).to_utf8(value);
        if (!text)
            return std::nullopt;
        if (!is_header_name(*text))
            return interp.throw_type_error("Failed to execute on 'Headers': Invalid name");
        return lowercase(*text);
    };
    js::define_method(interpreter, *headers, "append", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> const value = name ? internals.to_utf8(js::argument(args, 1)) : std::nullopt;
        if (!name || !value)
            return std::nullopt;
        if (!headers_append(interp, **found, *name, *value, "append"))
            return std::nullopt;
        return js::Value::undefined();
    });
    js::define_method(interpreter, *headers, "set", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> name = internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> value = name ? internals.to_utf8(js::argument(args, 1)) : std::nullopt;
        if (!name || !value)
            return std::nullopt;
        std::optional<bool> const admitted = admit_header(interp, **found, *name, *value, "set");
        if (!admitted)
            return std::nullopt;
        if (*admitted)
            (*found)->replace(std::move(*name), std::move(*value));
        return js::Value::undefined();
    });
    js::define_method(interpreter, *headers, "delete", 1, [name_of](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = name_of(interp, js::argument(args, 0));
        if (!name)
            return std::nullopt;
        HeadersObject& object = **found;
        if (object.guard == HeadersObject::Guard::Immutable)
            return interp.throw_type_error("Failed to execute 'delete' on 'Headers': Headers are immutable");
        if ((object.guard == HeadersObject::Guard::Request && is_forbidden_request_header(*name))
            || (object.guard == HeadersObject::Guard::Response && is_forbidden_response_header(*name))
            || (object.guard == HeadersObject::Guard::RequestNoCors && !is_cors_safelisted_request_header(*name, "")))
            return js::Value::undefined();
        object.remove(*name);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *headers, "get", 1, [name_of](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = name_of(interp, js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<std::string> const value = (*found)->combined(*name);
        return value ? internals_of(interp).string(*value) : js::Value::null();
    });
    js::define_method(interpreter, *headers, "getSetCookie", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        std::vector<js::Value> values;
        for (auto const& [name, value] : (*found)->list) {
            if (name == "set-cookie")
                values.push_back(interp.root(internals.string(value)));
        }
        return js::Value::object(interp.new_array(values));
    });
    js::define_method(interpreter, *headers, "has", 1, [name_of](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = name_of(interp, js::argument(args, 0));
        if (!name)
            return std::nullopt;
        return js::Value::boolean((*found)->has(*name));
    });
    js::define_method(interpreter, *headers, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<HeadersObject*> const found = this_headers(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'Function'.");
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(callback);
        js::Value const this_argument = js::argument(args, 1);
        interp.root(this_argument);
        for (auto const& [name, value] : (*found)->sorted_and_combined()) {
            js::Interpreter::Roots const pair_roots(interp);
            js::Value const arguments[3] = { interp.root(internals.string(value)), interp.root(internals.string(name)), this_value };
            if (!interp.call(callback, this_argument, arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    for (auto const& [name, kind] : { std::pair { "entries", 0 }, std::pair { "keys", 1 }, std::pair { "values", 2 } }) {
        int const which = kind;
        js::NativeFunction* method = js::define_method(interpreter, *headers, name, 0, [which](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<HeadersObject*> const found = this_headers(interp, this_value);
            if (!found)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            std::vector<js::Value> items;
            for (auto const& [key, value] : (*found)->sorted_and_combined()) {
                if (which == 1) {
                    items.push_back(interp.root(internals.string(key)));
                } else if (which == 2) {
                    items.push_back(interp.root(internals.string(value)));
                } else {
                    js::Value const pair[2] = { interp.root(internals.string(key)), interp.root(internals.string(value)) };
                    items.push_back(interp.root(js::Value::object(interp.new_array(pair))));
                }
            }
            return iterator_over(interp, items);
        });
        if (which == 0)
            headers->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), js::Value::object(method), js::builtin_attributes);
    }
}

void install_form_data(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* form_data = define_interface(in, "FormData", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            auto* data = interp.heap().allocate<FormDataObject>(internals.prototype("FormData"));
            interp.root(js::Value::object(data));
            js::Value const form = js::argument(args, 0);
            if (!form.is_undefined() && !form.is_null()) {
                dom::Node* node = internals.realm.node_of(form);
                auto* element = node ? dynamic_cast<dom::Element*>(node) : nullptr;
                if (element == nullptr || !element->is_html("form"))
                    return interp.throw_type_error("Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'.");
                collect_form_entries(internals, *element, *data);
            }
            return js::Value::object(data);
        },
        0);
    js::define_method(interpreter, *form_data, "append", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        for (js::Value const& argument_value : args)
            interp.root(argument_value);
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<FormDataObject::Entry> entry = make_entry(interp, *name, js::argument(args, 1), js::argument(args, 2));
        if (!entry)
            return std::nullopt;
        (*found)->entries.push_back(std::move(*entry));
        return js::Value::undefined();
    });
    js::define_method(interpreter, *form_data, "set", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        for (js::Value const& argument_value : args)
            interp.root(argument_value);
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<FormDataObject::Entry> entry = make_entry(interp, *name, js::argument(args, 1), js::argument(args, 2));
        if (!entry)
            return std::nullopt;
        auto& entries = (*found)->entries;
        auto const first = std::find_if(entries.begin(), entries.end(), [&](auto const& e) { return e.name == *name; });
        if (first == entries.end()) {
            entries.push_back(std::move(*entry));
        } else {
            *first = std::move(*entry);
            entries.erase(std::remove_if(first + 1, entries.end(), [&](auto const& e) { return e.name == *name; }), entries.end());
        }
        return js::Value::undefined();
    });
    js::define_method(interpreter, *form_data, "delete", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::erase_if((*found)->entries, [&](auto const& e) { return e.name == *name; });
        return js::Value::undefined();
    });
    js::define_method(interpreter, *form_data, "get", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        for (FormDataObject::Entry const& entry : (*found)->entries) {
            if (entry.name == *name)
                return entry_value(internals_of(interp), entry);
        }
        return js::Value::null();
    });
    js::define_method(interpreter, *form_data, "getAll", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        std::vector<js::Value> values;
        for (FormDataObject::Entry const& entry : (*found)->entries) {
            if (entry.name == *name)
                values.push_back(interp.root(entry_value(internals_of(interp), entry)));
        }
        return js::Value::object(interp.new_array(values));
    });
    js::define_method(interpreter, *form_data, "has", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        return js::Value::boolean(std::any_of((*found)->entries.begin(), (*found)->entries.end(), [&](auto const& e) { return e.name == *name; }));
    });
    js::define_method(interpreter, *form_data, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("Failed to execute 'forEach' on 'FormData': parameter 1 is not of type 'Function'.");
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(callback);
        js::Value const this_argument = js::argument(args, 1);
        interp.root(this_argument);
        std::vector<FormDataObject::Entry> const snapshot = (*found)->entries;
        for (FormDataObject::Entry const& entry : snapshot) {
            js::Interpreter::Roots const pair_roots(interp);
            js::Value const arguments[3] = { interp.root(entry_value(internals, entry)), interp.root(internals.string(entry.name)), this_value };
            if (!interp.call(callback, this_argument, arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    for (auto const& [name, kind] : { std::pair { "entries", 0 }, std::pair { "keys", 1 }, std::pair { "values", 2 } }) {
        int const which = kind;
        js::NativeFunction* method = js::define_method(interpreter, *form_data, name, 0, [which](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<FormDataObject*> const found = this_form_data(interp, this_value);
            if (!found)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            std::vector<js::Value> items;
            for (FormDataObject::Entry const& entry : (*found)->entries) {
                if (which == 1) {
                    items.push_back(interp.root(internals.string(entry.name)));
                } else if (which == 2) {
                    items.push_back(interp.root(entry_value(internals, entry)));
                } else {
                    js::Value const pair[2] = { interp.root(internals.string(entry.name)), interp.root(entry_value(internals, entry)) };
                    items.push_back(interp.root(js::Value::object(interp.new_array(pair))));
                }
            }
            return iterator_over(interp, items);
        });
        if (which == 0)
            form_data->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), js::Value::object(method), js::builtin_attributes);
    }
}

void install_request(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* request = define_interface(in, "Request", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'Request': 1 argument required, but only 0 present.");
            std::optional<RequestObject*> const made = make_request(interp, args[0], js::argument(args, 1));
            if (!made)
                return std::nullopt;
            return js::Value::object(*made);
        },
        1);
    define_getter(in, *request, "method", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        return found ? Native(internals_of(interp).string((*found)->method)) : std::nullopt;
    });
    define_getter(in, *request, "url", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        return found ? Native(internals_of(interp).string((*found)->url.serialize())) : std::nullopt;
    });
    define_getter(in, *request, "headers", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        return found ? Native(js::Value::object((*found)->headers)) : std::nullopt;
    });
    for (auto const& [name, member] : { std::pair { "mode", &RequestObject::mode }, std::pair { "credentials", &RequestObject::credentials },
             std::pair { "cache", &RequestObject::cache }, std::pair { "redirect", &RequestObject::redirect },
             std::pair { "referrer", &RequestObject::referrer }, std::pair { "referrerPolicy", &RequestObject::referrer_policy } }) {
        std::string RequestObject::* const field = member;
        define_getter(in, *request, name, [field](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<RequestObject*> const found = this_request(interp, this_value);
            if (!found)
                return std::nullopt;
            std::string const& value = (*found)->*field;
            return internals_of(interp).string(field == &RequestObject::referrer && value == "no-referrer" ? std::string() : value);
        });
    }
    for (std::string_view const name : { "destination", "integrity" }) {
        define_getter(in, *request, name, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<RequestObject*> const found = this_request(interp, this_value);
            return found ? Native(internals_of(interp).string("")) : std::nullopt;
        });
    }
    define_getter(in, *request, "duplex", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        return found ? Native(internals_of(interp).string("half")) : std::nullopt;
    });
    define_getter(in, *request, "keepalive", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        return found ? Native(js::Value::boolean((*found)->keepalive)) : std::nullopt;
    });
    for (std::string_view const name : { "isReloadNavigation", "isHistoryNavigation" }) {
        define_getter(in, *request, name, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<RequestObject*> const found = this_request(interp, this_value);
            return found ? Native(js::Value::boolean(false)) : std::nullopt;
        });
    }
    define_getter(in, *request, "signal", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->signal ? js::Value::object((*found)->signal) : js::Value::null();
    });
    js::define_method(interpreter, *request, "clone", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RequestObject*> const found = this_request(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->body_used)
            return interp.throw_type_error("Failed to execute 'clone' on 'Request': Request body is already used");
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        auto* copy = interp.heap().allocate<RequestObject>(internals.prototype("Request"));
        interp.root(js::Value::object(copy));
        RequestObject const& source = **found;
        copy->url = source.url;
        copy->method = source.method;
        copy->body = source.body;
        copy->mode = source.mode;
        copy->credentials = source.credentials;
        copy->cache = source.cache;
        copy->redirect = source.redirect;
        copy->referrer = source.referrer;
        copy->referrer_policy = source.referrer_policy;
        copy->keepalive = source.keepalive;
        copy->signal = source.signal;
        copy->headers = new_headers(internals, source.headers->guard);
        copy->headers->list = source.headers->list;
        return js::Value::object(copy);
    });
    install_body_mixin(in, *request);
}

void install_response(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* response = define_interface(in, "Response", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            // new Response(body, init): the status and its text, the
            // headers, then the body — which a null-body status refuses.
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            js::Value const body = js::argument(args, 0);
            js::Value const init = js::argument(args, 1);
            interp.root(body);
            interp.root(init);
            if (!init.is_undefined() && !init.is_object())
                return interp.throw_type_error("Failed to construct 'Response': The provided value is not of type 'ResponseInit'.");
            auto* object = interp.heap().allocate<ResponseObject>(internals.prototype("Response"));
            interp.root(js::Value::object(object));
            object->headers = new_headers(internals, HeadersObject::Guard::Response);
            std::optional<std::optional<js::Value>> field = init_field(interp, init, "status");
            if (!field)
                return std::nullopt;
            if (*field) {
                std::optional<double> const status = interp.to_number(**field);
                if (!status)
                    return std::nullopt;
                if (!(*status >= 200 && *status <= 599) || *status != std::floor(*status))
                    return interp.throw_range_error("Failed to construct 'Response': The status provided (" + js::number_to_utf8(*status) + ") is outside the range [200, 599].");
                object->status = static_cast<int>(*status);
            }
            std::optional<std::optional<std::string>> const text = init_member(interp, init, "statusText");
            if (!text)
                return std::nullopt;
            if (*text) {
                if (!is_header_value(**text))
                    return interp.throw_type_error("Failed to construct 'Response': Invalid statusText");
                object->status_text = **text;
            }
            field = init_field(interp, init, "headers");
            if (!field)
                return std::nullopt;
            if (*field && !fill_headers(interp, *object->headers, **field, "Response"))
                return std::nullopt;
            if (!body.is_null() && !body.is_undefined()) {
                if (object->status == 101 || object->status == 103 || object->status == 204 || object->status == 205 || object->status == 304)
                    return interp.throw_type_error("Failed to construct 'Response': Response with null body status cannot have body");
                std::optional<std::optional<BodyInit>> extracted = extract_body(interp, body);
                if (!extracted)
                    return std::nullopt;
                if (*extracted) {
                    object->body = std::move((*extracted)->bytes);
                    if (!(*extracted)->type.empty() && !object->headers->has("content-type"))
                        object->headers->append("content-type", (*extracted)->type);
                }
            }
            return js::Value::object(object);
        },
        0);
    js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key("Response"));
    js::define_method(interpreter, *constructor.as_object(), "error", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Heap::NoCollect const no_collect(interp.heap());
        auto* object = interp.heap().allocate<ResponseObject>(internals.prototype("Response"));
        object->type = "error";
        object->status = 0;
        object->headers = new_headers(internals, HeadersObject::Guard::Immutable);
        return js::Value::object(object);
    });
    js::define_method(interpreter, *constructor.as_object(), "redirect", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        std::optional<net::Url> const parsed = net::parse_url(*text, &internals.url);
        if (!parsed)
            return interp.throw_type_error("Failed to execute 'redirect' on 'Response': Failed to parse URL from " + *text);
        double status = 302;
        if (!js::argument(args, 1).is_undefined()) {
            std::optional<double> const given = interp.to_number(args[1]);
            if (!given)
                return std::nullopt;
            status = *given;
        }
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
            return interp.throw_range_error("Failed to execute 'redirect' on 'Response': Invalid status code");
        js::Heap::NoCollect const no_collect(interp.heap());
        auto* object = interp.heap().allocate<ResponseObject>(internals.prototype("Response"));
        object->status = static_cast<int>(status);
        object->headers = new_headers(internals, HeadersObject::Guard::Immutable);
        object->headers->append("location", parsed->serialize());
        return js::Value::object(object);
    });
    js::define_method(interpreter, *constructor.as_object(), "json", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        js::Interpreter::Roots const roots(interp);
        js::Value const init = js::argument(args, 1);
        interp.root(init);
        Native const text = json_stringify(interp, js::argument(args, 0));
        if (!text)
            return std::nullopt;
        if (!text->is_string())
            return interp.throw_type_error("Failed to execute 'json' on 'Response': The data is not JSON serializable");
        interp.root(*text);
        std::optional<js::Value> const response_constructor = interp.get(*interp.global(), interp.key("Response"));
        if (!response_constructor)
            return std::nullopt;
        js::Value const arguments[2] = { *text, init };
        std::optional<js::Value> const made = interp.construct(*response_constructor, arguments);
        if (!made)
            return std::nullopt;
        auto* object = static_cast<ResponseObject*>(made->as_object());
        object->headers->replace("content-type", "application/json");
        return *made;
    });
    define_getter(in, *response, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        return found ? Native(internals_of(interp).string((*found)->type)) : std::nullopt;
    });
    define_getter(in, *response, "url", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string((*found)->has_url ? (*found)->url.serialize(true) : std::string());
    });
    define_getter(in, *response, "redirected", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        return found ? Native(js::Value::boolean((*found)->redirected)) : std::nullopt;
    });
    define_getter(in, *response, "status", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        return found ? Native(js::Value::number((*found)->status)) : std::nullopt;
    });
    define_getter(in, *response, "ok", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        return found ? Native(js::Value::boolean((*found)->status >= 200 && (*found)->status <= 299)) : std::nullopt;
    });
    define_getter(in, *response, "statusText", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        return found ? Native(internals_of(interp).string((*found)->status_text)) : std::nullopt;
    });
    define_getter(in, *response, "headers", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        return found ? Native(js::Value::object((*found)->headers)) : std::nullopt;
    });
    js::define_method(interpreter, *response, "clone", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResponseObject*> const found = this_response(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->body_used)
            return interp.throw_type_error("Failed to execute 'clone' on 'Response': Response body is already used");
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        auto* copy = interp.heap().allocate<ResponseObject>(internals.prototype("Response"));
        interp.root(js::Value::object(copy));
        ResponseObject const& source = **found;
        copy->type = source.type;
        copy->url = source.url;
        copy->has_url = source.has_url;
        copy->redirected = source.redirected;
        copy->status = source.status;
        copy->status_text = source.status_text;
        copy->body = source.body;
        copy->headers = new_headers(internals, source.headers->guard);
        copy->headers->list = source.headers->list;
        return js::Value::object(copy);
    });
    install_body_mixin(in, *response);
}

} // namespace

void install_fetch(Realm::Internals& in)
{
    install_headers(in);
    install_form_data(in);
    install_request(in);
    install_response(in);
    js::define_method(in.interpreter, *in.interpreter.global(), "fetch", 1, fetch_function);
}

}
