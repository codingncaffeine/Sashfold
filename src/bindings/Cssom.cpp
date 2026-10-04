// The CSS Object Model's sheets and rules (CSSOM §6): document.styleSheets,
// a <style> or <link> element's sheet, CSSStyleSheet with its rules and
// insertRule/deleteRule, sheets a script constructs and adopts, and the
// CSSRule family over each kind of rule the engine reads. A sheet the
// object model has touched is written back as text for the cascade
// (dom::ScriptedSheet), which is what the style-in-script libraries rely
// on: they build a page's whole style through insertRule.

#include "bindings/Internal.h"

#include "core/Ascii.h"
#include "css/Parser.h"
#include "css/Selector.h"
#include "dom/Dom.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::bindings {

namespace {

std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// --- The rules, as the object model holds them ---------------------------------------

enum class RuleKind : std::uint8_t {
    Style,
    Import,
    Media,
    FontFace,
    Page,
    Keyframes,
    Keyframe,
    Namespace,
    CounterStyle,
    Supports,
    FontFeatureValues,
    Container,
    LayerBlock,
    LayerStatement,
    Scope,
    StartingStyle,
    Property,
    NestedDeclarations,
    FontPaletteValues,
};

// CSSRule.type (CSSOM §6.4.1): the legacy numbers, 0 for the kinds made
// after them.
int legacy_type(RuleKind kind)
{
    switch (kind) {
    case RuleKind::Style: return 1;
    case RuleKind::Import: return 3;
    case RuleKind::Media: return 4;
    case RuleKind::FontFace: return 5;
    case RuleKind::Page: return 6;
    case RuleKind::Keyframes: return 7;
    case RuleKind::Keyframe: return 8;
    case RuleKind::Namespace: return 10;
    case RuleKind::CounterStyle: return 11;
    case RuleKind::Supports: return 12;
    case RuleKind::FontFeatureValues: return 14;
    default: return 0;
    }
}

std::string_view interface_of(RuleKind kind)
{
    switch (kind) {
    case RuleKind::Style: return "CSSStyleRule";
    case RuleKind::Import: return "CSSImportRule";
    case RuleKind::Media: return "CSSMediaRule";
    case RuleKind::FontFace: return "CSSFontFaceRule";
    case RuleKind::Page: return "CSSPageRule";
    case RuleKind::Keyframes: return "CSSKeyframesRule";
    case RuleKind::Keyframe: return "CSSKeyframeRule";
    case RuleKind::Namespace: return "CSSNamespaceRule";
    case RuleKind::CounterStyle: return "CSSCounterStyleRule";
    case RuleKind::Supports: return "CSSSupportsRule";
    case RuleKind::FontFeatureValues: return "CSSFontFeatureValuesRule";
    case RuleKind::Container: return "CSSContainerRule";
    case RuleKind::LayerBlock: return "CSSLayerBlockRule";
    case RuleKind::LayerStatement: return "CSSLayerStatementRule";
    case RuleKind::Scope: return "CSSScopeRule";
    case RuleKind::StartingStyle: return "CSSStartingStyleRule";
    case RuleKind::Property: return "CSSPropertyRule";
    case RuleKind::NestedDeclarations: return "CSSNestedDeclarations";
    case RuleKind::FontPaletteValues: return "CSSFontPaletteValuesRule";
    }
    return "CSSRule";
}

// The kinds that hold declarations.
bool holds_declarations(RuleKind kind)
{
    switch (kind) {
    case RuleKind::Style:
    case RuleKind::FontFace:
    case RuleKind::Page:
    case RuleKind::Keyframe:
    case RuleKind::CounterStyle:
    case RuleKind::Property:
    case RuleKind::NestedDeclarations:
    case RuleKind::FontPaletteValues:
        return true;
    default:
        return false;
    }
}

struct CssomRule;
using RulePtr = std::shared_ptr<CssomRule>;

struct CssomRule {
    RuleKind kind = RuleKind::Style;
    std::string name; // the at-rule's name, lower case
    std::string prelude; // selectorText, the condition, the layer's name, the key...
    std::vector<css::Declaration> declarations;
    std::vector<RulePtr> rules;
    CssomRule* parent = nullptr;
    // The rule written as text, kept until it or something in it changes:
    // a sheet of thousands of rules is written by joining these.
    mutable std::string text;
    mutable bool written = false;
};

struct CssomSheet {
    std::vector<RulePtr> rules;
};

// Every rule above `rule` is written again too: their text holds its.
void unwritten(CssomRule* rule)
{
    for (; rule; rule = rule->parent)
        rule->written = false;
}

// A run of component values as text, its whitespace runs one space each,
// none at either end.
std::string tidy(std::vector<css::ComponentValue> const& values)
{
    std::string const raw = css_values_text(values);
    std::string out;
    out.reserve(raw.size());
    bool space = false;
    for (char const c : raw) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += c;
    }
    return out;
}

void write_rule(CssomRule const& rule, std::string& out, int depth);

void indent(std::string& out, int depth)
{
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

// A block of rules, one to a line under the head, as Blink and Gecko
// write them: "@media x {\n  a { }\n}".
void write_block(CssomRule const& rule, std::string const& head, std::string& out, int depth)
{
    out += head + " {\n";
    if (holds_declarations(rule.kind) && !rule.declarations.empty()) {
        indent(out, depth + 1);
        out += css_declarations_text(rule.declarations);
        out += '\n';
    }
    for (RulePtr const& child : rule.rules) {
        indent(out, depth + 1);
        write_rule(*child, out, depth + 1);
        out += '\n';
    }
    indent(out, depth);
    out += '}';
}

void write_rule(CssomRule const& rule, std::string& out, int depth)
{
    if (rule.written && depth == 0) {
        out += rule.text;
        return;
    }
    std::string text;
    auto const declarations_block = [&](std::string const& head) {
        text = head + " {";
        std::string const declarations = css_declarations_text(rule.declarations);
        text += declarations.empty() ? " }" : " " + declarations + " }";
    };
    switch (rule.kind) {
    case RuleKind::Style:
        if (rule.rules.empty())
            declarations_block(rule.prelude);
        else
            write_block(rule, rule.prelude, text, depth);
        break;
    case RuleKind::NestedDeclarations:
        text = css_declarations_text(rule.declarations);
        break;
    case RuleKind::Keyframe:
        declarations_block(rule.prelude);
        break;
    case RuleKind::FontFace:
    case RuleKind::Page:
    case RuleKind::CounterStyle:
    case RuleKind::Property:
    case RuleKind::FontPaletteValues:
        declarations_block("@" + rule.name + (rule.prelude.empty() ? "" : " " + rule.prelude));
        break;
    case RuleKind::Import:
    case RuleKind::Namespace:
    case RuleKind::LayerStatement:
        text = "@" + rule.name + " " + rule.prelude + ";";
        break;
    default:
        write_block(rule, "@" + rule.name + (rule.prelude.empty() ? "" : " " + rule.prelude), text, depth);
        break;
    }
    if (depth == 0) {
        rule.text = text;
        rule.written = true;
    }
    out += text;
}

std::string rule_text(CssomRule const& rule)
{
    std::string out;
    write_rule(rule, out, 0);
    return out;
}

std::string sheet_text(CssomSheet const& sheet)
{
    std::string out;
    for (RulePtr const& rule : sheet.rules) {
        write_rule(*rule, out, 0);
        out += '\n';
    }
    return out;
}

// The declarations an at-rule's block holds (the parser hands them over as
// runs of nested declarations).
std::vector<css::Declaration> block_declarations(css::AtRule const& at)
{
    std::vector<css::Declaration> declarations;
    for (css::Rule const& child : at.child_rules) {
        if (child.is_nested_declarations()) {
            for (css::Declaration const& declaration : child.nested_declarations().declarations)
                declarations.push_back(declaration);
        }
    }
    return declarations;
}

css::SelectorList const& any_parent()
{
    static css::SelectorList const list = *css::parse_selector_list(css::parse_component_value_list("*"));
    return list;
}

// A parsed rule as the object model holds it, or null for one it does not
// show: an invalid selector, an at-rule no engine knows, @charset.
// `in_style` says a style rule is around it, which makes a style rule's
// selector relative and a run of declarations a rule of its own.
RulePtr read_rule(css::Rule const& rule, CssomRule* parent, bool in_style)
{
    auto made = std::make_shared<CssomRule>();
    made->parent = parent;
    if (rule.is_nested_declarations()) {
        if (!in_style || rule.nested_declarations().declarations.empty())
            return nullptr;
        made->kind = RuleKind::NestedDeclarations;
        made->declarations = rule.nested_declarations().declarations;
        return made;
    }
    if (rule.is_qualified()) {
        css::QualifiedRule const& qualified = rule.qualified();
        if (parent && parent->kind == RuleKind::Keyframes) {
            made->kind = RuleKind::Keyframe;
            made->prelude = tidy(qualified.prelude);
            made->declarations = qualified.declarations;
            return made;
        }
        bool const valid = in_style ? css::parse_nested_selector_list(qualified.prelude, any_parent()).has_value()
                                    : css::parse_selector_list(qualified.prelude).has_value();
        if (!valid)
            return nullptr;
        made->kind = RuleKind::Style;
        made->prelude = tidy(qualified.prelude);
        made->declarations = qualified.declarations;
        for (css::Rule const& child : qualified.child_rules) {
            if (RulePtr read = read_rule(child, made.get(), true))
                made->rules.push_back(std::move(read));
        }
        return made;
    }
    css::AtRule const& at = rule.at_rule();
    made->name = lowered(at.name);
    made->prelude = tidy(at.prelude);
    std::string const& name = made->name;
    auto const children = [&] {
        for (css::Rule const& child : at.child_rules) {
            if (RulePtr read = read_rule(child, made.get(), in_style))
                made->rules.push_back(std::move(read));
        }
    };
    if (name == "media" || name == "supports" || name == "container" || name == "scope" || name == "starting-style") {
        if (!at.has_block)
            return nullptr;
        made->kind = name == "media" ? RuleKind::Media
            : name == "supports"     ? RuleKind::Supports
            : name == "container"    ? RuleKind::Container
            : name == "scope"        ? RuleKind::Scope
                                     : RuleKind::StartingStyle;
        children();
        return made;
    }
    if (name == "layer") {
        made->kind = at.has_block ? RuleKind::LayerBlock : RuleKind::LayerStatement;
        if (at.has_block)
            children();
        return made;
    }
    if (name == "keyframes" || name == "-webkit-keyframes") {
        if (!at.has_block)
            return nullptr;
        made->kind = RuleKind::Keyframes;
        for (css::Rule const& child : at.child_rules) {
            if (child.is_qualified()) {
                if (RulePtr read = read_rule(child, made.get(), false))
                    made->rules.push_back(std::move(read));
            }
        }
        return made;
    }
    struct Declared {
        std::string_view name;
        RuleKind kind;
    };
    static constexpr Declared declared[] = {
        { "font-face", RuleKind::FontFace },
        { "page", RuleKind::Page },
        { "counter-style", RuleKind::CounterStyle },
        { "property", RuleKind::Property },
        { "font-palette-values", RuleKind::FontPaletteValues },
    };
    for (Declared const& entry : declared) {
        if (name == entry.name) {
            if (!at.has_block)
                return nullptr;
            made->kind = entry.kind;
            made->declarations = block_declarations(at);
            return made;
        }
    }
    if (!in_style && !at.has_block && (name == "import" || name == "namespace")) {
        made->kind = name == "import" ? RuleKind::Import : RuleKind::Namespace;
        return made;
    }
    if (name == "font-feature-values" && at.has_block) {
        made->kind = RuleKind::FontFeatureValues;
        return made;
    }
    return nullptr;
}

std::vector<RulePtr> read_rules(std::string_view text)
{
    std::vector<RulePtr> rules;
    css::Stylesheet const sheet = css::parse_stylesheet(text);
    for (css::Rule const& rule : sheet.rules) {
        if (RulePtr read = read_rule(rule, nullptr, false))
            rules.push_back(std::move(read));
    }
    return rules;
}

// --- The objects ------------------------------------------------------------------------

class SheetObject;
class RuleObject;

// What a CSSStyleSheet stands for: a sheet of an element, or one a script
// made (CSSOM §6.1.2's constructed flag), whose rules the cascade reads
// once changed through here.
class SheetObject final : public js::Object {
public:
    SheetObject(js::Object* prototype, js::RealmRecord& the_record)
        : Object(prototype, Class::Host)
        , record(&the_record)
    {
    }
    js::RealmRecord* record;
    std::shared_ptr<CssomSheet> model = std::make_shared<CssomSheet>();
    std::shared_ptr<dom::ScriptedSheet> state = std::make_shared<dom::ScriptedSheet>();
    NodeWrapper* owner = nullptr; // the element, for a sheet of one
    bool constructed = false;
    std::string constructed_media;
    std::string href;
    js::Object* rule_list = nullptr; // cssRules, the same object every time
    std::unordered_map<CssomRule const*, RuleObject*> rule_objects;

    js::RealmRecord* home_realm() const override { return record; }
    void trace(js::Tracer& tracer) override;
};

class RuleObject final : public js::Object {
public:
    RuleObject(js::Object* prototype, js::RealmRecord& the_record, RulePtr the_rule, SheetObject* the_sheet)
        : Object(prototype, Class::Host)
        , record(&the_record)
        , rule(std::move(the_rule))
        , sheet(the_sheet)
    {
    }
    js::RealmRecord* record;
    RulePtr rule;
    SheetObject* sheet; // null once the rule is taken out of it
    js::Object* style = nullptr;
    js::Object* rule_list = nullptr;
    js::Object* media = nullptr;

    js::RealmRecord* home_realm() const override { return record; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(record);
        tracer.visit(sheet);
        tracer.visit(style);
        tracer.visit(rule_list);
        tracer.visit(media);
    }
};

void SheetObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(record);
    tracer.visit(owner);
    tracer.visit(rule_list);
    for (auto const& [rule, object] : rule_objects)
        tracer.visit(object);
}

// CSSRuleList: the live list of a sheet's rules or a rule's.
class RuleListObject final : public js::Object {
public:
    RuleListObject(js::Object* prototype, js::RealmRecord& the_record, SheetObject* the_sheet, RuleObject* the_rule)
        : Object(prototype, Class::Host)
        , record(&the_record)
        , sheet(the_sheet)
        , rule(the_rule)
    {
        mark_uncacheable();
    }
    js::RealmRecord* record;
    SheetObject* sheet; // the sheet's own list, or
    RuleObject* rule; // a rule's

    std::vector<RulePtr> const* rules() const
    {
        if (rule)
            return &rule->rule->rules;
        return sheet ? &sheet->model->rules : nullptr;
    }
    js::RealmRecord* home_realm() const override { return record; }
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const& key) const override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::vector<js::PropertyKey> own_keys() const override;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(record);
        tracer.visit(sheet);
        tracer.visit(rule);
    }
};

// MediaList over a sheet's media or a media rule's condition.
class MediaListObject final : public js::Object {
public:
    MediaListObject(js::Object* prototype, js::RealmRecord& the_record, SheetObject* the_sheet, RuleObject* the_rule)
        : Object(prototype, Class::Host)
        , record(&the_record)
        , sheet(the_sheet)
        , rule(the_rule)
    {
    }
    js::RealmRecord* record;
    SheetObject* sheet;
    RuleObject* rule;
    js::RealmRecord* home_realm() const override { return record; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(record);
        tracer.visit(sheet);
        tracer.visit(rule);
    }
};

// StyleSheetList: document.styleSheets, a snapshot of the sheets in order.
class SheetListObject final : public js::Object {
public:
    SheetListObject(js::Object* prototype, js::RealmRecord& the_record)
        : Object(prototype, Class::Host)
        , record(&the_record)
    {
        mark_uncacheable();
    }
    js::RealmRecord* record;
    std::vector<SheetObject*> sheets;
    js::RealmRecord* home_realm() const override { return record; }
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const& key) const override
    {
        if (key.is_index()) {
            if (key.as_index() < sheets.size())
                return js::PropertyDescriptor::data(js::Value::object(sheets[key.as_index()]), js::Enumerable);
            return std::nullopt;
        }
        return Object::get_own_property(key);
    }
    std::optional<js::Value> get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver) override
    {
        if (key.is_index())
            return key.as_index() < sheets.size() ? js::Value::object(sheets[key.as_index()]) : js::Value::undefined();
        return Object::get(interpreter, key, receiver);
    }
    std::vector<js::PropertyKey> own_keys() const override
    {
        std::vector<js::PropertyKey> keys;
        for (std::uint32_t i = 0; i < sheets.size(); ++i)
            keys.push_back(js::PropertyKey::index(i));
        for (js::PropertyKey const& key : Object::own_keys())
            keys.push_back(key);
        return keys;
    }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(record);
        for (SheetObject* sheet : sheets)
            tracer.visit(sheet);
    }
};

Realm::Internals& internals_from(js::RealmRecord* record)
{
    return static_cast<Realm*>(record->host_defined)->internals();
}


// A change through the object model: the sheet is written again for the
// cascade, and the page restyled.
void changed(SheetObject& sheet)
{
    std::shared_ptr<CssomSheet> const model = sheet.model;
    sheet.state->write = [model] { return sheet_text(*model); };
    sheet.state->touch();
    ++internals_from(sheet.record).mutations;
}

void changed(RuleObject& rule)
{
    unwritten(rule.rule.get());
    if (rule.sheet)
        changed(*rule.sheet);
}

RuleObject* rule_object(Realm::Internals& in, SheetObject* sheet, RulePtr const& rule)
{
    if (sheet) {
        if (auto const found = sheet->rule_objects.find(rule.get()); found != sheet->rule_objects.end())
            return found->second;
    }
    js::Interpreter::Roots const roots(in.interpreter);
    if (sheet)
        in.interpreter.root(js::Value::object(sheet));
    auto* object = in.interpreter.heap().allocate<RuleObject>(in.prototype(interface_of(rule->kind)), *in.realm_record, rule, sheet);
    if (sheet)
        sheet->rule_objects.emplace(rule.get(), object);
    return object;
}

std::optional<js::PropertyDescriptor> RuleListObject::get_own_property(js::PropertyKey const& key) const
{
    if (key.is_index()) {
        std::vector<RulePtr> const* list = rules();
        if (!list || key.as_index() >= list->size())
            return std::nullopt;
        js::Heap::NoCollect const guard(*heap());
        RuleObject* object = rule_object(internals_from(record), rule ? rule->sheet : sheet, (*list)[key.as_index()]);
        return js::PropertyDescriptor::data(js::Value::object(object), js::Enumerable);
    }
    return Object::get_own_property(key);
}

std::optional<js::Value> RuleListObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (key.is_index()) {
        std::vector<RulePtr> const* list = rules();
        if (!list || key.as_index() >= list->size())
            return js::Value::undefined();
        return js::Value::object(rule_object(internals_from(record), rule ? rule->sheet : sheet, (*list)[key.as_index()]));
    }
    return Object::get(interpreter, key, receiver);
}

std::vector<js::PropertyKey> RuleListObject::own_keys() const
{
    std::vector<js::PropertyKey> keys;
    if (std::vector<RulePtr> const* list = rules()) {
        for (std::uint32_t i = 0; i < list->size(); ++i)
            keys.push_back(js::PropertyKey::index(i));
    }
    for (js::PropertyKey const& key : Object::own_keys())
        keys.push_back(key);
    return keys;
}

js::Object* rule_list(Realm::Internals& in, SheetObject* sheet, RuleObject* rule)
{
    js::Object*& kept = rule ? rule->rule_list : sheet->rule_list;
    if (!kept) {
        js::Interpreter::Roots const roots(in.interpreter);
        in.interpreter.root(js::Value::object(rule ? static_cast<js::Object*>(rule) : sheet));
        kept = in.interpreter.heap().allocate<RuleListObject>(in.prototype("CSSRuleList"), *in.realm_record, rule ? nullptr : sheet, rule);
    }
    return kept;
}

// A rule taken out of its sheet keeps its object, with no sheet or parent.
void forget(SheetObject* sheet, CssomRule const& rule)
{
    if (!sheet)
        return;
    if (auto const found = sheet->rule_objects.find(&rule); found != sheet->rule_objects.end()) {
        found->second->sheet = nullptr;
        sheet->rule_objects.erase(found);
    }
    for (RulePtr const& child : rule.rules)
        forget(sheet, *child);
}

// insertRule (CSSOM §6.1.3, "insert a CSS rule"): one rule from the text,
// at the index, under the rules' own order: @import and @namespace first,
// nothing else before them. Throws as the object model says and answers
// nothing then.
std::optional<std::uint32_t> insert_rule(Realm::Internals& in, std::vector<RulePtr>& rules, CssomRule* parent,
    std::string const& text, double index_number, bool constructed)
{
    if (index_number < 0 || index_number > static_cast<double>(rules.size())) {
        in.throw_dom_exception("IndexSizeError", "The index provided (" + std::to_string(static_cast<long long>(index_number))
                + ") is larger than the maximum index (" + std::to_string(rules.size()) + ").");
        return std::nullopt;
    }
    auto const index = static_cast<std::size_t>(index_number);
    bool const in_style = parent && (parent->kind == RuleKind::Style || [&] {
        for (CssomRule const* above = parent; above; above = above->parent) {
            if (above->kind == RuleKind::Style)
                return true;
        }
        return false;
    }());
    RulePtr made;
    if (in_style) {
        // A rule nested in a style rule, or a run of declarations.
        css::Stylesheet const wrapped = css::parse_stylesheet("x{" + text + "}");
        if (wrapped.rules.size() == 1 && wrapped.rules[0].is_qualified()) {
            css::QualifiedRule const& outer = wrapped.rules[0].qualified();
            if (outer.child_rules.size() == 1 && outer.declarations.empty())
                made = read_rule(outer.child_rules[0], parent, true);
            else if (outer.child_rules.empty() && !outer.declarations.empty()) {
                made = std::make_shared<CssomRule>();
                made->kind = RuleKind::NestedDeclarations;
                made->parent = parent;
                made->declarations = outer.declarations;
            }
        }
    } else {
        css::Stylesheet const parsed = css::parse_stylesheet(text);
        if (parsed.rules.size() == 1)
            made = read_rule(parsed.rules[0], parent, false);
    }
    if (!made) {
        in.throw_dom_exception("SyntaxError", "Failed to parse the rule '" + text + "'.");
        return std::nullopt;
    }
    bool const early = made->kind == RuleKind::Import || made->kind == RuleKind::Namespace;
    if (made->kind == RuleKind::Import && constructed) {
        in.throw_dom_exception("SyntaxError", "Can't insert @import rules into a constructed stylesheet.");
        return std::nullopt;
    }
    if (!parent) {
        bool const later_early = index < rules.size() && (rules[index]->kind == RuleKind::Import || rules[index]->kind == RuleKind::Namespace);
        bool const earlier_late = index > 0 && rules[index - 1]->kind != RuleKind::Import && rules[index - 1]->kind != RuleKind::Namespace;
        if ((early && earlier_late) || (!early && later_early)) {
            in.throw_dom_exception("HierarchyRequestError", "Failed to insert the rule.");
            return std::nullopt;
        }
    } else if (early) {
        in.throw_dom_exception("HierarchyRequestError", "Failed to insert the rule.");
        return std::nullopt;
    }
    rules.insert(rules.begin() + static_cast<std::ptrdiff_t>(index), std::move(made));
    return static_cast<std::uint32_t>(index);
}

bool delete_rule(Realm::Internals& in, std::vector<RulePtr>& rules, SheetObject* sheet, double index_number)
{
    if (index_number < 0 || index_number >= static_cast<double>(rules.size())) {
        in.throw_dom_exception("IndexSizeError", "The index provided (" + std::to_string(static_cast<long long>(index_number))
                + ") is larger than the maximum index (" + std::to_string(rules.empty() ? 0 : rules.size() - 1) + ").");
        return false;
    }
    auto const index = static_cast<std::size_t>(index_number);
    forget(sheet, *rules[index]);
    rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// --- A sheet of an element -----------------------------------------------------------------

std::string element_sheet_text(dom::Element const& element)
{
    std::string text;
    for (dom::Node const* child : element.children()) {
        if (child->is_text())
            text += static_cast<dom::Text const*>(child)->data;
    }
    return text;
}

bool is_stylesheet_link(dom::Element const& element)
{
    if (!element.is_html("link"))
        return false;
    dom::Attr const* rel = element.find_attribute("rel");
    if (!rel)
        return false;
    bool stylesheet = false;
    std::string const lower = lowered(rel->value);
    std::size_t start = 0;
    while (start < lower.size()) {
        std::size_t const end = lower.find_first_of(" \t\n\r\f", start);
        std::string_view const token = std::string_view(lower).substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (token == "stylesheet")
            stylesheet = true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    // A link written disabled carries no sheet at all (HTML §4.6.7).
    return stylesheet && element.find_attribute("href") && !element.find_attribute("disabled");
}

bool carries_sheet(dom::Element const& element)
{
    if (!element.is_connected())
        return false;
    if (element.is_html("style") || element.is_svg("style")) {
        dom::Attr const* type = element.find_attribute("type");
        return !type || type->value.empty() || lowered(type->value) == "text/css";
    }
    return is_stylesheet_link(element);
}

SheetObject* sheet_object(Realm::Internals& in, dom::Element& element)
{
    if (!carries_sheet(element))
        return nullptr;
    NodeWrapper& wrapper = wrapper_for(in, element);
    bool const style = !element.is_html("link");
    if (js::Object* kept = wrapper.same_object("sheet")) {
        auto* sheet = static_cast<SheetObject*>(kept);
        // A <style> whose text was rewritten carries a new sheet: what the
        // object model did to the old one is gone with it.
        if (style) {
            std::string text = element_sheet_text(element);
            if (text != sheet->state->source) {
                for (RulePtr const& rule : sheet->model->rules)
                    forget(sheet, *rule);
                sheet->model->rules = read_rules(text);
                sheet->state->source = std::move(text);
                sheet->state->changed = false;
                ++sheet->state->version;
            }
        }
        return sheet;
    }
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&wrapper));
    auto* sheet = in.interpreter.heap().allocate<SheetObject>(in.prototype("CSSStyleSheet"), *in.realm_record);
    in.interpreter.root(js::Value::object(sheet));
    sheet->owner = &wrapper;
    if (style) {
        sheet->state->source = element_sheet_text(element);
        sheet->model->rules = read_rules(sheet->state->source);
    } else if (dom::Attr const* href = element.find_attribute("href")) {
        if (std::optional<net::Url> const url = net::parse_url(href->value, &in.url))
            sheet->href = url->serialize();
    }
    wrapper.keep_same_object("sheet", sheet);
    dom::Document& document = element.document();
    if (!document.scripted_sheet(element))
        document.scripted_sheets.emplace_back(&element, sheet->state);
    return sheet;
}

void collect_sheets(Realm::Internals& in, dom::Node& node, std::vector<SheetObject*>& out)
{
    if (node.is_element()) {
        auto& element = static_cast<dom::Element&>(node);
        if (element.is_html("style") || element.is_svg("style") || element.is_html("link")) {
            if (SheetObject* sheet = sheet_object(in, element))
                out.push_back(sheet);
            return;
        }
    }
    for (dom::Node* child : node.children())
        collect_sheets(in, *child, out);
}

// --- this checks ------------------------------------------------------------------------------

std::optional<SheetObject*> this_sheet(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* sheet = dynamic_cast<SheetObject*>(this_value.as_object()))
            return sheet;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

std::optional<RuleObject*> this_rule(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* rule = dynamic_cast<RuleObject*>(this_value.as_object()))
            return rule;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

// A sheet whose rules a script may not read: a link's from another origin
// (CSSOM §6.1.3), which this engine has not read here at all.
bool rules_readable(SheetObject const& sheet)
{
    return sheet.href.empty() || sheet.constructed;
}

// The declarations of a rule, for its CSSStyleDeclaration.
class RuleDeclarations final : public DeclarationStore {
public:
    RuleDeclarations(RulePtr the_rule, RuleObject* the_object)
        : rule(std::move(the_rule))
        , object(the_object)
    {
    }
    std::vector<css::Declaration> read() const override { return rule->declarations; }
    void write(std::vector<css::Declaration> declarations) override
    {
        rule->declarations = std::move(declarations);
        changed(*object);
    }
    RulePtr rule;
    RuleObject* object; // kept alive by the declaration's owner_rule
};

std::string media_text(MediaListObject const& list)
{
    if (list.rule)
        return list.rule->rule->prelude;
    if (!list.sheet)
        return {};
    if (list.sheet->constructed)
        return list.sheet->constructed_media;
    if (dom::Element const* owner = list.sheet->owner && !list.sheet->owner->detached() ? static_cast<dom::Element const*>(&list.sheet->owner->node()) : nullptr) {
        if (dom::Attr const* media = owner->find_attribute("media"))
            return media->value;
    }
    return {};
}

void set_media_text(Realm::Internals& in, MediaListObject& list, std::string text)
{
    if (list.rule) {
        list.rule->rule->prelude = std::move(text);
        changed(*list.rule);
        return;
    }
    if (!list.sheet)
        return;
    if (list.sheet->constructed) {
        list.sheet->constructed_media = std::move(text);
        changed(*list.sheet);
        return;
    }
    if (list.sheet->owner && !list.sheet->owner->detached())
        set_attribute(in, static_cast<dom::Element&>(list.sheet->owner->node()), "media", std::move(text));
}

std::vector<std::string> media_items(std::string const& text)
{
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t const comma = text.find(',', start);
        std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::size_t const first = item.find_first_not_of(" \t\n\r\f");
        std::size_t const last = item.find_last_not_of(" \t\n\r\f");
        if (first != std::string::npos)
            items.push_back(item.substr(first, last - first + 1));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return items;
}

js::Object* media_list(Realm::Internals& in, SheetObject* sheet, RuleObject* rule)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(rule ? static_cast<js::Object*>(rule) : sheet));
    return in.interpreter.heap().allocate<MediaListObject>(in.prototype("MediaList"), *in.realm_record, sheet, rule);
}

std::optional<MediaListObject*> this_media(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* list = dynamic_cast<MediaListObject*>(this_value.as_object()))
            return list;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

// replace()/replaceSync() (CSSOM §6.1.3): a constructed sheet's rules from
// text, its @import rules left out.
void replace_rules(SheetObject& sheet, std::string const& text)
{
    for (RulePtr const& rule : sheet.model->rules)
        forget(&sheet, *rule);
    std::vector<RulePtr> rules = read_rules(text);
    std::erase_if(rules, [](RulePtr const& rule) { return rule->kind == RuleKind::Import; });
    sheet.model->rules = std::move(rules);
    changed(sheet);
}

} // namespace

// --- Entry points -------------------------------------------------------------------------------

js::Value style_sheet_of(Realm::Internals& in, dom::Element& element)
{
    SheetObject* sheet = sheet_object(in, element);
    return sheet ? js::Value::object(sheet) : js::Value::null();
}

js::Value style_sheet_list(Realm::Internals& in, dom::Document& document)
{
    std::vector<SheetObject*> sheets;
    collect_sheets(in, document, sheets);
    js::Interpreter::Roots const roots(in.interpreter);
    for (SheetObject* sheet : sheets)
        in.interpreter.root(js::Value::object(sheet));
    auto* list = in.interpreter.heap().allocate<SheetListObject>(in.prototype("StyleSheetList"), *in.realm_record);
    list->sheets = std::move(sheets);
    return js::Value::object(list);
}

void install_cssom(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Value const array_values = *interpreter.get(js::Value::object(interpreter.intrinsics().array_prototype),
        js::PropertyKey::symbol(interpreter.atoms().symbol_iterator));

    // StyleSheet and CSSStyleSheet (CSSOM §6.1.1, §6.1.2).
    js::Object* style_sheet = define_interface(in, "StyleSheet", nullptr);
    js::Object* css_style_sheet = define_interface(in, "CSSStyleSheet", style_sheet,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const options = js::argument(args, 0);
            if (!options.is_undefined() && !options.is_null() && !options.is_object())
                return interp.throw_type_error("Failed to construct 'CSSStyleSheet': The provided value is not of type 'CSSStyleSheetInit'.");
            std::string media;
            bool disabled = false;
            if (options.is_object()) {
                js::Interpreter::Roots const roots(interp);
                interp.root(options);
                std::optional<js::Value> const given_media = interp.get(options, interp.key("media"));
                if (!given_media)
                    return std::nullopt;
                if (!given_media->is_undefined()) {
                    if (given_media->is_object()) {
                        if (auto* list = dynamic_cast<MediaListObject*>(given_media->as_object()))
                            media = media_text(*list);
                    } else {
                        std::optional<std::string> text = internals.to_utf8(*given_media);
                        if (!text)
                            return std::nullopt;
                        media = std::move(*text);
                    }
                }
                std::optional<js::Value> const given_disabled = interp.get(options, interp.key("disabled"));
                if (!given_disabled)
                    return std::nullopt;
                disabled = interp.to_boolean(*given_disabled);
            }
            auto* sheet = interp.heap().allocate<SheetObject>(internals.prototype("CSSStyleSheet"), *internals.realm_record);
            sheet->constructed = true;
            sheet->constructed_media = std::move(media);
            sheet->state->disabled = disabled;
            sheet->state->changed = true;
            return js::Value::object(sheet);
        },
        0);

    define_getter(in, *style_sheet, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_sheet(interp, this_value))
            return std::nullopt;
        return internals_of(interp).string("text/css");
    });
    define_getter(in, *style_sheet, "href", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        return (*sheet)->href.empty() ? js::Value::null() : internals_of(interp).string((*sheet)->href);
    });
    define_getter(in, *style_sheet, "ownerNode", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        return (*sheet)->owner ? js::Value::object((*sheet)->owner) : js::Value::null();
    });
    define_getter(in, *style_sheet, "parentStyleSheet", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_sheet(interp, this_value))
            return std::nullopt;
        return js::Value::null();
    });
    define_getter(in, *style_sheet, "title", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        if ((*sheet)->owner && !(*sheet)->owner->detached()) {
            if (dom::Attr const* title = static_cast<dom::Element&>((*sheet)->owner->node()).find_attribute("title"); title && !title->value.empty())
                return internals_of(interp).string(title->value);
        }
        return js::Value::null();
    });
    define_getter(in, *style_sheet, "media", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        return js::Value::object(media_list(internals_of(interp), *sheet, nullptr));
    });
    define_getter(
        in, *style_sheet, "disabled",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
            if (!sheet)
                return std::nullopt;
            return js::Value::boolean((*sheet)->state->disabled);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
            if (!sheet)
                return std::nullopt;
            bool const disabled = interp.to_boolean(js::argument(args, 0));
            if (disabled != (*sheet)->state->disabled) {
                (*sheet)->state->disabled = disabled;
                ++(*sheet)->state->version;
                ++internals_of(interp).mutations;
            }
            return js::Value::undefined();
        });

    define_getter(in, *css_style_sheet, "ownerRule", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_sheet(interp, this_value))
            return std::nullopt;
        return js::Value::null();
    });
    for (std::string_view const name : { "cssRules", "rules" }) {
        define_getter(in, *css_style_sheet, name, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
            if (!sheet)
                return std::nullopt;
            if (!rules_readable(**sheet))
                return internals_of(interp).throw_dom_exception("SecurityError", "Failed to read the 'cssRules' property from 'CSSStyleSheet': Cannot access rules");
            return js::Value::object(rule_list(internals_of(interp), *sheet, nullptr));
        });
    }
    define_operation(interpreter, *css_style_sheet, "insertRule", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        std::optional<double> index = 0.0;
        if (args.size() > 1)
            index = interp.to_number(args[1]);
        if (!index)
            return std::nullopt;
        if (!rules_readable(**sheet))
            return internals.throw_dom_exception("SecurityError", "Failed to execute 'insertRule' on 'CSSStyleSheet': Cannot access StyleSheet to insertRule");
        std::optional<std::uint32_t> const at = insert_rule(internals, (*sheet)->model->rules, nullptr, *text,
            std::isnan(*index) ? 0.0 : std::trunc(*index), (*sheet)->constructed);
        if (!at)
            return std::nullopt;
        changed(**sheet);
        return js::Value::number(*at);
    });
    define_operation(interpreter, *css_style_sheet, "deleteRule", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        if (!delete_rule(internals_of(interp), (*sheet)->model->rules, *sheet, std::isnan(*index) ? 0.0 : std::trunc(*index)))
            return std::nullopt;
        changed(**sheet);
        return js::Value::undefined();
    });
    // The legacy pair (CSSOM §6.1.3.1): addRule answers -1 whatever it does.
    define_operation(interpreter, *css_style_sheet, "addRule", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const selector = js::argument(args, 0).is_undefined() ? std::optional<std::string>("undefined") : internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> const block = js::argument(args, 1).is_undefined() ? std::optional<std::string>("undefined") : internals.to_utf8(js::argument(args, 1));
        if (!selector || !block)
            return std::nullopt;
        double index = static_cast<double>((*sheet)->model->rules.size());
        if (!js::argument(args, 2).is_undefined()) {
            std::optional<double> const given = interp.to_number(args[2]);
            if (!given)
                return std::nullopt;
            index = std::isnan(*given) ? 0.0 : std::trunc(*given);
        }
        std::string const text = *selector + " { " + (block->empty() ? "" : *block + " ") + "}";
        if (!insert_rule(internals, (*sheet)->model->rules, nullptr, text, index, (*sheet)->constructed))
            return std::nullopt;
        changed(**sheet);
        return js::Value::number(-1);
    });
    define_operation(interpreter, *css_style_sheet, "removeRule", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        if (!delete_rule(internals_of(interp), (*sheet)->model->rules, *sheet, std::isnan(*index) ? 0.0 : std::trunc(*index)))
            return std::nullopt;
        changed(**sheet);
        return js::Value::undefined();
    });
    define_operation(interpreter, *css_style_sheet, "replaceSync", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        if (!(*sheet)->constructed)
            return internals.throw_dom_exception("NotAllowedError", "Failed to execute 'replaceSync' on 'CSSStyleSheet': Can't call replaceSync on non-constructed CSSStyleSheets.");
        replace_rules(**sheet, *text);
        return js::Value::undefined();
    });
    define_promise_operation(interpreter, *css_style_sheet, "replace", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SheetObject*> const sheet = this_sheet(interp, this_value);
        if (!sheet)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        if (!(*sheet)->constructed)
            return internals.throw_dom_exception("NotAllowedError", "Failed to execute 'replace' on 'CSSStyleSheet': Can't call replace on non-constructed CSSStyleSheets.");
        replace_rules(**sheet, *text);
        return resolved_promise(interp, this_value);
    });

    // StyleSheetList and CSSRuleList: item() and length, indexed, iterable
    // as WebIDL makes a list with an indexed getter.
    js::Object* sheet_list = define_interface(in, "StyleSheetList", nullptr);
    define_getter(in, *sheet_list, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* list = this_value.is_object() ? dynamic_cast<SheetListObject*>(this_value.as_object()) : nullptr;
        if (!list)
            return interp.throw_type_error("Illegal invocation");
        return js::Value::number(static_cast<double>(list->sheets.size()));
    });
    define_operation(interpreter, *sheet_list, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* list = this_value.is_object() ? dynamic_cast<SheetListObject*>(this_value.as_object()) : nullptr;
        if (!list)
            return interp.throw_type_error("Illegal invocation");
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        if (*index < 0 || *index >= static_cast<double>(list->sheets.size()) || std::isnan(*index))
            return js::Value::null();
        return js::Value::object(list->sheets[static_cast<std::size_t>(*index)]);
    });
    sheet_list->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), array_values, js::builtin_attributes);

    js::Object* css_rule_list = define_interface(in, "CSSRuleList", nullptr);
    define_getter(in, *css_rule_list, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* list = this_value.is_object() ? dynamic_cast<RuleListObject*>(this_value.as_object()) : nullptr;
        if (!list)
            return interp.throw_type_error("Illegal invocation");
        std::vector<RulePtr> const* rules = list->rules();
        return js::Value::number(rules ? static_cast<double>(rules->size()) : 0);
    });
    define_operation(interpreter, *css_rule_list, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* list = this_value.is_object() ? dynamic_cast<RuleListObject*>(this_value.as_object()) : nullptr;
        if (!list)
            return interp.throw_type_error("Illegal invocation");
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        std::vector<RulePtr> const* rules = list->rules();
        if (!rules || std::isnan(*index) || *index < 0 || *index >= static_cast<double>(rules->size()))
            return js::Value::null();
        return js::Value::object(rule_object(internals_of(interp), list->rule ? list->rule->sheet : list->sheet, (*rules)[static_cast<std::size_t>(*index)]));
    });
    css_rule_list->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), array_values, js::builtin_attributes);

    // MediaList (CSSOM §4.1).
    js::Object* media = define_interface(in, "MediaList", nullptr);
    define_getter(
        in, *media, "mediaText",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MediaListObject*> const list = this_media(interp, this_value);
            if (!list)
                return std::nullopt;
            return internals_of(interp).string(media_text(**list));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<MediaListObject*> const list = this_media(interp, this_value);
            if (!list)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> text = js::argument(args, 0).is_null() ? std::optional<std::string>("") : internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            set_media_text(internals, **list, std::move(*text));
            return js::Value::undefined();
        });
    define_getter(in, *media, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MediaListObject*> const list = this_media(interp, this_value);
        if (!list)
            return std::nullopt;
        return js::Value::number(static_cast<double>(media_items(media_text(**list)).size()));
    });
    define_operation(interpreter, *media, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<MediaListObject*> const list = this_media(interp, this_value);
        if (!list)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        std::vector<std::string> const items = media_items(media_text(**list));
        if (std::isnan(*index) || *index < 0 || *index >= static_cast<double>(items.size()))
            return js::Value::null();
        return internals_of(interp).string(items[static_cast<std::size_t>(*index)]);
    });
    define_operation(interpreter, *media, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MediaListObject*> const list = this_media(interp, this_value);
        if (!list)
            return std::nullopt;
        return internals_of(interp).string(media_text(**list));
    });
    define_operation(interpreter, *media, "appendMedium", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<MediaListObject*> const list = this_media(interp, this_value);
        if (!list)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const medium = internals.to_utf8(js::argument(args, 0));
        if (!medium)
            return std::nullopt;
        std::vector<std::string> items = media_items(media_text(**list));
        if (std::find(items.begin(), items.end(), *medium) == items.end()) {
            items.push_back(*medium);
            std::string text;
            for (std::string const& item : items)
                text += (text.empty() ? "" : ", ") + item;
            set_media_text(internals, **list, std::move(text));
        }
        return js::Value::undefined();
    });
    define_operation(interpreter, *media, "deleteMedium", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<MediaListObject*> const list = this_media(interp, this_value);
        if (!list)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const medium = internals.to_utf8(js::argument(args, 0));
        if (!medium)
            return std::nullopt;
        std::vector<std::string> items = media_items(media_text(**list));
        auto const found = std::find(items.begin(), items.end(), *medium);
        if (found == items.end())
            return internals.throw_dom_exception("NotFoundError", "Failed to execute 'deleteMedium' on 'MediaList': Failed to delete '" + *medium + "'.");
        items.erase(found);
        std::string text;
        for (std::string const& item : items)
            text += (text.empty() ? "" : ", ") + item;
        set_media_text(internals, **list, std::move(text));
        return js::Value::undefined();
    });

    // CSSRule (CSSOM §6.4) and its kinds.
    js::Object* css_rule = define_interface(in, "CSSRule", nullptr);
    struct Constant {
        std::string_view name;
        int value;
    };
    static constexpr Constant constants[] = {
        { "STYLE_RULE", 1 },
        { "CHARSET_RULE", 2 },
        { "IMPORT_RULE", 3 },
        { "MEDIA_RULE", 4 },
        { "FONT_FACE_RULE", 5 },
        { "PAGE_RULE", 6 },
        { "KEYFRAMES_RULE", 7 },
        { "KEYFRAME_RULE", 8 },
        { "MARGIN_RULE", 9 },
        { "NAMESPACE_RULE", 10 },
        { "COUNTER_STYLE_RULE", 11 },
        { "SUPPORTS_RULE", 12 },
        { "FONT_FEATURE_VALUES_RULE", 14 },
    };
    if (std::optional<js::Value> const constructor = css_rule->get(interpreter, interpreter.key("constructor"), js::Value::object(css_rule));
        constructor && constructor->is_object()) {
        for (Constant const& constant : constants) {
            constructor->as_object()->put(interpreter.key(constant.name), js::Value::number(constant.value), 0);
            css_rule->put(interpreter.key(constant.name), js::Value::number(constant.value), 0);
        }
    }
    define_getter(
        in, *css_rule, "cssText",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<RuleObject*> const rule = this_rule(interp, this_value);
            if (!rule)
                return std::nullopt;
            return internals_of(interp).string(rule_text(*(*rule)->rule));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            // Setting it does nothing (CSSOM §6.4.1).
            if (!this_rule(interp, this_value))
                return std::nullopt;
            return js::Value::undefined();
        });
    define_getter(in, *css_rule, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        return js::Value::number(legacy_type((*rule)->rule->kind));
    });
    define_getter(in, *css_rule, "parentRule", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        CssomRule* const parent = (*rule)->rule->parent;
        if (!parent || !(*rule)->sheet)
            return js::Value::null();
        // The parent's object: the rule is reached through it.
        for (auto const& [held, object] : (*rule)->sheet->rule_objects) {
            if (held == parent)
                return js::Value::object(object);
        }
        return js::Value::null();
    });
    define_getter(in, *css_rule, "parentStyleSheet", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        return (*rule)->sheet ? js::Value::object((*rule)->sheet) : js::Value::null();
    });

    // The rule kinds that hold rules: cssRules, insertRule, deleteRule.
    auto const grouping = [&](js::Object& prototype) {
        define_getter(in, prototype, "cssRules", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<RuleObject*> const rule = this_rule(interp, this_value);
            if (!rule)
                return std::nullopt;
            return js::Value::object(rule_list(internals_of(interp), (*rule)->sheet, *rule));
        });
        define_operation(interpreter, prototype, "insertRule", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<RuleObject*> const rule = this_rule(interp, this_value);
            if (!rule)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            std::optional<double> index = 0.0;
            if (args.size() > 1)
                index = interp.to_number(args[1]);
            if (!index)
                return std::nullopt;
            CssomRule& held = *(*rule)->rule;
            std::optional<std::uint32_t> const at = insert_rule(internals, held.rules, &held, *text,
                std::isnan(*index) ? 0.0 : std::trunc(*index), false);
            if (!at)
                return std::nullopt;
            changed(**rule);
            return js::Value::number(*at);
        });
        define_operation(interpreter, prototype, "deleteRule", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<RuleObject*> const rule = this_rule(interp, this_value);
            if (!rule)
                return std::nullopt;
            std::optional<double> const index = interp.to_number(js::argument(args, 0));
            if (!index)
                return std::nullopt;
            if (!delete_rule(internals_of(interp), (*rule)->rule->rules, (*rule)->sheet, std::isnan(*index) ? 0.0 : std::trunc(*index)))
                return std::nullopt;
            changed(**rule);
            return js::Value::undefined();
        });
    };
    // The rule kinds that hold declarations: style.
    auto const declared = [&](js::Object& prototype) {
        define_getter(
            in, prototype, "style",
            [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<RuleObject*> const rule = this_rule(interp, this_value);
                if (!rule)
                    return std::nullopt;
                if (!(*rule)->style) {
                    auto store = std::make_shared<RuleDeclarations>((*rule)->rule, *rule);
                    js::Value const style = make_rule_style_declaration(internals_of(interp), std::move(store), *rule);
                    (*rule)->style = style.as_object();
                }
                return js::Value::object((*rule)->style);
            },
            [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                // [PutForwards=cssText]
                std::optional<RuleObject*> const rule = this_rule(interp, this_value);
                if (!rule)
                    return std::nullopt;
                std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                (*rule)->rule->declarations = css::parse_declaration_list(*text);
                changed(**rule);
                return js::Value::undefined();
            });
    };
    // A text attribute of a rule's prelude, read-only or written back.
    auto const prelude_text = [&](js::Object& prototype, std::string_view name, bool writable) {
        js::NativeFunction::Callback setter;
        if (writable) {
            setter = [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<RuleObject*> const rule = this_rule(interp, this_value);
                if (!rule)
                    return std::nullopt;
                std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                CssomRule& held = *(*rule)->rule;
                std::vector<css::ComponentValue> const values = css::parse_component_value_list(*text);
                if (held.kind == RuleKind::Style) {
                    // A selector that does not parse leaves the rule as it was.
                    bool nested = false;
                    for (CssomRule const* above = held.parent; above; above = above->parent)
                        nested = nested || above->kind == RuleKind::Style;
                    bool const valid = nested ? css::parse_nested_selector_list(values, any_parent()).has_value()
                                              : css::parse_selector_list(values).has_value();
                    if (!valid)
                        return js::Value::undefined();
                }
                held.prelude = tidy(values);
                changed(**rule);
                return js::Value::undefined();
            };
        }
        define_getter(
            in, prototype, name,
            [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<RuleObject*> const rule = this_rule(interp, this_value);
                if (!rule)
                    return std::nullopt;
                return internals_of(interp).string((*rule)->rule->prelude);
            },
            std::move(setter));
    };

    js::Object* grouping_rule = define_interface(in, "CSSGroupingRule", css_rule);
    grouping(*grouping_rule);
    js::Object* condition_rule = define_interface(in, "CSSConditionRule", grouping_rule);
    prelude_text(*condition_rule, "conditionText", false);

    js::Object* style_rule = define_interface(in, "CSSStyleRule", grouping_rule);
    prelude_text(*style_rule, "selectorText", true);
    declared(*style_rule);

    js::Object* media_rule = define_interface(in, "CSSMediaRule", condition_rule);
    define_getter(in, *media_rule, "media", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        if (!(*rule)->media)
            (*rule)->media = media_list(internals_of(interp), (*rule)->sheet, *rule);
        return js::Value::object((*rule)->media);
    });
    define_getter(in, *media_rule, "matches", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_rule(interp, this_value))
            return std::nullopt;
        return js::Value::boolean(true);
    });
    define_interface(in, "CSSSupportsRule", condition_rule);
    js::Object* container_rule = define_interface(in, "CSSContainerRule", condition_rule);
    prelude_text(*container_rule, "containerQuery", false);
    define_getter(in, *container_rule, "containerName", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        std::string const& prelude = (*rule)->rule->prelude;
        std::size_t const end = prelude.find_first_of(" (");
        std::string name = end == 0 ? std::string() : prelude.substr(0, end);
        if (name == "not" || name == "style" || name == "scroll-state")
            name.clear();
        return internals_of(interp).string(name);
    });
    js::Object* layer_block = define_interface(in, "CSSLayerBlockRule", grouping_rule);
    prelude_text(*layer_block, "name", false);
    js::Object* layer_statement = define_interface(in, "CSSLayerStatementRule", css_rule);
    define_getter(in, *layer_statement, "nameList", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        js::Object* array = interp.new_array();
        std::uint32_t index = 0;
        for (std::string const& name : media_items((*rule)->rule->prelude))
            array->put(js::PropertyKey::index(index++), internals_of(interp).string(name));
        return js::Value::object(array);
    });
    js::Object* scope_rule = define_interface(in, "CSSScopeRule", grouping_rule);
    for (std::string_view const end : { "start", "end" }) {
        bool const start = end == "start";
        define_getter(in, *scope_rule, end, [start](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<RuleObject*> const rule = this_rule(interp, this_value);
            if (!rule)
                return std::nullopt;
            // "(.a) to (.b)": the parenthesized selectors on each side.
            std::string const& prelude = (*rule)->rule->prelude;
            std::size_t const to = prelude.find(") to (");
            std::string part = start ? prelude.substr(0, to == std::string::npos ? prelude.size() : to + 1)
                                     : (to == std::string::npos ? std::string() : prelude.substr(to + 5));
            if (part.size() >= 2 && part.front() == '(' && part.back() == ')')
                return internals_of(interp).string(part.substr(1, part.size() - 2));
            return js::Value::null();
        });
    }
    define_interface(in, "CSSStartingStyleRule", grouping_rule);

    js::Object* import_rule = define_interface(in, "CSSImportRule", css_rule);
    define_getter(in, *import_rule, "href", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        std::vector<css::ComponentValue> const values = css::parse_component_value_list((*rule)->rule->prelude);
        for (css::ComponentValue const& value : values) {
            if (value.is_token(css::Token::Type::String) || value.is_token(css::Token::Type::Url))
                return internals_of(interp).string(value.token().value);
            if (value.is_function() && lowered(value.function().name) == "url") {
                for (css::ComponentValue const& inner : value.function().values) {
                    if (inner.is_token(css::Token::Type::String))
                        return internals_of(interp).string(inner.token().value);
                }
            }
        }
        return internals_of(interp).string("");
    });
    define_getter(in, *import_rule, "styleSheet", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_rule(interp, this_value))
            return std::nullopt;
        return js::Value::null();
    });

    js::Object* font_face_rule = define_interface(in, "CSSFontFaceRule", css_rule);
    declared(*font_face_rule);
    js::Object* page_rule = define_interface(in, "CSSPageRule", grouping_rule);
    prelude_text(*page_rule, "selectorText", true);
    declared(*page_rule);
    js::Object* keyframes_rule = define_interface(in, "CSSKeyframesRule", css_rule);
    prelude_text(*keyframes_rule, "name", true);
    grouping(*keyframes_rule);
    define_getter(in, *keyframes_rule, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*rule)->rule->rules.size()));
    });
    define_operation(interpreter, *keyframes_rule, "appendRule", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        CssomRule& held = *(*rule)->rule;
        css::Stylesheet const parsed = css::parse_stylesheet("@keyframes x { " + *text + " }");
        if (parsed.rules.size() == 1 && parsed.rules[0].is_at_rule()) {
            for (css::Rule const& child : parsed.rules[0].at_rule().child_rules) {
                if (child.is_qualified()) {
                    if (RulePtr read = read_rule(child, &held, false))
                        held.rules.push_back(std::move(read));
                }
            }
        }
        changed(**rule);
        return js::Value::undefined();
    });
    define_operation(interpreter, *keyframes_rule, "findRule", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        std::optional<std::string> const key = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!key)
            return std::nullopt;
        std::vector<RulePtr> const& rules = (*rule)->rule->rules;
        for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
            if ((*it)->prelude == *key)
                return js::Value::object(rule_object(internals_of(interp), (*rule)->sheet, *it));
        }
        return js::Value::null();
    });
    js::Object* keyframe_rule = define_interface(in, "CSSKeyframeRule", css_rule);
    prelude_text(*keyframe_rule, "keyText", true);
    declared(*keyframe_rule);
    js::Object* namespace_rule = define_interface(in, "CSSNamespaceRule", css_rule);
    define_getter(in, *namespace_rule, "namespaceURI", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        for (css::ComponentValue const& value : css::parse_component_value_list((*rule)->rule->prelude)) {
            if (value.is_token(css::Token::Type::String) || value.is_token(css::Token::Type::Url))
                return internals_of(interp).string(value.token().value);
        }
        return internals_of(interp).string("");
    });
    define_getter(in, *namespace_rule, "prefix", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RuleObject*> const rule = this_rule(interp, this_value);
        if (!rule)
            return std::nullopt;
        for (css::ComponentValue const& value : css::parse_component_value_list((*rule)->rule->prelude)) {
            if (value.is_token(css::Token::Type::Ident))
                return internals_of(interp).string(value.token().value);
            if (!value.is_token(css::Token::Type::Whitespace))
                break;
        }
        return internals_of(interp).string("");
    });
    js::Object* counter_style_rule = define_interface(in, "CSSCounterStyleRule", css_rule);
    prelude_text(*counter_style_rule, "name", false);
    js::Object* property_rule = define_interface(in, "CSSPropertyRule", css_rule);
    prelude_text(*property_rule, "name", false);
    js::Object* nested_declarations = define_interface(in, "CSSNestedDeclarations", css_rule);
    declared(*nested_declarations);
    define_interface(in, "CSSFontFeatureValuesRule", css_rule);
    define_interface(in, "CSSFontPaletteValuesRule", css_rule);

    // adoptedStyleSheets (CSSOM §6.1.4): constructed sheets of this
    // document, on the document and on its shadow roots.
    auto const adopted = [&](js::Object& prototype) {
        define_getter(
            in, prototype, "adoptedStyleSheets",
            [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                auto* wrapper = this_value.is_object() ? dynamic_cast<NodeWrapper*>(this_value.as_object()) : nullptr;
                if (!wrapper || wrapper->detached())
                    return interp.throw_type_error("Illegal invocation");
                js::Object* kept = wrapper->same_object("adoptedStyleSheets");
                if (kept)
                    return js::Value::object(kept);
                js::Object* array = interp.new_array();
                wrapper->keep_same_object("adoptedStyleSheets", array);
                return js::Value::object(array);
            },
            [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                Realm::Internals& internals = internals_of(interp);
                auto* wrapper = this_value.is_object() ? dynamic_cast<NodeWrapper*>(this_value.as_object()) : nullptr;
                if (!wrapper || wrapper->detached())
                    return interp.throw_type_error("Illegal invocation");
                js::Value const given = js::argument(args, 0);
                if (!given.is_object())
                    return interp.throw_type_error("Failed to set the 'adoptedStyleSheets' property: The provided value cannot be converted to a sequence.");
                js::Interpreter::Roots const roots(interp);
                interp.root(given);
                std::optional<js::Value> const length_value = interp.get(given, interp.key("length"));
                if (!length_value)
                    return std::nullopt;
                std::optional<double> const length = interp.to_number(*length_value);
                if (!length)
                    return std::nullopt;
                std::vector<SheetObject*> sheets;
                for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(std::max(0.0, *length)); ++i) {
                    std::optional<js::Value> const item = interp.get(given, js::PropertyKey::index(i));
                    if (!item)
                        return std::nullopt;
                    auto* sheet = item->is_object() ? dynamic_cast<SheetObject*>(item->as_object()) : nullptr;
                    if (!sheet)
                        return interp.throw_type_error("Failed to set the 'adoptedStyleSheets' property: Failed to convert value to 'CSSStyleSheet'.");
                    if (!sheet->constructed)
                        return internals.throw_dom_exception("NotAllowedError", "Failed to set the 'adoptedStyleSheets' property: Can't adopt non-constructed stylesheets.");
                    sheets.push_back(sheet);
                }
                // The array the getter answers, rewritten in place, and the
                // document's record of the states, in the same order.
                js::Object* array = interp.new_array();
                interp.root(js::Value::object(array));
                std::vector<std::shared_ptr<dom::ScriptedSheet>> states;
                std::uint32_t index = 0;
                for (SheetObject* sheet : sheets) {
                    array->put(js::PropertyKey::index(index++), js::Value::object(sheet));
                    sheet->state->write = [model = sheet->model] { return sheet_text(*model); };
                    sheet->state->touch();
                    states.push_back(sheet->state);
                }
                wrapper->keep_same_object("adoptedStyleSheets", array);
                dom::Node& root = wrapper->node();
                dom::Document& document = root.type() == dom::NodeType::Document ? static_cast<dom::Document&>(root) : root.document();
                bool found = false;
                for (auto& [owner, held] : document.adopted_sheets) {
                    if (owner == &root) {
                        held = states;
                        found = true;
                    }
                }
                if (!found)
                    document.adopted_sheets.emplace_back(&root, std::move(states));
                ++internals.mutations;
                return js::Value::undefined();
            });
    };
    adopted(*in.prototype("Document"));
    if (js::Object* shadow_root = in.prototype("ShadowRoot"))
        adopted(*shadow_root);
}

}
