#include "pdf/FormScript.h"

#include "core/Json.h"
#include "js/Interpreter.h"
#include "platform/Memory.h"
#include "platform/ScriptThread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace sashfold::pdf {

namespace {

constexpr int k_max_depth = 32;

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (char const c : text) {
        auto const u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", u);
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

std::string json_number(double v)
{
    if (!std::isfinite(v))
        return "0";
    std::ostringstream out;
    out.precision(10);
    out << v;
    return out.str();
}

std::string json_bool(bool v) { return v ? "true" : "false"; }

// A field dictionary's full name: the partial names up its parents.
std::string full_name(Document const& doc, Dict const& start)
{
    std::vector<std::string> parts;
    Dict current = start;
    for (int depth = 0; depth < k_max_depth; ++depth) {
        Object const t = doc.resolve(current.get("T"));
        if (t.is_string())
            parts.push_back(text_string_to_utf8(t.as_string()));
        Object const parent = doc.resolve(current.get("Parent"));
        if (!parent.is_dict())
            break;
        current = parent.as_dict();
    }
    std::string name;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        name += (name.empty() ? "" : ".") + *it;
    return name;
}

// A JavaScript action's script: a text string or a stream of one.
std::string script_text(Document const& doc, Object const& js)
{
    Object const resolved = doc.resolve(js);
    if (resolved.is_string())
        return text_string_to_utf8(resolved.as_string());
    if (Stream const* stream = resolved.as_stream()) {
        std::vector<std::uint8_t> const bytes = doc.decode(*stream);
        return text_string_to_utf8(std::string(bytes.begin(), bytes.end()));
    }
    return "";
}

// Field names an action names (/Fields of ResetForm, /T of Hide): each a
// reference to a field or a widget, or a fully qualified name.
std::vector<std::string> named_fields(Document const& doc, Object const& list_in)
{
    std::vector<std::string> names;
    Object const list = doc.resolve(list_in);
    auto const add = [&](Object const& item) {
        Object const resolved = doc.resolve(item);
        if (resolved.is_string())
            names.push_back(text_string_to_utf8(resolved.as_string()));
        else if (resolved.is_dict())
            names.push_back(full_name(doc, resolved.as_dict()));
    };
    if (list.is_array()) {
        for (Object const& item : list.as_array())
            add(item);
    } else if (!list.is_null()) {
        add(list_in);
    }
    return names;
}

// An action and the ones its /Next names, as the model's array items:
// {"js": ...} for JavaScript, the form's own kinds by their names, the
// rest left out. A /Next chain that loops is followed once.
void read_actions(Document const& doc, Object const& action_in, std::string& out, std::set<Ref>& seen, int depth)
{
    if (depth > k_max_depth)
        return;
    if (action_in.is_ref() && !seen.insert(action_in.as_ref()).second)
        return;
    Object const action = doc.resolve(action_in);
    if (action.is_array()) {
        for (Object const& item : action.as_array())
            read_actions(doc, item, out, seen, depth + 1);
        return;
    }
    if (!action.is_dict())
        return;
    Dict const& dict = action.as_dict();
    std::string const kind = doc.resolve(dict.get("S")).as_name();
    std::string item;
    if (kind == "JavaScript") {
        item = "{\"js\":" + json_string(script_text(doc, dict.get("JS"))) + "}";
    } else if (kind == "ResetForm") {
        item = "{\"type\":\"ResetForm\",\"fields\":[";
        std::vector<std::string> const names = named_fields(doc, dict.get("Fields"));
        for (std::size_t i = 0; i < names.size(); ++i)
            item += (i ? "," : "") + json_string(names[i]);
        item += "],\"exclude\":" + json_bool(doc.resolve(dict.get("Flags")).as_int(0) & 1) + "}";
    } else if (kind == "Hide") {
        item = "{\"type\":\"Hide\",\"fields\":[";
        std::vector<std::string> const names = named_fields(doc, dict.get("T"));
        for (std::size_t i = 0; i < names.size(); ++i)
            item += (i ? "," : "") + json_string(names[i]);
        item += "],\"hide\":" + json_bool(doc.resolve(dict.get("H")).as_bool(true)) + "}";
    } else if (kind == "Named") {
        item = "{\"type\":\"Named\",\"name\":" + json_string(doc.resolve(dict.get("N")).as_name()) + "}";
    }
    if (!item.empty())
        out += (out.empty() ? "" : ",") + item;
    if (Object const* next = dict.find("Next"))
        read_actions(doc, *next, out, seen, depth + 1);
}

std::string actions_json(Document const& doc, Object const& action)
{
    std::string items;
    std::set<Ref> seen;
    read_actions(doc, action, items, seen, 0);
    return "[" + items + "]";
}

// An additional-actions dictionary's triggers, as a JSON object of arrays.
std::string triggers_json(Document const& doc, Dict const& aa, std::initializer_list<char const*> keys)
{
    std::string out;
    for (char const* key : keys) {
        Object const* action = aa.find(key);
        if (!action)
            continue;
        std::string const list = actions_json(doc, *action);
        if (list != "[]")
            out += (out.empty() ? "" : ",") + json_string(key) + ":" + list;
    }
    return out;
}

// A color as the forms API writes one, from the operators a field's
// /DA or /MK gave ("1 0 0 rg").
std::string color_json(std::string const& operators)
{
    std::istringstream words(operators);
    std::vector<std::string> parts;
    std::string word;
    while (words >> word)
        parts.push_back(word);
    if (parts.empty())
        return "[\"T\"]";
    std::string const op = parts.back();
    parts.pop_back();
    char const* space = op == "g" || op == "G" ? "G" : op == "rg" || op == "RG" ? "RGB" : op == "k" || op == "K" ? "CMYK" : nullptr;
    if (!space)
        return "[\"T\"]";
    std::string out = std::string("[\"") + space + "\"";
    for (std::string const& p : parts)
        out += "," + json_number(std::strtod(p.c_str(), nullptr));
    return out + "]";
}

void collect_scripts(Document const& doc, Object const& node_in, std::vector<std::pair<std::string, std::string>>& out, int depth)
{
    if (depth > k_max_depth || out.size() > 10000)
        return;
    Dict const node = doc.resolve(node_in).as_dict();
    Array const& names = doc.resolve(node.get("Names")).as_array();
    for (std::size_t i = 0; i + 1 < names.size(); i += 2) {
        Object const name = doc.resolve(names[i]);
        Dict const action = doc.resolve(names[i + 1]).as_dict();
        if (!doc.resolve(action.get("S")).is_name("JavaScript"))
            continue;
        out.emplace_back(name.is_string() ? name.as_string() : std::string(), script_text(doc, action.get("JS")));
    }
    for (Object const& kid : doc.resolve(node.get("Kids")).as_array())
        collect_scripts(doc, kid, out, depth + 1);
}

struct ModelField {
    FormField first;
    std::vector<FormField> widgets;
    std::vector<std::size_t> pages;
};

std::vector<ModelField> model_fields(Document const& doc)
{
    std::vector<ModelField> fields;
    std::map<Ref, std::size_t> by_ref;
    for (std::size_t page = 0; page < doc.page_count(); ++page) {
        for (FormField& widget : page_fields(doc, page)) {
            auto const it = by_ref.find(widget.field);
            if (it == by_ref.end()) {
                by_ref.emplace(widget.field, fields.size());
                ModelField field;
                field.first = widget;
                fields.push_back(std::move(field));
            }
            ModelField& field = fields[by_ref.at(widget.field)];
            field.widgets.push_back(std::move(widget));
            field.pages.push_back(page);
        }
    }
    return fields;
}

} // namespace

bool has_form_scripts(Document const& doc)
{
    Dict const& catalog = doc.catalog();
    Dict const names = doc.resolve(catalog.get("Names")).as_dict();
    if (names.has("JavaScript"))
        return true;
    if (std::string const open = actions_json(doc, catalog.get("OpenAction")); open.find("\"js\"") != std::string::npos)
        return true;
    if (catalog.has("AA"))
        return true;
    for (std::size_t page = 0; page < doc.page_count(); ++page) {
        for (Object const& annotation_ref : doc.resolve(doc.page(page).dict->get("Annots")).as_array()) {
            Dict const annotation = doc.resolve(annotation_ref).as_dict();
            if (!doc.resolve(annotation.get("Subtype")).is_name("Widget"))
                continue;
            Dict current = annotation;
            for (int depth = 0; depth < k_max_depth; ++depth) {
                if (current.has("AA"))
                    return true;
                Object const parent = doc.resolve(current.get("Parent"));
                if (!parent.is_dict())
                    break;
                current = parent.as_dict();
            }
            if (actions_json(doc, annotation.get("A")).find("\"js\"") != std::string::npos)
                return true;
        }
    }
    return false;
}

std::string form_script_model(Document const& doc, FormValues const& values, std::string const& file_name)
{
    std::string out = "{\"numPages\":" + std::to_string(doc.page_count()) + ",\"fileName\":" + json_string(file_name);
    out += ",\"info\":{";
    {
        Dict const info = doc.resolve(doc.trailer().get("Info")).as_dict();
        bool first = true;
        for (char const* key : { "Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate", "ModDate" }) {
            Object const v = doc.resolve(info.get(key));
            if (!v.is_string())
                continue;
            out += (first ? "" : ",") + json_string(key) + ":" + json_string(text_string_to_utf8(v.as_string()));
            first = false;
        }
    }
    out += "},\"documentScripts\":[";
    {
        std::vector<std::pair<std::string, std::string>> scripts;
        collect_scripts(doc, doc.resolve(doc.resolve(doc.catalog().get("Names")).as_dict().get("JavaScript")), scripts, 0);
        // In the order of their names, as the name tree keeps them.
        std::stable_sort(scripts.begin(), scripts.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < scripts.size(); ++i)
            out += (i ? "," : "") + json_string(scripts[i].second);
    }
    out += "],\"openAction\":" + actions_json(doc, doc.catalog().get("OpenAction"));
    out += ",\"documentActions\":{" + triggers_json(doc, doc.resolve(doc.catalog().get("AA")).as_dict(), { "WC", "WS", "DS", "WP", "DP" }) + "}";
    Dict const acroform = doc.resolve(doc.catalog().get("AcroForm")).as_dict();
    out += ",\"calculationOrder\":[";
    {
        bool first = true;
        for (Object const& item : doc.resolve(acroform.get("CO")).as_array()) {
            Dict const field = doc.resolve(item).as_dict();
            if (field.empty())
                continue;
            out += (first ? "" : ",") + json_string(full_name(doc, field));
            first = false;
        }
    }
    out += "],\"fields\":[";
    std::vector<ModelField> const fields = model_fields(doc);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        FormField const& f = fields[i].first;
        FormValue const* value = &f.value;
        if (auto const it = values.find(f.field); it != values.end())
            value = &it->second;
        Dict const terminal = doc.resolve(Object::ref(f.field)).as_dict();
        char const* type = "text";
        switch (f.kind) {
        case FormField::Kind::CheckBox: type = "checkbox"; break;
        case FormField::Kind::Radio: type = "radio"; break;
        case FormField::Kind::PushButton: type = "button"; break;
        case FormField::Kind::ComboBox: type = "combo"; break;
        case FormField::Kind::ListBox: type = "list"; break;
        case FormField::Kind::Signature: type = "signature"; break;
        default: break;
        }
        std::string value_json;
        std::string default_json;
        {
            // The default value (/DV), inherited as /V is.
            Object dv;
            Dict current = terminal;
            for (int depth = 0; depth < k_max_depth && dv.is_null(); ++depth) {
                if (Object const* found = current.find("DV"))
                    dv = doc.resolve(*found);
                Object const parent = doc.resolve(current.get("Parent"));
                if (!parent.is_dict())
                    break;
                current = parent.as_dict();
            }
            default_json = dv.is_string() ? json_string(text_string_to_utf8(dv.as_string())) : dv.is_name() ? json_string(dv.as_name())
                : (f.kind == FormField::Kind::CheckBox || f.kind == FormField::Kind::Radio) ? "\"Off\""
                                                                                             : "\"\"";
        }
        switch (f.kind) {
        case FormField::Kind::CheckBox:
        case FormField::Kind::Radio:
            value_json = json_string(value->state.empty() ? "Off" : value->state);
            break;
        case FormField::Kind::ListBox:
            if (value->selections.size() > 1) {
                value_json = "[";
                for (std::size_t k = 0; k < value->selections.size(); ++k)
                    value_json += (k ? "," : "") + json_string(value->selections[k]);
                value_json += "]";
            } else {
                value_json = json_string(value->selections.empty() ? value->text : value->selections[0]);
            }
            break;
        default:
            value_json = json_string(value->text);
        }
        int display = 0;
        if (value->hidden ? *value->hidden : f.hidden())
            display = 1;
        else if (f.annotation_flags & 32)
            display = 3;
        else if (!(f.annotation_flags & 4))
            display = 2;
        char const* const alignments[] = { "left", "center", "right" };
        std::string const style = f.border_style == "D" ? "dashed" : f.border_style == "B" ? "beveled" : f.border_style == "I" ? "inset"
            : f.border_style == "U"                                                                                         ? "underline"
                                                                                                                            : "solid";
        out += (i ? "," : "") + std::string("{\"name\":") + json_string(f.name) + ",\"type\":\"" + type + "\"";
        out += ",\"value\":" + value_json + ",\"defaultValue\":" + default_json;
        out += ",\"options\":[";
        for (std::size_t k = 0; k < f.options.size(); ++k)
            out += (k ? ",[" : "[") + json_string(f.options[k].first) + "," + json_string(f.options[k].second) + "]";
        out += "],\"readonly\":" + json_bool(f.read_only()) + ",\"required\":" + json_bool(f.required());
        out += ",\"multiline\":" + json_bool(f.multiline()) + ",\"password\":" + json_bool(f.password()) + ",\"comb\":" + json_bool(f.comb());
        out += ",\"charLimit\":" + std::to_string(f.max_len) + ",\"editable\":" + json_bool(f.editable_combo());
        out += ",\"multipleSelection\":" + json_bool(f.multi_select()) + ",\"doNotScroll\":" + json_bool(f.do_not_scroll());
        out += ",\"doNotSpellCheck\":" + json_bool(f.do_not_spell_check());
        out += ",\"alignment\":\"" + std::string(alignments[std::clamp(f.quadding, 0, 2)]) + "\"";
        out += ",\"textSize\":" + json_number(f.da.font_size) + ",\"textFont\":" + json_string(f.da.font_name);
        out += ",\"textColor\":" + color_json(value->text_color ? *value->text_color : f.da.color_operators);
        out += ",\"fillColor\":" + color_json(f.background_color) + ",\"strokeColor\":" + color_json(f.border_color);
        out += ",\"borderStyle\":\"" + style + "\",\"lineWidth\":" + json_number(f.border_width);
        out += ",\"display\":" + std::to_string(display) + ",\"caption\":" + json_string(f.caption);
        out += ",\"userName\":" + json_string(text_string_to_utf8(doc.resolve(terminal.get("TU")).as_string()));
        out += ",\"actions\":{" + triggers_json(doc, doc.resolve(terminal.get("AA")).as_dict(), { "K", "F", "V", "C" }) + "}";
        out += ",\"widgets\":[";
        for (std::size_t w = 0; w < fields[i].widgets.size(); ++w) {
            FormField const& widget = fields[i].widgets[w];
            Dict const annotation = doc.resolve(Object::ref(widget.widget)).as_dict();
            std::string triggers = triggers_json(doc, doc.resolve(annotation.get("AA")).as_dict(), { "D", "U", "E", "X", "Fo", "Bl" });
            std::string const activation = actions_json(doc, annotation.get("A"));
            if (activation != "[]")
                triggers += (triggers.empty() ? "" : ",") + std::string("\"A\":") + activation;
            // The rectangle as the API gives it: upper left, lower right.
            out += (w ? "," : "") + std::string("{\"page\":") + std::to_string(fields[i].pages[w]) + ",\"rect\":[" + json_number(widget.rect.x0) + ","
                + json_number(widget.rect.y1) + "," + json_number(widget.rect.x1) + "," + json_number(widget.rect.y0) + "]";
            out += ",\"exportValue\":" + json_string(widget.on_state.empty() ? "Yes" : widget.on_state);
            out += ",\"actions\":{" + triggers + "}}";
        }
        out += "]}";
    }
    return out + "]}";
}

void apply_script_changes(Document const& doc, std::string const& answer, FormValues& values)
{
    std::optional<JsonValue> const parsed = JsonValue::parse(answer);
    if (!parsed)
        return;
    JsonValue const* changes = parsed->get("changes");
    if (!changes || !changes->is_object() || changes->as_object().empty())
        return;
    std::vector<ModelField> const fields = model_fields(doc);
    for (auto const& [name, change] : changes->as_object()) {
        if (!change.is_object())
            continue;
        ModelField const* field = nullptr;
        for (ModelField const& candidate : fields) {
            if (candidate.first.name == name) {
                field = &candidate;
                break;
            }
        }
        if (!field)
            continue;
        FormField const& f = field->first;
        auto it = values.find(f.field);
        if (it == values.end())
            it = values.emplace(f.field, f.value).first;
        FormValue& value = it->second;
        value.by_script = true;
        if (JsonValue const* v = change.get("value")) {
            switch (f.kind) {
            case FormField::Kind::CheckBox:
            case FormField::Kind::Radio: {
                std::string const state = v->is_string() ? v->as_string() : std::string();
                value.state = "Off";
                for (FormField const& widget : field->widgets) {
                    if (!state.empty() && widget.on_state == state)
                        value.state = state;
                }
                break;
            }
            case FormField::Kind::ListBox:
                value.selections.clear();
                if (v->is_array()) {
                    for (JsonValue const& item : v->as_array()) {
                        if (item.is_string())
                            value.selections.push_back(item.as_string());
                    }
                } else if (v->is_string() && !v->as_string().empty()) {
                    value.selections.push_back(v->as_string());
                }
                value.text = value.selections.empty() ? std::string() : value.selections[0];
                break;
            default:
                if (v->is_string())
                    value.text = v->as_string();
            }
        }
        if (JsonValue const* display = change.get("display"); display && display->is_string()) {
            if (display->as_string() == value.text)
                value.formatted.reset();
            else
                value.formatted = display->as_string();
        }
        if (JsonValue const* hidden = change.get("hidden"); hidden && hidden->is_bool())
            value.hidden = hidden->as_bool();
        if (JsonValue const* color = change.get("textColorOperators"); color && color->is_string()) {
            if (color->as_string().empty())
                value.text_color.reset();
            else
                value.text_color = color->as_string();
        }
    }
}

// --- The sandbox ---

struct FormSandbox::State {
    std::string model;
    Limits limits;
    std::mutex mutex;
    std::condition_variable wake;
    struct Job {
        std::string request;
        std::promise<std::string> answer;
    };
    std::deque<Job> jobs;
    bool stopping = false;
    std::unique_ptr<platform::ScriptThread> thread;

    void run()
    {
        js::Interpreter interpreter;
        interpreter.set_stack_budget(platform::js_stack_budget_for(platform::current_thread_stack_bytes()));
        interpreter.heap().set_limit(limits.heap_bytes);
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
        interpreter.set_interrupt([&deadline] { return std::chrono::steady_clock::now() > deadline; }, 1000);
        std::vector<std::string> console;
        interpreter.on_console = [&console](std::string_view level, std::string_view message) {
            if (console.size() < 1000)
                console.push_back(std::string(level) + ": " + std::string(message));
        };
        auto const arm = [&] { deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.milliseconds); };
        // The model's own source first, then the model.
        bool ready = false;
        {
            arm();
            js::Outcome const prelude = interpreter.run_script(form_script_prelude(), "form-scripts");
            if (prelude.ok) {
                ready = call(interpreter, "__sashfold_form_load", model).has_value();
            } else {
                console.push_back("error: " + interpreter.describe(prelude.value));
            }
            interpreter.clear_termination();
        }
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty())
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            std::string answer;
            if (!ready || interpreter.out_of_memory()) {
                answer = "{\"dead\":true}";
            } else {
                console.clear();
                arm();
                std::optional<std::string> const result = call(interpreter, "__sashfold_form_dispatch", job.request);
                deadline = std::chrono::steady_clock::time_point::max();
                if (interpreter.terminated()) {
                    interpreter.clear_termination();
                    answer = interpreter.out_of_memory() ? "{\"dead\":true}" : "{\"terminated\":true}";
                } else if (!result) {
                    answer = "{\"error\":true}";
                } else {
                    answer = *result;
                }
                // What the engine itself said (an error no script caught)
                // goes with the answer, for the viewer's console.
                if (!console.empty() && answer.size() > 1 && answer.back() == '}') {
                    std::string lines = ",\"engineConsole\":[";
                    for (std::size_t i = 0; i < console.size(); ++i)
                        lines += (i ? "," : "") + json_string(console[i]);
                    answer.insert(answer.size() - 1, lines + "]");
                }
            }
            job.answer.set_value(std::move(answer));
            // Promise jobs a script queued run here, under the same limits.
            arm();
            interpreter.run_jobs([](js::Value const&) {});
            deadline = std::chrono::steady_clock::time_point::max();
            interpreter.clear_termination();
        }
    }

    // A global function of the prelude called with one string; its string
    // result, or nullopt when it threw (said on the console).
    static std::optional<std::string> call(js::Interpreter& interpreter, char const* name, std::string const& argument)
    {
        std::optional<js::Value> const function = interpreter.get(js::Value::object(interpreter.global()), name);
        if (!function)
            return std::nullopt;
        js::Interpreter::Roots roots(interpreter);
        js::Value const text = interpreter.root(js::Value::string(interpreter.string(argument)));
        js::Value const arguments[] = { text };
        std::optional<js::Value> const result = interpreter.call(*function, js::Value(), arguments);
        if (!result) {
            if (interpreter.has_exception()) {
                std::string const thrown = interpreter.describe(interpreter.take_exception());
                if (interpreter.on_console)
                    interpreter.on_console("error", thrown);
            }
            return std::nullopt;
        }
        if (!result->is_string())
            return std::string();
        return result->as_string()->to_utf8();
    }
};

FormSandbox::FormSandbox(std::string model, Limits limits)
    : m_state(std::make_shared<State>())
{
    m_state->model = std::move(model);
    m_state->limits = limits;
    std::shared_ptr<State> const state = m_state;
    m_state->thread = std::make_unique<platform::ScriptThread>([state] { state->run(); });
}

FormSandbox::~FormSandbox()
{
    {
        std::lock_guard const lock(m_state->mutex);
        m_state->stopping = true;
    }
    m_state->wake.notify_all();
    m_state->thread->join();
}

std::string FormSandbox::request(std::string const& json)
{
    std::future<std::string> answer;
    {
        std::lock_guard const lock(m_state->mutex);
        State::Job job;
        job.request = json;
        answer = job.answer.get_future();
        m_state->jobs.push_back(std::move(job));
    }
    m_state->wake.notify_all();
    // The limit runs from when the request starts; others may be ahead of it.
    auto const wait = std::chrono::milliseconds(m_state->limits.milliseconds * 4 + 2000);
    if (answer.wait_for(wait) != std::future_status::ready)
        return "{\"terminated\":true}";
    return answer.get();
}

}
