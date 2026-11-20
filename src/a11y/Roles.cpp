#include "a11y/Roles.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

namespace sashfold::a11y {

namespace {

constexpr std::array<std::string_view, aria_count> aria_names = {
    "aria-activedescendant", "aria-actions", "aria-atomic", "aria-autocomplete", "aria-braillelabel",
    "aria-brailleroledescription", "aria-busy", "aria-checked", "aria-colcount", "aria-colindex", "aria-colindextext",
    "aria-colspan", "aria-controls", "aria-current", "aria-describedby", "aria-description", "aria-details",
    "aria-disabled", "aria-dropeffect", "aria-errormessage", "aria-expanded", "aria-flowto", "aria-grabbed",
    "aria-haspopup", "aria-hidden", "aria-invalid", "aria-keyshortcuts", "aria-label", "aria-labelledby", "aria-level",
    "aria-live", "aria-modal", "aria-multiline", "aria-multiselectable", "aria-orientation", "aria-owns",
    "aria-placeholder", "aria-posinset", "aria-pressed", "aria-readonly", "aria-relevant", "aria-required",
    "aria-roledescription", "aria-rowcount", "aria-rowindex", "aria-rowindextext", "aria-rowspan", "aria-selected",
    "aria-setsize", "aria-sort", "aria-valuemax", "aria-valuemin", "aria-valuenow", "aria-valuetext",
};

constexpr AriaSet set(std::initializer_list<Aria> list)
{
    AriaSet out = 0;
    for (Aria const a : list)
        out |= bit(a);
    return out;
}

using A = Aria;

struct Entry {
    Role role;
    std::string_view name;
    std::uint8_t name_from;
    std::vector<Role> superclasses;
    std::vector<Role> context = {};
    std::vector<Role> owned = {};
    AriaSet supported = 0;
    AriaSet required = 0;
    bool presentational = false;
    bool name_required = false;
    bool abstract = false;
};

constexpr std::uint8_t author = NameFromAuthor;
constexpr std::uint8_t contents = NameFromAuthor | NameFromContents;
constexpr std::uint8_t prohibited = NameProhibited;

// The characteristics tables of ARIA 1.2 §5.4 (and the 1.3 draft's for the
// roles it adds), DPUB-ARIA 1.1 §4 and Graphics ARIA §4. The states and
// properties are those each table lists as supported (inherited ones are
// added from the superclasses afterwards).
std::vector<Entry> entries()
{
    using R = Role;
    std::vector<Entry> e;
    auto add = [&](Entry entry) { e.push_back(std::move(entry)); };
    // The abstract roles.
    add({ R::RoleType, "roletype", 0, {}, {}, {}, 0, 0, false, false, true });
    add({ R::Structure, "structure", 0, { R::RoleType }, {}, {}, 0, 0, false, false, true });
    add({ R::Widget, "widget", 0, { R::RoleType }, {}, {}, 0, 0, false, false, true });
    add({ R::Window, "window", 0, { R::RoleType }, {}, {}, set({ A::Modal }), 0, false, false, true });
    add({ R::Section, "section", 0, { R::Structure }, {}, {}, 0, 0, false, false, true });
    add({ R::SectionHead, "sectionhead", contents, { R::Structure }, {}, {}, 0, 0, false, false, true });
    add({ R::Landmark, "landmark", author, { R::Section }, {}, {}, 0, 0, false, false, true });
    add({ R::Command, "command", author, { R::Widget }, {}, {}, 0, 0, false, false, true });
    add({ R::Composite, "composite", author, { R::Widget }, {}, {}, set({ A::ActiveDescendant, A::Disabled }), 0, false, false, true });
    add({ R::Input, "input", author, { R::Widget }, {}, {}, set({ A::Disabled }), 0, false, false, true });
    add({ R::Range, "range", author, { R::Structure }, {}, {}, set({ A::ValueMax, A::ValueMin, A::ValueNow, A::ValueText }), 0, false, false, true });
    add({ R::Select, "select", author, { R::Composite, R::Group }, {}, {}, set({ A::Orientation }), 0, false, false, true });

    // The concrete roles.
    add({ R::Alert, "alert", author, { R::Section } });
    add({ R::AlertDialog, "alertdialog", author, { R::Alert, R::Dialog }, {}, {}, 0, 0, false, true });
    add({ R::Application, "application", author, { R::Structure }, {}, {},
        set({ A::ActiveDescendant, A::Disabled, A::ErrorMessage, A::Expanded, A::HasPopup, A::Invalid }) });
    add({ R::Article, "article", author, { R::Document }, {}, {}, set({ A::PosInSet, A::SetSize }) });
    add({ R::Banner, "banner", author, { R::Landmark } });
    add({ R::Blockquote, "blockquote", author, { R::Section } });
    add({ R::Button, "button", contents, { R::Command }, {}, {}, set({ A::Disabled, A::Expanded, A::HasPopup, A::Pressed }), 0, true });
    add({ R::Caption, "caption", prohibited, { R::Section }, { R::Figure, R::Grid, R::Table, R::TreeGrid } });
    add({ R::Cell, "cell", contents, { R::Section }, { R::Row }, {},
        set({ A::ColIndex, A::ColIndexText, A::ColSpan, A::RowIndex, A::RowIndexText, A::RowSpan }) });
    add({ R::Checkbox, "checkbox", contents, { R::Input }, {}, {},
        set({ A::Checked, A::ErrorMessage, A::Expanded, A::Invalid, A::ReadOnly, A::Required }), set({ A::Checked }), true });
    add({ R::Code, "code", prohibited, { R::Section } });
    add({ R::ColumnHeader, "columnheader", contents, { R::Cell, R::GridCell, R::SectionHead }, { R::Row }, {}, set({ A::Sort }) });
    add({ R::Combobox, "combobox", author, { R::Input }, {}, {},
        set({ A::ActiveDescendant, A::AutoComplete, A::Controls, A::ErrorMessage, A::Expanded, A::HasPopup, A::Invalid,
            A::ReadOnly, A::Required }),
        set({ A::Expanded }) });
    add({ R::Comment, "comment", contents, { R::Article } });
    add({ R::Complementary, "complementary", author, { R::Landmark } });
    add({ R::ContentInfo, "contentinfo", author, { R::Landmark } });
    add({ R::Definition, "definition", author, { R::Section } });
    add({ R::Deletion, "deletion", prohibited, { R::Section } });
    add({ R::Dialog, "dialog", author, { R::Window }, {}, {}, 0, 0, false, true });
    add({ R::Document, "document", author, { R::Structure }, {}, {}, set({ A::Expanded }) });
    add({ R::Emphasis, "emphasis", prohibited, { R::Section } });
    add({ R::Feed, "feed", author, { R::List }, {}, { R::Article } });
    add({ R::Figure, "figure", author, { R::Section } });
    add({ R::Form, "form", author, { R::Landmark } });
    add({ R::Generic, "generic", prohibited, { R::Structure } });
    add({ R::Grid, "grid", author, { R::Composite, R::Table }, {}, { R::Row, R::RowGroup }, set({ A::MultiSelectable, A::ReadOnly }) });
    add({ R::GridCell, "gridcell", contents, { R::Cell, R::Widget }, { R::Row }, {},
        set({ A::Disabled, A::ErrorMessage, A::Expanded, A::HasPopup, A::Invalid, A::ReadOnly, A::Required, A::Selected }) });
    add({ R::Group, "group", author, { R::Section }, {}, {}, set({ A::ActiveDescendant, A::Disabled }) });
    add({ R::Heading, "heading", contents, { R::SectionHead }, {}, {}, set({ A::Level }), set({ A::Level }), false, true });
    add({ R::Image, "image", author, { R::Section }, {}, {}, 0, 0, true, true });
    add({ R::Insertion, "insertion", prohibited, { R::Section } });
    add({ R::Link, "link", contents, { R::Command }, {}, {}, set({ A::Disabled, A::Expanded, A::HasPopup }), 0, false, true });
    add({ R::List, "list", author, { R::Section }, {}, { R::ListItem } });
    add({ R::ListBox, "listbox", author, { R::Select }, {}, { R::Group, R::Option },
        set({ A::ErrorMessage, A::Expanded, A::Invalid, A::MultiSelectable, A::ReadOnly, A::Required }), 0, false, true });
    add({ R::ListItem, "listitem", author, { R::Section }, { R::List }, {}, set({ A::Level, A::PosInSet, A::SetSize }) });
    add({ R::Log, "log", author, { R::Section } });
    add({ R::Main, "main", author, { R::Landmark } });
    add({ R::Mark, "mark", prohibited, { R::Section } });
    add({ R::Marquee, "marquee", author, { R::Section } });
    add({ R::Math, "math", author, { R::Section } });
    add({ R::Menu, "menu", author, { R::Select }, {}, { R::Group, R::MenuItem, R::MenuItemCheckbox, R::MenuItemRadio } });
    add({ R::MenuBar, "menubar", author, { R::Menu }, {}, { R::Group, R::MenuItem, R::MenuItemCheckbox, R::MenuItemRadio } });
    add({ R::MenuItem, "menuitem", contents, { R::Command }, { R::Group, R::Menu, R::MenuBar }, {},
        set({ A::Disabled, A::Expanded, A::HasPopup, A::PosInSet, A::SetSize }), 0, false, true });
    add({ R::MenuItemCheckbox, "menuitemcheckbox", contents, { R::MenuItem }, { R::Group, R::Menu, R::MenuBar }, {},
        set({ A::Checked }), set({ A::Checked }), true, true });
    add({ R::MenuItemRadio, "menuitemradio", contents, { R::MenuItemCheckbox }, { R::Group, R::Menu, R::MenuBar }, {}, 0,
        set({ A::Checked }), true, true });
    add({ R::Meter, "meter", author, { R::Range }, {}, {}, 0, set({ A::ValueNow }), true, true });
    add({ R::Navigation, "navigation", author, { R::Landmark } });
    add({ R::None, "none", prohibited, { R::Structure } });
    add({ R::Note, "note", author, { R::Section } });
    add({ R::Option, "option", contents, { R::Input }, { R::Group, R::ListBox }, {},
        set({ A::Checked, A::PosInSet, A::Selected, A::SetSize }), 0, true, true });
    add({ R::Paragraph, "paragraph", prohibited, { R::Section } });
    add({ R::ProgressBar, "progressbar", author, { R::Range, R::Widget }, {}, {}, 0, 0, true, true });
    add({ R::Radio, "radio", contents, { R::Input }, {}, {}, set({ A::Checked, A::PosInSet, A::SetSize }), set({ A::Checked }), true, true });
    add({ R::RadioGroup, "radiogroup", author, { R::Group }, {}, { R::Radio },
        set({ A::ErrorMessage, A::Invalid, A::ReadOnly, A::Required }) });
    add({ R::Region, "region", author, { R::Landmark }, {}, {}, 0, 0, false, true });
    add({ R::Row, "row", contents, { R::Group, R::Widget }, { R::Grid, R::RowGroup, R::Table, R::TreeGrid },
        { R::Cell, R::ColumnHeader, R::GridCell, R::RowHeader },
        set({ A::ColIndex, A::ColIndexText, A::Expanded, A::Level, A::PosInSet, A::RowIndex, A::RowIndexText, A::Selected, A::SetSize }) });
    add({ R::RowGroup, "rowgroup", contents, { R::Structure }, { R::Grid, R::Table, R::TreeGrid }, { R::Row } });
    add({ R::RowHeader, "rowheader", contents, { R::Cell, R::GridCell, R::SectionHead }, { R::Row }, {}, set({ A::Sort }) });
    add({ R::ScrollBar, "scrollbar", author, { R::Range, R::Widget }, {}, {}, set({ A::Controls, A::Orientation }),
        set({ A::Controls, A::ValueNow }), true });
    add({ R::Search, "search", author, { R::Landmark } });
    add({ R::SearchBox, "searchbox", author, { R::TextBox }, {}, {}, 0, 0, false, true });
    add({ R::SectionFooter, "sectionfooter", author, { R::Section } });
    add({ R::SectionHeader, "sectionheader", author, { R::Section } });
    add({ R::Separator, "separator", author, { R::Structure }, {}, {}, set({ A::Orientation, A::ValueMax, A::ValueMin, A::ValueNow, A::ValueText, A::Disabled }), 0, true });
    add({ R::Slider, "slider", author, { R::Input, R::Range }, {}, {},
        set({ A::ErrorMessage, A::HasPopup, A::Invalid, A::Orientation, A::ReadOnly }), set({ A::ValueNow }), true, true });
    add({ R::SpinButton, "spinbutton", author, { R::Composite, R::Input, R::Range }, {}, {},
        set({ A::ErrorMessage, A::Invalid, A::ReadOnly, A::Required }), 0, false, true });
    add({ R::Status, "status", author, { R::Section } });
    add({ R::Strong, "strong", prohibited, { R::Section } });
    add({ R::Subscript, "subscript", prohibited, { R::Section } });
    add({ R::Suggestion, "suggestion", prohibited, { R::Section }, {}, { R::Insertion, R::Deletion } });
    add({ R::Superscript, "superscript", prohibited, { R::Section } });
    add({ R::Switch, "switch", contents, { R::Checkbox }, {}, {}, 0, set({ A::Checked }), true, true });
    add({ R::Tab, "tab", contents, { R::SectionHead, R::Widget }, { R::TabList }, {},
        set({ A::Disabled, A::Expanded, A::HasPopup, A::PosInSet, A::Selected, A::SetSize }), 0, true });
    add({ R::Table, "table", author, { R::Section }, {}, { R::Row, R::RowGroup }, set({ A::ColCount, A::RowCount }), 0, false, true });
    add({ R::TabList, "tablist", author, { R::Composite }, {}, { R::Tab }, set({ A::MultiSelectable, A::Orientation }) });
    add({ R::TabPanel, "tabpanel", author, { R::Section }, {}, {}, 0, 0, false, true });
    add({ R::Term, "term", author, { R::Section } });
    add({ R::TextBox, "textbox", author, { R::Input }, {}, {},
        set({ A::ActiveDescendant, A::AutoComplete, A::ErrorMessage, A::HasPopup, A::Invalid, A::MultiLine, A::Placeholder,
            A::ReadOnly, A::Required }),
        0, false, true });
    add({ R::Time, "time", author, { R::Section } });
    add({ R::Timer, "timer", author, { R::Status } });
    add({ R::Toolbar, "toolbar", author, { R::Group }, {}, {}, set({ A::Orientation }) });
    add({ R::Tooltip, "tooltip", contents, { R::Section }, {}, {}, 0, 0, false, true });
    add({ R::Tree, "tree", author, { R::Select }, {}, { R::Group, R::TreeItem },
        set({ A::ErrorMessage, A::Invalid, A::MultiSelectable, A::Required }), 0, false, true });
    add({ R::TreeGrid, "treegrid", author, { R::Grid, R::Tree }, {}, { R::Row, R::RowGroup }, 0, 0, false, true });
    add({ R::TreeItem, "treeitem", contents, { R::ListItem, R::Option }, { R::Group, R::Tree }, {}, set({ A::Expanded, A::HasPopup }), 0,
        false, true });

    // DPUB-ARIA 1.1 §4.
    add({ R::DocAbstract, "doc-abstract", author, { R::Section } });
    add({ R::DocAcknowledgments, "doc-acknowledgments", author, { R::Landmark } });
    add({ R::DocAfterword, "doc-afterword", author, { R::Landmark } });
    add({ R::DocAppendix, "doc-appendix", author, { R::Landmark } });
    add({ R::DocBacklink, "doc-backlink", contents, { R::Link }, {}, {}, 0, 0, false, true });
    add({ R::DocBiblioentry, "doc-biblioentry", author, { R::ListItem }, { R::List } });
    add({ R::DocBibliography, "doc-bibliography", author, { R::Landmark } });
    add({ R::DocBiblioref, "doc-biblioref", contents, { R::Link }, {}, {}, 0, 0, false, true });
    add({ R::DocChapter, "doc-chapter", author, { R::Landmark } });
    add({ R::DocColophon, "doc-colophon", author, { R::Section } });
    add({ R::DocConclusion, "doc-conclusion", author, { R::Landmark } });
    add({ R::DocCover, "doc-cover", author, { R::Image } });
    add({ R::DocCredit, "doc-credit", author, { R::Section } });
    add({ R::DocCredits, "doc-credits", author, { R::Landmark } });
    add({ R::DocDedication, "doc-dedication", author, { R::Section } });
    add({ R::DocEndnote, "doc-endnote", author, { R::ListItem }, { R::List } });
    add({ R::DocEndnotes, "doc-endnotes", author, { R::Landmark } });
    add({ R::DocEpigraph, "doc-epigraph", author, { R::Section } });
    add({ R::DocEpilogue, "doc-epilogue", author, { R::Landmark } });
    add({ R::DocErrata, "doc-errata", author, { R::Landmark } });
    add({ R::DocExample, "doc-example", author, { R::Section } });
    add({ R::DocFootnote, "doc-footnote", author, { R::Section } });
    add({ R::DocForeword, "doc-foreword", author, { R::Landmark } });
    add({ R::DocGlossary, "doc-glossary", author, { R::Landmark } });
    add({ R::DocGlossref, "doc-glossref", contents, { R::Link }, {}, {}, 0, 0, false, true });
    add({ R::DocIndex, "doc-index", author, { R::Navigation } });
    add({ R::DocIntroduction, "doc-introduction", author, { R::Landmark } });
    add({ R::DocNoteref, "doc-noteref", contents, { R::Link }, {}, {}, 0, 0, false, true });
    add({ R::DocNotice, "doc-notice", author, { R::Note } });
    add({ R::DocPagebreak, "doc-pagebreak", author, { R::Separator }, {}, {}, 0, 0, true, true });
    add({ R::DocPagefooter, "doc-pagefooter", prohibited, { R::Section } });
    add({ R::DocPageheader, "doc-pageheader", prohibited, { R::Section } });
    add({ R::DocPagelist, "doc-pagelist", author, { R::Navigation } });
    add({ R::DocPart, "doc-part", author, { R::Landmark }, {}, {}, 0, 0, false, true });
    add({ R::DocPreface, "doc-preface", author, { R::Landmark } });
    add({ R::DocPrologue, "doc-prologue", author, { R::Landmark } });
    add({ R::DocPullquote, "doc-pullquote", author, { R::None } });
    add({ R::DocQna, "doc-qna", author, { R::Section } });
    add({ R::DocSubtitle, "doc-subtitle", author, { R::SectionHead } });
    add({ R::DocTip, "doc-tip", author, { R::Note } });
    add({ R::DocToc, "doc-toc", author, { R::Navigation } });

    // Graphics ARIA §4.
    add({ R::GraphicsDocument, "graphics-document", author, { R::Document }, {}, {}, 0, 0, false, true });
    add({ R::GraphicsObject, "graphics-object", contents, { R::Group } });
    add({ R::GraphicsSymbol, "graphics-symbol", author, { R::Image }, {}, {}, 0, 0, true, true });

    // The platform-only roles: no ARIA name.
    add({ R::RootWebArea, "", author, { R::Document } });
    add({ R::StaticText, "", contents, {} });
    add({ R::LineBreak, "", contents, {} });
    add({ R::Label, "", contents, {} });
    add({ R::Legend, "", contents, {} });
    add({ R::FigCaption, "", contents, {} });
    add({ R::Abbr, "", author, {} });
    add({ R::Audio, "", author, {} });
    add({ R::Video, "", author, {} });
    add({ R::Canvas, "", author, {} });
    add({ R::Embed, "", author, {} });
    add({ R::IFrame, "", author, {} });
    add({ R::Cite, "", author, {} });
    add({ R::Kbd, "", author, {} });
    add({ R::Var, "", author, {} });
    add({ R::Ruby, "", author, {} });
    add({ R::DescriptionList, "", author, {} });
    add({ R::ListMarker, "", contents, {} });
    add({ R::DisclosureTriangle, "", contents, { R::Button }, {}, {}, set({ A::Expanded }) });
    add({ R::ImageMap, "", author, { R::Image } });
    return e;
}

struct Table {
    std::array<RoleInfo, role_count> infos {};
    std::array<std::vector<Role>, role_count> supers;
    std::array<std::vector<Role>, role_count> contexts;
    std::array<std::vector<Role>, role_count> owned;
    // The transitive closure of the superclasses, as a bit per role.
    std::array<std::array<bool, role_count>, role_count> ancestry {};
};

void close_supported(Table& table, Role role, std::array<bool, role_count>& done)
{
    std::size_t const index = static_cast<std::size_t>(role);
    if (done[index])
        return;
    done[index] = true;
    table.ancestry[index][index] = true;
    for (Role const super : table.supers[index]) {
        close_supported(table, super, done);
        std::size_t const s = static_cast<std::size_t>(super);
        table.infos[index].supported |= table.infos[s].supported;
        for (std::size_t i = 0; i < role_count; ++i) {
            if (table.ancestry[s][i])
                table.ancestry[index][i] = true;
        }
    }
}

Table const& table()
{
    static Table const built = [] {
        Table t;
        for (Entry& entry : entries()) {
            std::size_t const i = static_cast<std::size_t>(entry.role);
            t.supers[i] = std::move(entry.superclasses);
            t.contexts[i] = std::move(entry.context);
            t.owned[i] = std::move(entry.owned);
            RoleInfo& info = t.infos[i];
            info.name = entry.name;
            info.abstract = entry.abstract;
            info.name_from = entry.name_from;
            info.name_required = entry.name_required;
            info.children_presentational = entry.presentational;
            info.supported = entry.supported;
            info.required = entry.required;
        }
        std::array<bool, role_count> done {};
        for (std::size_t i = 0; i < role_count; ++i)
            close_supported(t, static_cast<Role>(i), done);
        for (std::size_t i = 0; i < role_count; ++i) {
            RoleInfo& info = t.infos[i];
            info.superclasses = t.supers[i];
            info.required_context = t.contexts[i];
            info.required_owned = t.owned[i];
            info.landmark = t.ancestry[i][static_cast<std::size_t>(Role::Landmark)];
        }
        return t;
    }();
    return built;
}

bool equals_ascii_ci(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

}

std::string_view aria_attribute_name(Aria a)
{
    return aria_names[static_cast<std::size_t>(a)];
}

std::optional<Aria> aria_from_attribute(std::string_view name)
{
    if (!name.starts_with("aria-"))
        return std::nullopt;
    for (std::size_t i = 0; i < aria_count; ++i) {
        if (aria_names[i] == name)
            return static_cast<Aria>(i);
    }
    return std::nullopt;
}

AriaSet global_attributes()
{
    return set({ A::Atomic, A::BrailleLabel, A::BrailleRoleDescription, A::Busy, A::Controls, A::Current, A::DescribedBy,
        A::Description, A::Details, A::Disabled, A::DropEffect, A::ErrorMessage, A::FlowTo, A::Grabbed, A::HasPopup,
        A::Hidden, A::Invalid, A::KeyShortcuts, A::Label, A::LabelledBy, A::Live, A::Owns, A::Relevant, A::RoleDescription,
        A::Actions });
}

RoleInfo const& info(Role role)
{
    return table().infos[static_cast<std::size_t>(role)];
}

std::string_view role_name(Role role)
{
    return info(role).name;
}

std::optional<Role> role_from_token(std::string_view token)
{
    if (token.empty())
        return std::nullopt;
    // The synonyms (ARIA 1.2 §5.4 presentation, img; §5.4 directory is
    // deprecated and maps to list; DPUB-ARIA 1.1 deprecates biblioentry and
    // endnote, whose elements are list items).
    if (equals_ascii_ci(token, "img"))
        return Role::Image;
    if (equals_ascii_ci(token, "presentation"))
        return Role::None;
    if (equals_ascii_ci(token, "directory"))
        return Role::List;
    if (equals_ascii_ci(token, "doc-biblioentry") || equals_ascii_ci(token, "doc-endnote"))
        return Role::ListItem;
    Table const& t = table();
    for (std::size_t i = 1; i < role_count; ++i) {
        std::string_view const name = t.infos[i].name;
        if (!name.empty() && equals_ascii_ci(token, name))
            return static_cast<Role>(i);
    }
    return std::nullopt;
}

bool is_a(Role role, Role ancestor)
{
    return table().ancestry[static_cast<std::size_t>(role)][static_cast<std::size_t>(ancestor)];
}

bool is_abstract(Role role)
{
    return info(role).abstract;
}

bool is_widget(Role role)
{
    return is_a(role, Role::Widget);
}

bool is_landmark(Role role)
{
    return info(role).landmark;
}

std::string_view implicit_live(Role role)
{
    switch (role) {
    case Role::Alert:
    case Role::AlertDialog:
        return "assertive";
    case Role::Log:
    case Role::Status:
        return "polite";
    case Role::Marquee:
    case Role::Timer:
        return "off";
    default:
        return {};
    }
}

std::string_view implicit_value(Role role, Aria attribute)
{
    switch (attribute) {
    case Aria::Orientation:
        switch (role) {
        case Role::ListBox:
        case Role::Menu:
        case Role::ScrollBar:
        case Role::Tree:
        case Role::TreeGrid:
            return "vertical";
        case Role::MenuBar:
        case Role::Separator:
        case Role::DocPagebreak:
        case Role::Slider:
        case Role::TabList:
        case Role::Toolbar:
            return "horizontal";
        default:
            return {};
        }
    case Aria::Level:
        return role == Role::Heading ? "2" : std::string_view {};
    case Aria::ValueMin:
        return role == Role::ScrollBar || role == Role::Slider || role == Role::Separator || role == Role::Meter
                || role == Role::ProgressBar
            ? "0"
            : std::string_view {};
    case Aria::ValueMax:
        return role == Role::ScrollBar || role == Role::Slider || role == Role::Separator || role == Role::Meter
                || role == Role::ProgressBar
            ? "100"
            : std::string_view {};
    case Aria::Atomic:
        return role == Role::Alert || role == Role::AlertDialog || role == Role::Status || role == Role::Timer ? "true" : std::string_view {};
    case Aria::HasPopup:
        return role == Role::Combobox ? "listbox" : std::string_view {};
    case Aria::Selected:
        return role == Role::Option || role == Role::Tab ? "false" : std::string_view {};
    case Aria::Live:
        return implicit_live(role);
    default:
        return {};
    }
}

std::span<Role const> all_roles()
{
    static std::vector<Role> const roles = [] {
        std::vector<Role> out;
        for (std::size_t i = 1; i < role_count; ++i) {
            if (!table().infos[i].name.empty())
                out.push_back(static_cast<Role>(i));
        }
        return out;
    }();
    return roles;
}

}
