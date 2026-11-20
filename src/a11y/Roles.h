#pragma once

// The role taxonomy of WAI-ARIA 1.2 (§5.3, §5.4: every role's superclasses,
// where its name comes from, its required context and owned elements,
// whether its children are presentational, the states and properties it
// supports and the values it implies), the roles WAI-ARIA 1.3 adds that the
// conformance tests exercise (image, mark, suggestion, comment,
// sectionheader, sectionfooter), and the roles of the two modules that
// extend it: DPUB-ARIA 1.1 (doc-*) and Graphics ARIA (graphics-*). The data
// is the specifications' characteristics tables, transcribed.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sashfold::a11y {

enum class Role : std::uint8_t {
    Unknown, // no role at all: an invalid token, or a node that is no element
    // WAI-ARIA 1.2 and 1.3.
    Alert,
    AlertDialog,
    Application,
    Article,
    Banner,
    Blockquote,
    Button,
    Caption,
    Cell,
    Checkbox,
    Code,
    ColumnHeader,
    Combobox,
    Command,
    Comment,
    Complementary,
    Composite,
    ContentInfo,
    Definition,
    Deletion,
    Dialog,
    Document,
    Emphasis,
    Feed,
    Figure,
    Form,
    Generic,
    Grid,
    GridCell,
    Group,
    Heading,
    Image,
    Input,
    Insertion,
    Landmark,
    Link,
    List,
    ListBox,
    ListItem,
    Log,
    Main,
    Mark,
    Marquee,
    Math,
    Menu,
    MenuBar,
    MenuItem,
    MenuItemCheckbox,
    MenuItemRadio,
    Meter,
    Navigation,
    None, // and its synonym presentation
    Note,
    Option,
    Paragraph,
    ProgressBar,
    Radio,
    RadioGroup,
    Range,
    Region,
    RoleType,
    Row,
    RowGroup,
    RowHeader,
    ScrollBar,
    Search,
    SearchBox,
    Section,
    SectionFooter,
    SectionHead,
    SectionHeader,
    Select,
    Separator,
    Slider,
    SpinButton,
    Status,
    Strong,
    Structure,
    Subscript,
    Suggestion,
    Superscript,
    Switch,
    Tab,
    Table,
    TabList,
    TabPanel,
    Term,
    TextBox,
    Time,
    Timer,
    Toolbar,
    Tooltip,
    Tree,
    TreeGrid,
    TreeItem,
    Widget,
    Window,
    // DPUB-ARIA 1.1.
    DocAbstract,
    DocAcknowledgments,
    DocAfterword,
    DocAppendix,
    DocBacklink,
    DocBiblioentry,
    DocBibliography,
    DocBiblioref,
    DocChapter,
    DocColophon,
    DocConclusion,
    DocCover,
    DocCredit,
    DocCredits,
    DocDedication,
    DocEndnote,
    DocEndnotes,
    DocEpigraph,
    DocEpilogue,
    DocErrata,
    DocExample,
    DocFootnote,
    DocForeword,
    DocGlossary,
    DocGlossref,
    DocIndex,
    DocIntroduction,
    DocNoteref,
    DocNotice,
    DocPagebreak,
    DocPagefooter,
    DocPageheader,
    DocPagelist,
    DocPart,
    DocPreface,
    DocPrologue,
    DocPullquote,
    DocQna,
    DocSubtitle,
    DocTip,
    DocToc,
    // Graphics ARIA 1.0.
    GraphicsDocument,
    GraphicsObject,
    GraphicsSymbol,
    // What the accessibility tree has that no ARIA role names: the root of a
    // document, a run of text, a line break, and the HTML elements HTML-AAM
    // maps to a platform role of their own but to no ARIA role (their
    // computed role is the empty string, as Core-AAM's "Computed Role" says
    // for a role the specification does not define).
    RootWebArea,
    StaticText,
    LineBreak,
    Label,
    Legend,
    FigCaption,
    Abbr,
    Audio,
    Video,
    Canvas,
    Embed,
    IFrame,
    Cite,
    Kbd,
    Var,
    Ruby,
    DescriptionList,
    ListMarker,
    DisclosureTriangle, // <summary>
    ImageMap,
    Count_,
};

inline constexpr std::size_t role_count = static_cast<std::size_t>(Role::Count_);

// The ARIA states and properties (ARIA 1.2 §6.6, and aria-actions of 1.3),
// numbered so that a set of them is one 64-bit word.
enum class Aria : std::uint8_t {
    ActiveDescendant,
    Actions,
    Atomic,
    AutoComplete,
    BrailleLabel,
    BrailleRoleDescription,
    Busy,
    Checked,
    ColCount,
    ColIndex,
    ColIndexText,
    ColSpan,
    Controls,
    Current,
    DescribedBy,
    Description,
    Details,
    Disabled,
    DropEffect,
    ErrorMessage,
    Expanded,
    FlowTo,
    Grabbed,
    HasPopup,
    Hidden,
    Invalid,
    KeyShortcuts,
    Label,
    LabelledBy,
    Level,
    Live,
    Modal,
    MultiLine,
    MultiSelectable,
    Orientation,
    Owns,
    Placeholder,
    PosInSet,
    Pressed,
    ReadOnly,
    Relevant,
    Required,
    RoleDescription,
    RowCount,
    RowIndex,
    RowIndexText,
    RowSpan,
    Selected,
    SetSize,
    Sort,
    ValueMax,
    ValueMin,
    ValueNow,
    ValueText,
    Count_,
};
inline constexpr std::size_t aria_count = static_cast<std::size_t>(Aria::Count_);
using AriaSet = std::uint64_t;
constexpr AriaSet bit(Aria a) { return AriaSet { 1 } << static_cast<unsigned>(a); }

// The content attribute's name ("aria-checked") and the attribute named so.
std::string_view aria_attribute_name(Aria);
std::optional<Aria> aria_from_attribute(std::string_view name);

// The global states and properties (ARIA 1.2 §6.4): applicable to every role.
// Those deprecated as globals in 1.2 (aria-disabled, aria-errormessage,
// aria-haspopup, aria-invalid) still count, as every engine counts them.
AriaSet global_attributes();

// Where a role's accessible name may come from (ARIA 1.2 §5.2.8).
enum NameFrom : std::uint8_t {
    NameFromAuthor = 1,
    NameFromContents = 2,
    NameProhibited = 4,
};

struct RoleInfo {
    std::string_view name; // the computed role string; empty for the roles ARIA does not define
    bool abstract = false;
    std::uint8_t name_from = 0;
    bool name_required = false;
    bool children_presentational = false;
    bool landmark = false;
    std::span<Role const> superclasses;
    std::span<Role const> required_context;
    // The roles of the elements it must own, a `group` among them standing
    // for the pairs ARIA writes "group → option".
    std::span<Role const> required_owned;
    // The states and properties it supports beyond the global ones,
    // its superclasses' included.
    AriaSet supported = 0;
    AriaSet required = 0;
};

RoleInfo const& info(Role);
std::string_view role_name(Role);
// The role a role attribute's token names (ASCII case-insensitive), the
// synonyms folded (img → image, presentation → none, directory → list,
// doc-biblioentry → listitem, doc-endnote → listitem), abstract roles
// included; nothing for an unknown token.
std::optional<Role> role_from_token(std::string_view token);
// Whether `role` is `ancestor` or has it among its superclasses, at any depth.
bool is_a(Role role, Role ancestor);
bool is_abstract(Role role);
// Whether the role is a widget (ARIA 1.2 §5.3.2), a landmark, a live
// region by default (alert, log, marquee, status, timer).
bool is_widget(Role role);
bool is_landmark(Role role);
// The live region politeness a role implies: "assertive" for alert,
// "polite" for log and status, "off" for marquee and timer; empty otherwise.
std::string_view implicit_live(Role role);
// The implicit value of a property the role gives (ARIA 1.2's "Implicit
// Value for Role"): aria-orientation, aria-level, the range values,
// aria-atomic, aria-haspopup, aria-selected; empty for none.
std::string_view implicit_value(Role role, Aria attribute);
// The ARIA roles this module ever computes, in the order the enumeration
// lists them: what a test walks.
std::span<Role const> all_roles();

}
