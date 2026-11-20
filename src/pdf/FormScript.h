#pragma once

// A PDF form's scripts (ISO 32000-2 §12.6.4.17 JavaScript actions, §12.6.3
// trigger events, §12.7.5 the field actions K, F, V and C), run as the
// browsers' viewers run them: in a realm of the script engine with the
// ECMAScript built-ins and the Acrobat forms object model
// (form_script_prelude) and nothing else — no page, no DOM, no network,
// no storage, no timers but the forms API's own. The realm lives on a
// thread of its own with a heap ceiling and a time limit on every request,
// so a hostile document can only hurt itself: a script that runs on is
// ended, one that takes too much memory ends every script of its
// document, and neither reaches the viewer.
//
// The viewer page asks in JSON (one request per event: open, keystroke,
// commit, focus, blur, mouse, timer, docEvent) and gets back what changed:
// field values, the text a format shows, visibility, colors, items, the
// alerts to show in a dialog of its own and the console lines.

#include "pdf/Document.h"
#include "pdf/Form.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::pdf {

// The object model's source, run in the realm before anything else.
std::string_view form_script_prelude();

// Whether the document carries JavaScript that its form runs: document
// scripts, an open action, document or field actions.
bool has_form_scripts(Document const& doc);

// The form as the realm is given it: every terminal field (name, kind,
// value with the viewer's over the file's, default, options, flags,
// colors, widgets with their pages, rectangles, on-states and actions),
// the field actions, the calculation order (/CO), the document's scripts
// (the /Names /JavaScript tree in name order), its open action and its
// /AA, the document information.
std::string form_script_model(Document const& doc, FormValues const& values, std::string const& file_name);

// The triggers a widget has actions for, of K, F, V, C (its field's) and
// D, U, E, X, Fo, Bl, A (its own): what the viewer must ask the scripts.
std::vector<std::string> widget_triggers(Document const& doc, FormField const& widget);

// What an answer changed, applied to the viewer's values: a value, the
// text shown, hidden or shown, the text color. Fields by their full name.
void apply_script_changes(Document const& doc, std::string const& answer, FormValues& values);

class FormSandbox {
public:
    struct Limits {
        std::size_t heap_bytes = 64u * 1024u * 1024u;
        int milliseconds = 1000; // a request's scripts, all of them
    };
    FormSandbox(std::string model, Limits limits);
    ~FormSandbox();
    FormSandbox(FormSandbox const&) = delete;
    FormSandbox& operator=(FormSandbox const&) = delete;

    // One request; its answer. Waits for the realm's thread, never longer
    // than the time limit and a little: a request cut short answers
    // {"terminated":true}, one after the heap's ceiling {"dead":true}.
    // Thread-safe; requests run one at a time in the order they came.
    std::string request(std::string const& json);

private:
    struct State;
    std::shared_ptr<State> m_state;
};

}
