#include "pdf/Writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace sashfold::pdf {

namespace {

using Bytes = std::vector<std::uint8_t>;

void put(std::string& out, std::string_view text)
{
    out.append(text.data(), text.size());
}

std::string real(double v)
{
    if (!std::isfinite(v))
        return "0";
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        return std::to_string(static_cast<long long>(v));
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.5f", v);
    std::string s = buffer;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

void serialize_into(Object const& object, std::string& out)
{
    if (object.is_null()) {
        put(out, "null");
    } else if (object.is_bool()) {
        put(out, object.as_bool() ? "true" : "false");
    } else if (object.is_int()) {
        put(out, std::to_string(object.as_int()));
    } else if (object.is_number()) {
        put(out, real(object.as_number()));
    } else if (object.is_string()) {
        // Hexadecimal: every byte, whatever it is, written the same way.
        std::string const& s = object.as_string();
        out.push_back('<');
        static constexpr char digits[] = "0123456789ABCDEF";
        for (char const c : s) {
            auto const u = static_cast<unsigned char>(c);
            out.push_back(digits[u >> 4]);
            out.push_back(digits[u & 15]);
        }
        out.push_back('>');
    } else if (object.is_name()) {
        out.push_back('/');
        for (char const c : object.as_name()) {
            auto const u = static_cast<unsigned char>(c);
            if (u < 0x21 || u > 0x7E || c == '#' || c == '/' || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
                || c == '{' || c == '}' || c == '%') {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "#%02X", u);
                put(out, buffer);
            } else {
                out.push_back(c);
            }
        }
    } else if (object.is_array()) {
        out.push_back('[');
        bool first = true;
        for (Object const& item : object.as_array()) {
            if (!first)
                out.push_back(' ');
            first = false;
            serialize_into(item, out);
        }
        out.push_back(']');
    } else if (object.is_dict() || object.is_stream()) {
        Dict const& dict = object.as_dict();
        put(out, "<<");
        for (auto const& [key, value] : dict.entries()) {
            serialize_into(Object::name(key), out);
            out.push_back(' ');
            serialize_into(value, out);
        }
        put(out, ">>");
        if (Stream const* stream = object.as_stream()) {
            Bytes const data = stream->raw();
            put(out, "\nstream\n");
            out.append(reinterpret_cast<char const*>(data.data()), data.size());
            put(out, "\nendstream");
        }
    } else if (object.is_ref()) {
        put(out, std::to_string(object.as_ref().num) + " " + std::to_string(object.as_ref().gen) + " R");
    }
}

// A stream made from bytes already in memory, for the update: its
// dictionary with /Length set.
Object stream_of(Dict dict, Bytes data)
{
    dict.set("Length", Object::integer(static_cast<std::int64_t>(data.size())));
    Stream stream;
    stream.dict = std::move(dict);
    auto const source = std::make_shared<Bytes const>(std::move(data));
    stream.source = source;
    stream.start = 0;
    stream.length = source->size();
    return Object::stream(std::move(stream));
}

// The objects an update writes, in order, with their numbers.
class Update {
public:
    explicit Update(Document const& doc)
        : m_doc(doc)
        , m_next(static_cast<int>(std::max<std::size_t>(1, doc.object_count())))
    {
        // The trailer's /Size may say more than the table shows.
        std::int64_t const size = doc.trailer().get("Size").as_int(0);
        if (size > m_next && size < 50'000'000)
            m_next = static_cast<int>(size);
    }

    Ref allocate() { return Ref { m_next++, 0 }; }
    void write(Ref ref, Object object) { m_objects[ref] = std::move(object); }
    bool empty() const { return m_objects.empty(); }

    // The file's bytes with the update appended.
    Bytes finish() const
    {
        Bytes out = m_doc.bytes();
        if (out.empty() || out.back() != '\n')
            out.push_back('\n');
        std::map<int, std::pair<std::size_t, int>> offsets; // number → (offset, generation)
        SecurityHandler const* const security = m_doc.security();
        for (auto const& [ref, object] : m_objects) {
            offsets[ref.num] = { out.size(), ref.gen };
            std::string text = std::to_string(ref.num) + " " + std::to_string(ref.gen) + " obj\n";
            serialize_into(security ? encrypted(*security, ref, object) : object, text);
            text += "\nendobj\n";
            out.insert(out.end(), text.begin(), text.end());
        }
        int const size = std::max(m_next, static_cast<int>(m_doc.trailer().get("Size").as_int(0)));
        std::size_t const xref_at = out.size();
        Dict trailer;
        trailer.set("Size", Object::integer(size));
        if (m_doc.startxref())
            trailer.set("Prev", Object::integer(static_cast<std::int64_t>(*m_doc.startxref())));
        for (char const* key : { "Root", "Info", "ID", "Encrypt" }) {
            if (Object const* value = m_doc.trailer().find(key))
                trailer.set(key, *value);
        }
        std::string text;
        if (m_doc.xref_is_stream()) {
            // A cross-reference stream (§7.5.8): a row per object written,
            // itself included, each 1 + 4 + 2 bytes.
            Ref const self = Ref { size, 0 };
            offsets[self.num] = { xref_at, 0 };
            Array index;
            Bytes rows;
            for (auto const& [num, entry] : offsets) {
                index.push_back(Object::integer(num));
                index.push_back(Object::integer(1));
                rows.push_back(1);
                for (int shift = 24; shift >= 0; shift -= 8)
                    rows.push_back(static_cast<std::uint8_t>(entry.first >> shift));
                rows.push_back(static_cast<std::uint8_t>(entry.second >> 8));
                rows.push_back(static_cast<std::uint8_t>(entry.second & 0xFF));
            }
            trailer.set("Type", Object::name("XRef"));
            trailer.set("Size", Object::integer(size + 1));
            trailer.set("W", Object::array({ Object::integer(1), Object::integer(4), Object::integer(2) }));
            trailer.set("Index", Object::array(std::move(index)));
            text = std::to_string(self.num) + " 0 obj\n";
            serialize_into(stream_of(trailer, std::move(rows)), text);
            text += "\nendobj\n";
        } else {
            text = "xref\n";
            // Subsections of consecutive numbers.
            auto it = offsets.begin();
            while (it != offsets.end()) {
                auto run_end = it;
                int count = 0;
                while (run_end != offsets.end() && run_end->first == it->first + count) {
                    ++run_end;
                    ++count;
                }
                text += std::to_string(it->first) + " " + std::to_string(count) + "\n";
                for (auto e = it; e != run_end; ++e) {
                    char line[32];
                    std::snprintf(line, sizeof line, "%010zu %05d n \n", e->second.first, e->second.second);
                    text += line;
                }
                it = run_end;
            }
            text += "trailer\n";
            serialize_into(Object::dict(trailer), text);
            text += "\n";
        }
        text += "startxref\n" + std::to_string(xref_at) + "\n%%EOF\n";
        out.insert(out.end(), text.begin(), text.end());
        return out;
    }

private:
    // The object with its strings and stream encrypted for its number.
    static Object encrypted(SecurityHandler const& security, Ref owner, Object const& object)
    {
        if (object.is_string())
            return Object::string(security.encrypt_string(owner, object.as_string()), true);
        if (object.is_array()) {
            Array items;
            for (Object const& item : object.as_array())
                items.push_back(encrypted(security, owner, item));
            return Object::array(std::move(items));
        }
        if (object.is_dict() && !object.is_stream()) {
            Dict dict;
            for (auto const& [key, value] : object.as_dict().entries())
                dict.set(key, encrypted(security, owner, value));
            return Object::dict(std::move(dict));
        }
        if (Stream const* stream = object.as_stream()) {
            Dict dict;
            for (auto const& [key, value] : stream->dict.entries())
                dict.set(key, encrypted(security, owner, value));
            return stream_of(std::move(dict), security.encrypt_stream(owner, stream->raw()));
        }
        return object;
    }

    Document const& m_doc;
    int m_next;
    std::map<Ref, Object> m_objects;
};

// The value entry a field dictionary carries for a value.
Object value_object(FormField const& field, FormValue const& value)
{
    switch (field.kind) {
    case FormField::Kind::CheckBox:
    case FormField::Kind::Radio:
        return Object::name(value.state.empty() ? "Off" : value.state);
    case FormField::Kind::ListBox: {
        if (value.selections.size() == 1)
            return Object::string(utf8_to_text_string(value.selections[0]));
        Array items;
        for (std::string const& s : value.selections)
            items.push_back(Object::string(utf8_to_text_string(s)));
        return Object::array(std::move(items));
    }
    default:
        return Object::string(utf8_to_text_string(value.text));
    }
}

} // namespace

std::string serialize_object(Object const& object)
{
    std::string out;
    serialize_into(object, out);
    return out;
}

Bytes save_with_form_values(Document const& doc, FormValues const& values)
{
    if (values.empty())
        return doc.bytes();
    Update update(doc);
    // Every widget on every page, grouped by its field: the field's
    // dictionary gets the value, each widget its appearance (a new stream,
    // or the state its value picks).
    std::map<Ref, std::vector<FormField>> widgets_by_field;
    for (std::size_t page = 0; page < doc.page_count(); ++page) {
        for (FormField& field : page_fields(doc, page)) {
            if (values.contains(field.field))
                widgets_by_field[field.field].push_back(std::move(field));
        }
    }
    for (auto const& [field_ref, widgets] : widgets_by_field) {
        FormValue const& value = values.at(field_ref);
        FormField const& first = widgets.front();
        if (first.read_only())
            continue;
        Dict field_dict = doc.resolve(Object::ref(field_ref)).as_dict();
        if (field_dict.empty())
            continue;
        field_dict.set("V", value_object(first, value));
        bool const buttons = first.kind == FormField::Kind::CheckBox || first.kind == FormField::Kind::Radio;
        for (FormField const& widget : widgets) {
            bool const merged = widget.widget == field_ref;
            Dict widget_dict = merged ? field_dict : doc.resolve(Object::ref(widget.widget)).as_dict();
            if (widget_dict.empty())
                continue;
            if (buttons) {
                widget_dict.set("AS", Object::name(widget_state(widget, &value)));
            } else {
                Appearance const made = widget_appearance(doc, widget, &value);
                Dict form;
                form.set("Type", Object::name("XObject"));
                form.set("Subtype", Object::name("Form"));
                form.set("BBox", Object::array({ Object::real(made.bbox.x0), Object::real(made.bbox.y0), Object::real(made.bbox.x1), Object::real(made.bbox.y1) }));
                if (made.matrix != std::array<double, 6> { { 1, 0, 0, 1, 0, 0 } }) {
                    Array matrix;
                    for (double const m : made.matrix)
                        matrix.push_back(Object::real(m));
                    form.set("Matrix", Object::array(std::move(matrix)));
                }
                form.set("Resources", Object::dict(made.resources));
                Ref const stream_ref = update.allocate();
                update.write(stream_ref, stream_of(std::move(form), Bytes(made.content.begin(), made.content.end())));
                Dict ap;
                ap.set("N", Object::ref(stream_ref));
                widget_dict.set("AP", Object::dict(std::move(ap)));
            }
            if (merged)
                field_dict = widget_dict;
            else
                update.write(widget.widget, Object::dict(std::move(widget_dict)));
        }
        update.write(field_ref, Object::dict(std::move(field_dict)));
    }
    if (update.empty())
        return doc.bytes();
    return update.finish();
}

}
