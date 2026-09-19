#include "ui/Browser.h"
#include "ui/Forms.h"
#include "ui/Frames.h"

#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "core/Ascii.h"
#include "core/Json.h"
#include "core/Unicode.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "platform/Clipboard.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "ui/PageImages.h"
#include "ui/Cosmetic.h"
#include "ui/Downloads.h"
#include "ui/Icons.h"
#include "ui/InternalPages.h"
#include "ui/Reader.h"
#include "ui/SourceSet.h"
#include "ui/ThemeGallery.h"
#include "ui/ThemeImport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <utility>

namespace sashfold::ui {

using platform::Cursor;
using platform::Key;
using platform::KeyEvent;

namespace {

// The glyphs that are words' company — the buttons' pictures are icons
// (ui/Icons.h), not letters.
constexpr char32_t glyph_ellipsis = 0x2026;
constexpr char32_t glyph_submenu = 0x203A; // an item that opens a menu of its own
constexpr char32_t glyph_check = 0x2713;

constexpr float font_ascent_ratio = 25.0f / 32.0f;
constexpr float font_descent_ratio = 7.0f / 32.0f;

// The most a page's icon may weigh: an icon is a few kilobytes, and a
// server answering the icon's URL with a page is not one to decode.
constexpr std::size_t max_icon_bytes = 1024u * 1024u;

// The most one of a theme's pictures may weigh as a file and hold once
// decoded: a header is a few thousand pixels wide and a couple of hundred
// tall, and a wallpaper four times a 4K screen is not a header.
constexpr std::uintmax_t max_theme_picture_bytes = 16u * 1024u * 1024u;
constexpr std::int64_t max_theme_picture_pixels = 16 * 1024 * 1024;
// The largest a repeating picture may be, at the display's scale, and still
// be kept as a scaled copy to tile from; past it, it is drawn copy by copy.
constexpr std::int64_t max_theme_tile_pixels = 1024 * 1024;

// The chrome's words — a tab's title, an address, a menu — are set in a face
// of the machine's: the families the theme names, else the machine's own
// interface face and its sans-serif, through our own font stack, kerned as
// the face's tables say. The list ends at the built-in face, which is all
// there is where the machine's fonts are off, as they are for a script:
// there every measure and every pixel is what it was when that face was all
// the chrome had. The fonts a page brings along take no part: the window's
// words are never a page's to dress.
std::vector<std::string>& chrome_font_families()
{
    static std::vector<std::string> families;
    return families;
}

// A list of families as CSS writes one: split at its commas, the space and
// the quotes about each taken off.
void set_chrome_font_family(std::string_view list)
{
    std::vector<std::string>& families = chrome_font_families();
    families.clear();
    std::size_t at = 0;
    while (at <= list.size()) {
        std::size_t const comma = std::min(list.find(',', at), list.size());
        std::string_view name = list.substr(at, comma - at);
        while (!name.empty() && (name.front() == ' ' || name.front() == '"' || name.front() == '\''))
            name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '"' || name.back() == '\''))
            name.remove_suffix(1);
        if (!name.empty())
            families.emplace_back(name);
        at = comma + 1;
    }
}

text::FontStack const& chrome_fonts(bool bold = false)
{
    text::FontRequest request;
    request.families = chrome_font_families();
    if (request.families.empty())
        request.families = { "system-ui", "sans-serif" };
    request.weight = bold ? 700 : 400;
    request.page_fonts = false;
    return text::FontManager::instance().resolve(request);
}

float text_width(std::u32string_view text, float size)
{
    return chrome_fonts().measure(text, size);
}

// The advance of the first `count` code points of a string of the chrome's:
// where a caret stands, where an underline begins.
float text_width_to(std::u32string_view text, std::size_t count, float size)
{
    return text_width(text.substr(0, std::min(count, text.size())), size);
}

// The ascent and descent of a page run's face at its size.
text::FaceMetrics run_metrics(layout::TextRun const& run)
{
    float const size = run.style->font_size;
    if (run.fonts)
        return run.fonts->primary().metrics(size);
    return text::FaceMetrics { size * font_ascent_ratio, size * font_descent_ratio, 0 };
}

// The advance of the first `count` code points of a page run.
float prefix_width(layout::TextRun const& run, std::size_t count)
{
    std::u32string_view const text(run.text);
    std::u32string_view const prefix = text.substr(0, std::min(count, text.size()));
    float const size = run.style->font_size;
    // A run with no faces of its own is the built-in face's, not the chrome's.
    return run.fonts ? run.fonts->measure(prefix, size) : text::SashfoldMono::measure(prefix, size);
}

// A string of the chrome's, glyph by glyph: each from the first face of the
// stack that has it, a pair the same face draws side by side kerned, a
// weight the face was not made in drawn heavier.
void draw_text(Bitmap& target, std::u32string_view text, float x, float baseline, float size,
    Color color, bool bold = false)
{
    text::FontStack const& fonts = chrome_fonts(bold);
    std::optional<text::FontStack::Glyph> previous;
    for (char32_t const c : text) {
        text::FontStack::Glyph const glyph = fonts.glyph_for(c);
        if (previous && previous->face == glyph.face)
            x += glyph.face->kerning(previous->glyph, glyph.glyph, size);
        glyph.face->draw_glyph(target, glyph.glyph, x, baseline, size, color, bold && !glyph.face->is_bold(), false);
        x += glyph.face->advance(glyph.glyph, size);
        previous = glyph;
    }
}

// The baseline that centers the chrome's face — its ascent and its descent
// at the size — in a rect.
float centered_baseline(Rect const& rect, float size)
{
    text::FaceMetrics const metrics = chrome_fonts().primary().metrics(size);
    return static_cast<float>(rect.y)
        + (static_cast<float>(rect.height) - metrics.ascent - metrics.descent) / 2.0f + metrics.ascent;
}

// The string, or as much of its beginning as fits the width with an
// ellipsis after it: by what the words measure, not by their count.
std::u32string ellipsize(std::u32string text, float max_width, float size)
{
    if (max_width <= 0)
        return {};
    if (text_width(text, size) <= max_width)
        return text;
    float const room = max_width - text_width(std::u32string_view(&glyph_ellipsis, 1), size);
    if (room < 0)
        return {};
    // The longest beginning that leaves the ellipsis its room.
    std::size_t low = 0;
    std::size_t high = text.size();
    while (low < high) {
        std::size_t const middle = (low + high + 1) / 2;
        if (text_width_to(text, middle, size) <= room)
            low = middle;
        else
            high = middle - 1;
    }
    text.resize(low);
    text.push_back(glyph_ellipsis);
    return text;
}

std::string collapse_whitespace(std::string_view text)
{
    std::string out;
    bool pending_space = false;
    for (char const c : text) {
        bool const space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (space) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out += c;
    }
    return out;
}

std::string find_title(dom::Node const& node)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("title")) {
            std::string text;
            for (dom::Node const* child : element.children()) {
                if (child->is_text())
                    text += static_cast<dom::Text const*>(child)->data;
            }
            return collapse_whitespace(text);
        }
    }
    for (dom::Node const* child : node.children()) {
        std::string const title = find_title(*child);
        if (!title.empty())
            return title;
    }
    return {};
}

// The href of the page's icon: the last <link> whose rel tokens include
// "icon" and that has an href (§4.6.6.1 makes the last of the equally
// appropriate ones the one to use); empty when the page names none.
std::string find_icon_href(dom::Node const& node)
{
    std::string found;
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("link")) {
            dom::Attr const* const rel = element.find_attribute("rel");
            dom::Attr const* const href = element.find_attribute("href");
            if (rel != nullptr && href != nullptr && !href->value.empty()) {
                std::string token;
                bool is_icon = false;
                for (char const c : rel->value + " ") {
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                        if (ascii_ci_equals(token, "icon"))
                            is_icon = true;
                        token.clear();
                    } else {
                        token += c;
                    }
                }
                if (is_icon)
                    return href->value;
            }
        }
    }
    for (dom::Node const* child : node.children()) {
        std::string const href = find_icon_href(*child);
        if (!href.empty())
            found = href;
    }
    return found;
}

dom::Element const* find_anchor_target(dom::Node const& node, std::string_view id)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (dom::Attr const* const attribute = element.find_attribute("id");
            attribute && attribute->value == id)
            return &element;
        if (element.is_html("a")) {
            if (dom::Attr const* const name = element.find_attribute("name");
                name && name->value == id)
                return &element;
        }
    }
    for (dom::Node const* child : node.children()) {
        if (dom::Element const* const found = find_anchor_target(*child, id))
            return found;
    }
    return nullptr;
}

bool starts_with_ci(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && ascii_ci_equals(text.substr(0, prefix.size()), prefix);
}

bool is_web_scheme(std::string const& scheme)
{
    return scheme == "http" || scheme == "https";
}

bool is_navigable_scheme(std::string const& scheme)
{
    return is_web_scheme(scheme) || scheme == "file" || scheme == "data" || scheme == "about"
        || scheme == "view-source" || scheme == "reader";
}

// strict-origin-when-cross-origin, the default: the full URL to the same
// origin, the origin alone across origins, nothing on an https-to-http
// downgrade, and nothing from pages that are not web origins.
std::string referrer_for(net::Url const* from, net::Url const& to)
{
    if (!from || !is_web_scheme(from->scheme))
        return {};
    if (from->scheme == "https" && to.scheme != "https")
        return {};
    if (from->serialize_origin() == to.serialize_origin()) {
        net::Url stripped = *from;
        stripped.fragment.reset();
        stripped.username.clear();
        stripped.password.clear();
        return stripped.serialize();
    }
    return from->serialize_origin() + "/";
}

std::size_t previous_code_point(std::string const& text, std::size_t at)
{
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        --at;
    return at;
}

std::size_t next_code_point(std::string const& text, std::size_t at)
{
    if (at >= text.size())
        return text.size();
    ++at;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        ++at;
    return at;
}

bool same_url(std::optional<net::Url> const& a, std::optional<net::Url> const& b)
{
    if (!a || !b)
        return !a && !b;
    return a->serialize() == b->serialize();
}

std::string host_of(net::Url const& url)
{
    return url.has_host() && !url.host.empty() ? url.serialize_host() : url.serialize();
}

bool is_about_blank(net::Url const& url)
{
    return url.scheme == "about" && url.serialize_path() == "blank";
}

bool is_about_newtab(net::Url const& url)
{
    return url.scheme == "about" && url.serialize_path() == "newtab";
}

Color mix(Color a, Color b, float amount)
{
    auto const channel = [amount](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (static_cast<float>(y) - x) * amount));
    };
    return Color::rgb(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b));
}

WallTime local_now()
{
    std::time_t const now = std::time(nullptr);
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return WallTime { local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_wday, local.tm_hour,
        local.tm_min, local.tm_sec };
}

std::string trim(std::string const& text)
{
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    std::size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return text.substr(start, end - start);
}

std::string_view bytes_view(std::vector<std::uint8_t> const& bytes)
{
    return std::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
}

} // namespace

struct Browser::Impl {
    // A place in the page's text: a run in tree order and a code point
    // offset within it.
    struct TextPosition {
        std::size_t run = 0;
        std::size_t offset = 0;

        friend bool operator==(TextPosition const&, TextPosition const&) = default;
        friend bool operator<(TextPosition const& a, TextPosition const& b)
        {
            return a.run != b.run ? a.run < b.run : a.offset < b.offset;
        }
    };

    // The anchor stays where the selection began; the focus follows the
    // mouse or the keyboard. Equal positions are a caret with nothing selected.
    struct Selection {
        TextPosition anchor;
        TextPosition focus;
    };

    // One occurrence of the find bar's query in the page's text.
    struct Match {
        TextPosition start;
        TextPosition end;
    };

    struct Tab {
        std::vector<HistoryEntry> history;
        std::size_t index = 0;
        // The page's scripts and the page. The realm's wrappers point into
        // the document, so on every path that ends a tab the realm must go
        // first. Move-assignment runs the members forward — closing a tab
        // that is not the last one assigns its right-hand neighbour over it —
        // so the realm is declared first and the old one ends before the old
        // tree is freed. Destruction runs the members backwards, which the
        // destructor below settles by ending the realm itself. (With the
        // document first, the smoke script's close-tab 0 freed a tree while
        // its realm still held wrappers into it; the sanitizer lane caught it.)
        std::unique_ptr<bindings::Realm> realm;
        std::unique_ptr<dom::Document> document;
        // The page's Content Security Policy: from the entry's headers and
        // the document's <meta> elements, asked about everything the page
        // fetches, runs and applies. Shared with the closures that fetch
        // for the page, so a tab may move in the vector under them.
        std::shared_ptr<net::ContentSecurityPolicy> policy;
        // The container the tab is in: its cookie jar and its storage are
        // that container's, kept apart from every other's. Empty for the
        // default container.
        std::string container;
        // Pinned: an icon alone at the strip's left, with no close button,
        // that "close the others" leaves standing. The pinned tabs are
        // always the strip's first tabs, in a row.
        bool pinned = false;
        // The realm's mutation count the styles and layout below reflect;
        // when the realm has moved on, they are computed again before use.
        std::uint64_t page_mutations = 0;
        std::vector<std::string> console; // the page's console output, oldest first
        std::vector<css::SheetSource> sheets; // the page's stylesheets, kept so a resize can restyle
        std::string sheet_signature; // which elements carried them, so a script change elsewhere keeps them
        std::vector<text::PageFont> fonts; // the fonts its @font-face rules brought along
        std::optional<css::StyleSet> style_set; // the sheets compiled for style_media
        css::MediaContext style_media;
        css::StyleMap styles;
        layout::ImageMap images; // the page's pictures, decoded
        layout::BackgroundImages backgrounds; // the pictures its styles name as backgrounds
        DrawnFrames frames; // its frames' pictures as last drawn, by element
        layout::ControlStates controls; // what the user typed and toggled in the page's forms
        layout::LayoutResult layout;
        // The viewport those styles and that layout were made for: the content
        // area's size and the scale. The window changing size lays nothing
        // out: a tab is brought up to date (ensure_fresh) when something is
        // about to read it — the tab in front once a frame, however many
        // sizes the window went through since the last; the others when
        // they are shown, or when their scripts ask where something is.
        int laid_out_width = -1;
        int laid_out_height = -1;
        float laid_out_scale = 0;
        std::vector<layout::TextRun const*> runs; // the layout's runs in tree order
        std::optional<Selection> selection; // positions into `runs`; dropped with the layout
        std::vector<Match> matches; // the find bar's query in this tab, refreshed with the layout
        std::size_t current_match = 0;
        dom::Node const* inspected = nullptr; // devtools: the node under inspection
        int tree_scroll = 0; // devtools: the first tree line shown
        bool images_owed = false; // pictures left for a later pass, taken on the next tick
        int scroll_y = 0;
        // How far the reader has moved each box that scrolls, and how much
        // of that the fragment tree already carries: a fresh layout carries
        // none of it, so the two are brought together after every one.
        layout::ScrollOffsets scrolls;
        layout::ScrollOffsets applied;
        layout::ScrollOffset applied_page; // and how much of the page's own scroll it carries
        // The box the keyboard moves — the one the reader last took a wheel
        // or a scrollbar to. Null means the page itself.
        dom::Element const* scroller = nullptr;
        // The frames the reader is inside, by the container that shows each:
        // the one whose document holds the focused control, whose picture is
        // painted again as the control changes; the one the keyboard scrolls,
        // from the last wheel over it; the one the selection is in, with the
        // view the selection's positions index, so a view made again drops it.
        dom::Element const* focus_frame = nullptr;
        dom::Element const* scroller_frame = nullptr;
        dom::Element const* selection_frame = nullptr;
        std::weak_ptr<FrameView> selection_view;
        std::string status;
        // The page's icon, drawn in the tab, and the URL it came from, so a
        // page that changes without changing its icon fetches it once.
        std::shared_ptr<Bitmap const> favicon;
        std::string favicon_key;

        Tab() = default;
        Tab(Tab&&) = default;
        Tab& operator=(Tab&&) = default;
        ~Tab() { realm.reset(); } // before the document, whatever the member order says

        HistoryEntry const* current() const
        {
            return history.empty() ? nullptr : &history[index];
        }
        HistoryEntry* current() { return history.empty() ? nullptr : &history[index]; }
    };

    enum class Mode { Push, Replace, Reload };

    struct Pending {
        std::size_t tab = 0;
        net::Url url;
        Mode mode = Mode::Push;
        bool https_first = false;
    };
    // A window a page asked to open, kept until the next tick: the ask
    // comes in the middle of a script the shell is running for a tab, and
    // a tab added then would move every tab under the caller's feet.
    struct PendingWindow {
        std::string container;
        net::Url url;
        std::size_t opener = 0; // the tab whose page asked, as it stood then
    };

    // A tab that was closed, kept so that it can come back where it stood:
    // its history with the pages' bytes, so that nothing is fetched again.
    struct ClosedTab {
        std::vector<HistoryEntry> history;
        std::size_t index = 0;
        std::string container;
        std::size_t position = 0; // where it stood in the strip
        // Its icon, so that the tab wears it the moment it is back.
        std::shared_ptr<Bitmap const> favicon;
        std::string favicon_key;
        bool pinned = false; // it comes back as it was
    };

    enum class Hover {
        None,
        Back,
        Forward,
        Reload,
        Reader,
        NewTab,
        Tab,
        TabClose,
        Address,
        FindBox,
        DevtoolsTree,
        DevtoolsStyles,
        Content,
        Minimize,
        Maximize,
        WindowClose,
        DragHandle,
        Palette, // the command palette's own box
        PaletteRow, // one of its commands, hover_index says which of the rows shown
        MenuButton, // the main menu's button in the toolbar
        Menu, // an open menu's box, off its rows
        MenuRow, // an item that can be chosen: hover_level says which menu, hover_index which item
    };

    Loader& loader;
    // The window's frame, when the shell draws it (see Browser::WindowRequest).
    bool window_controls = false;
    Browser::WindowRequest window_request = Browser::WindowRequest::None;
    Theme theme; // the theme drawn with: base_theme scaled to the display
    Theme base_theme; // the theme as written
    // A surface's pictures as the theme file lists them, front to back,
    // decoded when the theme is put on — one that cannot be had is left
    // out and said so in theme_problems — and the whole stack as it lies
    // over the header at this window's width and scale, laid out the first
    // time it is painted and kept until one of those changes: a paint
    // composites one bitmap, however many pictures and tiles made it.
    struct ThemeLayers {
        struct Layer {
            Bitmap picture;
            ThemePicture how;
            std::optional<Bitmap> scaled; // the picture at `scaled_for` device px per CSS px, when that is not 1
            float scaled_for = 0;
        };
        std::vector<Layer> layers;
        std::optional<Bitmap> laid;
        float laid_scale = 0;
    };
    ThemeLayers frame_pictures;
    ThemeLayers toolbar_pictures;
    ThemeLayers tab_background_pictures;
    std::vector<std::string> theme_problems;
    // Whether this window is the one in front: a theme may give the frame
    // of one that is not a color of its own.
    bool window_active = true;
    // The toolbar button the left button went down on and is still held
    // over: a theme may color a button that is held apart from one that is
    // pointed at.
    Hover pressed = Hover::None;
    float scale = 1; // device px per CSS px; the window's sizes and coordinates are device px
    std::string downloads_directory;
    // The reader's own themes (the profile's themes folder): where a theme
    // of another browser's, downloaded, is converted into. Empty: none is.
    std::string user_themes_directory;
    // Where about:themes reads Firefox's themes from: the add-ons site's
    // public search endpoint (a script's harness names a file instead).
    std::string theme_gallery_api = "https://addons.mozilla.org/api/v5/addons/search/";
    // Set by a download that turned out to be a theme and was put on: the
    // page the reader was on stays, and nothing was saved.
    bool theme_adopted = false;
    // What the themes page says at its top the next time it is drawn: the
    // theme just put on from it.
    std::string themes_notice;
    int width;
    int height;
    std::vector<Tab> tabs;
    std::size_t active = 0;
    std::vector<Pending> pending;
    std::vector<PendingWindow> pending_windows;
    // The ceiling on each page's script heap, in bytes; 0 is none. A page
    // opened from now on has it (Browser::set_js_heap_limit).
    std::size_t js_heap_limit = 0;
    // The tabs closed last, the newest at the back, as many as the browsers
    // keep: Ctrl+Shift+T brings the newest back.
    static constexpr std::size_t closed_tabs_kept = 25;
    std::vector<ClosedTab> closed_tabs;
    // The tabs the tab in front has opened since it came to the front: each
    // goes after the last, so that three links opened from one page stand
    // beside it in the order they were opened.
    std::size_t opened_beside = 0;
    // The containers the shell offers, in order (see Browser::Container).
    std::vector<Browser::Container> containers;
    // Every page's localStorage, an area per container and origin (the
    // key is the container's name, a newline, the origin), kept for as
    // long as the shell runs and handed to each realm at that origin;
    // node-based, so a realm's pointer into it holds.
    std::map<std::string, bindings::StorageArea> storage;

    std::string address;
    bool address_focus = false;
    bool select_all = false;
    std::size_t caret = 0;
    // An input method's composing text in the shell's own fields (a
    // control's lives in its tab's ControlStates), and which field it is in.
    std::string preedit;
    enum class PreeditOwner { None, Address, Find } preedit_owner = PreeditOwner::None;

    int mouse_x = -1;
    int mouse_y = -1;
    Hover hover = Hover::None;
    std::size_t hover_index = 0;
    std::size_t hover_level = 0; // which of the open menus, for Hover::MenuRow
    std::optional<net::Url> hover_link;
    dom::Element const* hover_link_frame = nullptr; // the frame the link under the pointer is in, when it is in one
    std::string hover_link_target; // that link's target attribute
    // Until when the reader's last click or key counts as a gesture a page
    // may open a window on.
    std::chrono::steady_clock::time_point activation_until {};
    bool selecting = false; // the left button went down on page text and is still held
    // The scrollbar thumb the left button took hold of, and where along it.
    struct BarDrag {
        dom::Element const* box = nullptr;
        bool vertical = true;
        float grab = 0;
    };
    std::optional<BarDrag> bar_drag;

    // The find bar: shared by the tabs, its matches kept per tab.
    bool find_open = false;
    bool find_focus = false;
    std::string find_query;
    std::size_t find_caret = 0; // bytes into find_query
    bool find_select_all = false;

    // Keyboard link-hints: a label on every visible link while they show.
    struct Hint {
        std::string label;
        net::Url url;
        float x = 0; // page coordinates of the link's first run
        float y = 0;
    };
    bool hints_active = false;
    std::string hint_typed;
    std::vector<Hint> hints;

    bool devtools_open = false; // the panel under the content: DOM tree, box and style

    // The command palette: the commands it offers, built when it opens;
    // which of them the query's words match, in order; and the one
    // highlighted among those.
    struct Command {
        std::string label;
        std::function<void()> run;
    };
    bool palette_open = false;
    std::string palette_query;
    std::size_t palette_caret = 0; // bytes into palette_query
    bool palette_select_all = false;
    std::vector<Command> palette_commands;
    std::vector<std::size_t> palette_matches; // indices into palette_commands
    std::size_t palette_index = 0; // into palette_matches
    static constexpr std::size_t palette_rows_shown = 8;
    std::vector<Browser::ThemePreset> theme_presets;
    std::optional<std::string> theme_request; // a preset's file the reader chose, until the host takes it

    // Menus: the one a right click, the Menu key or the toolbar's button
    // opened, and the submenus open from it, outermost first. An item
    // without a label is a separator; one with children opens them as a
    // submenu instead of running. What an item does is settled when its
    // menu is built, from values — a URL, a tab's index, a window point —
    // never from a pointer into a page, which may be gone by the choosing.
    struct MenuItem {
        std::string label;
        std::string shortcut; // shown at the row's right end, for the reader's memory only
        std::function<void()> run;
        bool enabled = true;
        bool checked = false;
        std::vector<MenuItem> children;

        bool separator() const { return label.empty(); }
        bool choosable() const { return !separator() && enabled; }
    };
    struct MenuLevel {
        std::vector<MenuItem> items;
        // Where it hangs from: its top left corner at (x, y), or, for a
        // submenu, beside the row of its parent that opened it.
        int x = 0;
        int y = 0;
        bool right_aligned = false; // (x, y) is its top right corner instead
        std::optional<std::size_t> parent_row;
        std::optional<std::size_t> highlighted;
    };
    std::vector<MenuLevel> menus;
    bool main_menu_open = false; // the open menu is the toolbar button's, which stays lit
    // The button whose press opened the menu, until it comes up: held down
    // and let go over an item, it chooses that item.
    int menu_press_button = 0;
    int menu_press_x = 0; // where the pointer was when that button went down
    int menu_press_y = 0;

    // The clock the pages' timers run on (ms); wall time unless the replay
    // installs a virtual one. And when the current entry into script began,
    // for the slow-script stop.
    std::function<double()> clock;
    std::chrono::steady_clock::time_point script_started = std::chrono::steady_clock::now();
    // The local time the new-tab page opens on; the OS's unless the replay
    // fixes one. And how many new-tab pages have been made, for which of
    // the theme's pictures the next one opens on.
    std::function<WallTime()> wall_clock;
    std::size_t new_tabs_opened = 0;

    Bitmap frame;
    bool dirty = true;
    Profile profile; // the counts and the milliseconds, since the start

    // Adds what a scope took to one of the profile's sums when the scope
    // ends, and, when asked, leaves that one span's own length too.
    struct Stopwatch {
        double& sum;
        double* last;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        explicit Stopwatch(double& the_sum, double* the_last = nullptr)
            : sum(the_sum)
            , last(the_last)
        {
        }
        Stopwatch(Stopwatch const&) = delete;
        Stopwatch& operator=(Stopwatch const&) = delete;
        ~Stopwatch()
        {
            double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            sum += ms;
            if (last)
                *last = ms;
        }
    };

    Impl(Loader& the_loader, Theme the_theme, int the_width, int the_height)
        : loader(the_loader)
        , theme(std::move(the_theme))
        , base_theme(theme)
        , width(std::max(the_width, 1))
        , height(std::max(the_height, 1))
        , frame(width, height, theme.chrome_background)
    {
        load_theme_pictures();
        add_blank_tab();
    }

    // --- Tabs and history -----------------------------------------------------

    Tab* active_tab() { return tabs.empty() ? nullptr : &tabs[std::min(active, tabs.size() - 1)]; }
    Tab const* active_tab() const
    {
        return tabs.empty() ? nullptr : &tabs[std::min(active, tabs.size() - 1)];
    }

    static HistoryEntry blank_entry()
    {
        HistoryEntry entry;
        entry.url = *net::parse_url("about:blank");
        entry.final_url = entry.url;
        entry.content_type = "text/html";
        entry.status = 200;
        return entry;
    }

    void add_blank_tab(std::string container = {})
    {
        Tab tab;
        tab.container = std::move(container);
        tab.history.push_back(blank_entry());
        std::size_t const at = insert_tab(tabs.size(), std::move(tab), true);
        render(tabs[at]);
        sync_address();
    }

    // A tab put into the strip at a place, in front or behind the one that
    // is. Whatever names a tab by where it stands — a load under way, the
    // tab in front — moves along with the tabs pushed right. Returns where
    // it stands.
    std::size_t insert_tab(std::size_t position, Tab tab, bool to_front)
    {
        close_menus(); // a tab's menu names its tab by where it stood
        position = std::min(position, tabs.size());
        bool const first = tabs.empty();
        tabs.insert(tabs.begin() + static_cast<std::ptrdiff_t>(position), std::move(tab));
        for (Pending& load : pending) {
            if (load.tab >= position)
                ++load.tab;
        }
        for (PendingWindow& window : pending_windows) {
            if (window.opener >= position)
                ++window.opener;
        }
        if (to_front || first) {
            active = position;
            opened_beside = 0;
        } else if (active >= position) {
            ++active;
        }
        dirty = true;
        return position;
    }

    // --- Containers -----------------------------------------------------------

    Browser::Container const* container_named(std::string_view name) const
    {
        if (name.empty())
            return nullptr;
        for (Browser::Container const& container : containers)
            if (container.name == name)
                return &container;
        return nullptr;
    }

    // The container after the active tab's, in the order offered; after
    // the last comes the default.
    std::string next_container() const
    {
        Tab const* const tab = active_tab();
        if (!tab || containers.empty())
            return {};
        if (tab->container.empty())
            return containers.front().name;
        for (std::size_t i = 0; i < containers.size(); ++i) {
            if (containers[i].name == tab->container)
                return i + 1 < containers.size() ? containers[i + 1].name : std::string();
        }
        return containers.front().name;
    }

    // --- Storage ----------------------------------------------------------------

    static std::string storage_key(std::string_view container, std::string_view origin)
    {
        return std::string(container) + "\n" + std::string(origin);
    }

    bindings::StorageArea* storage_area(std::string_view container, std::string_view origin)
    {
        return &storage[storage_key(container, origin)];
    }

    std::string storage_json() const
    {
        std::string out = "{\n  \"version\": 1,\n  \"areas\": [\n";
        bool first = true;
        for (auto const& [key, area] : storage) {
            if (area.items.empty())
                continue;
            std::size_t const newline = key.find('\n');
            std::string const container = key.substr(0, newline);
            std::string const origin = newline == std::string::npos ? std::string() : key.substr(newline + 1);
            out += first ? "    {" : ",\n    {";
            first = false;
            out += "\"container\": " + json_quoted(container) + ", \"origin\": " + json_quoted(origin) + ", \"items\": [";
            for (std::size_t i = 0; i < area.items.size(); ++i) {
                out += i == 0 ? "\n      [" : ",\n      [";
                out += json_quoted(area.items[i].first) + ", " + json_quoted(area.items[i].second) + "]";
            }
            out += area.items.empty() ? "]}" : "\n    ]}";
        }
        out += first ? "  ]\n}\n" : "\n  ]\n}\n";
        return out;
    }

    bool restore_storage(std::string_view text)
    {
        std::optional<JsonValue> const parsed = JsonValue::parse(text);
        if (!parsed || !parsed->is_object())
            return false;
        JsonValue const* const areas = parsed->get("areas");
        if (!areas || !areas->is_array())
            return false;
        for (JsonValue const& area_value : areas->as_array()) {
            if (!area_value.is_object())
                continue;
            JsonValue const* const origin = area_value.get("origin");
            JsonValue const* const items = area_value.get("items");
            if (!origin || !origin->is_string() || !items || !items->is_array())
                continue;
            std::string container;
            if (JsonValue const* const name = area_value.get("container"); name && name->is_string())
                container = name->as_string();
            bindings::StorageArea& area = *storage_area(container, origin->as_string());
            for (JsonValue const& item : items->as_array()) {
                if (!item.is_array() || item.as_array().size() != 2 || !item.as_array()[0].is_string()
                    || !item.as_array()[1].is_string())
                    continue;
                std::string const& key = item.as_array()[0].as_string();
                bool replaced = false;
                for (auto& [existing, value] : area.items) {
                    if (existing == key) {
                        value = item.as_array()[1].as_string();
                        replaced = true;
                    }
                }
                if (!replaced)
                    area.items.emplace_back(key, item.as_array()[1].as_string());
            }
        }
        return true;
    }

    std::uint64_t storage_changes() const
    {
        std::uint64_t total = 0;
        for (auto const& [key, area] : storage)
            total += area.changes;
        return total;
    }

    // --- Sessions -------------------------------------------------------------

    static std::string json_quoted(std::string_view text)
    {
        std::string out = "\"";
        for (char const ch : text) {
            auto const c = static_cast<unsigned char>(ch);
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += ch;
            }
        }
        out += '"';
        return out;
    }

    std::string session_json() const
    {
        std::string out = "{\n  \"version\": 1,\n  \"active\": " + std::to_string(std::min(active, tabs.empty() ? 0 : tabs.size() - 1))
            + ",\n  \"tabs\": [\n";
        // A tab as the session writes one: where it is in its history, its
        // container, `more` of its own, and every entry of the history.
        auto const write_tab = [&out](std::size_t index, std::string const& container, std::string const& more,
                                   std::vector<HistoryEntry> const& history, bool last) {
            out += "    {\"index\": " + std::to_string(index) + ", \"container\": " + json_quoted(container) + more
                + ", \"entries\": [\n";
            for (std::size_t e = 0; e < history.size(); ++e) {
                HistoryEntry const& entry = history[e];
                out += "      {\"url\": " + json_quoted(entry.url.serialize()) + ", \"final_url\": "
                    + json_quoted(entry.final_url.serialize()) + ", \"title\": " + json_quoted(entry.title)
                    + ", \"scroll\": " + std::to_string(entry.scroll_y) + "}";
                out += e + 1 < history.size() ? ",\n" : "\n";
            }
            out += "    ]}";
            out += last ? "\n" : ",\n";
        };
        std::string const pinned_member = ", \"pinned\": true"; // written for a pinned tab, and only for one
        for (std::size_t t = 0; t < tabs.size(); ++t)
            write_tab(tabs[t].index, tabs[t].container, tabs[t].pinned ? pinned_member : std::string(), tabs[t].history,
                t + 1 == tabs.size());
        out += "  ]";
        // The tabs closed last, the newest last, each with where it stood:
        // Ctrl+Shift+T brings them back after a restart as before it.
        if (!closed_tabs.empty()) {
            out += ",\n  \"closed\": [\n";
            for (std::size_t t = 0; t < closed_tabs.size(); ++t) {
                ClosedTab const& closed = closed_tabs[t];
                write_tab(closed.index, closed.container,
                    ", \"position\": " + std::to_string(closed.position) + (closed.pinned ? pinned_member : std::string()),
                    closed.history, t + 1 == closed_tabs.size());
            }
            out += "  ]";
        }
        out += "\n}\n";
        return out;
    }

    // The tabs a session describes, or none when the text is not one. An
    // entry comes back unloaded — its page fetched again when shown — but
    // for about:blank, which is nothing to fetch.
    // `list` names which of the session's lists: its open tabs, or the ones
    // closed last, whose places in the strip go to `positions`.
    static std::vector<Tab> tabs_of_session(JsonValue const& session, std::string_view list = "tabs",
        std::vector<std::size_t>* positions = nullptr)
    {
        std::vector<Tab> restored;
        JsonValue const* const tabs_value = session.get(list);
        if (!tabs_value || !tabs_value->is_array())
            return restored;
        for (JsonValue const& tab_value : tabs_value->as_array()) {
            JsonValue const* const entries = tab_value.is_object() ? tab_value.get("entries") : nullptr;
            if (!entries || !entries->is_array())
                continue;
            Tab tab;
            for (JsonValue const& entry_value : entries->as_array()) {
                JsonValue const* const url_value = entry_value.is_object() ? entry_value.get("url") : nullptr;
                if (!url_value || !url_value->is_string())
                    continue;
                std::optional<net::Url> const url = net::parse_url(url_value->as_string());
                if (!url)
                    continue;
                HistoryEntry entry = is_about_blank(*url) ? blank_entry() : HistoryEntry {};
                entry.url = *url;
                entry.final_url = *url;
                entry.unloaded = !is_about_blank(*url);
                if (JsonValue const* const final = entry_value.get("final_url"); final && final->is_string()) {
                    if (std::optional<net::Url> const parsed = net::parse_url(final->as_string()))
                        entry.final_url = *parsed;
                }
                if (JsonValue const* const title = entry_value.get("title"); title && title->is_string())
                    entry.title = title->as_string();
                if (JsonValue const* const scroll = entry_value.get("scroll"); scroll && scroll->is_number())
                    entry.scroll_y = std::max(0, static_cast<int>(std::min(scroll->as_number(), 1.0e9)));
                tab.history.push_back(std::move(entry));
            }
            if (tab.history.empty())
                continue;
            tab.index = tab.history.size() - 1;
            if (JsonValue const* const index = tab_value.get("index"); index && index->is_number() && index->as_number() >= 0)
                tab.index = std::min(static_cast<std::size_t>(index->as_number()), tab.history.size() - 1);
            if (JsonValue const* const container = tab_value.get("container"); container && container->is_string())
                tab.container = container->as_string();
            if (JsonValue const* const pinned = tab_value.get("pinned"); pinned && pinned->is_bool())
                tab.pinned = pinned->as_bool();
            tab.scroll_y = tab.history[tab.index].scroll_y;
            restored.push_back(std::move(tab));
            if (positions) {
                JsonValue const* const position = tab_value.get("position");
                positions->push_back(position && position->is_number() && position->as_number() >= 0
                        ? static_cast<std::size_t>(std::min(position->as_number(), 1.0e6))
                        : 0);
            }
        }
        return restored;
    }

    bool restore_session(std::string_view text)
    {
        std::optional<JsonValue> const session = JsonValue::parse(text);
        if (!session || !session->is_object())
            return false;
        std::vector<Tab> restored = tabs_of_session(*session);
        if (restored.empty())
            return false;
        // The old tabs go, their loads with them; the find bar and the
        // hints belonged to pages that are gone.
        pending.clear();
        blur_address();
        find_open = false;
        find_focus = false;
        hints_active = false;
        hints.clear();
        menus.clear();
        main_menu_open = false;
        tabs = std::move(restored);
        active = 0;
        opened_beside = 0;
        if (JsonValue const* const active_value = session->get("active"); active_value && active_value->is_number() && active_value->as_number() >= 0)
            active = std::min(static_cast<std::size_t>(active_value->as_number()), tabs.size() - 1);
        // The pinned tabs are the strip's first, whatever order a file says:
        // each one found after a tab that is not pinned moves up to the row.
        // (Nothing is drawn or loading yet: the tabs move, and the one in
        // front is followed; there is nothing else to tell.)
        std::size_t row = 0;
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            if (!tabs[i].pinned)
                continue;
            if (i != row) {
                std::rotate(tabs.begin() + static_cast<std::ptrdiff_t>(row), tabs.begin() + static_cast<std::ptrdiff_t>(i),
                    tabs.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                if (active == i)
                    active = row;
                else if (active >= row && active < i)
                    ++active;
            }
            ++row;
        }
        // The tabs closed last come over too, the newest of them kept when
        // the file lists more than are kept.
        closed_tabs.clear();
        std::vector<std::size_t> positions;
        std::vector<Tab> closed = tabs_of_session(*session, "closed", &positions);
        std::size_t const from = closed.size() > closed_tabs_kept ? closed.size() - closed_tabs_kept : 0;
        for (std::size_t i = from; i < closed.size(); ++i)
            closed_tabs.push_back(ClosedTab { std::move(closed[i].history), closed[i].index,
                std::move(closed[i].container), positions[i], nullptr, {}, closed[i].pinned });
        // The active tab shows at once — its title until its page arrives —
        // and its page is queued like a navigation; the others wait to be
        // shown.
        render(tabs[active]);
        ensure_loaded(active);
        sync_address();
        refresh_hover();
        dirty = true;
        return true;
    }

    // A tab whose current entry a session restored fetches its page the
    // first time the tab is shown: queued like a navigation, replacing the
    // entry, its scroll position kept.
    void ensure_loaded(std::size_t index)
    {
        if (index >= tabs.size())
            return;
        HistoryEntry const* const entry = tabs[index].current();
        if (entry && entry->unloaded)
            queue(index, entry->url, Mode::Replace);
    }

    std::string display_url(HistoryEntry const& entry) const
    {
        if (is_about_blank(entry.url) || is_about_newtab(entry.url))
            return {};
        if (entry.internal && !entry.error.empty())
            return entry.url.serialize();
        return entry.final_url.serialize();
    }

    std::string tab_title(Tab const& tab) const
    {
        HistoryEntry const* const entry = tab.current();
        if (!entry)
            return "New Tab";
        if (!entry->title.empty())
            return entry->title;
        std::string const url = display_url(*entry);
        return url.empty() ? "New Tab" : url;
    }

    void sync_address()
    {
        if (address_focus)
            return;
        Tab const* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        address = entry ? display_url(*entry) : std::string();
        caret = address.size();
        select_all = false;
    }

    void focus_address(bool select_everything)
    {
        find_focus = false;
        address_focus = true;
        select_all = select_everything && !address.empty();
        caret = address.size();
        dirty = true;
    }

    void blur_address()
    {
        if (!address_focus)
            return;
        address_focus = false;
        select_all = false;
        dirty = true;
    }

    // --- Chrome geometry -------------------------------------------------------

    ChromeLayout layout_chrome() const
    {
        Theme const& t = theme;
        ChromeLayout c;
        c.tab_strip = Rect { 0, 0, width, t.tab_strip_height };
        c.toolbar = Rect { 0, t.tab_strip_height, width, t.toolbar_height };
        int content_top = t.tab_strip_height + t.toolbar_height;
        if (find_open) {
            // The find bar sits under the toolbar and takes its height from the content.
            c.find_bar = Rect { 0, content_top, width, t.find_height };
            int const box_width = std::min(std::clamp(width / 2, 120, 360), std::max(0, width - 2 * t.padding));
            c.find_box = Rect { t.padding, content_top + (t.find_height - t.address_height) / 2,
                box_width, t.address_height };
            content_top += t.find_height;
        }
        // The page has the window down to its foot. What the shell has to
        // say — where a link leads, what is loading, a notice — floats over
        // the page's bottom corner while there is something to say, and is
        // not there otherwise: no bar is kept for it.
        c.content = Rect { 0, content_top, width, std::max(0, height - content_top) };
        if (devtools_open) {
            // The panel takes the bottom of the content: the tree left, the
            // inspected element's box and style right.
            int const panel = std::min(t.devtools_height, c.content.height / 2);
            c.devtools = Rect { 0, c.content.bottom() - panel, width, panel };
            c.content.height -= panel;
            int const tree_width = width * 55 / 100;
            c.devtools_tree = Rect { 0, c.devtools.y + t.border_width, tree_width, panel - t.border_width };
            c.devtools_styles = Rect { tree_width + t.padding, c.devtools.y + t.border_width + t.padding / 2,
                std::max(0, width - tree_width - 2 * t.padding), panel - t.border_width };
        }
        if (std::string const said = status_text(); status_worth_showing(said)) {
            // As wide as its words, up to three fifths of the window.
            int const wanted = static_cast<int>(text_width(decode_utf8(said), t.status_font_size) + 0.5f) + 2 * t.padding;
            int const box_width = std::min(wanted, std::max(0, width * 3 / 5));
            c.status = Rect { 0, c.content.bottom() - t.status_height, box_width, t.status_height };
        }
        if (palette_open) {
            // The palette floats over the top of the content, centred: the
            // query box, then the commands shown, a row of text each; with
            // nothing matching, one row's room for saying so.
            int const box_width = std::min(std::max(240, width * 3 / 5), std::max(0, width - 4 * t.padding));
            int const row_height = static_cast<int>(t.font_size * 2);
            std::size_t const shown = std::min(palette_matches.size(), palette_rows_shown);
            int const rows_height = static_cast<int>(std::max<std::size_t>(shown, 1)) * row_height;
            int const palette_height = std::min(c.content.height - 2 * t.padding,
                3 * t.padding + t.address_height + rows_height + t.padding);
            c.palette = Rect { (width - box_width) / 2, c.content.y + 2 * t.padding, box_width, std::max(0, palette_height) };
            c.palette_box = Rect { c.palette.x + t.padding, c.palette.y + t.padding, box_width - 2 * t.padding, t.address_height };
            int const rows_top = c.palette_box.bottom() + t.padding;
            for (std::size_t i = 0; i < shown; ++i) {
                Rect const row { c.palette.x + t.padding, rows_top + static_cast<int>(i) * row_height,
                    box_width - 2 * t.padding, row_height };
                if (row.bottom() > c.palette.bottom() - t.padding)
                    break;
                c.palette_rows.push_back(row);
            }
        }

        // The window's controls, when the shell draws the frame: three
        // buttons at the strip's right end, which the tabs make room for.
        int reserved = 0;
        c.window_controls = window_controls;
        if (window_controls) {
            int const button_y = (t.tab_strip_height - t.button_size) / 2;
            c.close_button = Rect { width - t.padding - t.button_size, button_y, t.button_size, t.button_size };
            c.maximize_button = Rect { c.close_button.x - t.padding - t.button_size, button_y, t.button_size, t.button_size };
            c.minimize_button = Rect { c.maximize_button.x - t.padding - t.button_size, button_y, t.button_size, t.button_size };
            reserved = 3 * (t.button_size + t.padding);
        }
        int const count = static_cast<int>(tabs.size());
        // A pinned tab is its icon and the room about it, no more; the
        // others share what the pinned ones leave.
        int const pinned = static_cast<int>(pinned_count());
        int const pinned_width = t.tab_icon_size + 2 * (t.padding + 2);
        int const shared = count - pinned;
        int const available = width - 3 * t.padding - t.button_size - reserved - pinned * (pinned_width + t.tab_gap);
        int tab_width = shared > 0 ? (available - (shared - 1) * t.tab_gap) / shared : t.tab_max_width;
        tab_width = std::clamp(tab_width, t.tab_min_width, t.tab_max_width);
        // Attached, a tab stands on the toolbar; floating, it sits in the
        // middle of the strip with the strip showing above and below it.
        int const tab_y = t.tab_shape == TabShape::Floating ? (t.tab_strip_height - t.tab_height) / 2
                                                            : t.tab_strip_height - t.tab_height;
        int const close_size = std::max(10, t.tab_height - 12);
        int x = t.padding;
        for (int i = 0; i < count; ++i) {
            int const this_width = i < pinned ? pinned_width : tab_width;
            Rect const tab { x, tab_y, this_width, t.tab_height };
            c.tabs.push_back(tab);
            // A pinned tab has no close button: an empty rect, which nothing hits.
            c.tab_close_buttons.push_back(i < pinned ? Rect {}
                                                     : Rect { tab.right() - t.padding - close_size,
                                                           tab.y + (tab.height - close_size) / 2, close_size, close_size });
            x += this_width + t.tab_gap;
        }
        int const new_tab_x = std::min(x + t.padding / 2, width - t.padding - t.button_size - reserved);
        c.new_tab_button = Rect { new_tab_x, tab_y + (t.tab_height - t.button_size) / 2,
            t.button_size, t.button_size };

        int const button_y = c.toolbar.y + (t.toolbar_height - t.button_size) / 2;
        int const step = t.button_size + t.padding;
        c.back_button = Rect { t.padding, button_y, t.button_size, t.button_size };
        c.forward_button = Rect { t.padding + step, button_y, t.button_size, t.button_size };
        c.reload_button = Rect { t.padding + 2 * step, button_y, t.button_size, t.button_size };
        int const address_x = c.reload_button.right() + 2 * t.padding;
        // The main menu's button sits at the toolbar's right end, the
        // reader button before it; the address bar takes what lies between.
        c.menu_button = Rect { std::max(address_x, width - t.padding - t.button_size), button_y,
            t.button_size, t.button_size };
        c.reader_button = Rect { std::max(address_x, c.menu_button.x - step), button_y,
            t.button_size, t.button_size };
        c.address = Rect { address_x, c.toolbar.y + (t.toolbar_height - t.address_height) / 2,
            std::max(0, c.reader_button.x - 2 * t.padding - address_x), t.address_height };

        // The open menus, over everything: each submenu placed by the menu
        // it opened from.
        c.menus.reserve(menus.size());
        for (std::size_t i = 0; i < menus.size(); ++i)
            c.menus.push_back(lay_out_menu(menus[i], i > 0 ? &c.menus[i - 1] : nullptr));
        return c;
    }

    int max_scroll(Tab const& tab) const
    {
        ChromeLayout const c = layout_chrome();
        int const page_height = static_cast<int>(tab.layout.page_height + 0.5f);
        return std::max(0, page_height - c.content.height);
    }

    void set_scroll(Tab& tab, int y)
    {
        int const clamped = std::clamp(y, 0, max_scroll(tab));
        if (clamped == tab.scroll_y)
            return;
        tab.scroll_y = clamped;
        if (HistoryEntry* const entry = tab.current())
            entry->scroll_y = clamped;
        settle_scrolls(tab);
        dirty = true;
    }

    // Puts the reader's scrolling on the fragments — each box's own, then
    // the page's, then whatever sticks — so that everything reading the
    // tree, the painter included, sees the coordinates the content is
    // drawn at. After every change to any of them.
    void settle_scrolls(Tab& tab)
    {
        ChromeLayout const c = layout_chrome();
        layout::apply_scroll(tab.layout.root, tab.scrolls, tab.applied);
        layout::apply_page_scroll(tab.layout, static_cast<float>(std::max(1, c.content.width)),
            static_cast<float>(std::max(1, c.content.height)),
            layout::ScrollOffset { 0, static_cast<float>(tab.scroll_y) }, tab.applied_page,
            &tab.applied);
    }

    // --- Boxes that scroll ---------------------------------------------------------

    // The scrollport of a box that scrolls: its padding box, where its
    // content shows and where its bars are drawn.
    static bool in_scrollport(layout::Fragment const& box, float px, float py)
    {
        css::ComputedStyle const& s = *box.style;
        float const left = box.x + s.border_left.width;
        float const top = box.y + s.border_top.width;
        float const right = box.x + box.width - s.border_right.width;
        float const bottom = box.y + box.height - s.border_bottom.width;
        return px >= left && px < right && py >= top && py < bottom;
    }

    // A box a reader can move by hand: `hidden` clips and can be scrolled
    // by a script, but gives a reader no way in at all.
    static bool reader_can_scroll(layout::Fragment const& box)
    {
        if (!layout::is_scroll_container(box) || !box.element)
            return false;
        css::ComputedStyle const& s = *box.style;
        return s.overflow_x != css::Overflow::Hidden || s.overflow_y != css::Overflow::Hidden;
    }

    // How a box takes one push of the wheel: down the page when it has room
    // there, across it when that is the only way it can move — a wide table
    // or a strip of cards under a plain wheel, which is what a reader gets
    // everywhere else. Absent when the box has no room either way, and the
    // push then belongs to the box around it.
    static std::optional<layout::ScrollOffset> wheel_delta(
        layout::Fragment const& box, float delta, layout::ScrollOffsets const& at)
    {
        layout::ScrollOffset const now = layout::scroll_of(box, &at);
        if ((delta > 0 && now.y < box.scroll_range_y) || (delta < 0 && now.y > 0))
            return layout::ScrollOffset { 0, delta };
        if ((delta > 0 && now.x < box.scroll_range_x) || (delta < 0 && now.x > 0))
            return layout::ScrollOffset { delta, 0 };
        return std::nullopt;
    }

    // The box a wheel over this page point should move: the innermost one
    // whose scrollport holds the point and that has room left to take it. A
    // box with none passes the notch on to the box around it, and the page
    // answers when none of them can — the chaining every browser does.
    static layout::Fragment const* scroller_at(
        layout::Fragment const& box, float px, float py, float delta, layout::ScrollOffsets const& at)
    {
        bool const clips = layout::is_scroll_container(box);
        if (clips && !in_scrollport(box, px, py))
            return nullptr; // what is inside it is out of sight here
        for (layout::Fragment const& child : box.children) {
            if (layout::Fragment const* const found = scroller_at(child, px, py, delta, at))
                return found;
        }
        if (!clips || !reader_can_scroll(box))
            return nullptr;
        return wheel_delta(box, delta, at) ? &box : nullptr;
    }

    // Moves one box's content, and the fragments with it. False when the
    // box had no room to move that way, so the reader's push passes on.
    bool scroll_box_to(Tab& tab, layout::Fragment const& box, layout::ScrollOffset want)
    {
        if (!box.element)
            return false;
        want.x = std::clamp(want.x, 0.0f, box.scroll_range_x);
        want.y = std::clamp(want.y, 0.0f, box.scroll_range_y);
        layout::ScrollOffset& at = tab.scrolls[box.element];
        if (want.x == at.x && want.y == at.y)
            return false;
        at = want;
        settle_scrolls(tab);
        dirty = true;
        return true;
    }

    bool scroll_box_by(Tab& tab, layout::Fragment const& box, float dx, float dy)
    {
        layout::ScrollOffset const at = layout::scroll_of(box, &tab.scrolls);
        return scroll_box_to(tab, box, layout::ScrollOffset { at.x + dx, at.y + dy });
    }

    // The box the keyboard moves, when it is still on the page and still
    // has somewhere to go.
    layout::Fragment const* keyboard_scroller(Tab const& tab) const
    {
        if (!tab.scroller)
            return nullptr;
        layout::Fragment const* const box = fragment_for(tab.layout.root, tab.scroller);
        if (!box || (box->scroll_range_x <= 0 && box->scroll_range_y <= 0))
            return nullptr;
        return box;
    }

    // The innermost box that scrolls whose scrollport holds a page point,
    // whether or not it has room left to move.
    static layout::Fragment const* scrollport_at(layout::Fragment const& box, float px, float py)
    {
        bool const clips = layout::is_scroll_container(box);
        if (clips && !in_scrollport(box, px, py))
            return nullptr;
        for (layout::Fragment const& child : box.children) {
            if (layout::Fragment const* const found = scrollport_at(child, px, py))
                return found;
        }
        return clips ? &box : nullptr;
    }

    // The boxes that scroll between a rectangle of the page and the root,
    // outermost first: what has to move for that rectangle to be in sight.
    // `found` is set when the walk reached the fragment asked for.
    static bool scroll_chain(layout::Fragment const& box, layout::Fragment const* target,
        layout::TextRun const* run, std::vector<layout::Fragment const*>& chain)
    {
        if (&box == target)
            return true;
        for (layout::TextRun const& own : box.runs) {
            if (&own == run)
                return true;
        }
        bool const clips = layout::is_scroll_container(box);
        if (clips)
            chain.push_back(&box);
        for (layout::Fragment const& child : box.children) {
            if (scroll_chain(child, target, run, chain))
                return true;
        }
        if (clips)
            chain.pop_back();
        return false;
    }

    // Moves every box that scrolls around a rectangle of the page until the
    // rectangle is inside each of their scrollports, outermost first — an
    // inner box travels with the outer one, so its own numbers are only
    // settled once the outer one has stopped. What the page itself has to
    // do is left to the caller: the rectangle's coordinates are live and
    // have moved by the time this returns.
    void reveal_within_boxes(Tab& tab, std::vector<layout::Fragment const*> const& chain,
        std::function<Rect()> const& where)
    {
        for (layout::Fragment const* box : chain) {
            css::ComputedStyle const& s = *box->style;
            Rect const target = where();
            float const port_left = box->x + s.border_left.width;
            float const port_top = box->y + s.border_top.width;
            float const port_right = box->x + box->width - s.border_right.width;
            float const port_bottom = box->y + box->height - s.border_bottom.width;
            // A target too big for the scrollport is shown from its start
            // edge: seeing its end and nothing else is no use to a reader.
            float dx = 0;
            if (static_cast<float>(target.x) < port_left
                || static_cast<float>(target.width) > port_right - port_left)
                dx = static_cast<float>(target.x) - port_left;
            else if (static_cast<float>(target.right()) > port_right)
                dx = static_cast<float>(target.right()) - port_right;
            float dy = 0;
            if (static_cast<float>(target.y) < port_top
                || static_cast<float>(target.height) > port_bottom - port_top)
                dy = static_cast<float>(target.y) - port_top;
            else if (static_cast<float>(target.bottom()) > port_bottom)
                dy = static_cast<float>(target.bottom()) - port_bottom;
            if (dx != 0 || dy != 0)
                scroll_box_by(tab, *box, dx, dy);
        }
    }

    // A scrollbar under a page point: which box's, which axis, and whether
    // the pointer took hold of the thumb or of the track beside it.
    struct BarHit {
        layout::Fragment const* box = nullptr;
        bool vertical = true;
        bool on_thumb = false;
        float grab = 0; // how far into the thumb the pointer took hold
    };

    static std::optional<BarHit> bar_at(layout::Fragment const& box, float px, float py,
        layout::ScrollOffsets const& at)
    {
        bool const clips = layout::is_scroll_container(box);
        if (clips && !in_scrollport(box, px, py))
            return std::nullopt;
        for (layout::Fragment const& child : box.children) {
            if (std::optional<BarHit> const found = bar_at(child, px, py, at))
                return found;
        }
        if (!clips || !reader_can_scroll(box))
            return std::nullopt;
        layout::ScrollOffset const offset = layout::scroll_of(box, &at);
        int const x = static_cast<int>(px);
        int const y = static_cast<int>(py);
        if (std::optional<paint::ScrollbarGeometry> const bar
            = paint::vertical_scrollbar(box, offset)) {
            if (bar->track.contains(x, y)) {
                return BarHit { &box, true, bar->thumb.contains(x, y),
                    py - static_cast<float>(bar->thumb.y) };
            }
        }
        if (std::optional<paint::ScrollbarGeometry> const bar
            = paint::horizontal_scrollbar(box, offset)) {
            if (bar->track.contains(x, y)) {
                return BarHit { &box, false, bar->thumb.contains(x, y),
                    px - static_cast<float>(bar->thumb.x) };
            }
        }
        return std::nullopt;
    }

    // The pointer has the thumb: where along the track it now stands is
    // where the content stands along its own range.
    void drag_bar_to(Tab& tab, layout::Fragment const& box, bool vertical, float grab, float px,
        float py)
    {
        layout::ScrollOffset const at = layout::scroll_of(box, &tab.scrolls);
        std::optional<paint::ScrollbarGeometry> const bar
            = vertical ? paint::vertical_scrollbar(box, at) : paint::horizontal_scrollbar(box, at);
        if (!bar)
            return;
        float const track_start
            = static_cast<float>(vertical ? bar->track.y : bar->track.x);
        float const travel = static_cast<float>(vertical ? bar->track.height - bar->thumb.height
                                                         : bar->track.width - bar->thumb.width);
        if (travel <= 0)
            return;
        float const range = vertical ? box.scroll_range_y : box.scroll_range_x;
        float const along = std::clamp(((vertical ? py : px) - grab - track_start) / travel, 0.0f, 1.0f);
        layout::ScrollOffset want = at;
        (vertical ? want.y : want.x) = along * range;
        scroll_box_to(tab, box, want);
    }

    // A click beside the thumb moves by a scrollport at a time, the way it
    // does everywhere: towards the pointer.
    void page_bar(Tab& tab, BarHit const& hit)
    {
        layout::Fragment const& box = *hit.box;
        css::ComputedStyle const& s = *box.style;
        float const port = hit.vertical
            ? box.height - s.border_top.width - s.border_bottom.width
            : box.width - s.border_left.width - s.border_right.width;
        float const step = hit.grab < 0 ? -port : port;
        scroll_box_by(tab, box, hit.vertical ? 0.0f : step, hit.vertical ? step : 0.0f);
    }

    // --- Rendering a history entry ------------------------------------------------

    // What media queries see: the content area.
    css::MediaContext media_context()
    {
        ChromeLayout const c = layout_chrome();
        return css::MediaContext { static_cast<float>(std::max(1, c.content.width)),
            static_cast<float>(std::max(1, c.content.height)), scale };
    }

    // Device px to the CSS px a page's script sees, and back.
    float to_css_px(float device_px) const { return device_px / scale; }
    int to_device_px(double css_px) const { return static_cast<int>(std::lround(css_px * static_cast<double>(scale))); }

    // Styles depend on the viewport through media queries: computed when a
    // page arrives and again when the content area changes size.
    // The styles, computed again. `for_the_viewport` says the viewport is all
    // that has changed since they were last computed — the window changed
    // size, the document did not: then the sheets are read again only when a
    // breakpoint was crossed (a condition of theirs comes to something else
    // at the new size), and the styles are computed again only when they can
    // differ — a breakpoint crossed, or a length of theirs taken against the
    // viewport. Between breakpoints, for a page with no such length, a new
    // size costs a layout and nothing else.
    void restyle(Tab& tab, bool for_the_viewport = false)
    {
        if (!tab.document)
            return;
        text::FontManager::instance().set_page_fonts(tab.fonts);
        css::MediaContext const media = media_context();
        bool const same_viewport = tab.style_media.width == media.width && tab.style_media.height == media.height
            && tab.style_media.device_scale == media.device_scale;
        bool compiled = false;
        if (!tab.style_set || (!same_viewport && !tab.style_set->same_rules_for(media))) {
            Stopwatch const compiling(profile.sheets_ms);
            net::Url const* const page_url
                = tab.index < tab.history.size() ? &tab.history[tab.index].final_url : nullptr;
            tab.style_set.emplace(tab.sheets, media, page_url);
            if (net::ContentSecurityPolicy* const policy = tab.policy.get()) {
                tab.style_set->set_style_attribute_check([policy](dom::Element const&, std::string_view text) {
                    return !policy->inline_refusal(net::InlineKind::StyleAttribute, {}, text);
                });
            }
            compiled = true;
        } else if (!same_viewport) {
            tab.style_set->set_viewport(media.width, media.height);
        }
        tab.style_media = media;
        if (for_the_viewport && !compiled && !tab.styles.empty() && !tab.style_set->viewport_lengths())
            return; // the same rules, and nothing in them measured against the viewport
        {
            Stopwatch const resolving(profile.restyle_ms);
            tab.styles = css::resolve_styles(*tab.document, *tab.style_set);
        }
        ++profile.restyles;
    }

    // Fetches for a tab's frames through the loader, with the page as first
    // party: a frame's document, an object's or an embed's whatever its
    // status, which goes with it, as a tab shows a server's error page, and
    // anything else only when it arrived.
    FrameFetcher frame_fetcher(Tab& tab)
    {
        HistoryEntry const* const entry = tab.current();
        net::Url const page_url = entry ? entry->final_url : net::Url {};
        std::string const container = tab.container;
        return [this, page_url, container](net::Url const& url, net::Url const& from, net::ResourceKind kind,
                   net::RequestGuard const& guard) -> std::optional<FrameResponse> {
            net::FetchResult result
                = loader.load_subresource(url, page_url, referrer_for(&from, url), kind, guard, container);
            bool const document = kind == net::ResourceKind::Subdocument || kind == net::ResourceKind::Object;
            if (!result.response || (!document && result.response->status != 200))
                return std::nullopt;
            std::string const* const type = net::find_header(result.response->headers, "content-type");
            return FrameResponse { std::move(result.response->body), type ? *type : "", result.response->final_url,
                std::move(result.response->headers), result.response->status };
        };
    }

    void relayout(Tab& tab)
    {
        if (!tab.document)
            return;
        // The page's own fonts answer this tab's families; another tab may
        // have set its own since.
        text::FontManager::instance().set_page_fonts(tab.fonts);
        ChromeLayout const c = layout_chrome();
        tab.laid_out_width = c.content.width;
        tab.laid_out_height = c.content.height;
        tab.laid_out_scale = scale;
        layout::EmbeddedStates const embedded = tab.realm ? bindings::embedded_states(*tab.realm) : layout::EmbeddedStates {};
        {
            Stopwatch const laying_out(profile.relayout_ms);
            tab.layout = layout::layout_document(*tab.document, tab.styles,
                static_cast<float>(std::max(1, c.content.width)), &tab.images, &tab.controls,
                static_cast<float>(std::max(1, c.content.height)), scale, tab.realm ? &embedded : nullptr);
        }
        ++profile.relayouts;
        // The frames' documents, drawn into the layout; a frame drawn before
        // at the same size is taken as it was — but the frame whose document
        // holds the focused control is drawn again, and so is the one that
        // held it, since the caret and what was typed are in the picture.
        // Their documents set fonts of their own, so the page's are put back.
        if (HistoryEntry const* const entry = tab.current()) {
            dom::Element const* const focus_frame
                = tab.controls.focused ? frame_holding_in(tab.frames, tab.controls.focused->document()) : nullptr;
            for (dom::Element const* const container : { tab.focus_frame, focus_frame }) {
                if (container)
                    mark_stale(tab.frames, container);
            }
            tab.focus_frame = focus_frame;
            {
                Stopwatch const drawing(profile.frames_ms);
                draw_frames(entry->final_url, tab.layout, frame_fetcher(tab), scale, tab.policy.get(), &tab.frames, tab.realm.get(),
                    &tab.controls);
            }
            text::FontManager::instance().set_page_fonts(tab.fonts);
        }
        tab.scroll_y = std::clamp(tab.scroll_y, 0, max_scroll(tab));
        // A page laid out again is at every scrollport's origin: what the
        // reader had moved is put back on, held inside whatever the new
        // shape can reach. Before the runs are gathered, so the selection
        // and find-in-page read the coordinates the content is drawn at.
        tab.applied.clear();
        tab.applied_page = {};
        settle_scrolls(tab);
        // The selection pointed into the old layout's runs — the page's, or
        // a frame's whose view was made again; one in a frame whose view
        // stands keeps its positions and its picture.
        tab.runs.clear();
        gather_runs(tab.layout.root, tab.runs);
        bool keep_selection = false;
        if (tab.selection && tab.selection_frame) {
            std::shared_ptr<FrameView> const view = tab.selection_view.lock();
            keep_selection = view && view_of(tab, tab.selection_frame) == view.get();
        }
        if (!keep_selection) {
            tab.selection.reset();
            tab.selection_frame = nullptr;
            tab.selection_view.reset();
            selecting = false;
        }
        update_matches(tab);
        stop_hints(); // the labels pointed into the old layout
    }

    // --- Frames the reader is inside ----------------------------------------------

    // A live frame's view by its container, at any depth of the frames drawn.
    static FrameView* view_in(DrawnFrames const& frames, dom::Element const* container)
    {
        if (!container)
            return nullptr;
        if (auto const it = frames.find(container); it != frames.end())
            return it->second.view.get();
        for (auto const& [element, drawn] : frames) {
            if (!drawn.view)
                continue;
            if (FrameView* const found = view_in(drawn.view->frames, container))
                return found;
        }
        return nullptr;
    }
    static FrameView* view_of(Tab const& tab, dom::Element const* container) { return view_in(tab.frames, container); }

    // The container of the frame drawn live whose document this is, at any
    // depth; null for a document that is no frame's here.
    static dom::Element const* frame_holding_in(DrawnFrames const& frames, dom::Document const& document)
    {
        for (auto const& [element, drawn] : frames) {
            if (!drawn.view)
                continue;
            if (drawn.view->document == &document)
                return element;
            if (dom::Element const* const found = frame_holding_in(drawn.view->frames, document))
                return found;
        }
        return nullptr;
    }

    // Marks a frame's picture stale, and every frame's it is inside, since a
    // frame taken from the cache brings its inner frames' pictures with it.
    static bool mark_stale(DrawnFrames& frames, dom::Element const* container)
    {
        for (auto& [element, drawn] : frames) {
            if (element == container || (drawn.view && mark_stale(drawn.view->frames, container))) {
                drawn.stale = true;
                return true;
            }
        }
        return false;
    }

    // The realm whose document an element is in: a frame's, else the page's.
    bindings::Realm* realm_for(Tab& tab, dom::Element const& element)
    {
        if (FrameView* const view = view_of(tab, frame_holding_in(tab.frames, element.document())))
            return view->realm;
        return tab.realm.get();
    }

    // One frame on the way down from the page to a point or a container:
    // its view, its fragment in the tree above, the map its picture is kept
    // in, and — when found by a point — the point in its document.
    struct FrameStep {
        FrameView* view = nullptr;
        layout::Fragment* fragment = nullptr;
        dom::Element const* container = nullptr;
        DrawnFrames* holder = nullptr;
        float x = 0;
        float y = 0;
    };

    static layout::Fragment* fragment_for_mut(layout::Fragment& fragment, dom::Element const* target)
    {
        if (fragment.element == target)
            return &fragment;
        for (layout::Fragment& child : fragment.children) {
            if (layout::Fragment* const found = fragment_for_mut(child, target))
                return found;
        }
        return nullptr;
    }

    // The innermost frame drawn live whose box holds a point of a tree.
    static layout::Fragment* frame_fragment_at(layout::Fragment& fragment, DrawnFrames const& frames, float px, float py)
    {
        if (!reachable_within(fragment, px, py))
            return nullptr;
        for (layout::Fragment& child : fragment.children) {
            if (layout::Fragment* const hit = frame_fragment_at(child, frames, px, py))
                return hit;
        }
        if (!fragment.element || !fragment.image)
            return nullptr;
        auto const it = frames.find(fragment.element);
        if (it == frames.end() || !it->second.view)
            return nullptr;
        layout::Fragment::ImageBox const& box = *fragment.image;
        bool const inside = px >= box.x && px < box.x + box.width && py >= box.y && py < box.y + box.height;
        return inside ? &fragment : nullptr;
    }

    // The live frames under a point of a tree, outermost first, the point
    // carried into each one's document.
    static void frames_under(layout::Fragment& root, DrawnFrames& frames, float px, float py, std::vector<FrameStep>& chain)
    {
        layout::Fragment* const hit = frame_fragment_at(root, frames, px, py);
        if (!hit)
            return;
        auto const it = frames.find(hit->element);
        FrameView& view = *it->second.view;
        float const x = px - hit->image->x;
        float const y = py - hit->image->y + static_cast<float>(view.scroll_y);
        chain.push_back(FrameStep { &view, hit, hit->element, &frames, x, y });
        frames_under(view.layout.root, view.frames, x, y, chain);
    }

    // The live frames under a page point, outermost first; empty over the
    // page itself.
    static std::vector<FrameStep> frames_at(Tab& tab, float px, float py)
    {
        std::vector<FrameStep> chain;
        frames_under(tab.layout.root, tab.frames, px, py, chain);
        return chain;
    }

    static bool frames_to_in(layout::Fragment& root, DrawnFrames& frames, dom::Element const* container,
        std::vector<FrameStep>& chain)
    {
        for (auto& [element, drawn] : frames) {
            if (!drawn.view)
                continue;
            layout::Fragment* const fragment = fragment_for_mut(root, element);
            if (!fragment || !fragment->image)
                continue;
            chain.push_back(FrameStep { drawn.view.get(), fragment, element, &frames, 0, 0 });
            if (element == container || frames_to_in(drawn.view->layout.root, drawn.view->frames, container, chain))
                return true;
            chain.pop_back();
        }
        return false;
    }

    // The live frames down to a container, outermost first; empty when it
    // is not drawn live.
    static std::vector<FrameStep> frames_to(Tab& tab, dom::Element const* container)
    {
        std::vector<FrameStep> chain;
        if (container && !frames_to_in(tab.layout.root, tab.frames, container, chain))
            chain.clear();
        return chain;
    }

    // Where a chain's innermost document has its origin, in page
    // coordinates: a point in that document is the page point less this.
    static std::pair<float, float> origin_of(std::vector<FrameStep> const& chain)
    {
        float x = 0;
        float y = 0;
        for (FrameStep const& step : chain) {
            x += step.fragment->image->x;
            y += step.fragment->image->y - static_cast<float>(step.view->scroll_y);
        }
        return { x, y };
    }

    // Paints a chain's frames again from their views, innermost first, each
    // into its fragment and its keeper, so the page shows the change without
    // a layout; the selection's bands go onto the frame that holds it.
    void repaint_frames(Tab& tab, std::vector<FrameStep> const& chain)
    {
        for (std::size_t i = chain.size(); i-- > 0;) {
            FrameStep const& step = chain[i];
            std::shared_ptr<Bitmap const> picture = step.view->paint();
            if (tab.selection && tab.selection_frame == step.container) {
                Bitmap banded(step.view->width, step.view->height, Color::rgba(0, 0, 0, 0));
                banded.blit(*picture, 0, 0);
                auto const [start, end] = ordered(*tab.selection);
                paint_bands(banded, step.view->runs, start, end, theme.selection, static_cast<float>(step.view->scroll_y));
                picture = std::make_shared<Bitmap const>(std::move(banded));
            }
            step.fragment->image->bitmap = picture;
            if (auto const it = step.holder->find(step.container); it != step.holder->end())
                it->second.bitmap = picture;
        }
        text::FontManager::instance().set_page_fonts(tab.fonts);
        dirty = true;
    }

    // Moves a chain's innermost document down its frame; true when it moved.
    bool scroll_frame_to(Tab& tab, std::vector<FrameStep> const& chain, int y)
    {
        if (chain.empty())
            return false;
        FrameView& view = *chain.back().view;
        int const want = std::clamp(y, 0, view.max_scroll());
        if (want == view.scroll_y)
            return false;
        view.scroll_y = want;
        view.settle_scrolls();
        repaint_frames(tab, chain);
        return true;
    }
    bool scroll_frame_by(Tab& tab, std::vector<FrameStep> const& chain, int dy)
    {
        return !chain.empty() && scroll_frame_to(tab, chain, chain.back().view->scroll_y + dy);
    }

    // Moves a box inside a chain's innermost document; true when it moved.
    bool scroll_frame_box_by(Tab& tab, std::vector<FrameStep> const& chain, layout::Fragment const& box, float dx, float dy)
    {
        if (chain.empty() || !box.element)
            return false;
        FrameView& view = *chain.back().view;
        layout::ScrollOffset const at = layout::scroll_of(box, &view.scrolls);
        layout::ScrollOffset const want { std::clamp(at.x + dx, 0.0f, box.scroll_range_x),
            std::clamp(at.y + dy, 0.0f, box.scroll_range_y) };
        if (want.x == at.x && want.y == at.y)
            return false;
        view.scrolls[box.element] = want;
        view.settle_scrolls();
        repaint_frames(tab, chain);
        return true;
    }

    // Navigates a window of the tab — a frame's, else the page's — as the
    // document would navigate itself.
    void navigate_document(Tab& tab, dom::Document const& document, net::Url const& url)
    {
        if (FrameView* const view = view_of(tab, frame_holding_in(tab.frames, document))) {
            view->realm->navigate(url);
            return;
        }
        queue(index_of(tab), url, Mode::Push);
    }

    // The view of the frame a link's target names, at any depth; null when
    // no frame drawn live has that name.
    static FrameView* view_named(DrawnFrames const& frames, std::string const& name)
    {
        for (auto const& [element, drawn] : frames) {
            if (!drawn.view)
                continue;
            if (std::optional<std::string> const own = drawn.view->realm->target_name(); own && *own == name)
                return drawn.view.get();
            if (FrameView* const found = view_named(drawn.view->frames, name))
                return found;
        }
        return nullptr;
    }

    // The link under the pointer, followed: in this tab, or, from inside a
    // frame, where its target says — the frame itself, the frame around it,
    // the page, a frame by name, or a new tab.
    void follow_link()
    {
        Tab* const tab = active_tab();
        if (!tab || !hover_link)
            return;
        net::Url const url = *hover_link;
        std::string const target = hover_link_target;
        // The frames down to the link's own, empty for a link on the page.
        std::vector<FrameStep> const chain = frames_to(*tab, hover_link_frame);
        bool const self = target.empty() || ascii_ci_equals(target, "_self");
        if (ascii_ci_equals(target, "_blank")) {
            open_in_front(url);
        } else if (ascii_ci_equals(target, "_top") || (chain.empty() && (self || ascii_ci_equals(target, "_parent")))) {
            open(url);
        } else if (ascii_ci_equals(target, "_parent")) {
            if (chain.size() == 1)
                open(url);
            else
                chain[chain.size() - 2].view->realm->navigate(url);
        } else if (self) {
            chain.back().view->realm->navigate(url);
        } else if (FrameView* const named = view_named(tab->frames, target)) {
            named->realm->navigate(url);
        } else {
            open_in_front(url); // a name no frame goes by: a tab of its own
        }
        dirty = true;
    }

    // --- Scripts ---------------------------------------------------------------------

    static double wall_ms()
    {
        using namespace std::chrono;
        return static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count())
            / 1000.0;
    }

    double script_now() const { return clock ? clock() : wall_ms(); }

    // The tab a document belongs to. Tabs move when the vector grows, so a
    // hook keeps the document and looks its tab up each time.
    Tab* tab_of(dom::Document const* document)
    {
        for (Tab& tab : tabs) {
            if (tab.document.get() == document)
                return &tab;
        }
        return nullptr;
    }

    std::size_t index_of(Tab const& tab) const { return static_cast<std::size_t>(&tab - tabs.data()); }

    // Styles and layout are brought up to date with what scripts changed
    // since they were last computed. Called before anything reads them.
    void ensure_fresh(Tab& tab)
    {
        if (!tab.document)
            return;
        if (tab.realm && tab.page_mutations != tab.realm->tree_mutation_count()) {
            refresh_page(tab); // styles and layout both, for the viewport as it is
            dirty = true;
            return;
        }
        // Laid out for another viewport — the window has changed size or
        // scale since, or the find bar or the developer tools took room.
        ChromeLayout const c = layout_chrome();
        if (tab.laid_out_width != c.content.width || tab.laid_out_height != c.content.height
            || tab.laid_out_scale != scale) {
            restyle(tab, true);
            relayout(tab);
            if (&tab == active_tab())
                refresh_hover();
            dirty = true;
        }
    }

    // How many of a page's pictures one pass fetches: the page shows after
    // the first pass, and takes the rest a pass per tick.
    static constexpr std::size_t images_per_pass = 64;

    // A page's pictures fetched through the loader with the page as first
    // party, under its policy's guard.
    ImageFetcher image_fetcher(Tab& tab)
    {
        HistoryEntry const* const entry = tab.current();
        net::Url const page_url = entry ? entry->final_url : net::Url {};
        net::ContentSecurityPolicy* const policy = tab.policy.get();
        std::string const container = tab.container;
        return [this, page_url, policy, container](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
            net::RequestGuard const guard = policy ? policy->guard(net::ResourceKind::Image) : net::RequestGuard {};
            net::FetchResult result = loader.load_subresource(url, page_url, referrer_for(&page_url, url),
                net::ResourceKind::Image, guard, container);
            if (!result.response || result.response->status != 200)
                return std::nullopt;
            return std::move(result.response->body);
        };
    }

    // The next pass over a page's pictures, for a page shown before all of
    // them were in: the page is laid out again as they arrive.
    void continue_images(Tab& tab)
    {
        tab.images_owed = false;
        HistoryEntry const* const entry = tab.current();
        if (!entry || !tab.document)
            return;
        layout::EmbeddedStates const embedded = tab.realm ? bindings::embedded_states(*tab.realm) : layout::EmbeddedStates {};
        bool more = false;
        {
            Stopwatch const collecting(profile.images_ms);
            layout::ImageMap fresh = collect_images(*tab.document, &entry->final_url, image_fetcher(tab), media_context(),
                tab.realm ? &embedded : nullptr, ImagePass { &tab.images, images_per_pass, &more });
            for (auto& [element, image] : fresh)
                tab.images[element] = std::move(image);
        }
        tab.images_owed = more;
        relayout(tab);
        dirty = true;
    }

    // What the stylesheets are: the elements that carry them, so that a
    // script change elsewhere in the tree does not recompile every sheet.
    static std::string sheet_signature(dom::Node const& node)
    {
        std::string signature;
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            if (element.is_html("style") || element.is_svg("style")) {
                signature += "s" + std::to_string(reinterpret_cast<std::uintptr_t>(&element)) + ":";
                for (dom::Node const* child : element.children()) {
                    if (child->is_text())
                        signature += std::to_string(static_cast<dom::Text const*>(child)->data.size()) + ",";
                }
            } else if (element.is_html("link")) {
                signature += "l" + std::to_string(reinterpret_cast<std::uintptr_t>(&element)) + ":";
                if (dom::Attr const* href = element.find_attribute("href"))
                    signature += href->value;
                if (dom::Attr const* rel = element.find_attribute("rel"))
                    signature += rel->value;
                if (dom::Attr const* media = element.find_attribute("media"))
                    signature += media->value;
                signature += ";";
            }
        }
        for (dom::Node const* child : node.children())
            signature += sheet_signature(*child);
        return signature;
    }

    static bool has_unfetched_image(dom::Node const& node, layout::ImageMap const& images)
    {
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            if (element.is_html("img") && !images.contains(&element))
                return true;
        }
        for (dom::Node const* child : node.children()) {
            if (has_unfetched_image(*child, images))
                return true;
        }
        return false;
    }

    // The page's hooks: what its scripts ask of the shell. Each keeps the
    // document, never the Tab, and looks the tab up when called.
    std::unique_ptr<bindings::Realm> make_realm(Tab& tab, net::Url const& url)
    {
        dom::Document* const document = tab.document.get();
        net::Url const page_url = url;
        std::string const container = tab.container;
        bindings::HostHooks hooks;
        hooks.policy = tab.policy.get();
        hooks.fetch_script = [this, page_url, container](net::Url const& target, net::RequestGuard const& guard) -> std::optional<std::string> {
            net::FetchResult result = loader.load_subresource(target, page_url, referrer_for(&page_url, target),
                net::ResourceKind::Script, guard, container);
            if (!result.response || result.response->status != 200)
                return std::nullopt;
            return std::string(result.response->body.begin(), result.response->body.end());
        };
        hooks.fetch_resource = [this, page_url, container](net::Url const& target, net::ResourceRequest const& request,
                                   net::RequestGuard const& guard) {
            return loader.load_resource(target, page_url, referrer_for(&page_url, target), request, guard, container);
        };
        hooks.local_storage = [this, container](std::string const& origin) { return storage_area(container, origin); };
        hooks.now = [this] { return script_now(); };
        hooks.should_stop = [this] {
            return std::chrono::steady_clock::now() - script_started > std::chrono::seconds(10);
        };
        hooks.js_heap_limit = js_heap_limit;
        // A page over its heap's ceiling: the reader is told where the
        // shell says things, and the words stay until something else is said.
        hooks.out_of_memory = [this, document] {
            if (Tab* const owner = tab_of(document)) {
                owner->status = "This page ran out of memory: its scripts were stopped";
                dirty = true;
            }
        };
        // An element of a frame's document is measured in that document,
        // from the frame's own viewport: a frame's hooks are the page's.
        hooks.layout_box = [this, document](dom::Element const& element) -> std::optional<bindings::LayoutBox> {
            Tab* const owner = tab_of(document);
            if (!owner)
                return std::nullopt;
            ensure_fresh(*owner);
            FrameView const* const view = view_of(*owner, frame_holding_in(owner->frames, element.document()));
            std::optional<bindings::LayoutBox> box
                = bindings::find_element_box(view ? view->layout.root : owner->layout.root, element);
            if (box && scale != 1) {
                box->x = static_cast<float>(to_css_px(box->x));
                box->y = static_cast<float>(to_css_px(box->y));
                box->width = static_cast<float>(to_css_px(box->width));
                box->height = static_cast<float>(to_css_px(box->height));
            }
            return box;
        };
        hooks.computed_style = [this, document](dom::Element const& element) -> css::ComputedStyle const* {
            Tab* const owner = tab_of(document);
            if (!owner)
                return nullptr;
            ensure_fresh(*owner);
            FrameView const* const view = view_of(*owner, frame_holding_in(owner->frames, element.document()));
            css::StyleMap const& styles = view ? view->styles : owner->styles;
            auto const it = styles.find(&element);
            return it == styles.end() ? nullptr : &it->second;
        };
        hooks.navigate = [this, document](net::Url const& target) {
            if (Tab* const owner = tab_of(document))
                queue(index_of(*owner), target, Mode::Push);
        };
        // A window the page opens: a new tab in the page's container, with
        // no history behind it and so no referrer and no opener, only while
        // the reader's last click or key is fresh — opened on the next tick,
        // never in the middle of the script asking.
        hooks.open_window = [this, document](net::Url const& target, bool) {
            if (Tab const* const owner = tab_of(document))
                pending_windows.push_back(PendingWindow { owner->container, target, index_of(*owner) });
        };
        hooks.user_activation = [this] { return std::chrono::steady_clock::now() < activation_until; };
        hooks.scroll_to = [this, document](dom::Document const& from, int, int y) {
            Tab* const owner = tab_of(document);
            if (!owner)
                return;
            ensure_fresh(*owner);
            if (dom::Element const* const frame_container = frame_holding_in(owner->frames, from)) {
                scroll_frame_to(*owner, frames_to(*owner, frame_container), to_device_px(y));
                return;
            }
            set_scroll(*owner, to_device_px(y));
        };
        hooks.scroll_position = [this, document](dom::Document const& from) -> std::pair<int, int> {
            Tab* const owner = tab_of(document);
            if (!owner)
                return { 0, 0 };
            if (FrameView const* const view = view_of(*owner, frame_holding_in(owner->frames, from)))
                return { 0, static_cast<int>(std::lround(to_css_px(static_cast<float>(view->scroll_y)))) };
            return { 0, static_cast<int>(std::lround(to_css_px(static_cast<float>(owner->scroll_y)))) };
        };
        hooks.cookie_get = [this, page_url, container] { return loader.cookies_for(page_url, container); };
        hooks.cookie_set = [this, page_url, container](std::string_view line) { loader.set_cookie(page_url, line, container); };
        hooks.control_value = [this, document](dom::Element const& element) -> std::optional<std::string> {
            Tab* const owner = tab_of(document);
            if (!owner)
                return std::nullopt;
            auto const it = owner->controls.states.find(&element);
            if (it == owner->controls.states.end())
                return std::nullopt;
            return it->second.value;
        };
        hooks.set_control_value = [this, document](dom::Element const& element, std::string_view value) {
            if (Tab* const owner = tab_of(document)) {
                layout::ControlState& state = state_of(*owner, element);
                state.value = std::string(value);
                state.caret = decode_utf8(*state.value).size();
            }
        };
        hooks.control_checked = [this, document](dom::Element const& element) -> std::optional<bool> {
            Tab* const owner = tab_of(document);
            if (!owner)
                return std::nullopt;
            auto const it = owner->controls.states.find(&element);
            if (it == owner->controls.states.end())
                return std::nullopt;
            return it->second.checked;
        };
        hooks.set_control_checked = [this, document](dom::Element const& element, bool checked) {
            if (Tab* const owner = tab_of(document))
                state_of(*owner, element).checked = checked;
        };
        hooks.focus = [this, document](dom::Element const* element) {
            if (Tab* const owner = tab_of(document)) {
                owner->controls.focused = element;
                if (element)
                    caret_to_end(*owner, *element);
            }
        };
        hooks.focused = [this, document]() -> dom::Element const* {
            Tab* const owner = tab_of(document);
            return owner ? owner->controls.focused : nullptr;
        };
        hooks.image_size = [this, document](dom::Element const& element) -> std::optional<std::pair<int, int>> {
            Tab* const owner = tab_of(document);
            if (!owner)
                return std::nullopt;
            auto const it = owner->images.find(&element);
            if (it == owner->images.end() || !it->second.bitmap)
                return std::nullopt;
            // The picture's density is per device px; naturalWidth is CSS px.
            float const density = (it->second.density > 0 ? it->second.density : 1.0f) * scale;
            return std::pair<int, int> { static_cast<int>(std::lround(static_cast<float>(it->second.bitmap->width()) / density)),
                static_cast<int>(std::lround(static_cast<float>(it->second.bitmap->height()) / density)) };
        };
        hooks.image_decodes = [](std::vector<std::uint8_t> const& bytes) { return decode_image_bytes(bytes).has_value(); };
        hooks.submit_form = [this, document](dom::Element const& form, dom::Element const* submitter) {
            Tab* const owner = tab_of(document);
            HistoryEntry const* const entry = owner ? owner->current() : nullptr;
            if (!owner || !entry)
                return;
            // A form in a frame's document submits against that document's
            // URL, and lands in the frame.
            FrameView const* const view = view_of(*owner, frame_holding_in(owner->frames, form.document()));
            std::optional<net::Url> const target = get_submission_url(form,
                submitter ? submitter : default_submitter(form), &owner->controls, view ? view->realm->url() : entry->final_url);
            if (target && submission_allowed(*owner, *target))
                navigate_document(*owner, form.document(), *target);
        };
        hooks.console = [this, document](std::string_view level, std::string_view message) {
            std::string const line = std::string(level) + ": " + std::string(message);
            if (Tab* const owner = tab_of(document)) {
                if (owner->console.size() >= 500)
                    owner->console.erase(owner->console.begin());
                owner->console.push_back(line);
            }
            if (level == "error")
                std::fprintf(stderr, "[page] %s\n", line.c_str());
        };
        css::MediaContext const media = media_context();
        hooks.viewport_width = static_cast<float>(to_css_px(media.width));
        hooks.viewport_height = static_cast<float>(to_css_px(media.height));
        hooks.device_scale = scale;
        hooks.user_agent = std::string(net::user_agent());
        // A frame's document gets a realm of its own, fetched through the tab
        // that shows the page, by the framing rules its frames are drawn by.
        hooks.frame_document = [this, document](dom::Element const& iframe, net::Url const& base,
                                   net::ContentSecurityPolicy* policy, std::vector<bindings::FrameAncestor> const& ancestors,
                                   std::optional<net::Url> const& target) -> std::optional<bindings::FrameDocument> {
            Tab* const owner = tab_of(document);
            if (!owner)
                return std::nullopt;
            return frame_document_for(iframe, base, policy, ancestors, target, frame_fetcher(*owner));
        };
        return std::make_unique<bindings::Realm>(*document, url, std::move(hooks));
    }

    // Styles, pictures and layout from the document as it stands: after
    // the parse, and again whenever its scripts changed something.
    void refresh_page(Tab& tab)
    {
        HistoryEntry* const entry = tab.current();
        if (!entry || !tab.document)
            return;
        // The page's stylesheets come through the loader with the page as
        // first party and the usual referrer policy; a sheet that fails to
        // load is simply absent.
        net::Url const& page_url = entry->final_url;
        net::ContentSecurityPolicy* const policy = tab.policy.get();
        auto const fetch_kind = [&](net::ResourceKind kind) {
            return [&, kind](net::Url const& url, std::string_view nonce) -> std::optional<css::FetchedSheet> {
                net::RequestGuard const guard = policy ? policy->guard(kind, std::string(nonce)) : net::RequestGuard {};
                net::FetchResult result
                    = loader.load_subresource(url, page_url, referrer_for(&page_url, url), kind, guard, tab.container);
                if (!result.response || result.response->status != 200)
                    return std::nullopt;
                std::string const* header = net::find_header(result.response->headers, "content-type");
                return css::FetchedSheet { std::move(result.response->body), header ? *header : "" };
            };
        };
        auto const fetch_sheet = fetch_kind(net::ResourceKind::Stylesheet);
        auto const fetch_font = fetch_kind(net::ResourceKind::Font);
        std::string const signature = sheet_signature(*tab.document);
        if (signature != tab.sheet_signature || !tab.style_set) {
            Stopwatch const collecting(profile.sheets_ms);
            // The head's <meta> policies, for a page parsed without scripts.
            if (policy)
                bindings::adopt_meta_policies(*policy, *tab.document);
            css::InlineSheetCheck inline_check;
            if (policy) {
                inline_check = [policy](dom::Element const& style, std::string_view text) {
                    dom::Attr const* const nonce = style.find_attribute("nonce");
                    return !policy->inline_refusal(net::InlineKind::Style, nonce ? nonce->value : std::string(), text);
                };
            }
            tab.sheets = css::collect_stylesheets(*tab.document, &page_url, fetch_sheet, media_context(), inline_check);
            // The lists' element-hiding rules for this page, last, so their
            // !important beats the page's own.
            if (net::Blocklists const* const lists = loader.content_lists()) {
                if (std::optional<css::SheetSource> hiding = cosmetic_sheet(*lists, page_url, *tab.document))
                    tab.sheets.push_back(std::move(*hiding));
            }
            tab.fonts = css::collect_page_fonts(tab.sheets, fetch_font, media_context());
            tab.style_set.reset();
            tab.sheet_signature = signature;
        }
        restyle(tab);
        auto const fetch_image = [&](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
            net::RequestGuard const guard = policy ? policy->guard(net::ResourceKind::Image) : net::RequestGuard {};
            net::FetchResult result = loader.load_subresource(url, page_url, referrer_for(&page_url, url),
                net::ResourceKind::Image, guard, tab.container);
            if (!result.response || result.response->status != 200)
                return std::nullopt;
            return std::move(result.response->body);
        };
        // Pictures are decoded once: only a page with an <img> not seen yet
        // is walked again (the fetches come from the cache; the decoding
        // does not).
        // An object or an embed its realm decided shows a picture is walked
        // for too, once.
        layout::EmbeddedStates const embedded = tab.realm ? bindings::embedded_states(*tab.realm) : layout::EmbeddedStates {};
        bool const unfetched_embedded = std::any_of(embedded.begin(), embedded.end(), [&tab](auto const& decided) {
            return decided.second == layout::Embedded::Image && !tab.images.contains(decided.first);
        });
        {
            Stopwatch const collecting(profile.images_ms);
            if (tab.images.empty() || has_unfetched_image(*tab.document, tab.images) || unfetched_embedded) {
                // One pass now, so the page shows; the rest a pass per tick.
                bool more = false;
                layout::ImageMap fresh = collect_images(*tab.document, &page_url, fetch_image, media_context(),
                    tab.realm ? &embedded : nullptr, ImagePass { &tab.images, images_per_pass, &more });
                for (auto& [element, image] : fresh)
                    tab.images[element] = std::move(image);
                tab.images_owed = more;
            }
            // The backgrounds the styles name now that were not had before.
            for (auto& [url, bitmap] : collect_background_images(tab.styles, fetch_image, &tab.backgrounds))
                tab.backgrounds[url] = std::move(bitmap);
        }
        if (!entry->unloaded) // a restored entry keeps its saved title until its page arrives
            entry->title = find_title(*tab.document);
        // The page's icon: the last <link rel=icon>, else /favicon.ico on a
        // web scheme; fetched once per URL through the same loader, decoded
        // by what its bytes say, an ICO at the tab's size. A page with none,
        // or one whose icon fails, draws no icon and its title stays put.
        std::optional<net::Url> icon_url;
        if (std::string const href = find_icon_href(*tab.document); !href.empty())
            icon_url = net::parse_url(href, &page_url);
        else if (is_web_scheme(page_url.scheme) && !entry->internal)
            icon_url = net::parse_url("/favicon.ico", &page_url); // not for an error page: no request to a site that failed or was refused
        std::string const icon_key = icon_url ? icon_url->serialize() : std::string();
        if (icon_key != tab.favicon_key) {
            tab.favicon_key = icon_key;
            tab.favicon.reset();
            if (icon_url) {
                if (std::optional<std::vector<std::uint8_t>> const bytes = fetch_image(*icon_url);
                    bytes && bytes->size() <= max_icon_bytes) {
                    if (std::optional<Bitmap> decoded = decode_image_bytes(*bytes, theme.tab_icon_size))
                        tab.favicon = std::make_shared<Bitmap const>(std::move(*decoded));
                }
            }
        }
        relayout(tab);
        tab.page_mutations = tab.realm ? tab.realm->tree_mutation_count() : 0;
    }

    // (An entry a session restored keeps the title it was saved with until
    // its page arrives; the line above that reads the title is guarded.)

    void render(Tab& tab)
    {
        HistoryEntry* const entry = tab.current();
        // The realm goes before the document its wrappers point into.
        tab.realm.reset();
        tab.console.clear();
        if (!entry) {
            tab.document.reset();
            tab.scrolls.clear();
            tab.applied.clear();
            tab.scroller = nullptr;
            tab.frames.clear();
            return;
        }
        // The new-tab page is of the moment it is shown, not of its first
        // opening: coming back to it gets the time now and the next picture.
        if (is_about_newtab(entry->url))
            set_document(*entry, new_tab_document());
        std::string const& type = entry->content_type;
        std::string generated;
        std::string_view source = bytes_view(entry->bytes);
        bool const html = type.empty() || starts_with_ci(type, "text/html")
            || starts_with_ci(type, "application/xhtml");
        if (!html) {
            if (starts_with_ci(type, "text/"))
                generated = text_page(entry->final_url.serialize(), entry->bytes);
            else
                generated = unsupported_content_page(entry->final_url.serialize(), type,
                    entry->bytes.size());
            source = generated;
        }
        tab.document = std::make_unique<dom::Document>();
        tab.controls = {}; // a new document: nothing typed into it yet
        // ⛔ And nothing scrolled in it. These are keyed by element, and the
        // elements of the document being replaced are about to be freed: a
        // new document that happens to put an element at the same address
        // would inherit a scroll offset belonging to a page that is gone.
        tab.scrolls.clear();
        tab.applied.clear();
        tab.scroller = nullptr;
        tab.inspected = nullptr;
        tab.tree_scroll = 0;
        tab.images.clear();
        tab.backgrounds.clear();
        tab.frames.clear();
        tab.sheets.clear();
        tab.fonts.clear();
        tab.style_set.reset();
        tab.sheet_signature.clear();
        tab.styles.clear();
        tab.layout = layout::LayoutResult {};
        tab.runs.clear();
        tab.selection.reset();
        tab.page_mutations = ~std::uint64_t(0); // nothing computed yet: the first question computes
        tab.scroll_y = entry->scroll_y;
        // The page's policy, from its headers now and from its <meta>
        // elements as the parse meets them; every violation is a console
        // line of the page's.
        tab.policy = std::make_shared<net::ContentSecurityPolicy>(entry->final_url);
        tab.policy->set_reporter([this, document = tab.document.get()](std::string_view message) {
            if (Tab* const owner = tab_of(document)) {
                if (owner->console.size() >= 500)
                    owner->console.erase(owner->console.begin());
                owner->console.push_back("error: " + std::string(message));
            }
        });
        for (std::string const& header : entry->csp_headers)
            tab.policy->add_header(header, false);
        for (std::string const& header : entry->csp_report_only_headers)
            tab.policy->add_header(header, true);
        // The page is parsed with its scripts running, each as its end tag
        // goes by; a script that asks for a box gets the page laid out as
        // it stands. A document sandboxed without allow-scripts parses with
        // scripting off, so its <noscript> content shows.
        tab.realm = make_realm(tab, entry->final_url);
        script_started = std::chrono::steady_clock::now();
        html::parse_document_bytes_into(*tab.document, source,
            tab.policy->sandbox_allows_scripts() ? tab.realm.get() : nullptr);
        tab.realm->document_parsed();
        refresh_page(tab);
        if (entry->scroll_y == 0 && entry->final_url.fragment && !entry->final_url.fragment->empty())
            scroll_to_fragment(tab, *entry->final_url.fragment);
        dirty = true;
    }

    // The element a page point lands on: a text run's, a control's, else
    // the deepest box.
    dom::Element const* element_under(Tab const& tab, int x, int y) const
    {
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!point)
            return nullptr;
        dom::Element const* element = hit_run(tab.layout.root, point->first, point->second);
        if (!element)
            element = hit_control(tab.layout.root, point->first, point->second);
        if (!element)
            element = element_at_point(tab.layout.root, point->first, point->second);
        return element;
    }

    // A window key as a page sees it.
    static bindings::KeyInit key_init_for(KeyEvent const& key)
    {
        bindings::KeyInit init;
        init.ctrl = key.ctrl;
        init.shift = key.shift;
        init.alt = key.alt;
        auto const named = [&](char const* name, int code) {
            init.key = name;
            init.code = name;
            init.key_code = code;
        };
        switch (key.key) {
        case Key::Enter: named("Enter", 13); break;
        case Key::Escape: named("Escape", 27); break;
        case Key::Backspace: named("Backspace", 8); break;
        case Key::Delete: named("Delete", 46); break;
        case Key::Tab: named("Tab", 9); break;
        case Key::Space:
            init.key = " ";
            init.code = "Space";
            init.key_code = 32;
            break;
        case Key::Left: named("ArrowLeft", 37); break;
        case Key::Right: named("ArrowRight", 39); break;
        case Key::Up: named("ArrowUp", 38); break;
        case Key::Down: named("ArrowDown", 40); break;
        case Key::Home: named("Home", 36); break;
        case Key::End: named("End", 35); break;
        case Key::PageUp: named("PageUp", 33); break;
        case Key::PageDown: named("PageDown", 34); break;
        case Key::F5: named("F5", 116); break;
        case Key::F10: named("F10", 121); break;
        case Key::F12: named("F12", 123); break;
        case Key::Menu: named("ContextMenu", 93); break;
        case Key::Letter: {
            char32_t const upper = key.letter;
            if (upper >= U'0' && upper <= U'9') {
                init.key = std::string(1, static_cast<char>(upper));
                init.code = "Digit" + init.key;
            } else {
                init.key = std::string(1, static_cast<char>(key.shift ? upper : upper - U'A' + U'a'));
                init.code = "Key" + std::string(1, static_cast<char>(upper));
            }
            init.key_code = static_cast<int>(upper);
            break;
        }
        case Key::None: break;
        }
        return init;
    }

    static std::optional<float> fragment_top(layout::Fragment const& fragment,
        dom::Element const* target)
    {
        if (fragment.element == target)
            return fragment.y;
        for (layout::TextRun const& run : fragment.runs) {
            for (dom::Node const* node = run.element; node; node = node->parent()) {
                if (node == target)
                    return run.baseline_y - run_metrics(run).ascent;
            }
        }
        for (layout::Fragment const& child : fragment.children) {
            if (std::optional<float> const top = fragment_top(child, target))
                return top;
        }
        return std::nullopt;
    }

    void scroll_to_fragment(Tab& tab, std::string const& fragment)
    {
        if (!tab.document)
            return;
        dom::Element const* const target = find_anchor_target(*tab.document, fragment);
        if (!target)
            return;
        // A target inside a box that scrolls needs that box moved as well;
        // its box is read again after each one, having travelled with it. A
        // target with no box of its own — an anchor that is only a name —
        // still moves the page below.
        if (layout::Fragment const* const box = fragment_for(tab.layout.root, target)) {
            auto const box_of_target = [box] {
                return Rect { static_cast<int>(box->x), static_cast<int>(box->y),
                    static_cast<int>(box->width) + 1, static_cast<int>(box->height) + 1 };
            };
            std::vector<layout::Fragment const*> chain;
            if (scroll_chain(tab.layout.root, box, nullptr, chain) && !chain.empty())
                reveal_within_boxes(tab, chain, box_of_target);
        }
        if (std::optional<float> const top = fragment_top(tab.layout.root, target))
            set_scroll(tab, static_cast<int>(*top));
    }

    // --- Loading -----------------------------------------------------------------

    void queue(std::size_t tab_index, net::Url url, Mode mode, bool https_first = false)
    {
        if (tab_index >= tabs.size())
            return;
        tabs[tab_index].status = "Loading " + url.serialize();
        pending.push_back(Pending { tab_index, std::move(url), mode, https_first });
        dirty = true;
    }

    static void set_document(HistoryEntry& entry, std::string const& html)
    {
        entry.bytes.assign(html.begin(), html.end());
        entry.content_type = "text/html";
    }

    static void fail_entry(HistoryEntry& entry, net::Url const& url, std::string const& error)
    {
        entry.internal = true;
        entry.error = error;
        std::string const host = host_of(url);
        if (error.find("certificate validation failed") != std::string::npos) {
            set_document(entry, certificate_error_page(host, url.serialize()));
            entry.error = "Certificate validation failed for " + host;
        } else if (error.starts_with("kept off by ") || error.starts_with("blocked by ")) {
            // The loader's blocklists refused the navigation: "<why> <list>: <rule>".
            bool const nefarious = error.starts_with("kept off by ");
            std::string const rest = error.substr(nefarious ? 12 : 11);
            std::size_t const colon = rest.find(": ");
            std::string const list = colon == std::string::npos ? rest : rest.substr(0, colon);
            std::string const rule = colon == std::string::npos ? std::string() : rest.substr(colon + 2);
            set_document(entry, blocked_page(url.serialize(), list, rule, nefarious));
            entry.error = (nefarious ? "Kept off " : "Blocked ") + host + " (" + list + ")";
        } else {
            set_document(entry, error_page("Sashfold can't reach " + host, error, url.serialize()));
        }
    }

    // A response the engine cannot render, or one the server marked as an
    // attachment: saved with the mark of the web, never opened, and the
    // page says what landed where. Returns the status-bar text.
    std::string receive_download(HistoryEntry& entry, net::FetchResponse& response,
        std::string const* disposition, std::string const& referrer)
    {
        std::string const name = download_file_name(disposition, response.final_url);
        std::string const type = entry.content_type;
        std::string const url = response.final_url.serialize();
        // A theme of Firefox's or Chrome's: converted into the reader's themes
        // from the bytes that arrived, offered from now on, and put on. No
        // file is saved for it and the reader's page stays (see perform).
        if (std::optional<std::string> const put_on = adopt_theme(name, type, response.body)) {
            theme_adopted = true;
            return "Theme put on: " + *put_on + " \xe2\x80\x94 it is under Themes in the menu from now on";
        }
        entry.internal = true;
        if (downloads_directory.empty()) {
            set_document(entry, unsupported_content_page(url, type, response.body.size()));
            return "Not saved, no downloads folder: " + name + " ("
                + std::to_string(response.body.size()) + " bytes)";
        }
        DownloadResult const saved
            = save_download(downloads_directory, name, response.body, response.final_url, referrer);
        if (!saved.error.empty()) {
            set_document(entry, error_page("Download failed", saved.error, url));
            return "Download failed: " + saved.error;
        }
        set_document(entry,
            download_page(saved.file_name, saved.path, response.body.size(), type, saved.marked));
        return "Downloaded " + saved.file_name + " (" + std::to_string(response.body.size())
            + " bytes)";
    }

    // What arrived is a browser theme — by its name (.xpi, .crx, .zip) or by
    // the type it came as — and converts as one: written among the reader's
    // themes, offered from now on, and put on. Its name; nothing for what is
    // anything else (an extension that is no theme, a zip of something else),
    // which is then the download it would have been.
    std::optional<std::string> adopt_theme(std::string const& name, std::string const& type,
        std::vector<std::uint8_t> const& bytes)
    {
        if (user_themes_directory.empty())
            return std::nullopt;
        bool const firefox = ascii_ci_equals(type, "application/x-xpinstall") || name.ends_with(".xpi");
        bool const chrome = ascii_ci_equals(type, "application/x-chrome-extension") || name.ends_with(".crx");
        if (!firefox && !chrome && !name.ends_with(".zip"))
            return std::nullopt;
        std::vector<std::string> problems;
        std::optional<ImportedTheme> const imported = import_browser_theme_archive(bytes,
            firefox  ? std::optional<BrowserThemeKind>(BrowserThemeKind::Firefox)
            : chrome ? std::optional<BrowserThemeKind>(BrowserThemeKind::Chrome)
                     : std::nullopt,
            name, &problems);
        if (!imported)
            return std::nullopt;
        std::optional<std::string> const written = write_imported_theme(*imported,
            (std::filesystem::path(user_themes_directory) / "converted").string(), &problems);
        std::optional<Theme> const converted = written ? Theme::load(*written, &problems) : std::nullopt;
        if (!converted)
            return std::nullopt;
        bool const offered = std::any_of(theme_presets.begin(), theme_presets.end(),
            [&](Browser::ThemePreset const& preset) { return preset.path == *written; });
        if (!offered)
            theme_presets.push_back({ converted->name, *written });
        put_on_theme(*written);
        return converted->name;
    }

    // about:themes (ui/ThemeGallery.h): the reader's themes to put on, the
    // gallery of Firefox's — read from the add-ons site's catalogue now,
    // because the page was opened, and at no other time — and the box for a
    // Chrome theme's address. What the address asks for (?use, ?crx) is done
    // on the way.
    std::string themes_document(net::Url const& url, std::size_t tab_index, bool reload)
    {
        ThemesPage page;
        page.query = gallery_query_of(url.query);
        page.can_adopt = !user_themes_directory.empty();
        page.notice = std::move(themes_notice);
        themes_notice.clear();
        if (page.query.use && *page.query.use < theme_presets.size()) {
            Browser::ThemePreset const chosen = theme_presets[*page.query.use];
            put_on_theme(chosen.path);
            page.notice = "Put on: " + chosen.name;
        }
        if (page.query.crx) {
            std::optional<std::string> const id = chrome_theme_id(*page.query.crx);
            std::optional<net::Url> const crx = id ? net::parse_url(chrome_crx_url(*id)) : std::nullopt;
            if (crx) {
                // Fetched like any address; what arrives is a theme, and is put on.
                queue(tab_index, *crx, Mode::Push);
                page.notice = "Fetching that theme from the Chrome Web Store\xe2\x80\xa6";
            } else {
                page.notice = "That is no Chrome Web Store address or id: an id is thirty-two letters, a to p.";
            }
        }
        for (Browser::ThemePreset const& preset : theme_presets)
            page.own.push_back({ preset.name, preset.name == base_theme.name });

        // A harness names a file to read in the catalogue's place; the
        // catalogue itself is asked by a search address.
        bool const catalogue = theme_gallery_api.starts_with("https://") || theme_gallery_api.starts_with("http://");
        std::string const asked = catalogue ? amo_search_url(theme_gallery_api, page.query) : theme_gallery_api;
        if (std::optional<net::Url> const source = net::parse_url(asked)) {
            std::string const container = tab_index < tabs.size() ? tabs[tab_index].container : std::string();
            net::FetchResult result = loader.load(*source, "", reload, container);
            if (!result.response) {
                page.gallery_error = result.error;
            } else if (result.response->status != 200 && result.response->status != 0) {
                page.gallery_error = "the catalogue answered " + std::to_string(result.response->status);
            } else {
                page.gallery = parse_amo_search(bytes_view(result.response->body), result.response->final_url.serialize());
                if (!page.gallery)
                    page.gallery_error = "what came back is not the catalogue's answer";
            }
        } else {
            page.gallery_error = "no catalogue address";
        }
        return themes_page(page);
    }

    // The themes page in a tab of its own, in front — or the tab that has it.
    void open_themes_page()
    {
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            HistoryEntry const* const entry = tabs[i].current();
            if (entry && entry->url.scheme == "about" && entry->url.serialize_path() == "themes") {
                select_tab(i);
                return;
            }
        }
        if (std::optional<net::Url> const themes = net::parse_url("about:themes"))
            open_tab_in({}, *themes);
    }

    void show_internal(std::string const& html, net::Url const& url, std::string const& status)
    {
        Tab* const tab = active_tab();
        if (!tab)
            return;
        HistoryEntry entry;
        entry.url = url;
        entry.final_url = url;
        entry.internal = true;
        entry.error = status;
        set_document(entry, html);
        commit(*tab, std::move(entry), Mode::Push);
        tab->status = status;
        sync_address();
    }

    void commit(Tab& tab, HistoryEntry entry, Mode mode)
    {
        if (mode == Mode::Push || tab.history.empty()) {
            if (!tab.history.empty())
                tab.history.resize(tab.index + 1);
            tab.history.push_back(std::move(entry));
            tab.index = tab.history.size() - 1;
        } else {
            entry.scroll_y = tab.history[tab.index].scroll_y;
            tab.history[tab.index] = std::move(entry);
        }
        render(tab);
    }

    bool perform(Pending const& load)
    {
        if (load.tab >= tabs.size())
            return false;
        Tab& tab = tabs[load.tab];
        HistoryEntry const* const from = tab.current();
        HistoryEntry entry;
        entry.url = load.url;
        entry.final_url = load.url;
        bool fell_back_to_http = false;
        std::string download_status;

        if (load.url.scheme == "about" && load.url.serialize_path() == "sashfold") {
            set_document(entry, about_sashfold_page());
            entry.internal = true;
            entry.status = 200;
        } else if (is_about_newtab(load.url)) {
            set_document(entry, new_tab_document());
            entry.internal = true;
            entry.status = 200;
        } else if (load.url.scheme == "about" && load.url.serialize_path() == "themes") {
            set_document(entry, themes_document(load.url, load.tab, load.mode == Mode::Reload));
            entry.internal = true;
            entry.status = 200;
        } else if (load.url.scheme == "view-source") {
            std::optional<net::Url> const inner = net::parse_url(load.url.serialize_path());
            if (!inner) {
                fail_entry(entry, load.url, "view-source: needs a URL after it");
            } else {
                net::FetchResult result = loader.load(*inner, "", load.mode == Mode::Reload, tab.container);
                if (!result.response) {
                    fail_entry(entry, *inner, result.error);
                } else {
                    set_document(entry, source_page(inner->serialize(), result.response->body));
                    entry.internal = true;
                    entry.status = result.response->status;
                    entry.from_cache = result.response->from_cache;
                }
            }
        } else if (load.url.scheme == "reader") {
            // The page behind the reader: fetched like any document, then
            // reduced to its article.
            std::optional<net::Url> const inner = net::parse_url(load.url.serialize_path());
            if (!inner) {
                fail_entry(entry, load.url, "reader: needs a URL after it");
            } else {
                net::FetchResult result = loader.load(*inner, "", load.mode == Mode::Reload, tab.container);
                if (!result.response) {
                    fail_entry(entry, *inner, result.error);
                } else {
                    std::unique_ptr<dom::Document> const document
                        = html::parse_document_bytes(bytes_view(result.response->body));
                    set_document(entry, reader_page(*document, result.response->final_url));
                    entry.internal = true;
                    entry.status = result.response->status;
                    entry.from_cache = result.response->from_cache;
                }
            }
        } else {
            std::string const referrer = referrer_for(from ? &from->final_url : nullptr, load.url);
            net::FetchResult result = loader.load(load.url, referrer, load.mode == Mode::Reload, tab.container);
            if (!result.response && load.https_first
                && result.error.find("could not connect") != std::string::npos) {
                // HTTPS-first: a host that does not answer on 443 gets one
                // plain-HTTP try, and the address bar says so.
                net::Url http = load.url;
                http.scheme = "http";
                net::FetchResult retry = loader.load(http, referrer, load.mode == Mode::Reload, tab.container);
                if (retry.response) {
                    result = std::move(retry);
                    entry.url = http;
                    fell_back_to_http = true;
                }
            }
            if (!result.response) {
                fail_entry(entry, entry.url, result.error);
            } else {
                net::FetchResponse& response = *result.response;
                entry.final_url = response.final_url;
                entry.status = response.status;
                entry.from_cache = response.from_cache;
                if (std::string const* const type = net::find_header(response.headers, "content-type"))
                    entry.content_type = *type;
                for (net::Header const& header : response.headers) {
                    if (ascii_ci_equals(header.name, "content-security-policy"))
                        entry.csp_headers.push_back(header.value);
                    else if (ascii_ci_equals(header.name, "content-security-policy-report-only"))
                        entry.csp_report_only_headers.push_back(header.value);
                }
                std::string const* const disposition
                    = net::find_header(response.headers, "content-disposition");
                bool const attachment = disposition && starts_with_ci(trim(*disposition), "attachment");
                if (attachment || !is_renderable_content_type(entry.content_type))
                    download_status = receive_download(entry, response, disposition, referrer);
                else
                    entry.bytes = std::move(response.body);
                if (theme_adopted) {
                    // A theme, and it is on: the page the reader chose it
                    // from stays where it is — nothing was navigated to,
                    // nothing was saved — and the themes page, if that is
                    // the page, is drawn again to say which is on now.
                    theme_adopted = false;
                    tab.status = download_status;
                    if (from && from->url.scheme == "about" && from->url.serialize_path() == "themes") {
                        themes_notice = download_status;
                        // The same words, order and page as the reader had.
                        if (std::optional<net::Url> const again = net::parse_url(gallery_address(gallery_query_of(from->url.query))))
                            queue(load.tab, *again, Mode::Replace);
                    }
                    refresh_hover();
                    dirty = true;
                    return true;
                }
            }
        }

        std::string status;
        if (!download_status.empty()) {
            status = download_status;
        } else if (!entry.error.empty()) {
            status = entry.error;
        } else if (fell_back_to_http) {
            status = "Loaded over plain HTTP: the site did not answer on HTTPS";
        } else {
            status = "Done";
            if (entry.status != 0 && entry.status != 200 && !entry.internal)
                status += " (HTTP " + std::to_string(entry.status) + ")";
            if (entry.from_cache)
                status += " (from cache)";
        }
        commit(tab, std::move(entry), load.mode);
        tab.status = status;
        if (&tab == active_tab())
            sync_address();
        refresh_hover();
        dirty = true;
        return true;
    }

    // --- Navigation --------------------------------------------------------------

    void navigate(std::string const& typed_raw)
    {
        std::string const typed = trim(typed_raw);
        if (typed.empty())
            return;
        std::optional<net::Url> url = net::parse_url(typed);
        bool https_first = false;
        if (!url || !is_navigable_scheme(url->scheme)) {
            bool const host_like = typed.find(' ') == std::string::npos
                && (typed.find('.') != std::string::npos || starts_with_ci(typed, "localhost"));
            if (host_like)
                url = net::parse_url("https://" + typed);
            else
                url.reset();
            if (!url) {
                // No default search: a browser that sends what you type to a
                // search engine has made a deal on your behalf.
                std::optional<net::Url> const about = net::parse_url("about:blank");
                show_internal(error_page("That is not a web address",
                                  "Sashfold does not send what you type to a search engine. "
                                  "Type a URL, or a host name like example.org.",
                                  typed),
                    *about, "Not a web address: " + typed);
                return;
            }
            https_first = true;
        }
        queue(active, *url, Mode::Push, https_first);
    }

    void open(net::Url const& url)
    {
        Tab* const tab = active_tab();
        if (!tab)
            return;
        HistoryEntry const* const current = tab->current();
        // A fragment on the current document scrolls; it does not reload.
        if (current && !current->internal && url.fragment
            && url.serialize(true) == current->final_url.serialize(true)) {
            HistoryEntry copy = *current;
            copy.url = url;
            copy.final_url = url;
            copy.scroll_y = 0;
            tab->history.resize(tab->index + 1);
            tab->history.push_back(std::move(copy));
            tab->index = tab->history.size() - 1;
            scroll_to_fragment(*tab, *url.fragment);
            sync_address();
            dirty = true;
            return;
        }
        queue(active, url, Mode::Push);
    }

    void go(int delta)
    {
        Tab* const tab = active_tab();
        if (!tab || tab->history.empty())
            return;
        auto const target = static_cast<std::ptrdiff_t>(tab->index) + delta;
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(tab->history.size()))
            return;
        if (HistoryEntry* const entry = tab->current())
            entry->scroll_y = tab->scroll_y;
        tab->index = static_cast<std::size_t>(target);
        if (HistoryEntry const* const entry = tab->current(); entry && entry->unloaded) {
            // A restored entry: its page is fetched now, in place.
            queue(index_of(*tab), entry->url, Mode::Replace);
            sync_address();
            return;
        }
        render(*tab);
        tab->status = "Done";
        sync_address();
        refresh_hover();
        dirty = true;
    }

    void reload()
    {
        Tab* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        if (!entry)
            return;
        queue(active, entry->url, Mode::Reload);
    }

    // A new tab opens on the new-tab page, the address bar empty and
    // focused: the blank entry becomes the page, in place. In a container
    // when one is named; the status line says which.
    void new_tab_in(std::string const& container)
    {
        blur_address();
        add_blank_tab(container);
        if (HistoryEntry* const entry = tabs[active].current()) {
            entry->url = *net::parse_url("about:newtab");
            entry->final_url = entry->url;
            entry->internal = true;
            render(tabs[active]);
            sync_address();
        }
        if (!container.empty())
            tabs[active].status = "New tab in the " + container + " container";
        focus_address(false);
        refresh_hover();
    }

    void new_tab() { new_tab_in({}); }

    // The pictures the theme's folder holds, as file: URLs in name order,
    // begun at the one whose turn it is: each new tab opens on the next
    // picture, and the page cycles from there.
    std::vector<std::string> new_tab_pictures()
    {
        std::vector<std::string> urls;
        if (theme.new_tab_backgrounds.empty())
            return urls;
        std::error_code error;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_entry const& entry :
            std::filesystem::directory_iterator(theme.new_tab_backgrounds, error)) {
            if (!entry.is_regular_file(error))
                continue;
            std::string extension = entry.path().extension().string();
            for (char& c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".gif"
                || extension == ".bmp" || extension == ".svg")
                files.push_back(entry.path());
        }
        if (files.empty())
            return urls;
        std::sort(files.begin(), files.end());
        std::size_t const first = new_tabs_opened++ % files.size();
        for (std::size_t i = 0; i < files.size(); ++i) {
            std::string generic = files[(first + i) % files.size()].generic_string();
            if (!generic.starts_with("/"))
                generic = "/" + generic; // file:///C:/... on Windows
            if (std::optional<net::Url> const url = net::parse_url("file://" + generic))
                urls.push_back(url->serialize());
        }
        return urls;
    }

    // The new-tab page as of this moment: the local time, the date in
    // words, the theme's colors and its pictures.
    std::string new_tab_document()
    {
        static constexpr char const* weekdays[]
            = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        static constexpr char const* months[] = { "January", "February", "March", "April", "May", "June", "July",
            "August", "September", "October", "November", "December" };
        WallTime const now = wall_clock ? wall_clock() : local_now();
        NewTabPage page;
        page.hour = std::clamp(now.hour, 0, 23);
        page.minute = std::clamp(now.minute, 0, 59);
        page.next_minute_ms = (60 - std::clamp(now.second, 0, 59)) * 1000;
        page.date = std::string(weekdays[std::clamp(now.weekday, 0, 6)]) + ", " + std::to_string(now.day) + " "
            + months[std::clamp(now.month, 1, 12) - 1];
        page.pictures = new_tab_pictures();
        page.rotate_ms = theme.new_tab_rotate_ms;
        // The page's own colors where the theme names them, the chrome's
        // where it does not; a background named alone is that color flat.
        page.background = theme.new_tab_background.value_or(theme.chrome_background);
        page.background_end = theme.new_tab_background_end.value_or(
            theme.new_tab_background ? *theme.new_tab_background
                                     : mix(theme.tab_active_background, theme.accent, 0.35f));
        page.text = theme.new_tab_text.value_or(theme.chrome_text);
        page.text_muted = theme.new_tab_text_muted.value_or(theme.chrome_text_muted);
        return new_tab_page(page);
    }

    // A tab with somewhere to come back to: one that never left the blank
    // page or the new-tab page is not kept, as no browser keeps it.
    static bool worth_reopening(Tab const& tab)
    {
        return std::any_of(tab.history.begin(), tab.history.end(), [](HistoryEntry const& entry) {
            return !is_about_blank(entry.url) && !is_about_newtab(entry.url);
        });
    }

    // The tab about to close goes on the stack of those that can come back,
    // and the oldest falls off the far end.
    void remember_closed(std::size_t index)
    {
        Tab& tab = tabs[index];
        if (!worth_reopening(tab))
            return;
        if (HistoryEntry* const entry = tab.current())
            entry->scroll_y = tab.scroll_y;
        closed_tabs.push_back(ClosedTab { std::move(tab.history), tab.index, tab.container, index,
            std::move(tab.favicon), std::move(tab.favicon_key), tab.pinned });
        tab.history.clear();
        if (closed_tabs.size() > closed_tabs_kept)
            closed_tabs.erase(closed_tabs.begin());
    }

    // The tab closed last comes back where it stood, in front, with its
    // history and the place it had scrolled to; its page is drawn from the
    // bytes kept, or fetched when a session brought it over without them.
    void reopen_closed_tab()
    {
        if (closed_tabs.empty())
            return;
        ClosedTab closed = std::move(closed_tabs.back());
        closed_tabs.pop_back();
        if (closed.history.empty())
            return;
        blur_address();
        Tab tab;
        tab.history = std::move(closed.history);
        tab.index = std::min(closed.index, tab.history.size() - 1);
        tab.container = std::move(closed.container);
        tab.favicon = std::move(closed.favicon);
        tab.favicon_key = std::move(closed.favicon_key);
        tab.scroll_y = tab.history[tab.index].scroll_y;
        // Pinned as it was, and so among the pinned tabs — or after them.
        tab.pinned = closed.pinned;
        closed.position = closed.pinned ? std::min(closed.position, pinned_count())
                                        : std::max(closed.position, pinned_count());
        std::size_t const at = insert_tab(closed.position, std::move(tab), true);
        show_kept(at);
    }

    // A tab beside the one it copies, in front, with the same history at the
    // same place, in the same container; nothing is fetched for it.
    void duplicate_tab(std::size_t index)
    {
        if (index >= tabs.size())
            return;
        blur_address();
        Tab const& from = tabs[index];
        Tab tab;
        tab.history = from.history;
        tab.index = from.index;
        tab.container = from.container;
        tab.favicon = from.favicon; // the same picture, shared
        tab.favicon_key = from.favicon_key;
        tab.pinned = from.pinned; // a pinned tab's copy stands beside it, pinned
        if (HistoryEntry* const entry = tab.current())
            entry->scroll_y = from.scroll_y;
        tab.scroll_y = from.scroll_y;
        std::size_t const at = insert_tab(index + 1, std::move(tab), true);
        show_kept(at);
    }

    // --- Pinned tabs ----------------------------------------------------------------

    // How many tabs are pinned: they are the strip's first, in a row.
    std::size_t pinned_count() const
    {
        std::size_t count = 0;
        while (count < tabs.size() && tabs[count].pinned)
            ++count;
        return count;
    }

    // A tab taken from one place in the strip to another, the tabs between
    // moving over by one. Whatever names a tab by where it stands — a load
    // under way, a window asked for, the tab in front — follows its tab.
    void move_tab(std::size_t from, std::size_t to)
    {
        if (from >= tabs.size() || to >= tabs.size() || from == to)
            return;
        close_menus(); // a tab's menu names its tab by where it stood
        auto const moved = [from, to](std::size_t index) {
            if (index == from)
                return to;
            if (from < to && index > from && index <= to)
                return index - 1;
            if (to < from && index >= to && index < from)
                return index + 1;
            return index;
        };
        if (from < to)
            std::rotate(tabs.begin() + static_cast<std::ptrdiff_t>(from), tabs.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                tabs.begin() + static_cast<std::ptrdiff_t>(to) + 1);
        else
            std::rotate(tabs.begin() + static_cast<std::ptrdiff_t>(to), tabs.begin() + static_cast<std::ptrdiff_t>(from),
                tabs.begin() + static_cast<std::ptrdiff_t>(from) + 1);
        for (Pending& load : pending)
            load.tab = moved(load.tab);
        for (PendingWindow& window : pending_windows)
            window.opener = moved(window.opener);
        active = moved(active);
        opened_beside = 0;
        refresh_hover();
        dirty = true;
    }

    // Pinning takes a tab to the end of the pinned tabs, unpinning to the
    // first place after them: where both browsers put it.
    void set_pinned(std::size_t index, bool pinned)
    {
        if (index >= tabs.size() || tabs[index].pinned == pinned)
            return;
        std::size_t const row = pinned_count();
        tabs[index].pinned = pinned;
        move_tab(index, pinned ? row : row - 1);
        close_menus();
        dirty = true;
    }

    // A tab whose history came with it is shown from what it holds.
    void show_kept(std::size_t index)
    {
        Tab& tab = tabs[index];
        HistoryEntry const* const entry = tab.current();
        if (entry && entry->unloaded) {
            ensure_loaded(index);
        } else {
            render(tab); // at the place its entry had scrolled to
            tab.status = "Done";
        }
        sync_address();
        refresh_hover();
        dirty = true;
    }

    void close_tab(std::size_t index)
    {
        if (index >= tabs.size())
            return;
        close_menus();
        remember_closed(index);
        opened_beside = 0;
        tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(index));
        for (Pending& load : pending) {
            if (load.tab > index)
                --load.tab;
        }
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                          [&](Pending const& load) { return load.tab == index; }),
            pending.end());
        for (PendingWindow& window : pending_windows) {
            if (window.opener > index)
                --window.opener;
        }
        if (tabs.empty()) {
            add_blank_tab();
            focus_address(false);
        } else if (active > index) {
            --active; // the active tab shifted left
        } else if (active >= tabs.size()) {
            active = tabs.size() - 1; // closed the active, last tab
        }
        // Closing the active tab elsewhere activates its right-hand neighbour,
        // which now sits at the same index.
        blur_address();
        sync_address();
        refresh_hover();
        dirty = true;
    }

    void select_tab(std::size_t index)
    {
        if (index >= tabs.size())
            return;
        if (index != active)
            opened_beside = 0; // what another tab opens stands beside that tab
        active = index;
        ensure_loaded(index); // a restored tab fetches its page now
        ensure_fresh(tabs[index]); // its scripts may have run while another tab showed
        blur_address();
        sync_address();
        if (find_open)
            update_matches(tabs[index]); // the query may have changed while another tab showed
        refresh_hover();
        dirty = true;
    }

    // --- Hit testing --------------------------------------------------------------

    // What a box that clips holds cannot be reached outside its padding
    // box, on the axes it clips: `overflow: clip visible` holds the sides
    // in and lets the content run off the top and bottom. Since a box that
    // scrolls moves its content within that box, a link that has been
    // scrolled out of sight is out of reach as well — which is the whole
    // point of the clip.
    static bool reachable_within(layout::Fragment const& box, float px, float py)
    {
        if (!box.style || !box.style->overflow_applies)
            return true;
        css::ComputedStyle const& s = *box.style;
        if (s.overflow_x != css::Overflow::Visible
            && (px < box.x + s.border_left.width || px >= box.x + box.width - s.border_right.width))
            return false;
        if (s.overflow_y != css::Overflow::Visible
            && (py < box.y + s.border_top.width || py >= box.y + box.height - s.border_bottom.width))
            return false;
        return true;
    }

    static dom::Element const* hit_run(layout::Fragment const& fragment, float x, float y)
    {
        if (!reachable_within(fragment, x, y))
            return nullptr;
        for (layout::Fragment const& child : fragment.children) {
            if (dom::Element const* const hit = hit_run(child, x, y))
                return hit;
        }
        if (fragment.image && x >= fragment.x && x < fragment.x + fragment.width && y >= fragment.y
            && y < fragment.y + fragment.height)
            return fragment.element;
        for (layout::TextRun const& run : fragment.runs) {
            text::FaceMetrics const metrics = run_metrics(run);
            float const top = run.baseline_y - metrics.ascent;
            float const bottom = run.baseline_y + metrics.descent;
            float const right = run.x + run.width;
            if (x >= run.x && x < right && y >= top && y < bottom)
                return run.element;
        }
        return nullptr;
    }

    // The control whose box holds page point (px, py).
    static dom::Element const* hit_control(layout::Fragment const& fragment, float px, float py)
    {
        if (!reachable_within(fragment, px, py))
            return nullptr;
        if (fragment.control && fragment.element) {
            layout::Fragment::ControlBox const& box = *fragment.control;
            if (px >= box.x && px < box.x + box.width && py >= box.y && py < box.y + box.height)
                return fragment.element;
        }
        for (layout::Fragment const& child : fragment.children) {
            if (dom::Element const* const hit = hit_control(child, px, py))
                return hit;
        }
        return nullptr;
    }

    // The first control inside a node, in tree order.
    static dom::Element const* first_control_within(dom::Node const& node)
    {
        for (dom::Node const* child : node.children()) {
            if (!child->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*child);
            if (layout::is_control(element))
                return &element;
            if (dom::Element const* const found = first_control_within(element))
                return found;
        }
        return nullptr;
    }

    // The page coordinates of a window point inside the content area.
    std::optional<std::pair<float, float>> page_point(int x, int y) const
    {
        ChromeLayout const c = layout_chrome();
        Tab const* const tab = active_tab();
        if (!tab || !tab->document || !c.content.contains(x, y))
            return std::nullopt;
        return std::pair<float, float> { static_cast<float>(x - c.content.x),
            static_cast<float>(y - c.content.y + tab->scroll_y) };
    }

    // The control under a window point: its own box, or the control a
    // <label> whose text was hit stands for.
    dom::Element const* control_at(int x, int y)
    {
        Tab* const tab = active_tab();
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!tab || !point)
            return nullptr;
        // In a frame, the frame's document and its layout answer.
        std::vector<FrameStep> const chain = frames_at(*tab, point->first, point->second);
        layout::Fragment const& root = chain.empty() ? tab->layout.root : chain.back().view->layout.root;
        dom::Document const& document = chain.empty() ? *tab->document : *chain.back().view->document;
        float const px = chain.empty() ? point->first : chain.back().x;
        float const py = chain.empty() ? point->second : chain.back().y;
        if (dom::Element const* const control = hit_control(root, px, py))
            return control;
        dom::Element const* const hit = hit_run(root, px, py);
        for (dom::Node const* node = hit; node; node = node->parent()) {
            if (!node->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*node);
            if (!element.is_html("label"))
                continue;
            if (dom::Attr const* const target = element.find_attribute("for")) {
                dom::Element const* const named = element_by_id(document, target->value);
                return named && layout::is_control(*named) ? named : nullptr;
            }
            return first_control_within(element);
        }
        return nullptr;
    }

    // A link under a window point: its URL, resolved against the document it
    // is in; the frame that document is shown in, when it is a frame's; and
    // its target attribute.
    struct LinkHit {
        net::Url url;
        dom::Element const* frame = nullptr;
        std::string target;
    };

    std::optional<LinkHit> link_under(int x, int y)
    {
        ChromeLayout const c = layout_chrome();
        Tab* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        if (!tab || !tab->document || !entry || !c.content.contains(x, y))
            return std::nullopt;
        float px = static_cast<float>(x - c.content.x);
        float py = static_cast<float>(y - c.content.y + tab->scroll_y);
        std::vector<FrameStep> const chain = frames_at(*tab, px, py);
        layout::Fragment const* root = &tab->layout.root;
        net::Url base = entry->final_url;
        dom::Element const* link_frame = nullptr;
        if (!chain.empty()) {
            FrameStep const& step = chain.back();
            root = &step.view->layout.root;
            base = step.view->realm->url();
            link_frame = step.container;
            px = step.x;
            py = step.y;
        }
        dom::Element const* const hit = hit_run(*root, px, py);
        for (dom::Node const* node = hit; node; node = node->parent()) {
            if (!node->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*node);
            if (!element.is_html("a"))
                continue;
            dom::Attr const* const href = element.find_attribute("href");
            if (!href)
                continue;
            std::optional<net::Url> url = net::parse_url(href->value, &base);
            if (!url)
                return std::nullopt;
            dom::Attr const* const target = element.find_attribute("target");
            return LinkHit { std::move(*url), link_frame, target ? target->value : std::string() };
        }
        return std::nullopt;
    }

    std::optional<net::Url> link_at(int x, int y)
    {
        std::optional<LinkHit> const hit = link_under(x, y);
        return hit ? std::optional<net::Url>(hit->url) : std::nullopt;
    }

    struct TextHit {
        int x = 0;
        int y = 0;
    };

    std::optional<TextHit> find_text_in(layout::Fragment const& fragment, std::string const& needle,
        Tab const& tab, ChromeLayout const& c) const
    {
        for (layout::TextRun const& run : fragment.runs) {
            std::string const text = to_utf8(run.text);
            std::size_t const at = text.find(needle);
            if (at == std::string::npos)
                continue;
            std::size_t const before = decode_utf8(text.substr(0, at)).size();
            std::size_t const length = decode_utf8(needle).size();
            float const start = prefix_width(run, before);
            float const center_x = run.x + start + (prefix_width(run, before + length) - start) / 2.0f;
            float const center_y = run.baseline_y - run_metrics(run).ascent / 2.0f;
            return TextHit { c.content.x + static_cast<int>(center_x),
                c.content.y + static_cast<int>(center_y) - tab.scroll_y };
        }
        for (layout::Fragment const& child : fragment.children) {
            if (std::optional<TextHit> const hit = find_text_in(child, needle, tab, c))
                return hit;
        }
        return std::nullopt;
    }

    void refresh_hover()
    {
        if (mouse_x >= 0 && mouse_y >= 0)
            update_hover(mouse_x, mouse_y);
    }

    void update_hover(int x, int y)
    {
        // What is under the pointer is asked of the page as it is laid out
        // for the window as it is now.
        if (Tab* const front = active_tab())
            ensure_fresh(*front);
        int const from_x = mouse_x;
        int const from_y = mouse_y;
        mouse_x = x;
        mouse_y = y;
        ChromeLayout const c = layout_chrome();
        Hover next = Hover::None;
        std::size_t index = 0;
        std::size_t level = 0;
        std::optional<net::Url> link;
        dom::Element const* link_frame = nullptr;
        std::string link_target;
        for (std::size_t i = 0; i < c.tabs.size(); ++i) {
            if (c.tab_close_buttons[i].contains(x, y)) {
                next = Hover::TabClose;
                index = i;
                break;
            }
            if (c.tabs[i].contains(x, y)) {
                next = Hover::Tab;
                index = i;
                break;
            }
        }
        if (next == Hover::None) {
            if (c.new_tab_button.contains(x, y))
                next = Hover::NewTab;
            else if (c.window_controls && c.close_button.contains(x, y))
                next = Hover::WindowClose;
            else if (c.window_controls && c.maximize_button.contains(x, y))
                next = Hover::Maximize;
            else if (c.window_controls && c.minimize_button.contains(x, y))
                next = Hover::Minimize;
            else if (c.window_controls && c.tab_strip.contains(x, y))
                next = Hover::DragHandle; // the strip's empty part moves the window
            else if (c.back_button.contains(x, y))
                next = Hover::Back;
            else if (c.forward_button.contains(x, y))
                next = Hover::Forward;
            else if (c.reload_button.contains(x, y))
                next = Hover::Reload;
            else if (c.reader_button.contains(x, y))
                next = Hover::Reader;
            else if (c.menu_button.contains(x, y))
                next = Hover::MenuButton;
            else if (c.address.contains(x, y))
                next = Hover::Address;
            else if (find_open && c.find_box.contains(x, y))
                next = Hover::FindBox;
            else if (devtools_open && c.devtools_tree.contains(x, y))
                next = Hover::DevtoolsTree;
            else if (devtools_open && c.devtools.contains(x, y))
                next = Hover::DevtoolsStyles;
            else if (c.content.contains(x, y)) {
                next = Hover::Content;
                if (std::optional<LinkHit> found = link_under(x, y)) {
                    link = std::move(found->url);
                    link_frame = found->frame;
                    link_target = std::move(found->target);
                }
            }
        }
        if (palette_open) {
            // The palette lies over the content: what is under it is out of reach.
            for (std::size_t i = 0; i < c.palette_rows.size(); ++i) {
                if (c.palette_rows[i].contains(x, y)) {
                    next = Hover::PaletteRow;
                    index = i;
                    link.reset();
                    link_frame = nullptr;
                    break;
                }
            }
            if (next != Hover::PaletteRow && c.palette.contains(x, y)) {
                next = Hover::Palette;
                link.reset();
                link_frame = nullptr;
            }
        }
        if (!menus.empty()) {
            // An open menu holds the pointer: nothing under it or beside it
            // answers until it closes. The innermost menu is on top.
            next = Hover::None;
            index = 0;
            link.reset();
            link_frame = nullptr;
            link_target.clear();
            for (std::size_t l = std::min(c.menus.size(), menus.size()); l-- > 0 && next == Hover::None;) {
                MenuBox const& box = c.menus[l];
                for (std::size_t i = 0; i < box.rows.size(); ++i) {
                    if (!box.rows[i].contains(x, y))
                        continue;
                    if (menus[l].items[i].choosable()) {
                        next = Hover::MenuRow;
                        index = i;
                        level = l;
                    } else {
                        next = Hover::Menu;
                    }
                    break;
                }
                if (next == Hover::None && box.box.contains(x, y))
                    next = Hover::Menu;
            }
        }
        if (next != hover || index != hover_index || level != hover_level || !same_url(link, hover_link)
            || link_frame != hover_link_frame) {
            hover = next;
            hover_index = index;
            hover_level = level;
            hover_link = std::move(link);
            hover_link_frame = link_frame;
            hover_link_target = std::move(link_target);
            dirty = true;
        }
        if (!menus.empty())
            follow_pointer_in_menus(from_x, from_y);
    }

    // --- The command palette ----------------------------------------------------

    static std::string ascii_lowercase(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
            c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        return out;
    }

    // Every command the shell has right now, in the order the palette
    // lists them: the tabs, the containers and the themes by name.
    std::vector<Command> palette_command_list()
    {
        std::vector<Command> commands;
        commands.push_back({ "New tab", [this] { new_tab(); } });
        for (Browser::Container const& container : containers)
            commands.push_back({ "New tab in " + container.name, [this, name = container.name] { new_tab_in(name); } });
        commands.push_back({ "Close tab", [this] { close_tab(active); } });
        if (!closed_tabs.empty())
            commands.push_back({ "Reopen closed tab", [this] { reopen_closed_tab(); } });
        commands.push_back({ "Duplicate tab", [this] { duplicate_tab(active); } });
        commands.push_back({ "Reload", [this] { reload(); } });
        commands.push_back({ "Back", [this] { go(-1); } });
        commands.push_back({ "Forward", [this] { go(+1); } });
        commands.push_back({ "Find in page", [this] { open_find(); } });
        commands.push_back({ "More themes\xe2\x80\xa6", [this] { open_themes_page(); } });
        commands.push_back({ "Reader mode", [this] { toggle_reader(); } });
        commands.push_back({ "Developer tools", [this] { toggle_devtools(); } });
        commands.push_back({ "View source", [this] { view_source(); } });
        for (std::size_t i = 0; i < tabs.size(); ++i)
            commands.push_back({ "Switch to tab: " + tab_title(tabs[i]), [this, i] { select_tab(i); } });
        for (Browser::ThemePreset const& preset : theme_presets)
            commands.push_back({ "Theme: " + preset.name, [this, path = preset.path] { put_on_theme(path); } });
        return commands;
    }

    // The commands whose labels carry every word of the query, in order.
    void filter_palette()
    {
        std::vector<std::string> words;
        std::string const query = ascii_lowercase(palette_query);
        std::size_t start = 0;
        while (start < query.size()) {
            while (start < query.size() && query[start] == ' ')
                ++start;
            std::size_t end = start;
            while (end < query.size() && query[end] != ' ')
                ++end;
            if (end > start)
                words.push_back(query.substr(start, end - start));
            start = end;
        }
        palette_matches.clear();
        for (std::size_t i = 0; i < palette_commands.size(); ++i) {
            std::string const label = ascii_lowercase(palette_commands[i].label);
            bool all = true;
            for (std::string const& word : words)
                all = all && label.find(word) != std::string::npos;
            if (all)
                palette_matches.push_back(i);
        }
        palette_index = 0;
        dirty = true;
    }

    void open_palette(std::string const& query)
    {
        blur_address();
        blur_find();
        hints_active = false;
        palette_open = true;
        palette_query = query;
        palette_caret = query.size();
        palette_select_all = false;
        palette_commands = palette_command_list();
        filter_palette();
        refresh_hover();
    }

    void close_palette()
    {
        if (!palette_open)
            return;
        palette_open = false;
        palette_query.clear();
        palette_caret = 0;
        palette_select_all = false;
        palette_commands.clear();
        palette_matches.clear();
        palette_index = 0;
        refresh_hover();
        dirty = true;
    }

    // The first of the matches shown: the list scrolls to keep the
    // highlighted one among the rows.
    std::size_t palette_first_shown() const
    {
        return palette_index >= palette_rows_shown ? palette_index - palette_rows_shown + 1 : 0;
    }

    std::string palette_selection() const
    {
        if (!palette_open || palette_index >= palette_matches.size())
            return {};
        return palette_commands[palette_matches[palette_index]].label;
    }

    // Runs the match at `which` among the matches: the palette closes
    // first, since the command may open something of its own.
    void run_palette_match(std::size_t which)
    {
        if (which >= palette_matches.size()) {
            close_palette();
            return;
        }
        std::function<void()> const run = palette_commands[palette_matches[which]].run;
        close_palette();
        if (run)
            run();
        dirty = true;
    }

    void edit_palette(KeyEvent const& key)
    {
        if (key.key == Key::Enter) {
            run_palette_match(palette_index);
            return;
        }
        if (key.key == Key::Escape) {
            close_palette();
            return;
        }
        if (key.key == Key::Down || key.key == Key::Up) {
            if (!palette_matches.empty()) {
                std::size_t const count = palette_matches.size();
                palette_index = key.key == Key::Down ? (palette_index + 1) % count : (palette_index + count - 1) % count;
                dirty = true;
            }
            return;
        }
        if (edit_text(palette_query, palette_caret, palette_select_all, key))
            filter_palette();
    }

    void type_into_palette(char32_t code_point)
    {
        if (palette_select_all) {
            palette_query.clear();
            palette_caret = 0;
            palette_select_all = false;
        }
        std::string utf8;
        append_utf8(utf8, code_point);
        palette_query.insert(palette_caret, utf8);
        palette_caret += utf8.size();
        filter_palette();
    }

    // A theme preset chosen: put on now, and left for the host to keep.
    void put_on_theme(std::string const& path)
    {
        std::vector<std::string> problems;
        std::optional<Theme> loaded = Theme::load(path, &problems);
        if (!loaded)
            return;
        set_base_theme(std::move(*loaded));
        theme_request = path;
    }

    void set_base_theme(Theme loaded)
    {
        base_theme = std::move(loaded);
        theme = base_theme.scaled(scale);
        load_theme_pictures();
        for (Tab& tab : tabs) {
            // The new-tab page is made of the theme's colors and pictures:
            // one that is showing is made again, not left in the theme it
            // opened under.
            HistoryEntry const* const entry = tab.current();
            if (entry && is_about_newtab(entry->url))
                render(tab);
            else
                relayout(tab);
        }
        refresh_hover();
        dirty = true;
    }

    // The pictures the theme names, read and decoded, a surface at a time.
    // One that cannot be had — no such file, too large, in no format
    // decoded here — is left out, the rest of the theme stands, and
    // theme_problems says which and why.
    void load_theme_pictures()
    {
        // The face its words are set in goes on with the rest of the theme.
        set_chrome_font_family(base_theme.font_family);
        theme_problems.clear();
        auto const load = [&](std::vector<ThemePicture> const& named, ThemeLayers& into, char const* surface) {
            into = ThemeLayers {};
            for (std::size_t i = 0; i < named.size(); ++i) {
                auto const problem = [&](std::string const& what) {
                    theme_problems.push_back(std::string("theme: images.") + surface + "[" + std::to_string(i)
                        + "]: " + what + ": " + named[i].path);
                };
                std::error_code error;
                std::uintmax_t const size = std::filesystem::file_size(named[i].path, error);
                if (error) {
                    problem("cannot read");
                    continue;
                }
                if (size > max_theme_picture_bytes) {
                    problem("larger than a theme's picture may be");
                    continue;
                }
                std::vector<std::uint8_t> bytes;
                bool opened = false;
                {
                    std::ifstream file(named[i].path, std::ios::binary);
                    opened = static_cast<bool>(file);
                    if (opened)
                        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                }
                if (!opened) {
                    problem("cannot read"); // there, and not ours to open
                    continue;
                }
                std::optional<Bitmap> decoded = decode_image_bytes(bytes);
                if (!decoded || decoded->width() <= 0 || decoded->height() <= 0) {
                    problem("not a picture in a format read here");
                    continue;
                }
                if (static_cast<std::int64_t>(decoded->width()) * decoded->height() > max_theme_picture_pixels) {
                    problem("more pixels than a theme's picture may have");
                    continue;
                }
                into.layers.push_back({ std::move(*decoded), named[i], std::nullopt, 0 });
            }
        };
        load(base_theme.frame_pictures, frame_pictures, "frame");
        load(base_theme.toolbar_pictures, toolbar_pictures, "toolbar");
        load(base_theme.tab_background_pictures, tab_background_pictures, "tab-background");
    }

    // A surface's pictures as they lie over `area` — the header, from the
    // window's top left — at this scale, back to front: each held where
    // its theme holds it and repeated from there as it says. Laid out when
    // first asked for and again when the area or the scale has changed;
    // null for a surface with no pictures.
    Bitmap const* laid_pictures(ThemeLayers& surface, Rect const& area)
    {
        if (surface.layers.empty() || area.is_empty())
            return nullptr;
        if (surface.laid && surface.laid->width() == area.width && surface.laid->height() == area.height
            && surface.laid_scale == scale)
            return &*surface.laid;
        Bitmap laid(area.width, area.height, Color::rgba(0, 0, 0, 0));
        auto const floor_div = [](int value, int by) { return value >= 0 ? value / by : -((-value + by - 1) / by); };
        for (auto layer = surface.layers.rbegin(); layer != surface.layers.rend(); ++layer) {
            // A picture's size is in CSS px, as every metric of a theme is.
            int const tile_width = std::max(1, static_cast<int>(std::lround(static_cast<float>(layer->picture.width()) * scale)));
            int const tile_height = std::max(1, static_cast<int>(std::lround(static_cast<float>(layer->picture.height()) * scale)));
            auto const held = [&](ThemePicture::Hold hold, int room, int size) {
                switch (hold) {
                case ThemePicture::Hold::Start: return 0;
                case ThemePicture::Hold::Center: return floor_div(room - size, 2);
                case ThemePicture::Hold::End: return room - size;
                }
                return 0;
            };
            // Where the picture itself lies.
            int const at_x = held(layer->how.across, area.width, tile_width);
            int const at_y = held(layer->how.down, area.height, tile_height);
            bool const repeats = layer->how.repeat_across || layer->how.repeat_down;
            if (!repeats || static_cast<std::int64_t>(tile_width) * tile_height > max_theme_tile_pixels) {
                // Shown once, or so large that few copies fit: drawn
                // straight in, copy by copy. Only what lands inside the
                // area is worked out, so a wallpaper held by its top costs
                // the strip of it that shows, at any scale.
                int const first_x = layer->how.repeat_across ? at_x + floor_div(-at_x, tile_width) * tile_width : at_x;
                int const first_y = layer->how.repeat_down ? at_y + floor_div(-at_y, tile_height) * tile_height : at_y;
                for (int y = first_y; y < area.height; y += tile_height) {
                    for (int x = first_x; x < area.width; x += tile_width) {
                        laid.draw_scaled(layer->picture, Rect { x, y, tile_width, tile_height });
                        if (!layer->how.repeat_across)
                            break;
                    }
                    if (!layer->how.repeat_down)
                        break;
                }
                continue;
            }
            // A tile: scaled once and kept, then read around and around,
            // so that a tile of a pixel or two costs the area and not a
            // call for every copy.
            Bitmap const* tile = &layer->picture;
            if (scale != 1) {
                if (!layer->scaled || layer->scaled_for != scale) {
                    Bitmap scaled(tile_width, tile_height, Color::rgba(0, 0, 0, 0));
                    scaled.draw_scaled(layer->picture, Rect { 0, 0, tile_width, tile_height });
                    layer->scaled = std::move(scaled);
                    layer->scaled_for = scale;
                }
                tile = &*layer->scaled;
            }
            int const from_x = layer->how.repeat_across ? 0 : std::max(0, at_x);
            int const to_x = layer->how.repeat_across ? area.width : std::min(area.width, at_x + tile_width);
            int const from_y = layer->how.repeat_down ? 0 : std::max(0, at_y);
            int const to_y = layer->how.repeat_down ? area.height : std::min(area.height, at_y + tile_height);
            for (int y = from_y; y < to_y; ++y) {
                int const tile_y = (y - at_y) - floor_div(y - at_y, tile_height) * tile_height;
                for (int x = from_x; x < to_x; ++x) {
                    int const tile_x = (x - at_x) - floor_div(x - at_x, tile_width) * tile_width;
                    laid.blend_pixel(x, y, tile->pixel(tile_x, tile_y));
                }
            }
        }
        surface.laid = std::move(laid);
        surface.laid_scale = scale;
        return &*surface.laid;
    }

    // A surface's pictures shown through `clip` — and, for a tab, through
    // the rows of its rounded shape, so that they land on the pixels the
    // tab's own fill takes and on no other.
    void paint_pictures(ThemeLayers& surface, Rect const& area, std::vector<Rect> const& through)
    {
        Bitmap const* const laid = laid_pictures(surface, area);
        if (!laid)
            return;
        for (Rect const& clip : through) {
            if (clip.is_empty())
                continue;
            frame.set_clip(clip);
            frame.draw(*laid, area.x, area.y);
        }
        frame.set_clip(std::nullopt);
    }

    // --- Menus ---------------------------------------------------------------------

    int menu_row_height() const { return static_cast<int>(theme.font_size * 2); }
    int menu_separator_height() const { return theme.padding + theme.border_width; }
    // The room between a menu's edge and its rows: a menu opens with its
    // corner at the pointer, and the pointer must not be on an item then.
    int menu_inset() const { return std::max(3, theme.padding / 2 + theme.border_width); }

    // An open menu's box and its rows. The outermost hangs from its corner
    // — its far corner, when it is aligned to its right — and goes to the
    // pointer's other side where the window leaves it no room; a submenu
    // hangs beside the row that opened it, on whichever side has room. A
    // window too short for every row shows the rows it has room for.
    MenuBox lay_out_menu(MenuLevel const& level, MenuBox const* parent) const
    {
        Theme const& t = theme;
        // The widest label and the widest shortcut as the chrome's face
        // measures them, and between and beside them room counted in the
        // width of its figures.
        float const figure = text_width(U"0", t.font_size);
        float label_width = 0;
        float shortcut_width = 0;
        bool any_checked = false;
        bool any_children = false;
        int rows_height = 0;
        for (MenuItem const& item : level.items) {
            label_width = std::max(label_width, text_width(decode_utf8(item.label), t.font_size));
            shortcut_width = std::max(shortcut_width, text_width(decode_utf8(item.shortcut), t.font_size));
            any_checked = any_checked || item.checked;
            any_children = any_children || !item.children.empty();
            rows_height += item.separator() ? menu_separator_height() : menu_row_height();
        }
        // The label, a column for the check marks when any item has one, and
        // at the right end the shortcuts or the mark of a submenu.
        float const right_column = std::max(shortcut_width > 0 ? shortcut_width + 3 * figure : 0.0f,
            any_children ? 2 * figure : 0.0f);
        float const columns = label_width + (any_checked ? 2 * figure : 0.0f) + right_column;
        int const inset = menu_inset();
        int const wanted = static_cast<int>(std::ceil(columns)) + 4 * t.padding + 2 * inset;
        int const least = static_cast<int>(std::ceil(12 * figure)) + 4 * t.padding + 2 * inset;
        int const box_width = std::min(std::max(wanted, least), std::max(1, width));
        int const box_height = std::min(rows_height + 2 * inset, std::max(1, height));
        int x = level.x;
        int y = level.y;
        if (parent && level.parent_row && *level.parent_row < parent->rows.size()) {
            // To the parent's right; to its left when only that side has
            // room; and with room on neither, against the window's edge on
            // the roomier side, over as little of the parent as it can be.
            int const overlap = 2 * t.border_width;
            int const to_right = parent->box.right() - overlap;
            int const to_left = parent->box.x - box_width + overlap;
            if (to_right + box_width <= width)
                x = to_right;
            else if (to_left >= 0)
                x = to_left;
            else
                x = width - parent->box.right() >= parent->box.x ? width - box_width : 0;
            y = parent->rows[*level.parent_row].y - inset;
        } else {
            if (level.right_aligned)
                x -= box_width;
            else if (x + box_width > width)
                x = level.x - box_width;
            if (y + box_height > height)
                y = level.y - box_height >= 0 ? level.y - box_height : height - box_height;
        }
        x = std::clamp(x, 0, std::max(0, width - box_width));
        y = std::clamp(y, 0, std::max(0, height - box_height));
        MenuBox box;
        box.box = Rect { x, y, box_width, box_height };
        int row_y = y + inset;
        for (MenuItem const& item : level.items) {
            int const row_height = item.separator() ? menu_separator_height() : menu_row_height();
            if (row_y + row_height > box.box.bottom() - inset)
                break;
            box.rows.push_back(Rect { x + inset, row_y, box_width - 2 * inset, row_height });
            row_y += row_height;
        }
        return box;
    }

    // A separator divides two groups of items: one at either end, or a
    // second in a row, divides nothing and is dropped.
    static void tidy_menu(std::vector<MenuItem>& items)
    {
        std::vector<MenuItem> kept;
        for (MenuItem& item : items) {
            if (item.separator() && (kept.empty() || kept.back().separator()))
                continue;
            tidy_menu(item.children);
            kept.push_back(std::move(item));
        }
        while (!kept.empty() && kept.back().separator())
            kept.pop_back();
        items = std::move(kept);
    }

    void open_menu(std::vector<MenuItem> items, int x, int y, bool right_aligned = false)
    {
        close_menus();
        tidy_menu(items);
        if (items.empty())
            return;
        MenuLevel level;
        level.items = std::move(items);
        level.x = x;
        level.y = y;
        level.right_aligned = right_aligned;
        menus.push_back(std::move(level));
        refresh_hover();
        dirty = true;
    }

    void close_menus()
    {
        if (menus.empty())
            return;
        menus.clear();
        main_menu_open = false;
        refresh_hover();
        dirty = true;
    }

    // The next item that can be chosen after `from` — before it, going
    // backwards — round the ends; from nowhere, the first or the last.
    static std::optional<std::size_t> next_choosable(std::vector<MenuItem> const& items,
        std::optional<std::size_t> from, bool forwards)
    {
        std::size_t const count = items.size();
        if (count == 0)
            return std::nullopt;
        std::size_t index = from ? *from : (forwards ? count - 1 : 0);
        for (std::size_t step = 0; step < count; ++step) {
            index = forwards ? (index + 1) % count : (index + count - 1) % count;
            if (items[index].choosable())
                return index;
        }
        return std::nullopt;
    }

    // The submenu of an item, open beside it; whatever was open deeper
    // than the item's own menu closes first.
    void open_submenu(std::size_t level, std::size_t index, bool highlight_first)
    {
        if (level >= menus.size() || index >= menus[level].items.size())
            return;
        menus.resize(level + 1);
        MenuLevel next;
        next.items = menus[level].items[index].children;
        next.parent_row = index;
        if (highlight_first)
            next.highlighted = next_choosable(next.items, std::nullopt, true);
        menus[level].highlighted = index;
        menus.push_back(std::move(next));
        dirty = true;
    }

    // An item chosen: its submenu opens, or the menus close and it runs —
    // in that order, since what it does may open a menu of its own.
    void choose_menu_item_at(std::size_t level, std::size_t index, bool by_keyboard)
    {
        if (level >= menus.size() || index >= menus[level].items.size())
            return;
        MenuItem& item = menus[level].items[index];
        if (!item.choosable())
            return;
        if (!item.children.empty()) {
            open_submenu(level, index, by_keyboard);
            return;
        }
        std::function<void()> const run = std::move(item.run);
        close_menus();
        if (run)
            run();
        refresh_hover();
        dirty = true;
    }

    bool choose_menu_item(std::string const& label)
    {
        for (std::size_t level = menus.size(); level-- > 0;) {
            std::vector<MenuItem> const& items = menus[level].items;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].label == label && items[i].choosable()) {
                    choose_menu_item_at(level, i, false);
                    return true;
                }
            }
        }
        return false;
    }

    std::string menu_text() const
    {
        std::string out;
        if (menus.empty())
            return out;
        for (MenuItem const& item : menus.back().items) {
            if (!out.empty())
                out += " | ";
            if (item.separator()) {
                out += "-";
                continue;
            }
            std::string const label = (item.checked ? "*" : "") + item.label + (item.children.empty() ? "" : " >");
            out += item.enabled ? label : "(" + label + ")";
        }
        return out;
    }

    // The pointer highlights the item it is over and opens that item's
    // submenu, closing any other — unless it is on its way to the submenu
    // already open and only crossing the rows between: a move that goes
    // mostly towards that submenu leaves things as they are.
    void follow_pointer_in_menus(int from_x, int from_y)
    {
        if (hover != Hover::MenuRow || hover_level >= menus.size())
            return;
        std::size_t const level = hover_level;
        std::size_t const index = hover_index;
        bool const submenu_open = menus.size() > level + 1;
        if (menus[level].highlighted == index && (submenu_open || menus[level].items[index].children.empty()))
            return;
        if (submenu_open && from_x >= 0) {
            ChromeLayout const c = layout_chrome();
            if (level + 1 < c.menus.size()) {
                int const dx = mouse_x - from_x;
                int const dy = std::abs(mouse_y - from_y);
                bool const towards = c.menus[level + 1].box.x >= c.menus[level].box.x ? dx > 0 : dx < 0;
                if (towards && dy <= 2 * std::abs(dx))
                    return;
            }
        }
        menus.resize(level + 1);
        menus[level].highlighted = index;
        if (!menus[level].items[index].children.empty())
            open_submenu(level, index, false);
        dirty = true;
    }

    // A key while a menu is open: every one of them is the menu's.
    void menu_key(KeyEvent const& key)
    {
        std::size_t const last = menus.size() - 1;
        std::optional<std::size_t> const highlighted = menus[last].highlighted;
        switch (key.key) {
        case Key::Escape:
            if (last > 0)
                menus.pop_back();
            else
                close_menus();
            break;
        case Key::Menu:
        case Key::F10:
            close_menus();
            break;
        case Key::Down: menus[last].highlighted = next_choosable(menus[last].items, highlighted, true); break;
        case Key::Up: menus[last].highlighted = next_choosable(menus[last].items, highlighted, false); break;
        case Key::Home: menus[last].highlighted = next_choosable(menus[last].items, std::nullopt, true); break;
        case Key::End: menus[last].highlighted = next_choosable(menus[last].items, std::nullopt, false); break;
        case Key::Right:
            if (highlighted && menus[last].items[*highlighted].choosable()
                && !menus[last].items[*highlighted].children.empty())
                open_submenu(last, *highlighted, true);
            break;
        case Key::Left:
            if (last > 0)
                menus.pop_back();
            break;
        case Key::Enter: // not Space: its text would arrive after the menu closed, at whatever has the focus
            if (highlighted)
                choose_menu_item_at(last, *highlighted, true);
            break;
        default:
            break;
        }
        dirty = true;
    }

    // A letter typed at a menu goes to the next item that begins with it.
    void menu_letter(char32_t code_point)
    {
        MenuLevel& level = menus.back();
        std::size_t const count = level.items.size();
        if (count == 0 || code_point >= 0x80)
            return;
        auto const lower = [](char32_t c) {
            return static_cast<char32_t>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        };
        std::size_t index = level.highlighted.value_or(count - 1);
        for (std::size_t step = 0; step < count; ++step) {
            index = (index + 1) % count;
            MenuItem const& item = level.items[index];
            if (!item.choosable() || static_cast<unsigned char>(item.label[0]) >= 0x80)
                continue;
            if (lower(static_cast<unsigned char>(item.label[0])) == lower(code_point)) {
                level.highlighted = index;
                dirty = true;
                return;
            }
        }
    }

    // The items every menu builds from.
    static MenuItem menu_item(std::string label, std::string shortcut, std::function<void()> run, bool enabled = true)
    {
        MenuItem item;
        item.label = std::move(label);
        item.shortcut = std::move(shortcut);
        item.run = std::move(run);
        item.enabled = enabled;
        return item;
    }

    bool can_view_source() const
    {
        Tab const* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        return entry && !entry->internal;
    }

    void view_source()
    {
        Tab const* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        if (!entry || entry->internal)
            return;
        if (std::optional<net::Url> const url = net::parse_url("view-source:" + entry->final_url.serialize()))
            queue(active, *url, Mode::Push);
    }

    // A tab in each container, the default first: what "new container
    // tab" and "open link in new container tab" open into.
    std::vector<MenuItem> container_items(std::function<void(std::string const&)> const& open_in) const
    {
        std::vector<MenuItem> items;
        items.push_back(menu_item("No container", {}, [open_in] { open_in({}); }));
        for (Browser::Container const& container : containers)
            items.push_back(menu_item(container.name, {}, [open_in, name = container.name] { open_in(name); }));
        return items;
    }

    // The picture box under a page point.
    static dom::Element const* hit_picture(layout::Fragment const& fragment, float x, float y)
    {
        if (!reachable_within(fragment, x, y))
            return nullptr;
        for (layout::Fragment const& child : fragment.children) {
            if (dom::Element const* const hit = hit_picture(child, x, y))
                return hit;
        }
        if (fragment.image && fragment.element && x >= fragment.x && x < fragment.x + fragment.width
            && y >= fragment.y && y < fragment.y + fragment.height)
            return fragment.element;
        return nullptr;
    }

    // The picture under a window point — an <img>'s, in the page or in a
    // frame — by the URL the page chose for it.
    std::optional<net::Url> picture_under(int x, int y)
    {
        Tab* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!tab || !entry || !point)
            return std::nullopt;
        std::vector<FrameStep> const chain = frames_at(*tab, point->first, point->second);
        layout::Fragment const* root = &tab->layout.root;
        net::Url base = entry->final_url;
        float px = point->first;
        float py = point->second;
        if (!chain.empty()) {
            root = &chain.back().view->layout.root;
            base = chain.back().view->realm->url();
            px = chain.back().x;
            py = chain.back().y;
        }
        dom::Element const* const hit = hit_picture(*root, px, py);
        if (!hit || !hit->is_html("img"))
            return std::nullopt;
        std::optional<ImageSource> const source = select_image_source(*hit, &base, tab->style_media);
        return source ? std::optional<net::Url>(source->url) : std::nullopt;
    }

    // The extension a picture's bytes ask for, by what they begin with;
    // none when they are no format known here.
    static std::string picture_extension(std::vector<std::uint8_t> const& bytes)
    {
        auto const begins = [&](std::string_view prefix) {
            return bytes.size() >= prefix.size()
                && std::equal(prefix.begin(), prefix.end(), bytes.begin(),
                    [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
        };
        if (begins("\x89PNG"))
            return ".png";
        if (begins("GIF8"))
            return ".gif";
        if (begins("\xFF\xD8"))
            return ".jpg";
        if (begins("BM"))
            return ".bmp";
        if (begins("<svg") || begins("<?xml"))
            return ".svg";
        return {};
    }

    // A picture saved to the downloads folder, as a download is: fetched
    // as the page fetched it, which the cache answers.
    void save_picture(net::Url const& url)
    {
        Tab* const tab = active_tab();
        if (!tab || downloads_directory.empty())
            return;
        HistoryEntry const* const entry = tab->current();
        std::optional<std::vector<std::uint8_t>> const bytes = image_fetcher(*tab)(url);
        if (!bytes) {
            tab->status = "Could not fetch " + url.serialize();
        } else {
            std::string const referrer = entry ? referrer_for(&entry->final_url, url) : std::string();
            // A name without an extension — a data: URL's, a script's
            // endpoint — takes the one its bytes say it should have.
            std::string name = download_file_name(nullptr, url);
            if (name.find('.') == std::string::npos)
                name += picture_extension(*bytes);
            DownloadResult const saved = save_download(downloads_directory, name, *bytes, url, referrer);
            tab->status = saved.error.empty() ? "Saved " + saved.file_name + " to " + downloads_directory
                                              : "Could not save the picture: " + saved.error;
        }
        dirty = true;
    }

    // The page hears a right click at a window point — `mousedown`, then
    // `contextmenu`, in the document the point is in — and answers whether
    // the shell's menu may show: preventing the default keeps it away.
    bool page_allows_menu(Tab& tab, int x, int y)
    {
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!point || !tab.document)
            return true;
        std::vector<FrameStep> const chain = frames_at(tab, point->first, point->second);
        FrameView* const view = chain.empty() ? nullptr : chain.back().view;
        bindings::Realm* const realm = view ? view->realm : tab.realm.get();
        if (!realm)
            return true;
        layout::Fragment const& root = view ? view->layout.root : tab.layout.root;
        float const px = view ? chain.back().x : point->first;
        float const py = view ? chain.back().y : point->second;
        dom::Element const* target = hit_run(root, px, py);
        if (!target)
            target = hit_control(root, px, py);
        if (!target)
            target = element_at_point(root, px, py);
        if (!target)
            return true;
        ChromeLayout const chrome = layout_chrome();
        bindings::MouseInit init;
        init.button = 2;
        init.client_x = static_cast<int>(std::lround(to_css_px(view ? px : static_cast<float>(x - chrome.content.x))));
        init.client_y = static_cast<int>(std::lround(to_css_px(
            view ? py - static_cast<float>(view->scroll_y) : static_cast<float>(y - chrome.content.y))));
        dom::Element& element = const_cast<dom::Element&>(*target);
        script_started = std::chrono::steady_clock::now();
        realm->dispatch_mouse_event(element, "mousedown", init);
        bool const proceed = realm->dispatch_mouse_event(element, "contextmenu", init);
        ensure_fresh(tab);
        dirty = true;
        return proceed;
    }

    // The menu for what is at a window point of the content: a link, a
    // picture, a selection or a field each bring their items, the page its
    // own when none of them is there, and Inspect closes every one.
    // `regardless` is the reader's Shift: the menu shows whatever the page
    // said.
    void open_content_menu(int x, int y, bool regardless)
    {
        Tab* const tab = active_tab();
        if (!tab)
            return;
        blur_address();
        blur_find();
        bool const allowed = page_allows_menu(*tab, x, y);
        if (!tab->document || (!allowed && !regardless))
            return;
        update_hover(x, y); // the page may have changed under the pointer
        std::vector<MenuItem> items;
        bool specific = false;
        if (hover_link) {
            net::Url const url = *hover_link;
            bool const opens = is_navigable_scheme(url.scheme);
            items.push_back(menu_item("Open link in new tab", {}, [this, url] { open_in_new_tab(url); }, opens));
            if (!containers.empty()) {
                MenuItem in_container = menu_item("Open link in new container tab", {}, {}, opens);
                in_container.children = container_items(
                    [this, url](std::string const& name) { open_tab_beside(name, url, active, false); });
                items.push_back(std::move(in_container));
            }
            items.push_back({});
            items.push_back(menu_item("Copy link address", {}, [url] { platform::write_clipboard_text(url.serialize()); }));
            specific = true;
        }
        if (std::optional<net::Url> const picture = picture_under(x, y)) {
            net::Url const url = *picture;
            items.push_back({});
            items.push_back(menu_item("Open image in new tab", {}, [this, url] { open_in_new_tab(url); }));
            items.push_back(menu_item("Copy image address", {}, [url] { platform::write_clipboard_text(url.serialize()); }));
            items.push_back(menu_item("Save image", {}, [this, url] { save_picture(url); }, !downloads_directory.empty()));
            specific = true;
        }
        dom::Element const* const control = control_at(x, y);
        bool const field = control && layout::is_text_kind(layout::control_kind(*control))
            && !control->has_attribute("disabled");
        if (field) {
            // A right click puts the caret in the field, as a left one does.
            activate_control(*control);
            std::optional<std::string> const clipboard = platform::read_clipboard_text();
            bool const can_paste = clipboard && !clipboard->empty() && !control->has_attribute("readonly");
            items.push_back({});
            // A field has a caret and no selection of its own yet, so there
            // is nothing in it to cut or copy.
            items.push_back(menu_item("Cut", "Ctrl+X", {}, false));
            items.push_back(menu_item("Copy", "Ctrl+C", {}, false));
            items.push_back(menu_item("Paste", "Ctrl+V", [this] { paste(); }, can_paste));
            specific = true;
        } else if (!selected_text(*tab).empty()) {
            items.push_back({});
            items.push_back(menu_item("Copy", "Ctrl+C", [this] { copy_selection(); }));
            specific = true;
        }
        if (!specific) {
            items.push_back(menu_item("Back", "Alt+Left", [this] { go(-1); }, can_go(-1)));
            items.push_back(menu_item("Forward", "Alt+Right", [this] { go(+1); }, can_go(+1)));
            items.push_back(menu_item("Reload", "Ctrl+R", [this] { reload(); }));
            items.push_back({});
            items.push_back(menu_item("Select all", "Ctrl+A", [this] {
                if (Tab* const current = active_tab())
                    select_all_text(*current);
            }, !tab->runs.empty()));
            items.push_back({});
            items.push_back(menu_item("View page source", "Ctrl+U", [this] { view_source(); }, can_view_source()));
        }
        // The point the reader asked about, as the page's own coordinates:
        // the panel opening moves the content's bottom edge, never its top.
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        items.push_back({});
        items.push_back(menu_item("Inspect", "F12", [this, point] {
            if (!devtools_open)
                toggle_devtools();
            Tab* const current = active_tab();
            if (current && point)
                inspect_page_point(*current, point->first, point->second);
        }, point.has_value()));
        open_menu(std::move(items), x + 1, y + 1);
    }

    void reload_tab(std::size_t index)
    {
        if (index >= tabs.size())
            return;
        if (HistoryEntry const* const entry = tabs[index].current())
            queue(index, entry->url, Mode::Reload);
    }

    // Every tab but one closes, from the far end so the indices hold.
    void close_other_tabs(std::size_t keep)
    {
        if (keep >= tabs.size())
            return;
        // A pinned tab is not one of "the others": it closes when it is
        // asked to itself, and not in passing.
        for (std::size_t i = tabs.size(); i-- > 0;) {
            if (i != keep && !tabs[i].pinned)
                close_tab(i);
        }
    }

    void close_tabs_after(std::size_t index)
    {
        for (std::size_t i = tabs.size(); i-- > index + 1;) {
            if (!tabs[i].pinned)
                close_tab(i);
        }
    }

    // Whether "close the others" / "close those to the right" would close
    // anything: a tab that is not pinned, elsewhere than `index` / after it.
    bool closable_besides(std::size_t index, bool after_only) const
    {
        for (std::size_t i = after_only ? index + 1 : 0; i < tabs.size(); ++i) {
            if (i != index && !tabs[i].pinned)
                return true;
        }
        return false;
    }

    void open_tab_menu(std::size_t index, int x, int y)
    {
        if (index >= tabs.size())
            return;
        std::vector<MenuItem> items;
        items.push_back(menu_item("New tab", "Ctrl+T", [this] { new_tab(); }));
        items.push_back({});
        items.push_back(menu_item("Reload tab", "Ctrl+R", [this, index] { reload_tab(index); }));
        items.push_back(menu_item("Duplicate tab", {}, [this, index] { duplicate_tab(index); }));
        bool const pinned = tabs[index].pinned;
        items.push_back(menu_item(pinned ? "Unpin tab" : "Pin tab", {}, [this, index, pinned] { set_pinned(index, !pinned); }));
        items.push_back({});
        items.push_back(menu_item("Close tab", "Ctrl+W", [this, index] { close_tab(index); }));
        items.push_back(menu_item("Close other tabs", {}, [this, index] { close_other_tabs(index); },
            closable_besides(index, false)));
        items.push_back(menu_item("Close tabs to the right", {}, [this, index] { close_tabs_after(index); },
            closable_besides(index, true)));
        items.push_back({});
        items.push_back(menu_item("Reopen closed tab", "Ctrl+Shift+T", [this] { reopen_closed_tab(); },
            !closed_tabs.empty()));
        open_menu(std::move(items), x + 1, y + 1);
    }

    // The address bar's text, whole, to the clipboard and out of the bar.
    void cut_address()
    {
        if (address.empty())
            return;
        platform::write_clipboard_text(address);
        address.clear();
        caret = 0;
        select_all = false;
        dirty = true;
    }

    void open_address_menu(int x, int y)
    {
        blur_find();
        focus_address(true);
        std::optional<std::string> const clipboard = platform::read_clipboard_text();
        bool const can_paste = clipboard && !clipboard->empty();
        bool const has_text = !address.empty();
        std::vector<MenuItem> items;
        items.push_back(menu_item("Cut", "Ctrl+X", [this] { cut_address(); }, has_text));
        items.push_back(menu_item("Copy", "Ctrl+C", [this] { copy_selection(); }, has_text));
        items.push_back(menu_item("Paste", "Ctrl+V", [this] { paste(); }, can_paste));
        items.push_back(menu_item("Paste and go", {}, [this] {
            std::optional<std::string> const text = platform::read_clipboard_text();
            if (!text || trim(*text).empty())
                return;
            blur_address();
            navigate(*text);
        }, can_paste));
        items.push_back({});
        items.push_back(menu_item("Select all", "Ctrl+A", [this] { focus_address(true); }, has_text));
        open_menu(std::move(items), x + 1, y + 1);
    }

    // The window's main menu, hung from its button's right end.
    void open_main_menu()
    {
        ChromeLayout const c = layout_chrome();
        std::vector<MenuItem> items;
        items.push_back(menu_item("New tab", "Ctrl+T", [this] { new_tab(); }));
        if (!containers.empty()) {
            MenuItem in_container = menu_item("New container tab", {}, {});
            in_container.children = container_items([this](std::string const& name) { new_tab_in(name); });
            items.push_back(std::move(in_container));
        }
        items.push_back({});
        items.push_back(menu_item("Find in page", "Ctrl+F", [this] { open_find(); }));
        MenuItem reader = menu_item("Reader mode", {}, [this] { toggle_reader(); }, reader_available());
        if (Tab const* const tab = active_tab(); tab && tab->current())
            reader.checked = tab->current()->url.scheme == "reader";
        items.push_back(std::move(reader));
        items.push_back({});
        if (!theme_presets.empty()) {
            MenuItem themes = menu_item("Themes", {}, {});
            for (Browser::ThemePreset const& preset : theme_presets) {
                MenuItem item = menu_item(preset.name, {}, [this, path = preset.path] { put_on_theme(path); });
                item.checked = preset.name == base_theme.name;
                themes.children.push_back(std::move(item));
            }
            themes.children.push_back({});
            themes.children.push_back(menu_item("More themes\xe2\x80\xa6", {}, [this] { open_themes_page(); }));
            items.push_back(std::move(themes));
        }
        items.push_back(menu_item("Command palette", "Ctrl+Shift+P", [this] { open_palette({}); }));
        items.push_back({});
        MenuItem devtools = menu_item("Developer tools", "F12", [this] { toggle_devtools(); });
        devtools.checked = devtools_open;
        items.push_back(std::move(devtools));
        items.push_back(menu_item("View page source", "Ctrl+U", [this] { view_source(); }, can_view_source()));
        items.push_back({});
        items.push_back(menu_item("About Sashfold", {}, [this] {
            if (std::optional<net::Url> const about = net::parse_url("about:sashfold"))
                open_tab_in({}, *about);
        }));
        items.push_back(menu_item("Quit", "Ctrl+Shift+Q", [this] { window_request = Browser::WindowRequest::Close; }));
        open_menu(std::move(items), c.menu_button.right(), c.menu_button.bottom() + theme.border_width, true);
        main_menu_open = !menus.empty();
    }

    // The menu the keyboard asks for — the Menu key, Shift+F10 — belongs
    // to whatever has the focus: the address bar, a field, else the page
    // at its top corner.
    void open_menu_by_keyboard()
    {
        ChromeLayout const c = layout_chrome();
        if (address_focus) {
            open_address_menu(c.address.x + theme.padding, c.address.bottom());
        } else if (std::optional<Rect> const field = tab_with_focused_control() ? text_input_area() : std::nullopt;
                   field && c.content.contains(field->x, field->y)) {
            open_content_menu(field->x, field->y, false);
        } else {
            open_content_menu(c.content.x + theme.padding, c.content.y + theme.padding, false);
        }
    }

    // A right click: the menu of what it landed on.
    void open_menu_at(int x, int y, bool regardless)
    {
        hints_active = false;
        if (palette_open) {
            close_palette(); // a press outside it, as any other
            return;
        }
        switch (hover) {
        case Hover::Tab:
        case Hover::TabClose: open_tab_menu(hover_index, x, y); break;
        case Hover::Address: open_address_menu(x, y); break;
        case Hover::Content: open_content_menu(x, y, regardless); break;
        default: break;
        }
    }

    // --- Input ---------------------------------------------------------------------

    // --- Selection ---------------------------------------------------------------------

    static void gather_runs(layout::Fragment const& fragment, std::vector<layout::TextRun const*>& out)
    {
        for (layout::TextRun const& run : fragment.runs)
            out.push_back(&run);
        for (layout::Fragment const& child : fragment.children)
            gather_runs(child, out);
    }

    // The code point offset in a run nearest to page x.
    static std::size_t offset_at(layout::TextRun const& run, float px)
    {
        std::size_t best = 0;
        float best_distance = std::fabs(run.x - px);
        for (std::size_t i = 1; i <= run.text.size(); ++i) {
            float const distance = std::fabs(run.x + prefix_width(run, i) - px);
            if (distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }
        return best;
    }

    // The text position nearest to a page point: inside the run under it,
    // else the nearest run on its line, else the end of the nearest line
    // above, else the very start.
    static std::optional<TextPosition> position_at(std::vector<layout::TextRun const*> const& runs, float px, float py)
    {
        if (runs.empty())
            return std::nullopt;
        std::optional<std::size_t> same_line;
        float same_line_distance = 0;
        std::optional<std::size_t> above;
        float above_bottom = 0;
        for (std::size_t i = 0; i < runs.size(); ++i) {
            layout::TextRun const& run = *runs[i];
            if (run.text.empty())
                continue;
            text::FaceMetrics const metrics = run_metrics(run);
            float const top = run.baseline_y - metrics.ascent;
            float const bottom = run.baseline_y + metrics.descent;
            if (py >= top && py < bottom) {
                if (px >= run.x && px < run.x + run.width)
                    return TextPosition { i, offset_at(run, px) };
                float const distance = px < run.x ? run.x - px : px - (run.x + run.width);
                if (!same_line || distance < same_line_distance) {
                    same_line = i;
                    same_line_distance = distance;
                }
            } else if (bottom <= py && (!above || bottom >= above_bottom)) {
                above = i;
                above_bottom = bottom;
            }
        }
        if (same_line) {
            layout::TextRun const& run = *runs[*same_line];
            return TextPosition { *same_line, px < run.x ? 0 : run.text.size() };
        }
        if (above)
            return TextPosition { *above, runs[*above]->text.size() };
        return TextPosition { 0, 0 };
    }

    static std::pair<TextPosition, TextPosition> ordered(Selection const& selection)
    {
        if (selection.focus < selection.anchor)
            return { selection.focus, selection.anchor };
        return { selection.anchor, selection.focus };
    }

    // The runs a tab's selection indexes: the frame's it is in, else the page's.
    static std::vector<layout::TextRun const*> const& runs_of(Tab const& tab)
    {
        if (tab.selection_frame) {
            if (FrameView const* const view = view_of(tab, tab.selection_frame))
                return view->runs;
        }
        return tab.runs;
    }

    // The selected text: each run's slice, lines separated by newlines.
    static std::string selected_text(Tab const& tab)
    {
        std::vector<layout::TextRun const*> const& runs = runs_of(tab);
        if (!tab.selection || runs.empty())
            return {};
        auto const [start, end] = ordered(*tab.selection);
        std::string out;
        std::optional<float> last_baseline;
        for (std::size_t i = start.run; i <= end.run && i < runs.size(); ++i) {
            layout::TextRun const& run = *runs[i];
            std::size_t const from = i == start.run ? std::min(start.offset, run.text.size()) : 0;
            std::size_t const to = i == end.run ? std::min(end.offset, run.text.size()) : run.text.size();
            if (to <= from)
                continue;
            if (last_baseline && run.baseline_y != *last_baseline)
                out += '\n';
            last_baseline = run.baseline_y;
            out += to_utf8(std::u32string_view(run.text).substr(from, to - from));
        }
        return out;
    }

    // A position one code point along, crossing into the next or previous
    // run past a run's ends.
    static TextPosition step(Tab const& tab, TextPosition position, int direction)
    {
        std::vector<layout::TextRun const*> const& runs = runs_of(tab);
        std::size_t const size = runs[position.run]->text.size();
        if (direction > 0) {
            if (position.offset < size) {
                ++position.offset;
            } else if (position.run + 1 < runs.size()) {
                ++position.run;
                position.offset = std::min<std::size_t>(1, runs[position.run]->text.size());
            }
        } else {
            if (position.offset > 0) {
                --position.offset;
            } else if (position.run > 0) {
                --position.run;
                std::size_t const previous = runs[position.run]->text.size();
                position.offset = previous > 0 ? previous - 1 : 0;
            }
        }
        return position;
    }

    // The position on the line above or below, at the same x.
    static std::optional<TextPosition> line_step(Tab const& tab, TextPosition position, int direction)
    {
        std::vector<layout::TextRun const*> const& runs = runs_of(tab);
        layout::TextRun const& run = *runs[position.run];
        float const x = run.x + prefix_width(run, position.offset);
        std::optional<float> target;
        for (layout::TextRun const* const other : runs) {
            if (other->text.empty())
                continue;
            float const baseline = other->baseline_y;
            bool const candidate = direction > 0 ? baseline > run.baseline_y : baseline < run.baseline_y;
            if (!candidate)
                continue;
            if (!target || (direction > 0 ? baseline < *target : baseline > *target))
                target = baseline;
        }
        if (!target)
            return std::nullopt;
        return position_at(runs, x, *target);
    }

    // The first or last position on the line a position sits on.
    static TextPosition line_end(Tab const& tab, TextPosition position, bool end)
    {
        std::vector<layout::TextRun const*> const& runs = runs_of(tab);
        float const baseline = runs[position.run]->baseline_y;
        std::size_t index = position.run;
        if (end) {
            while (index + 1 < runs.size() && runs[index + 1]->baseline_y == baseline)
                ++index;
            return TextPosition { index, runs[index]->text.size() };
        }
        while (index > 0 && runs[index - 1]->baseline_y == baseline)
            --index;
        return TextPosition { index, 0 };
    }

    // Drops the selection; a frame that showed it is painted again without it.
    void clear_selection(Tab& tab)
    {
        dom::Element const* const previous = tab.selection_frame;
        tab.selection.reset();
        tab.selection_frame = nullptr;
        tab.selection_view.reset();
        selecting = false;
        if (previous)
            repaint_frames(tab, frames_to(tab, previous));
        dirty = true;
    }

    // A selection begins under the pointer, in one document: the page's, or
    // the frame's the pointer is over.
    void start_selection(Tab& tab, int x, int y)
    {
        clear_selection(tab);
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!point)
            return;
        std::vector<FrameStep> const chain = frames_at(tab, point->first, point->second);
        bool const in_frame = !chain.empty();
        std::vector<layout::TextRun const*> const& runs = in_frame ? chain.back().view->runs : tab.runs;
        std::optional<TextPosition> const position
            = position_at(runs, in_frame ? chain.back().x : point->first, in_frame ? chain.back().y : point->second);
        if (!position)
            return;
        tab.selection = Selection { *position, *position };
        if (in_frame) {
            FrameStep const& step = chain.back();
            tab.selection_frame = step.container;
            if (auto const it = step.holder->find(step.container); it != step.holder->end())
                tab.selection_view = it->second.view;
        }
        selecting = true;
        dirty = true;
    }

    void mouse_move(int x, int y)
    {
        update_hover(x, y);
        if (bar_drag) {
            Tab* const tab = active_tab();
            ChromeLayout const c = layout_chrome();
            layout::Fragment const* const box
                = tab ? fragment_for(tab->layout.root, bar_drag->box) : nullptr;
            if (box) {
                // The pointer may leave the bar; the thumb follows it along
                // its own axis all the same, which is what a drag means.
                drag_bar_to(*tab, *box, bar_drag->vertical, bar_drag->grab,
                    static_cast<float>(x - c.content.x),
                    static_cast<float>(y - c.content.y + tab->scroll_y));
            }
            return;
        }
        if (!selecting)
            return;
        Tab* const tab = active_tab();
        if (!tab || !tab->selection)
            return;
        // The drag may leave the content area: the point is held to its edges.
        ChromeLayout const c = layout_chrome();
        float const px = static_cast<float>(std::clamp(x, c.content.x, c.content.x + c.content.width - 1)
            - c.content.x);
        float const py = static_cast<float>(std::clamp(y, c.content.y, c.content.y + c.content.height - 1)
            - c.content.y + tab->scroll_y);
        if (tab->selection_frame) {
            // A selection in a frame follows the pointer in that document,
            // past the frame's edges too.
            std::vector<FrameStep> const chain = frames_to(*tab, tab->selection_frame);
            if (chain.empty())
                return;
            auto const [ox, oy] = origin_of(chain);
            if (std::optional<TextPosition> const focus = position_at(chain.back().view->runs, px - ox, py - oy);
                focus && !(*focus == tab->selection->focus)) {
                tab->selection->focus = *focus;
                repaint_frames(*tab, chain);
            }
            return;
        }
        if (std::optional<TextPosition> const focus = position_at(tab->runs, px, py);
            focus && !(*focus == tab->selection->focus)) {
            tab->selection->focus = *focus;
            dirty = true;
        }
    }

    void mouse_up(int button)
    {
        if (button == 1) {
            selecting = false;
            bar_drag.reset();
            if (pressed != Hover::None) {
                pressed = Hover::None;
                dirty = true;
            }
        }
        // The button that opened a menu, let go over one of its items after
        // a slide there, chooses it: press, slide, release. A click's own
        // few pixels of travel are no slide — a menu opens with its first
        // item a hair from the pointer, and a shaky click must not choose it.
        if (button == menu_press_button) {
            menu_press_button = 0;
            int const travelled = std::abs(mouse_x - menu_press_x) + std::abs(mouse_y - menu_press_y);
            int const slide = static_cast<int>(std::lround(12 * scale));
            if (!menus.empty() && hover == Hover::MenuRow && travelled >= slide)
                choose_menu_item_at(hover_level, hover_index, false);
        }
    }

    void select_all_text(Tab& tab)
    {
        if (tab.runs.empty())
            return;
        clear_selection(tab); // the page's text, whatever frame held one
        tab.selection = Selection { TextPosition { 0, 0 },
            TextPosition { tab.runs.size() - 1, tab.runs.back()->text.size() } };
        dirty = true;
    }

    // Shift with an arrow, Home or End: the focus moves, the anchor stays.
    bool extend_selection(Tab& tab, KeyEvent const& key)
    {
        if (!tab.selection || tab.runs.empty())
            return false;
        TextPosition const focus = tab.selection->focus;
        std::optional<TextPosition> next;
        switch (key.key) {
        case Key::Left: next = step(tab, focus, -1); break;
        case Key::Right: next = step(tab, focus, +1); break;
        case Key::Up: next = line_step(tab, focus, -1); break;
        case Key::Down: next = line_step(tab, focus, +1); break;
        case Key::Home: next = line_end(tab, focus, false); break;
        case Key::End: next = line_end(tab, focus, true); break;
        default: return false;
        }
        if (next && !(*next == focus)) {
            tab.selection->focus = *next;
            if (tab.selection_frame)
                repaint_frames(tab, frames_to(tab, tab.selection_frame));
            dirty = true;
        }
        return true;
    }

    void copy_selection()
    {
        if (address_focus) {
            if (!address.empty())
                platform::write_clipboard_text(address);
            return;
        }
        Tab const* const tab = active_tab();
        if (!tab)
            return;
        std::string const text = selected_text(*tab);
        if (!text.empty())
            platform::write_clipboard_text(text);
    }

    // The clipboard's text typed into whatever has focus.
    void paste()
    {
        std::optional<std::string> const text = platform::read_clipboard_text();
        if (!text || text->empty())
            return;
        bool const into_control = !address_focus && tab_with_focused_control() != nullptr;
        if (!address_focus && !into_control)
            return;
        for (char32_t const c : decode_utf8(*text)) {
            if (c >= 0x20 && c != 0x7F)
                text_input(c);
        }
    }

    // --- Devtools --------------------------------------------------------------------

    static constexpr int tree_line_height = 16;

    struct TreeLine {
        int depth;
        dom::Node const* node;
        std::string text;
    };

    // The document as an outline: elements with their id and class, text
    // nodes as short quotes.
    static void build_tree(dom::Node const& node, int depth, std::vector<TreeLine>& lines)
    {
        for (dom::Node const* child : node.children()) {
            if (child->is_element()) {
                auto const& element = static_cast<dom::Element const&>(*child);
                std::string text = "<" + element.local_name();
                if (dom::Attr const* const id = element.find_attribute("id"))
                    text += " id=\"" + id->value + "\"";
                if (dom::Attr const* const classes = element.find_attribute("class"))
                    text += " class=\"" + classes->value + "\"";
                text += ">";
                lines.push_back(TreeLine { depth, child, std::move(text) });
                build_tree(*child, depth + 1, lines);
            } else if (child->is_text()) {
                std::string const snippet
                    = collapse_whitespace(static_cast<dom::Text const*>(child)->data);
                if (snippet.empty())
                    continue;
                lines.push_back(TreeLine { depth, child,
                    "\"" + (snippet.size() > 60 ? snippet.substr(0, 57) + "..." : snippet) + "\"" });
            }
        }
    }

    // The nearest element: the node itself, or a text node's parent.
    static dom::Element const* element_of(dom::Node const* node)
    {
        for (dom::Node const* current = node; current; current = current->parent()) {
            if (current->is_element())
                return static_cast<dom::Element const*>(current);
        }
        return nullptr;
    }

    // "tag#id.class.class" for the nearest element.
    static std::string node_summary(dom::Node const* node)
    {
        dom::Element const* const element = element_of(node);
        if (!element)
            return {};
        std::string summary = element->local_name();
        if (dom::Attr const* const id = element->find_attribute("id"))
            summary += "#" + id->value;
        if (dom::Attr const* const classes = element->find_attribute("class")) {
            std::string name;
            for (char const c : classes->value + " ") {
                if (c == ' ' || c == '\t' || c == '\n') {
                    if (!name.empty())
                        summary += "." + name;
                    name.clear();
                } else {
                    name += c;
                }
            }
        }
        return summary;
    }

    static layout::Fragment const* fragment_for(layout::Fragment const& fragment,
        dom::Element const* target)
    {
        if (fragment.element == target)
            return &fragment;
        for (layout::Fragment const& child : fragment.children) {
            if (layout::Fragment const* const found = fragment_for(child, target))
                return found;
        }
        return nullptr;
    }

    // The deepest box under a page point.
    static dom::Element const* element_at_point(layout::Fragment const& fragment, float px, float py)
    {
        for (layout::Fragment const& child : fragment.children) {
            if (!reachable_within(child, px, py))
                continue; // scrolled or clipped out of sight, and out of reach with it
            if (dom::Element const* const hit = element_at_point(child, px, py))
                return hit;
        }
        if (fragment.element && px >= fragment.x && px < fragment.x + fragment.width && py >= fragment.y
            && py < fragment.y + fragment.height)
            return fragment.element;
        return nullptr;
    }

    static std::string number_text(float value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.2f", static_cast<double>(value));
        std::string text = buffer;
        while (text.size() > 1 && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
        return text;
    }

    static std::string length_text(css::LengthPercent const& length)
    {
        switch (length.kind) {
        case css::LengthPercent::Kind::Auto: return "auto";
        case css::LengthPercent::Kind::Px: return number_text(length.value) + "px";
        case css::LengthPercent::Kind::Percent: return number_text(length.value) + "%";
        case css::LengthPercent::Kind::Calc:
            return "calc(" + number_text(length.percent) + "% "
                + (length.value < 0 ? "- " + number_text(-length.value) : "+ " + number_text(length.value)) + "px)";
        case css::LengthPercent::Kind::MinContent: return "min-content";
        case css::LengthPercent::Kind::MaxContent: return "max-content";
        case css::LengthPercent::Kind::FitContent: return "fit-content";
        }
        return "?";
    }

    static std::string color_text(Color color)
    {
        char buffer[16];
        if (color.a == 255)
            std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x", color.r, color.g, color.b);
        else
            std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x%02x", color.r, color.g, color.b, color.a);
        return buffer;
    }

    static char const* overflow_text(css::Overflow overflow)
    {
        switch (overflow) {
        case css::Overflow::Visible: return "visible";
        case css::Overflow::Clip: return "clip";
        case css::Overflow::Hidden: return "hidden";
        case css::Overflow::Auto: return "auto";
        case css::Overflow::Scroll: return "scroll";
        }
        return "visible";
    }

    static char const* display_text(css::Display display)
    {
        switch (display) {
        case css::Display::Block: return "block";
        case css::Display::Inline: return "inline";
        case css::Display::ListItem: return "list-item";
        case css::Display::FlowRoot: return "flow-root";
        case css::Display::Flex: return "flex";
        case css::Display::Grid: return "grid";
        case css::Display::InlineBlock: return "inline-block";
        case css::Display::InlineFlex: return "inline-flex";
        case css::Display::InlineGrid: return "inline-grid";
        case css::Display::Table: return "table";
        case css::Display::InlineTable: return "inline-table";
        case css::Display::TableRowGroup: return "table-row-group";
        case css::Display::TableHeaderGroup: return "table-header-group";
        case css::Display::TableFooterGroup: return "table-footer-group";
        case css::Display::TableRow: return "table-row";
        case css::Display::TableCell: return "table-cell";
        case css::Display::TableCaption: return "table-caption";
        case css::Display::TableColumnGroup: return "table-column-group";
        case css::Display::TableColumn: return "table-column";
        case css::Display::None: return "none";
        }
        return "?";
    }

    // What the styles pane says about the inspected node.
    std::vector<std::string> style_lines(Tab const& tab) const
    {
        std::vector<std::string> lines;
        dom::Element const* const element = element_of(tab.inspected);
        if (!element) {
            lines.push_back("Click the page, or a line of the tree, to inspect an element.");
            return lines;
        }
        lines.push_back(node_summary(element));
        if (layout::Fragment const* const box = fragment_for(tab.layout.root, element))
            lines.push_back("box " + number_text(box->x) + "," + number_text(box->y) + "  "
                + number_text(box->width) + " x " + number_text(box->height));
        else
            lines.push_back("box inline");
        auto const it = tab.styles.find(element);
        if (it == tab.styles.end())
            return lines;
        css::ComputedStyle const& s = it->second;
        std::string display = std::string("display ") + display_text(s.display);
        if (s.floating != css::Float::None)
            display += std::string("  float ") + (s.floating == css::Float::Left ? "left" : "right");
        if (s.overflow != css::Overflow::Visible) {
            display += std::string("  overflow ") + overflow_text(s.overflow_x);
            if (s.overflow_y != s.overflow_x)
                display += std::string(" ") + overflow_text(s.overflow_y);
        }
        lines.push_back(display);
        lines.push_back("width " + length_text(s.width) + "  height " + length_text(s.height));
        lines.push_back("margin " + length_text(s.margin_top) + " " + length_text(s.margin_right) + " "
            + length_text(s.margin_bottom) + " " + length_text(s.margin_left));
        lines.push_back("padding " + length_text(s.padding_top) + " " + length_text(s.padding_right) + " "
            + length_text(s.padding_bottom) + " " + length_text(s.padding_left));
        lines.push_back("border " + number_text(s.border_top.width) + " " + number_text(s.border_right.width)
            + " " + number_text(s.border_bottom.width) + " " + number_text(s.border_left.width));
        std::string const family
            = s.font_family && !s.font_family->empty() ? s.font_family->front() : "(default)";
        lines.push_back("font " + family + " " + number_text(s.font_size) + "px" + (s.bold() ? " bold" : "")
            + (s.font_style == css::FontStyle::Italic ? " italic" : "") + "  line-height "
            + number_text(s.line_height_px()));
        lines.push_back("color " + color_text(s.color) + "  background " + color_text(s.background_color));
        if (s.display == css::Display::Flex || s.display == css::Display::InlineFlex) {
            bool const row = s.flex_direction == css::FlexDirection::Row
                || s.flex_direction == css::FlexDirection::RowReverse;
            lines.push_back(std::string("flex ") + (row ? "row" : "column")
                + (s.flex_wrap != css::FlexWrap::NoWrap ? " wrap" : ""));
        }
        return lines;
    }

    // Selects a node, keeping its line in view.
    void inspect(Tab& tab, dom::Node const* node)
    {
        tab.inspected = node;
        std::vector<TreeLine> lines;
        if (tab.document)
            build_tree(*tab.document, 0, lines);
        ChromeLayout const c = layout_chrome();
        int const visible = std::max(1, c.devtools_tree.height / tree_line_height);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].node != node)
                continue;
            int const index = static_cast<int>(i);
            if (index < tab.tree_scroll || index >= tab.tree_scroll + visible)
                tab.tree_scroll = std::max(0, index - visible / 2);
            break;
        }
        dirty = true;
    }

    // The element under a window point: its run, its control, or its box.
    void inspect_at(Tab& tab, int x, int y)
    {
        if (std::optional<std::pair<float, float>> const point = page_point(x, y))
            inspect_page_point(tab, point->first, point->second);
    }

    void inspect_page_point(Tab& tab, float px, float py)
    {
        dom::Element const* element = hit_run(tab.layout.root, px, py);
        if (!element)
            element = hit_control(tab.layout.root, px, py);
        if (!element)
            element = element_at_point(tab.layout.root, px, py);
        if (element)
            inspect(tab, element);
    }

    void inspect_tree_line(Tab& tab, int y)
    {
        ChromeLayout const c = layout_chrome();
        std::vector<TreeLine> lines;
        if (tab.document)
            build_tree(*tab.document, 0, lines);
        int const index = tab.tree_scroll + (y - c.devtools_tree.y) / tree_line_height;
        if (index >= 0 && static_cast<std::size_t>(index) < lines.size())
            inspect(tab, lines[static_cast<std::size_t>(index)].node);
    }

    void toggle_devtools()
    {
        devtools_open = !devtools_open;
        if (Tab* const tab = active_tab())
            set_scroll(*tab, tab->scroll_y); // the content is shorter or taller now
        dirty = true;
    }

    // --- Keyboard link-hints ---------------------------------------------------------

    // A label on every link with a run in view, in reading order, from a
    // home-row alphabet with as many letters as the count needs.
    void start_hints(Tab& tab)
    {
        hints.clear();
        hint_typed.clear();
        HistoryEntry const* const entry = tab.current();
        if (!entry)
            return;
        ChromeLayout const c = layout_chrome();
        float const top = static_cast<float>(tab.scroll_y);
        float const bottom = top + static_cast<float>(c.content.height);
        std::vector<dom::Element const*> seen;
        for (layout::TextRun const* const run : tab.runs) {
            if (run->text.empty())
                continue;
            text::FaceMetrics const metrics = run_metrics(*run);
            if (run->baseline_y + metrics.descent < top || run->baseline_y - metrics.ascent > bottom)
                continue;
            dom::Element const* anchor = nullptr;
            for (dom::Node const* node = run->element; node; node = node->parent()) {
                if (!node->is_element())
                    continue;
                auto const& element = static_cast<dom::Element const&>(*node);
                if (element.is_html("a") && element.find_attribute("href")) {
                    anchor = &element;
                    break;
                }
            }
            if (!anchor || std::find(seen.begin(), seen.end(), anchor) != seen.end())
                continue;
            std::optional<net::Url> const url
                = net::parse_url(anchor->find_attribute("href")->value, &entry->final_url);
            if (!url)
                continue;
            seen.push_back(anchor);
            hints.push_back(Hint { {}, *url, run->x, run->baseline_y - metrics.ascent });
        }
        if (hints.empty())
            return;
        static constexpr char alphabet[] = "asdfghjkl";
        std::size_t length = 1;
        std::size_t room = 9;
        while (room < hints.size()) {
            room *= 9;
            ++length;
        }
        for (std::size_t i = 0; i < hints.size(); ++i) {
            std::string label(length, 'a');
            std::size_t value = i;
            for (std::size_t k = length; k > 0; --k) {
                label[k - 1] = alphabet[value % 9];
                value /= 9;
            }
            hints[i].label = label;
        }
        hints_active = true;
        dirty = true;
    }

    void stop_hints()
    {
        if (!hints_active && hints.empty())
            return;
        hints_active = false;
        hints.clear();
        hint_typed.clear();
        dirty = true;
    }

    // A letter narrows the labels; the full label follows its link; a
    // letter no label starts with begins again; Escape puts them away.
    void hint_key(KeyEvent const& key)
    {
        if (key.key == Key::Escape) {
            stop_hints();
            return;
        }
        if (key.key == Key::Backspace) {
            if (!hint_typed.empty())
                hint_typed.pop_back();
            dirty = true;
            return;
        }
        if (key.key != Key::Letter || key.ctrl || key.alt)
            return;
        hint_typed += static_cast<char>(to_ascii_lowercase(key.letter));
        bool any = false;
        for (Hint const& hint : hints) {
            if (hint.label == hint_typed) {
                net::Url const url = hint.url;
                stop_hints();
                open(url);
                return;
            }
            if (hint.label.compare(0, hint_typed.size(), hint_typed) == 0)
                any = true;
        }
        if (!any)
            hint_typed.clear();
        dirty = true;
    }

    // --- Reader mode -----------------------------------------------------------------

    // A page fetched from the web, a file or a data: URL can be read; the
    // shell's own pages cannot. A reader page can always be left.
    bool reader_available() const
    {
        Tab const* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        if (!entry)
            return false;
        if (entry->url.scheme == "reader")
            return true;
        std::string const& scheme = entry->final_url.scheme;
        return !entry->internal
            && (is_web_scheme(scheme) || scheme == "file" || scheme == "data");
    }

    // Into reader mode for the current page, or back out of it.
    void toggle_reader()
    {
        Tab* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        if (!entry || !reader_available())
            return;
        if (entry->url.scheme == "reader") {
            if (std::optional<net::Url> const inner = net::parse_url(entry->url.serialize_path()))
                queue(active, *inner, Mode::Push);
            return;
        }
        if (std::optional<net::Url> const url = net::parse_url("reader:" + entry->final_url.serialize()))
            queue(active, *url, Mode::Push);
    }

    // --- Find in page ----------------------------------------------------------------

    // A translucent band over the runs between two positions.
    // Translucent bands over a stretch of runs, on a picture of the document
    // seen from `scroll_y` down.
    static void paint_bands(Bitmap& content, std::vector<layout::TextRun const*> const& runs, TextPosition start,
        TextPosition end, Color color, float scroll_y)
    {
        for (std::size_t i = start.run; i <= end.run && i < runs.size(); ++i) {
            layout::TextRun const& run = *runs[i];
            std::size_t const from = i == start.run ? std::min(start.offset, run.text.size()) : 0;
            std::size_t const to = i == end.run ? std::min(end.offset, run.text.size()) : run.text.size();
            if (to <= from)
                continue;
            text::FaceMetrics const metrics = run_metrics(run);
            float const x1 = run.x + prefix_width(run, from);
            float const x2 = run.x + prefix_width(run, to);
            float const top = run.baseline_y - metrics.ascent - scroll_y;
            content.fill_rect(Rect { static_cast<int>(x1 + 0.5f), static_cast<int>(top + 0.5f),
                                  static_cast<int>(x2 - x1 + 0.5f),
                                  static_cast<int>(metrics.ascent + metrics.descent + 0.5f) },
                color);
        }
    }

    static void paint_bands(Bitmap& content, Tab const& tab, TextPosition start, TextPosition end, Color color)
    {
        paint_bands(content, tab.runs, start, end, color, static_cast<float>(tab.scroll_y));
    }

    // The editing keys the find box takes — Backspace, Delete, Left, Right,
    // Home, End — over a UTF-8 string with a byte caret and a select-all
    // flag. True when the key was one of those.
    static bool edit_text(std::string& text, std::size_t& caret, bool& select_all, KeyEvent const& key)
    {
        switch (key.key) {
        case Key::Backspace:
            if (select_all) {
                text.clear();
                caret = 0;
                select_all = false;
            } else if (caret > 0) {
                std::size_t const previous = previous_code_point(text, caret);
                text.erase(previous, caret - previous);
                caret = previous;
            }
            return true;
        case Key::Delete:
            if (select_all) {
                text.clear();
                caret = 0;
                select_all = false;
            } else if (caret < text.size()) {
                text.erase(caret, next_code_point(text, caret) - caret);
            }
            return true;
        case Key::Left:
            if (select_all) {
                select_all = false;
                caret = 0;
            } else {
                caret = previous_code_point(text, caret);
            }
            return true;
        case Key::Right:
            if (select_all) {
                select_all = false;
                caret = text.size();
            } else {
                caret = next_code_point(text, caret);
            }
            return true;
        case Key::Home:
            select_all = false;
            caret = 0;
            return true;
        case Key::End:
            select_all = false;
            caret = text.size();
            return true;
        default:
            return false;
        }
    }

    // ASCII letters folded to lowercase; everything else as it is.
    static std::u32string folded(std::u32string_view text)
    {
        std::u32string out;
        out.reserve(text.size());
        for (char32_t const c : text)
            out.push_back(c < 0x80 ? to_ascii_lowercase(c) : c);
        return out;
    }

    // Every occurrence of the query in the tab's text, line by line; the
    // current match keeps its place while it still exists.
    void update_matches(Tab& tab)
    {
        tab.matches.clear();
        if (!find_open || find_query.empty()) {
            tab.current_match = 0;
            return;
        }
        std::u32string const needle = folded(decode_utf8(find_query));
        std::size_t line_start = 0;
        while (line_start < tab.runs.size()) {
            std::size_t line_end = line_start + 1;
            while (line_end < tab.runs.size()
                && tab.runs[line_end]->baseline_y == tab.runs[line_start]->baseline_y)
                ++line_end;
            std::u32string line;
            std::vector<std::size_t> starts;
            for (std::size_t i = line_start; i < line_end; ++i) {
                starts.push_back(line.size());
                line += folded(tab.runs[i]->text);
            }
            auto const locate = [&](std::size_t index) {
                std::size_t run = 0;
                for (std::size_t k = 0; k < starts.size(); ++k) {
                    if (starts[k] <= index)
                        run = k;
                }
                return TextPosition { line_start + run, index - starts[run] };
            };
            for (std::size_t at = line.find(needle); at != std::u32string::npos;
                 at = line.find(needle, at + needle.size()))
                tab.matches.push_back(Match { locate(at), locate(at + needle.size()) });
            line_start = line_end;
        }
        if (tab.current_match >= tab.matches.size())
            tab.current_match = 0;
    }

    // Scrolls so the current match's line is in view.
    void scroll_to_match(Tab& tab)
    {
        if (tab.matches.empty())
            return;
        Match const& match = tab.matches[std::min(tab.current_match, tab.matches.size() - 1)];
        if (match.start.run >= tab.runs.size())
            return;
        layout::TextRun const& run = *tab.runs[match.start.run];
        text::FaceMetrics const metrics = run_metrics(run);
        // A match inside a box that scrolls is out of sight until that box
        // is moved, and moving the page alone would never show it. The
        // rectangle is read again after each box moves, since the ones
        // inside it travelled along.
        auto const box_of_run = [&] {
            return Rect { static_cast<int>(run.x),
                static_cast<int>(run.baseline_y - metrics.ascent), static_cast<int>(run.width) + 1,
                static_cast<int>(metrics.ascent + metrics.descent) + 1 };
        };
        std::vector<layout::Fragment const*> chain;
        if (scroll_chain(tab.layout.root, nullptr, &run, chain) && !chain.empty())
            reveal_within_boxes(tab, chain, box_of_run);
        Rect const at = box_of_run();
        ChromeLayout const c = layout_chrome();
        if (at.y >= tab.scroll_y && at.bottom() <= tab.scroll_y + c.content.height)
            return;
        set_scroll(tab, at.y - c.content.height / 3);
    }

    void focus_find(bool select_everything)
    {
        find_focus = true;
        find_select_all = select_everything && !find_query.empty();
        find_caret = find_query.size();
        dirty = true;
    }

    void blur_find()
    {
        if (!find_focus)
            return;
        find_focus = false;
        find_select_all = false;
        dirty = true;
    }

    void open_find()
    {
        find_open = true;
        blur_address();
        focus_find(true);
        if (Tab* const tab = active_tab()) {
            update_matches(*tab);
            scroll_to_match(*tab);
        }
        dirty = true;
    }

    void close_find()
    {
        find_open = false;
        find_focus = false;
        find_select_all = false;
        for (Tab& tab : tabs)
            update_matches(tab);
        dirty = true;
    }

    void find_step(int direction)
    {
        Tab* const tab = active_tab();
        if (!tab || tab->matches.empty())
            return;
        std::size_t const count = tab->matches.size();
        tab->current_match = (tab->current_match + (direction > 0 ? 1 : count - 1)) % count;
        scroll_to_match(*tab);
        dirty = true;
    }

    void refind()
    {
        if (Tab* const tab = active_tab()) {
            tab->current_match = 0;
            update_matches(*tab);
            scroll_to_match(*tab);
        }
        dirty = true;
    }

    void edit_find(KeyEvent const& key)
    {
        if (key.key == Key::Enter) {
            find_step(key.shift ? -1 : +1);
            return;
        }
        if (key.key == Key::Escape) {
            close_find();
            return;
        }
        if (edit_text(find_query, find_caret, find_select_all, key))
            refind();
    }

    void type_into_find(char32_t code_point)
    {
        if (find_select_all) {
            find_query.clear();
            find_caret = 0;
            find_select_all = false;
        }
        std::string utf8;
        append_utf8(utf8, code_point);
        find_query.insert(find_caret, utf8);
        find_caret += utf8.size();
        refind();
    }

    std::string find_status(Tab const& tab) const
    {
        if (!find_open || find_query.empty())
            return {};
        if (tab.matches.empty())
            return "No matches";
        return std::to_string(tab.current_match + 1) + " of " + std::to_string(tab.matches.size());
    }

    // --- Forms -----------------------------------------------------------------------

    Tab* tab_with_focused_control()
    {
        Tab* const tab = active_tab();
        return tab && tab->document && tab->controls.focused ? tab : nullptr;
    }

    layout::ControlState& state_of(Tab& tab, dom::Element const& control)
    {
        return tab.controls.states[&control];
    }

    static std::string utf8_of(std::u32string const& text)
    {
        std::string out;
        for (char32_t const c : text)
            append_utf8(out, c);
        return out;
    }

    // Puts the caret at the end of a text control's value.
    void caret_to_end(Tab& tab, dom::Element const& control)
    {
        if (layout::is_text_kind(layout::control_kind(control)))
            state_of(tab, control).caret
                = decode_utf8(layout::control_value(control, &tab.controls)).size();
    }

    void blur_control()
    {
        Tab* const tab = active_tab();
        if (!tab || !tab->controls.focused)
            return;
        tab->controls.focused = nullptr;
        relayout(*tab);
        dirty = true;
    }

    // Checks a radio button and clears the rest of its group: the same
    // name within the same form.
    void check_radio(Tab& tab, dom::Element const& radio)
    {
        dom::Attr const* const name = radio.find_attribute("name");
        dom::Element const* const form = form_owner(radio, *tab.document);
        for (dom::Element const* const other : focusable_controls(*tab.document)) {
            if (layout::control_kind(*other) != layout::ControlKind::Radio)
                continue;
            dom::Attr const* const other_name = other->find_attribute("name");
            bool const same_group = name && other_name && other_name->value == name->value
                && form_owner(*other, *tab.document) == form;
            if (other == &radio || same_group)
                state_of(tab, *other).checked = other == &radio;
        }
    }

    // Submits the form a control belongs to — the control itself as the
    // submitter when it is a submit button, else the form's first one.
    void submit(Tab& tab, dom::Element const& control)
    {
        HistoryEntry const* const entry = tab.current();
        // The control's own document: a frame's, when it is in one.
        dom::Document const& document = control.document();
        dom::Element const* const form = form_owner(control, document);
        if (!form || !entry)
            return;
        dom::Element const* const submitter
            = layout::control_kind(control) == layout::ControlKind::Submit ? &control
                                                                            : default_submitter(*form);
        FrameView const* const view = view_of(tab, frame_holding_in(tab.frames, document));
        std::optional<net::Url> const url
            = get_submission_url(*form, submitter, &tab.controls, view ? view->realm->url() : entry->final_url);
        if (!url) {
            tab.status = "This form posts; only GET forms are written yet";
            dirty = true;
            return;
        }
        if (!submission_allowed(tab, *url))
            return;
        navigate_document(tab, document, *url);
    }

    // The page's policy's say on a form submission: a sandbox without
    // allow-forms submits nothing, and form-action judges the target.
    bool submission_allowed(Tab& tab, net::Url const& target)
    {
        if (!tab.policy)
            return true;
        if (!tab.policy->sandbox_allows_forms()) {
            tab.status = "This page's policy sandboxes it without forms";
            dirty = true;
            return false;
        }
        if (tab.policy->form_action_refusal(target)) {
            tab.status = "This form's target is refused by the page's Content Security Policy";
            dirty = true;
            return false;
        }
        return true;
    }

    // A click on a control: focus for every kind, a toggle for a box, a
    // submission for a submit button.
    void activate_control(dom::Element const& control)
    {
        Tab* const tab = active_tab();
        if (!tab || !tab->document || control.has_attribute("disabled"))
            return;
        using layout::ControlKind;
        tab->controls.focused = &control;
        caret_to_end(*tab, control);
        switch (layout::control_kind(control)) {
        case ControlKind::Checkbox:
            state_of(*tab, control).checked = !layout::control_checked(control, &tab->controls);
            break;
        case ControlKind::Radio:
            check_radio(*tab, control);
            break;
        case ControlKind::Submit:
            submit(*tab, control);
            break;
        default:
            break;
        }
        relayout(*tab);
        dirty = true;
    }

    // Moves focus to the next (or previous) control in tree order.
    void focus_neighbor(Tab& tab, bool backwards)
    {
        std::vector<dom::Element const*> const controls = focusable_controls(*tab.document);
        if (controls.empty())
            return;
        std::size_t index = 0;
        for (std::size_t i = 0; i < controls.size(); ++i) {
            if (controls[i] == tab.controls.focused) {
                index = backwards ? (i + controls.size() - 1) % controls.size()
                                  : (i + 1) % controls.size();
                break;
            }
        }
        tab.controls.focused = controls[index];
        caret_to_end(tab, *controls[index]);
    }

    // A key while a page control has focus; true when the control took it.
    bool edit_control(KeyEvent const& key)
    {
        Tab* const tab = tab_with_focused_control();
        if (!tab)
            return false;
        using layout::ControlKind;
        dom::Element const& control = *tab->controls.focused;
        ControlKind const kind = layout::control_kind(control);
        layout::ControlState& state = state_of(*tab, control);
        bool changed = true;
        if (key.key == Key::Tab) {
            focus_neighbor(*tab, key.shift);
        } else if (key.key == Key::Escape) {
            tab->controls.focused = nullptr;
        } else if (layout::is_text_kind(kind)) {
            bool const locked = control.has_attribute("readonly");
            std::u32string value = decode_utf8(layout::control_value(control, &tab->controls));
            std::size_t position = std::min(state.caret, value.size());
            switch (key.key) {
            case Key::Left:
                if (position > 0)
                    --position;
                break;
            case Key::Right:
                if (position < value.size())
                    ++position;
                break;
            case Key::Home:
                position = 0;
                break;
            case Key::End:
                position = value.size();
                break;
            case Key::Backspace:
                if (position > 0 && !locked) {
                    value.erase(position - 1, 1);
                    --position;
                }
                break;
            case Key::Delete:
                if (position < value.size() && !locked)
                    value.erase(position, 1);
                break;
            case Key::Enter:
                if (kind == ControlKind::TextArea) {
                    if (!locked) {
                        value.insert(position, 1, U'\n');
                        ++position;
                    }
                } else {
                    submit(*tab, control);
                    return true;
                }
                break;
            default:
                changed = false;
                break;
            }
            if (changed) {
                state.value = utf8_of(value);
                state.caret = position;
            }
            changed = true; // a focused field keeps every key from the page
        } else {
            switch (kind) {
            case ControlKind::Checkbox:
                if (key.key == Key::Space)
                    state.checked = !layout::control_checked(control, &tab->controls);
                else
                    changed = false;
                break;
            case ControlKind::Radio:
                if (key.key == Key::Space)
                    check_radio(*tab, control);
                else
                    changed = false;
                break;
            case ControlKind::Submit:
                if (key.key == Key::Space || key.key == Key::Enter) {
                    submit(*tab, control);
                    return true;
                }
                changed = false;
                break;
            case ControlKind::Select:
                if (key.key == Key::Up || key.key == Key::Down) {
                    layout::SelectOptions const options
                        = layout::select_options(control, &tab->controls);
                    if (!options.values.empty()) {
                        std::size_t index = options.selected;
                        if (key.key == Key::Up && index > 0)
                            --index;
                        if (key.key == Key::Down && index + 1 < options.values.size())
                            ++index;
                        state.value = options.values[index];
                    }
                } else {
                    changed = false;
                }
                break;
            default:
                changed = false;
                break;
            }
        }
        if (!changed)
            return false;
        relayout(*tab);
        dirty = true;
        return true;
    }

    // A typed character into the focused text control.
    void type_into_control(char32_t code_point)
    {
        Tab* const tab = tab_with_focused_control();
        if (!tab)
            return;
        dom::Element const& control = *tab->controls.focused;
        if (!layout::is_text_kind(layout::control_kind(control)) || control.has_attribute("disabled")
            || control.has_attribute("readonly"))
            return;
        layout::ControlState& state = state_of(*tab, control);
        std::u32string value = decode_utf8(layout::control_value(control, &tab->controls));
        std::size_t const position = std::min(state.caret, value.size());
        value.insert(position, 1, code_point);
        state.value = utf8_of(value);
        state.caret = position + 1;
        relayout(*tab);
        dirty = true;
    }

    void mouse_down(int x, int y, int button, platform::Modifiers const& modifiers)
    {
        update_hover(x, y);
        note_activation();
        menu_press_button = 0;
        if (!menus.empty()) {
            // An open menu takes the press: on an item it chooses it, on
            // the menu's own box it does nothing, and anywhere else it
            // closes the menus and does nothing more — but for a right
            // click, which asks for the menu of where it landed.
            if (hover == Hover::MenuRow) {
                choose_menu_item_at(hover_level, hover_index, false);
                return;
            }
            if (hover == Hover::Menu)
                return;
            close_menus();
            if (button != 3)
                return;
        }
        if (button == 3) {
            open_menu_at(x, y, modifiers.shift);
            if (!menus.empty()) {
                menu_press_button = button;
                menu_press_x = x;
                menu_press_y = y;
            }
            dirty = true;
            return;
        }
        if (button == 1) {
            // The frame the shell draws: a press in the band along the
            // window's edges resizes it, before anything under the band.
            if (window_controls) {
                if (Browser::WindowRequest const edge = resize_edge_at(x, y); edge != Browser::WindowRequest::None) {
                    window_request = edge;
                    return;
                }
            }
            // A press outside the palette closes it; one on a row runs the row.
            if (palette_open && hover != Hover::Palette && hover != Hover::PaletteRow)
                close_palette();
            pressed = hover; // a toolbar button paints itself held until the button comes up
            switch (hover) {
            case Hover::Palette: break;
            case Hover::PaletteRow: run_palette_match(palette_first_shown() + hover_index); break;
            case Hover::TabClose: close_tab(hover_index); break;
            case Hover::Tab: select_tab(hover_index); break;
            case Hover::NewTab: new_tab(); break;
            case Hover::Minimize: window_request = Browser::WindowRequest::Minimize; break;
            case Hover::Maximize: window_request = Browser::WindowRequest::ToggleMaximize; break;
            case Hover::WindowClose: window_request = Browser::WindowRequest::Close; break;
            case Hover::DragHandle: window_request = Browser::WindowRequest::Move; break;
            case Hover::Back: go(-1); break;
            case Hover::Forward: go(+1); break;
            case Hover::Reload: reload(); break;
            case Hover::Reader: toggle_reader(); break;
            case Hover::MenuButton:
                open_main_menu();
                if (!menus.empty()) {
                    menu_press_button = button;
                    menu_press_x = x;
                    menu_press_y = y;
                }
                break;
            case Hover::Menu: // no menu is open here: the block above answered
            case Hover::MenuRow: break;
            case Hover::Address: focus_address(true); break;
            case Hover::FindBox:
                blur_address();
                focus_find(true);
                break;
            case Hover::DevtoolsTree:
                if (Tab* const tab = active_tab())
                    inspect_tree_line(*tab, y);
                break;
            case Hover::DevtoolsStyles:
                break;
            case Hover::Content:
                blur_address();
                blur_find();
                // A bar is drawn over the content, so it answers first: a
                // thumb is taken hold of, the track beside it moves a
                // scrollport at a time.
                if (Tab* const tab = active_tab()) {
                    std::optional<std::pair<float, float>> const point = page_point(x, y);
                    std::optional<BarHit> const hit
                        = point ? bar_at(tab->layout.root, point->first, point->second, tab->scrolls)
                                : std::nullopt;
                    if (hit) {
                        tab->scroller = hit->box->element;
                        if (hit->on_thumb)
                            bar_drag = BarDrag { hit->box->element, hit->vertical, hit->grab };
                        else
                            page_bar(*tab, *hit);
                        break;
                    }
                }
                if (devtools_open) {
                    // With the panel open a click inspects instead of navigating.
                    if (Tab* const tab = active_tab())
                        inspect_at(*tab, x, y);
                    break;
                }
                // The page hears the click first — the frame's document, for
                // a click inside a frame — and preventDefault keeps the shell
                // from following a link or toggling a control.
                if (Tab* const tab = active_tab(); tab && tab->document) {
                    std::optional<std::pair<float, float>> const point = page_point(x, y);
                    std::vector<FrameStep> const chain
                        = point ? frames_at(*tab, point->first, point->second) : std::vector<FrameStep> {};
                    FrameView* const view = chain.empty() ? nullptr : chain.back().view;
                    bindings::Realm* const realm = view ? view->realm : tab->realm.get();
                    if (point && realm) {
                        layout::Fragment const& root = view ? view->layout.root : tab->layout.root;
                        float const px = view ? chain.back().x : point->first;
                        float const py = view ? chain.back().y : point->second;
                        dom::Element const* target = hit_run(root, px, py);
                        if (!target)
                            target = hit_control(root, px, py);
                        if (!target)
                            target = element_at_point(root, px, py);
                        if (target) {
                            ChromeLayout const chrome = layout_chrome();
                            bindings::MouseInit init;
                            init.client_x = static_cast<int>(std::lround(
                                to_css_px(view ? px : static_cast<float>(x - chrome.content.x))));
                            init.client_y = static_cast<int>(std::lround(to_css_px(
                                view ? py - static_cast<float>(view->scroll_y) : static_cast<float>(y - chrome.content.y))));
                            init.ctrl = modifiers.ctrl;
                            init.shift = modifiers.shift;
                            init.alt = modifiers.alt;
                            dom::Element& element = const_cast<dom::Element&>(*target);
                            script_started = std::chrono::steady_clock::now();
                            realm->dispatch_mouse_event(element, "mousedown", init);
                            bool const proceed = realm->dispatch_mouse_event(element, "click", init);
                            ensure_fresh(*tab);
                            dirty = true;
                            if (!proceed || !tab->document)
                                break;
                            update_hover(x, y); // the page may have changed under the pointer
                        }
                    }
                }
                if (dom::Element const* const control = control_at(x, y)) {
                    activate_control(*control);
                } else {
                    blur_control();
                    if (hover_link && modifiers.ctrl && is_navigable_scheme(hover_link->scheme)) {
                        net::Url const url = *hover_link; // the hover moves with the tabs
                        // Ctrl with the click: a tab of its own, behind the
                        // page; with Shift as well, in front of it.
                        if (modifiers.shift)
                            open_in_front(url);
                        else
                            open_in_new_tab(url);
                    } else if (hover_link) {
                        follow_link();
                    } else if (Tab* const tab = active_tab()) {
                        start_selection(*tab, x, y);
                    }
                }
                break;
            case Hover::None: blur_address(); break;
            }
        } else if (button == 2) {
            if (hover == Hover::Content && hover_link) {
                // The middle button: a tab of its own, as Ctrl with a click.
                net::Url const url = *hover_link; // the hover moves with the tabs
                if (modifiers.shift)
                    open_in_front(url);
                else
                    open_in_new_tab(url);
            } else if (hover == Hover::Tab || hover == Hover::TabClose)
                close_tab(hover_index);
        }
        dirty = true;
    }

    // A link the reader sends to a tab of its own — Ctrl or the middle button
    // with the click, the link's menu — stays in the tab's container and
    // opens BEHIND the tab in front, which the reader goes on reading.
    void open_in_new_tab(net::Url const& url)
    {
        Tab const* const from = active_tab();
        open_tab_beside(from ? from->container : std::string(), url, active, false);
    }

    // A link that asks for a tab of its own (target=_blank) opens in front.
    void open_in_front(net::Url const& url)
    {
        Tab const* const from = active_tab();
        open_tab_beside(from ? from->container : std::string(), url, active, true);
    }

    // A tab opened from a tab stands beside it. One that comes to the front
    // stands right beside; one opened behind goes after the tabs the tab in
    // front has already opened since it came to the front, so that three
    // links opened from one page stand in the order they were opened. (Where
    // both browsers put them.)
    void open_tab_beside(std::string const& container, net::Url const& url, std::size_t opener, bool to_front)
    {
        if (to_front)
            blur_address();
        Tab tab;
        tab.container = container;
        tab.history.push_back(blank_entry());
        // The run is the tab in front's: a tab behind it that opens a window
        // neither reads it nor adds to it.
        bool const from_front = !to_front && opener == active;
        std::size_t const run = from_front ? opened_beside : 0;
        // Beside its opener — after the pinned tabs, where the opener is one.
        std::size_t const position
            = std::max(tabs.empty() ? 0 : std::min(opener, tabs.size() - 1) + 1 + run, pinned_count());
        std::size_t const at = insert_tab(position, std::move(tab), to_front);
        if (from_front)
            opened_beside = run + 1;
        render(tabs[at]);
        if (to_front)
            sync_address();
        queue(at, url, Mode::Push);
    }

    // A tab at the strip's end, in front: what the main menu opens.
    void open_tab_in(std::string const& container, net::Url const& url)
    {
        blur_address();
        add_blank_tab(container);
        queue(active, url, Mode::Push);
    }

    // The reader acted: a page may open a window for the next seconds
    // (HTML §6.4.2's transient activation, at the duration browsers keep).
    void note_activation() { activation_until = std::chrono::steady_clock::now() + std::chrono::seconds(5); }

    // The keyboard moves whatever the reader last moved by hand, and the
    // page when that box has run out or there is none.
    void scroll_by(int delta)
    {
        Tab* const tab = active_tab();
        if (!tab)
            return;
        if (tab->scroller_frame && scroll_frame_by(*tab, frames_to(*tab, tab->scroller_frame), delta))
            return;
        if (layout::Fragment const* const box = keyboard_scroller(*tab)) {
            if (scroll_box_by(*tab, *box, 0, static_cast<float>(delta)))
                return;
        }
        set_scroll(*tab, tab->scroll_y + delta);
    }

    // Home and End: the far end of whatever the keyboard is moving.
    void scroll_to_end(bool far_end)
    {
        Tab* const tab = active_tab();
        if (!tab)
            return;
        if (tab->scroller_frame) {
            std::vector<FrameStep> const chain = frames_to(*tab, tab->scroller_frame);
            if (!chain.empty() && scroll_frame_to(*tab, chain, far_end ? chain.back().view->max_scroll() : 0))
                return;
        }
        if (layout::Fragment const* const box = keyboard_scroller(*tab)) {
            layout::ScrollOffset const at = layout::scroll_of(*box, &tab->scrolls);
            if (scroll_box_to(*tab, *box,
                    layout::ScrollOffset { at.x, far_end ? box->scroll_range_y : 0.0f }))
                return;
        }
        set_scroll(*tab, far_end ? max_scroll(*tab) : 0);
    }

    // A wheel over the content: the innermost box under the pointer that
    // can take it, else the page. Whatever took it is what the keyboard
    // moves from here.
    void wheel_at(int x, int y, int notches)
    {
        scroll_pixels_at(x, y, -notches * theme.scroll_step);
    }

    // The content under the point moves by `delta` device px, positive
    // bringing what is below into view: the scrolling box under the point
    // takes what it can, else the page.
    void scroll_pixels_at(int x, int y, int delta)
    {
        if (delta == 0)
            return;
        Tab* const tab = active_tab();
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (tab && point) {
            // A frame under the point takes the push first — a box inside
            // it, else its document — and passes it to the frame around it,
            // and on to the page, when it has no room left; the keyboard
            // follows whichever frame took it.
            std::vector<FrameStep> chain = frames_at(*tab, point->first, point->second);
            while (!chain.empty()) {
                FrameStep const& step = chain.back();
                layout::Fragment const* const inner = scroller_at(
                    step.view->layout.root, step.x, step.y, static_cast<float>(delta), step.view->scrolls);
                std::optional<layout::ScrollOffset> const push
                    = inner ? wheel_delta(*inner, static_cast<float>(delta), step.view->scrolls) : std::nullopt;
                dom::Element const* const container = step.container;
                if ((inner && push && scroll_frame_box_by(*tab, chain, *inner, push->x, push->y))
                    || scroll_frame_by(*tab, chain, delta)) {
                    tab->scroller = nullptr;
                    tab->scroller_frame = container;
                    return;
                }
                chain.pop_back();
            }
            tab->scroller_frame = nullptr;
            layout::Fragment const* const box = scroller_at(
                tab->layout.root, point->first, point->second, static_cast<float>(delta), tab->scrolls);
            std::optional<layout::ScrollOffset> const step
                = box ? wheel_delta(*box, static_cast<float>(delta), tab->scrolls) : std::nullopt;
            if (box && step) {
                tab->scroller = box->element;
                scroll_box_by(*tab, *box, step->x, step->y);
                return;
            }
            tab->scroller = nullptr;
        }
        scroll_by(delta);
    }

    void key_down(KeyEvent const& key)
    {
        // A key scrolls, selects and moves through the page as it is laid
        // out for the window as it is now.
        if (Tab* const front = active_tab())
            ensure_fresh(*front);
        clear_preedit();
        note_activation();
        // An open menu takes every key.
        if (!menus.empty()) {
            menu_key(key);
            return;
        }
        if (hints_active) {
            hint_key(key);
            return;
        }
        if (key.key == Key::F12
            || (key.ctrl && key.shift && key.key == Key::Letter && key.letter == U'I')) {
            toggle_devtools();
            return;
        }
        // The palette takes every key while it is open.
        if (palette_open) {
            edit_palette(key);
            return;
        }
        if (key.ctrl && key.shift && key.key == Key::Letter && key.letter == U'P') {
            open_palette({});
            return;
        }
        // The page hears a key meant for it — not the shell's own chords —
        // at the focused control, else the document; preventDefault keeps
        // the shell's handling from running.
        if (!key.ctrl && !key.alt && !address_focus && !find_focus) {
            if (Tab* const tab = active_tab(); tab && tab->realm && tab->document) {
                dom::Element const* const target = tab->controls.focused;
                script_started = std::chrono::steady_clock::now();
                bindings::Realm* const realm = target ? realm_for(*tab, *target) : tab->realm.get();
                bool const proceed = realm->dispatch_key_event(const_cast<dom::Element*>(target), "keydown",
                    key_init_for(key));
                ensure_fresh(*tab);
                if (!proceed) {
                    dirty = true;
                    return;
                }
            }
        }
        if (key.key == Key::Menu || (key.key == Key::F10 && key.shift)) {
            open_menu_by_keyboard();
            return;
        }
        if (key.ctrl && key.shift && key.key == Key::Letter && key.letter == U'Q') {
            window_request = Browser::WindowRequest::Close;
            return;
        }
        if (key.ctrl && key.key == Key::Letter) {
            switch (key.letter) {
            case U'L': focus_address(true); return;
            case U'T':
                if (key.shift)
                    reopen_closed_tab(); // the tab closed last, back where it stood
                else
                    new_tab();
                return;
            case U'U': view_source(); return;
            case U'X':
                if (address_focus)
                    cut_address();
                return;
            case U'N':
                if (key.shift)
                    new_tab_in(next_container()); // the container after this tab's, round to the default
                return;
            case U'W': close_tab(active); return;
            case U'R': reload(); return;
            case U'A':
                if (address_focus) {
                    select_all = !address.empty();
                    dirty = true;
                } else if (Tab* const tab = active_tab(); tab && !tab_with_focused_control()) {
                    select_all_text(*tab);
                }
                return;
            case U'C': copy_selection(); return;
            case U'V': paste(); return;
            case U'F': open_find(); return;
            case U'9': select_tab(tabs.size() - 1); return;
            default:
                if (key.letter >= U'1' && key.letter <= U'8')
                    select_tab(static_cast<std::size_t>(key.letter - U'1'));
                return;
            }
        }
        if (key.ctrl && key.key == Key::Tab) {
            if (tabs.size() > 1)
                select_tab(key.shift ? (active + tabs.size() - 1) % tabs.size()
                                     : (active + 1) % tabs.size());
            return;
        }
        if (key.alt && key.key == Key::Left) {
            go(-1);
            return;
        }
        if (key.alt && key.key == Key::Right) {
            go(+1);
            return;
        }
        if (key.key == Key::F5) {
            reload();
            return;
        }
        if (find_focus) {
            edit_find(key);
            return;
        }
        if (address_focus) {
            edit_address(key);
            return;
        }
        if (edit_control(key))
            return;
        Tab* const tab = active_tab();
        if (!tab)
            return;
        // A bare f with nothing focused: labels on the links in view.
        if (key.key == Key::Letter && !key.ctrl && !key.alt && !key.shift
            && (key.letter == U'F' || key.letter == U'f')) {
            start_hints(*tab);
            return;
        }
        if (key.shift && extend_selection(*tab, key))
            return;
        if (key.key == Key::Escape && tab->selection) {
            clear_selection(*tab);
            return;
        }
        ChromeLayout const c = layout_chrome();
        int const page = std::max(theme.scroll_step, c.content.height - theme.scroll_step);
        switch (key.key) {
        case Key::Down: scroll_by(theme.scroll_step); break;
        case Key::Up: scroll_by(-theme.scroll_step); break;
        case Key::PageDown: scroll_by(page); break;
        case Key::Space: scroll_by(key.shift ? -page : page); break;
        case Key::PageUp: scroll_by(-page); break;
        case Key::Home: scroll_to_end(false); break;
        case Key::End: scroll_to_end(true); break;
        case Key::Backspace: go(key.shift ? +1 : -1); break;
        default: break;
        }
    }

    void edit_address(KeyEvent const& key)
    {
        switch (key.key) {
        case Key::Enter: {
            std::string const typed = address;
            blur_address();
            navigate(typed);
            break;
        }
        case Key::Escape:
            blur_address();
            sync_address();
            break;
        case Key::Backspace:
            if (select_all) {
                address.clear();
                caret = 0;
                select_all = false;
            } else if (caret > 0) {
                std::size_t const previous = previous_code_point(address, caret);
                address.erase(previous, caret - previous);
                caret = previous;
            }
            break;
        case Key::Delete:
            if (select_all) {
                address.clear();
                caret = 0;
                select_all = false;
            } else if (caret < address.size()) {
                address.erase(caret, next_code_point(address, caret) - caret);
            }
            break;
        case Key::Left:
            if (select_all) {
                select_all = false;
                caret = 0;
            } else {
                caret = previous_code_point(address, caret);
            }
            break;
        case Key::Right:
            if (select_all) {
                select_all = false;
                caret = address.size();
            } else {
                caret = next_code_point(address, caret);
            }
            break;
        case Key::Home:
            select_all = false;
            caret = 0;
            break;
        case Key::End:
            select_all = false;
            caret = address.size();
            break;
        default:
            return;
        }
        dirty = true;
    }

    // --- An input method's composing text -------------------------------------

    // Shown at the focused field's caret, underlined, and part of nothing
    // until the method commits it as text; empty clears it.
    void set_preedit(std::string const& text)
    {
        if (find_focus) {
            preedit = text;
            preedit_owner = PreeditOwner::Find;
        } else if (address_focus) {
            preedit = text;
            preedit_owner = PreeditOwner::Address;
        } else if (Tab* const tab = tab_with_focused_control()) {
            if (tab->controls.preedit == text && tab->controls.preedit_owner == tab->controls.focused)
                return;
            tab->controls.preedit = text;
            tab->controls.preedit_owner = tab->controls.focused;
            relayout(*tab);
        } else {
            return;
        }
        dirty = true;
    }

    // Text or a key arrived, or focus moved: whatever was composing is over
    // (a method commits what it composed before anything else is typed).
    void clear_preedit()
    {
        if (!preedit.empty()) {
            preedit.clear();
            dirty = true;
        }
        preedit_owner = PreeditOwner::None;
        for (Tab& tab : tabs) {
            if (!tab.controls.preedit.empty() || tab.controls.preedit_owner) {
                tab.controls.preedit.clear();
                tab.controls.preedit_owner = nullptr;
                relayout(tab);
                dirty = true;
            }
        }
    }

    // The band along the window's edges that resizes it, when the shell
    // draws the frame: which edge or corner a point is in, or none.
    Browser::WindowRequest resize_edge_at(int x, int y) const
    {
        using Request = Browser::WindowRequest;
        int const band = std::max(4, static_cast<int>(std::lround(6 * scale)));
        bool const left = x < band;
        bool const right = x >= width - band;
        bool const top = y < band;
        bool const bottom = y >= height - band;
        if (top && left)
            return Request::ResizeTopLeft;
        if (top && right)
            return Request::ResizeTopRight;
        if (bottom && left)
            return Request::ResizeBottomLeft;
        if (bottom && right)
            return Request::ResizeBottomRight;
        if (top)
            return Request::ResizeTop;
        if (bottom)
            return Request::ResizeBottom;
        if (left)
            return Request::ResizeLeft;
        if (right)
            return Request::ResizeRight;
        return Request::None;
    }

    // Composing text belongs to the field that had focus when it was set:
    // once focus has moved on, it is over.
    void drop_stale_preedit()
    {
        bool const stale = (preedit_owner == PreeditOwner::Address && !address_focus)
            || (preedit_owner == PreeditOwner::Find && !find_focus);
        if (stale) {
            preedit.clear();
            preedit_owner = PreeditOwner::None;
            dirty = true;
        }
        for (Tab& tab : tabs) {
            if (tab.controls.preedit_owner && tab.controls.preedit_owner != tab.controls.focused) {
                tab.controls.preedit.clear();
                tab.controls.preedit_owner = nullptr;
                relayout(tab);
                dirty = true;
            }
        }
    }

    // The caret's box of the focused field, window coordinates: where an
    // input method composes.
    std::optional<Rect> text_input_area()
    {
        drop_stale_preedit();
        ChromeLayout const c = layout_chrome();
        Theme const& t = theme;
        auto const caret_in = [&](Rect const& box, int text_left, std::string const& text, std::size_t at) {
            Rect const inner { box.x + t.border_width, box.y + t.border_width, box.width - 2 * t.border_width,
                box.height - 2 * t.border_width };
            // After the words before it, as the chrome's face measures them.
            std::u32string const before = decode_utf8(text.substr(0, std::min(at, text.size())));
            int const x = text_left + static_cast<int>(text_width(before, t.font_size) + 0.5f);
            return Rect { std::min(x, inner.right()), inner.y, 1, std::max(1, inner.height) };
        };
        if (palette_open)
            return caret_in(c.palette_box, c.palette_box.x + t.border_width + t.padding, palette_query, palette_caret);
        if (find_focus && find_open)
            return caret_in(c.find_box, c.find_box.x + t.border_width + t.padding, find_query, find_caret);
        if (address_focus)
            return caret_in(c.address, c.address.x + t.border_width + t.padding + 2, address, caret);
        Tab* const tab = tab_with_focused_control();
        if (!tab)
            return std::nullopt;
        // The control's box, in the page or in the frame it is in, moved by
        // where that frame's document sits on the page.
        std::vector<FrameStep> const chain
            = frames_to(*tab, frame_holding_in(tab->frames, tab->controls.focused->document()));
        layout::Fragment const& root = chain.empty() ? tab->layout.root : chain.back().view->layout.root;
        auto const [ox, oy] = origin_of(chain);
        layout::Fragment const* const box = fragment_for(root, tab->controls.focused);
        if (!box || !box->control || !box->control->caret_x)
            return std::nullopt;
        return Rect { c.content.x + static_cast<int>(std::lround(*box->control->caret_x + ox)),
            c.content.y + static_cast<int>(std::lround(box->control->y + oy)) - tab->scroll_y, 1,
            std::max(1, static_cast<int>(std::lround(box->control->height))) };
    }

    void text_input(char32_t code_point)
    {
        if (code_point < 0x20 || code_point == 0x7F || hints_active)
            return; // a hint's letters are keys, not text
        clear_preedit();
        if (!menus.empty()) {
            menu_letter(code_point);
            return;
        }
        if (palette_open) {
            type_into_palette(code_point);
            return;
        }
        if (find_focus) {
            type_into_find(code_point);
            return;
        }
        if (!address_focus) {
            type_into_control(code_point);
            // The control's page hears what was typed.
            if (Tab* const tab = tab_with_focused_control(); tab && tab->realm) {
                bindings::InputInit init;
                append_utf8(init.data, code_point);
                dom::Element& control = const_cast<dom::Element&>(*tab->controls.focused);
                script_started = std::chrono::steady_clock::now();
                realm_for(*tab, control)->dispatch_input_event(control, "input", init);
                ensure_fresh(*tab);
                dirty = true;
            }
            return;
        }
        if (select_all) {
            address.clear();
            caret = 0;
            select_all = false;
        }
        std::string utf8;
        append_utf8(utf8, code_point);
        address.insert(caret, utf8);
        caret += utf8.size();
        dirty = true;
    }

    // --- Painting ------------------------------------------------------------------

    // A button's icon is drawn four sevenths the button's size: sixteen px in
    // a button of twenty-eight, and in step with it at any scale.
    int icon_size() const { return std::max(8, theme.button_size * 4 / 7); }

    void paint_button(Rect const& rect, Icon icon, bool enabled, bool hovered, bool held = false)
    {
        if (hovered && enabled)
            frame.fill_round_rect(rect, theme.button_corner_radius,
                held ? theme.button_active_background : theme.button_hover_background);
        draw_icon(frame, icon, rect, icon_size(), enabled ? theme.toolbar_icon : theme.button_disabled_text);
    }

    void paint()
    {
        Stopwatch const painting(profile.paint_ms, &profile.last_paint_ms);
        ++profile.paints;
        profile.painted_pixels += static_cast<std::uint64_t>(frame.width()) * static_cast<std::uint64_t>(frame.height());
        drop_stale_preedit();
        Theme const& t = theme;
        ChromeLayout const c = layout_chrome();
        Tab const* const tab = active_tab();
        net::Url const* const url = tab && tab->current() ? &tab->current()->final_url : nullptr;

        // The header — the tab strip and the toolbar — begins as the frame:
        // its color, another when the window is not the one in front, and
        // over it the theme's pictures. Everything after composites, so
        // whatever a theme makes translucent lets the frame through.
        Rect const header { 0, 0, width, c.toolbar.bottom() };
        frame.fill_rect(header, window_active ? t.chrome_background : t.chrome_background_inactive);
        paint_pictures(frame_pictures, header, { header });

        // Tab strip.
        for (std::size_t i = 0; i < c.tabs.size(); ++i) {
            Rect const rect = c.tabs[i];
            bool const is_active = i == active;
            bool const hovered = (hover == Hover::Tab || hover == Hover::TabClose) && hover_index == i;
            // Attached, only the top corners round: the shape runs on below
            // the tab by the corners' height and is cut off at the tab's
            // foot, not left for the toolbar to cover — a toolbar with an
            // alpha would show it. Floating, the tab is its own rounded box.
            bool const floating = t.tab_shape == TabShape::Floating;
            Rect const shape { rect.x, rect.y, rect.width, rect.height + (floating ? 0 : t.tab_corner_radius) };
            auto const fill_tab = [&](Color color) {
                // A tab the color of the frame is a tab that shows the frame.
                if (color == t.chrome_background)
                    return;
                frame.set_clip(rect);
                frame.fill_round_rect(shape, t.tab_corner_radius, color);
                frame.set_clip(std::nullopt);
            };
            auto const tab_pictures = [&](ThemeLayers& pictures) {
                if (pictures.layers.empty())
                    return;
                std::vector<Rect> rows = Bitmap::round_rect_bands(shape, t.tab_corner_radius);
                for (Rect& row : rows) // cut off at the tab's foot, as its fill is
                    row.height = std::max(0, std::min(row.bottom(), rect.bottom()) - row.y);
                paint_pictures(pictures, header, rows);
            };
            if (is_active) {
                // The tab in front is of a piece with the toolbar: its color,
                // and the toolbar's pictures carried up into it.
                fill_tab(t.tab_active_background);
                tab_pictures(toolbar_pictures);
            } else {
                // Any other wears its color at rest and its pictures, and the
                // pointer's color goes over both.
                fill_tab(t.tab_inactive_background);
                tab_pictures(tab_background_pictures);
                if (hovered)
                    fill_tab(t.tab_hover_background);
            }
            // The tab in front wears the theme's line, when the theme names
            // one: along its top where tabs are attached, around it where
            // they float — the outline that browser gives its tab in front.
            if (is_active && t.tab_line.a != 0) {
                if (floating)
                    frame.fill_round_box(rect, t.tab_corner_radius, t.border_width, t.tab_line, Color::rgba(0, 0, 0, 0));
                else
                    frame.fill_rect(Rect { rect.x + t.tab_corner_radius, rect.y, std::max(0, rect.width - 2 * t.tab_corner_radius),
                                        std::max(2, t.border_width * 2) },
                        t.tab_line);
            }
            // A tab in a container wears the container's colour along its top.
            if (Browser::Container const* const container = container_named(tabs[i].container)) {
                int const stripe = std::max(2, t.border_width * 2);
                frame.fill_rect(Rect { rect.x + t.tab_corner_radius, rect.y, std::max(0, rect.width - 2 * t.tab_corner_radius), stripe },
                    container->color);
            }
            if (tabs[i].pinned) {
                // A pinned tab is its icon, in the middle of it: the page's
                // own, or a sheet of paper for a page that has none. No
                // title, and nothing to close it by.
                int const size = t.tab_icon_size;
                if (tabs[i].favicon) {
                    frame.draw_scaled(*tabs[i].favicon,
                        Rect { rect.x + (rect.width - size) / 2, rect.y + (rect.height - size) / 2, size, size });
                } else {
                    draw_icon(frame, Icon::Page, rect, size, is_active ? t.tab_text : t.chrome_text_muted);
                }
                continue;
            }
            Rect const close = c.tab_close_buttons[i];
            float text_x = static_cast<float>(rect.x + t.padding + 2);
            if (tabs[i].favicon) {
                // The icon before the title, which moves over only when one
                // decoded, so a page without one paints as it always did.
                int const size = t.tab_icon_size;
                Rect const icon { rect.x + t.padding + 2, rect.y + (rect.height - size) / 2, size, size };
                frame.draw_scaled(*tabs[i].favicon, icon);
                text_x += static_cast<float>(size + t.padding);
            }
            float const max_width = static_cast<float>(close.x - t.padding) - text_x;
            std::u32string const title
                = ellipsize(decode_utf8(tab_title(tabs[i])), max_width, t.tab_font_size);
            draw_text(frame, title, text_x, centered_baseline(rect, t.tab_font_size), t.tab_font_size,
                is_active ? t.tab_text : t.chrome_text_muted);
            bool const close_hover = hover == Hover::TabClose && hover_index == i;
            if (close_hover)
                frame.fill_round_rect(close, close.width / 2, t.button_hover_background);
            // The cross is two thirds the size of the round it sits in.
            draw_icon(frame, Icon::Close, close, std::max(8, close.width * 2 / 3),
                is_active ? t.tab_text : close_hover ? t.chrome_text : t.chrome_text_muted);
        }
        if (hover == Hover::NewTab)
            frame.fill_round_rect(c.new_tab_button, t.button_corner_radius, t.button_hover_background);
        draw_icon(frame, Icon::Plus, c.new_tab_button, icon_size(), t.chrome_text);
        if (c.window_controls) {
            // The window's own controls: a line, a square, a cross.
            auto const button = [&](Rect const& rect, Hover which, Icon icon) {
                if (hover == which)
                    frame.fill_round_rect(rect, t.button_corner_radius, t.button_hover_background);
                draw_icon(frame, icon, rect, icon_size(), t.chrome_text);
            };
            button(c.minimize_button, Hover::Minimize, Icon::Minimize);
            button(c.maximize_button, Hover::Maximize, Icon::Maximize);
            button(c.close_button, Hover::WindowClose, Icon::Close);
        }

        // Toolbar.
        frame.fill_rect(c.toolbar, t.toolbar_background);
        paint_pictures(toolbar_pictures, header, { c.toolbar });
        frame.fill_rect(Rect { 0, c.toolbar.bottom() - t.border_width, width, t.border_width },
            t.chrome_border);
        // The line a theme may draw along the toolbar's top, broken where
        // the tab in front runs into the toolbar — which a floating tab
        // does not.
        if (t.toolbar_top_separator.a != 0) {
            Rect const front = active < c.tabs.size() && t.tab_shape == TabShape::Attached ? c.tabs[active] : Rect {};
            int const gap_from = front.is_empty() ? width : std::clamp(front.x, 0, width);
            int const gap_to = front.is_empty() ? width : std::clamp(front.right(), gap_from, width);
            frame.fill_rect(Rect { 0, c.toolbar.y, gap_from, t.border_width }, t.toolbar_top_separator);
            frame.fill_rect(Rect { gap_to, c.toolbar.y, width - gap_to, t.border_width }, t.toolbar_top_separator);
        }
        paint_button(c.back_button, Icon::Back, can_go(-1), hover == Hover::Back, pressed == Hover::Back);
        paint_button(c.forward_button, Icon::Forward, can_go(+1), hover == Hover::Forward, pressed == Hover::Forward);
        paint_button(c.reload_button, Icon::Reload, tab && tab->current() != nullptr,
            hover == Hover::Reload, pressed == Hover::Reload);
        paint_button(c.reader_button, Icon::Reader, reader_available(), hover == Hover::Reader, pressed == Hover::Reader);
        paint_button(c.menu_button, Icon::Menu, true, hover == Hover::MenuButton || main_menu_open);

        // Address bar.
        // A field with the focus wears its own colors, which a theme may
        // set apart from the field at rest.
        Color const address_fill = address_focus ? t.address_background_focus : t.address_background;
        Color const address_ink = address_focus ? t.address_text_focus : t.address_text;
        frame.fill_round_box(c.address, t.address_corner_radius, t.border_width,
            address_focus ? t.address_border_focus : t.address_border, address_fill);
        Rect const inner { c.address.x + t.border_width, c.address.y + t.border_width,
            c.address.width - 2 * t.border_width, c.address.height - 2 * t.border_width };
        int text_left = inner.x + t.padding + 2;
        if (url && is_web_scheme(url->scheme) && !address_focus) {
            int const dot = std::max(4, t.address_height / 4);
            frame.fill_round_rect(Rect { text_left, inner.y + (inner.height - dot) / 2, dot, dot },
                dot / 2, url->scheme == "https" ? t.secure_indicator : t.insecure_indicator);
            text_left += dot + t.padding;
        }
        Rect const text_area { text_left, inner.y, std::max(0, inner.right() - t.padding - text_left),
            inner.height };
        if (!text_area.is_empty()) {
            // Drawn onto the field through a clip, and not onto a strip of
            // the field's color laid over it: a field a theme makes
            // translucent must stay so under its text.
            frame.set_clip(text_area);
            // The composing text of an input method sits at the caret,
            // underlined, and the caret stands after it.
            std::u32string const composing
                = address_focus && preedit_owner == PreeditOwner::Address ? decode_utf8(preedit) : std::u32string();
            std::size_t const caret_index = decode_utf8(address.substr(0, caret)).size();
            std::u32string text = decode_utf8(address);
            text.insert(std::min(caret_index, text.size()), composing);
            Rect const local { 0, 0, text_area.width, text_area.height };
            float const baseline = static_cast<float>(text_area.y) + centered_baseline(local, t.font_size);
            // Where the words before a place end, as the chrome's face measures them.
            auto const reach = [&](std::size_t count) {
                return static_cast<int>(text_width_to(text, count, t.font_size) + 0.5f);
            };
            bool const selected = address_focus && select_all && !text.empty();
            if (selected)
                frame.fill_rect(Rect { text_area.x, text_area.y + 2, static_cast<int>(text_width(text, t.font_size) + 0.5f),
                                    text_area.height - 4 },
                    t.address_selection);
            draw_text(frame, text, static_cast<float>(text_area.x), baseline, t.font_size,
                selected ? t.address_selection_text : address_ink);
            if (!composing.empty()) {
                int const from = reach(caret_index);
                int const to = reach(caret_index + composing.size());
                frame.fill_rect(Rect { text_area.x + from, text_area.y + text_area.height - 6, to - from, 1 }, address_ink);
            }
            if (address_focus) {
                int const caret_x = reach(caret_index + composing.size());
                frame.fill_rect(Rect { text_area.x + caret_x, text_area.y + 4, 1, text_area.height - 8 }, t.accent);
            }
            frame.set_clip(std::nullopt);
        }

        // Find bar: the query box and the count of matches.
        if (find_open) {
            frame.fill_rect(c.find_bar, t.chrome_background);
            frame.fill_rect(Rect { 0, c.find_bar.bottom() - t.border_width, width, t.border_width },
                t.chrome_border);
            Color const find_fill = find_focus ? t.address_background_focus : t.address_background;
            Color const find_ink = find_focus ? t.address_text_focus : t.address_text;
            frame.fill_round_box(c.find_box, t.address_corner_radius, t.border_width,
                find_focus ? t.address_border_focus : t.address_border, find_fill);
            Rect const box_inner { c.find_box.x + t.border_width, c.find_box.y + t.border_width,
                c.find_box.width - 2 * t.border_width, c.find_box.height - 2 * t.border_width };
            Rect const box_text { box_inner.x + t.padding, box_inner.y,
                std::max(0, box_inner.width - 2 * t.padding), box_inner.height };
            if (!box_text.is_empty()) {
                frame.set_clip(box_text); // onto the box itself, as the address bar's text is
                std::u32string const composing
                    = find_focus && preedit_owner == PreeditOwner::Find ? decode_utf8(preedit) : std::u32string();
                std::size_t const caret_index = decode_utf8(find_query.substr(0, find_caret)).size();
                std::u32string query = decode_utf8(find_query);
                query.insert(std::min(caret_index, query.size()), composing);
                Rect const local { 0, 0, box_text.width, box_text.height };
                float const baseline = static_cast<float>(box_text.y) + centered_baseline(local, t.font_size);
                auto const reach = [&](std::size_t count) {
                    return static_cast<int>(text_width_to(query, count, t.font_size) + 0.5f);
                };
                if (find_focus && find_select_all && !query.empty())
                    frame.fill_rect(Rect { box_text.x, box_text.y + 2, static_cast<int>(text_width(query, t.font_size) + 0.5f),
                                        box_text.height - 4 },
                        t.address_selection);
                draw_text(frame, ellipsize(query, static_cast<float>(box_text.width), t.font_size),
                    static_cast<float>(box_text.x), baseline, t.font_size,
                    find_focus && find_select_all && !query.empty() ? t.address_selection_text : find_ink);
                if (!composing.empty()) {
                    int const from = reach(caret_index);
                    int const to = reach(caret_index + composing.size());
                    frame.fill_rect(Rect { box_text.x + from, box_text.y + box_text.height - 6, to - from, 1 }, find_ink);
                }
                if (find_focus) {
                    int const caret_x = reach(caret_index + composing.size());
                    frame.fill_rect(Rect { box_text.x + caret_x, box_text.y + 4, 1, box_text.height - 8 }, t.accent);
                }
                frame.set_clip(std::nullopt);
            }
            std::string const count = tab ? find_status(*tab) : std::string();
            if (!count.empty()) {
                Rect const label { c.find_box.right() + 2 * t.padding, c.find_bar.y,
                    std::max(0, width - c.find_box.right() - 3 * t.padding), t.find_height };
                draw_text(frame, decode_utf8(count), static_cast<float>(label.x),
                    centered_baseline(label, t.font_size), t.font_size, t.chrome_text_muted);
            }
        }

        // Content.
        frame.fill_rect(c.content, t.content_background);
        if (tab && tab->document && !c.content.is_empty()) {
            Bitmap content(c.content.width, c.content.height, t.content_background);
            paint::paint_page(content, tab->layout, 0, -static_cast<float>(tab->scroll_y),
                &tab->backgrounds, &tab->scrolls);
            // The find bar's matches, the current one stronger; then the
            // selection over them, all as translucent bands.
            if (find_open) {
                for (std::size_t m = 0; m < tab->matches.size(); ++m) {
                    Match const& match = tab->matches[m];
                    paint_bands(content, *tab, match.start, match.end,
                        m == tab->current_match ? t.find_current : t.find_highlight);
                }
            }
            if (tab->selection && !tab->selection_frame) { // a frame's is in the frame's picture
                auto const [start, end] = ordered(*tab->selection);
                paint_bands(content, *tab, start, end, t.selection);
            }
            if (hints_active) {
                // The labels still possible, each at its link's top-left.
                float const size = t.tab_font_size;
                for (Hint const& hint : hints) {
                    if (hint.label.compare(0, hint_typed.size(), hint_typed) != 0)
                        continue;
                    std::u32string const label = decode_utf8(hint.label);
                    int const label_width = static_cast<int>(text_width(label, size) + 0.5f) + 6;
                    int const label_height = static_cast<int>(size + 0.5f) + 4;
                    Rect const box { static_cast<int>(hint.x + 0.5f),
                        static_cast<int>(hint.y - static_cast<float>(tab->scroll_y) + 0.5f) - 2,
                        label_width, label_height };
                    content.fill_round_rect(box, 3, t.hint_background);
                    draw_text(content, label, static_cast<float>(box.x + 3), centered_baseline(box, size),
                        size, t.hint_text, true);
                }
            }
            if (devtools_open && tab->inspected) {
                // The inspected element's box, or the runs of an inline one.
                if (dom::Element const* const element = element_of(tab->inspected)) {
                    if (layout::Fragment const* const box = fragment_for(tab->layout.root, element)) {
                        content.fill_rect(Rect { static_cast<int>(box->x + 0.5f),
                                              static_cast<int>(box->y - static_cast<float>(tab->scroll_y) + 0.5f),
                                              static_cast<int>(box->width + 0.5f),
                                              static_cast<int>(box->height + 0.5f) },
                            t.selection);
                    } else {
                        for (std::size_t i = 0; i < tab->runs.size(); ++i) {
                            layout::TextRun const& run = *tab->runs[i];
                            bool inside = false;
                            for (dom::Node const* node = run.element; node; node = node->parent()) {
                                if (node == element) {
                                    inside = true;
                                    break;
                                }
                            }
                            if (inside)
                                paint_bands(content, *tab, TextPosition { i, 0 },
                                    TextPosition { i, run.text.size() }, t.selection);
                        }
                    }
                }
            }
            int const page_height = static_cast<int>(tab->layout.page_height + 0.5f);
            if (page_height > c.content.height) {
                int const track = c.content.height;
                int const thumb = std::max(20, static_cast<int>(
                    static_cast<long long>(track) * c.content.height / page_height));
                int const travel = std::max(0, track - thumb);
                int const scroll_range = max_scroll(*tab);
                int const thumb_y = scroll_range > 0
                    ? static_cast<int>(static_cast<long long>(travel) * tab->scroll_y / scroll_range)
                    : 0;
                Color const thumb_color = Color::rgba(t.chrome_text_muted.r, t.chrome_text_muted.g,
                    t.chrome_text_muted.b, 140);
                content.fill_round_rect(Rect { c.content.width - 8, thumb_y + 2, 5, thumb - 4 }, 2,
                    thumb_color);
            }
            frame.blit(content, c.content.x, c.content.y);
        }

        // Devtools panel: the tree on the left, the inspected element's box
        // and style on the right.
        if (devtools_open) {
            frame.fill_rect(c.devtools, t.chrome_background);
            frame.fill_rect(Rect { 0, c.devtools.y, width, t.border_width }, t.chrome_border);
            frame.fill_rect(Rect { c.devtools_styles.x - t.padding, c.devtools.y, t.border_width,
                                c.devtools.height },
                t.chrome_border);
            float const size = t.status_font_size;
            if (tab && tab->document) {
                std::vector<TreeLine> lines;
                build_tree(*tab->document, 0, lines);
                int const visible = std::max(0, c.devtools_tree.height / tree_line_height);
                for (int i = 0; i < visible; ++i) {
                    auto const index = static_cast<std::size_t>(tab->tree_scroll + i);
                    if (index >= lines.size())
                        break;
                    TreeLine const& line = lines[index];
                    Rect const row { c.devtools_tree.x, c.devtools_tree.y + i * tree_line_height,
                        c.devtools_tree.width, tree_line_height };
                    if (line.node == tab->inspected)
                        frame.fill_rect(row, t.tab_active_background);
                    float const x = static_cast<float>(row.x + t.padding + line.depth * 12);
                    float const room = static_cast<float>(row.right() - t.padding) - x;
                    draw_text(frame, ellipsize(decode_utf8(line.text), room, size), x,
                        centered_baseline(row, size), size,
                        line.node->is_element() ? t.chrome_text : t.chrome_text_muted);
                }
                std::vector<std::string> const details = style_lines(*tab);
                for (std::size_t i = 0; i < details.size(); ++i) {
                    Rect const row { c.devtools_styles.x,
                        c.devtools_styles.y + static_cast<int>(i) * tree_line_height,
                        c.devtools_styles.width, tree_line_height };
                    if (row.bottom() > c.devtools.bottom())
                        break;
                    draw_text(frame,
                        ellipsize(decode_utf8(details[i]), static_cast<float>(row.width - t.padding), size),
                        static_cast<float>(row.x), centered_baseline(row, size), size,
                        i == 0 ? t.chrome_text : t.chrome_text_muted);
                }
            }
        }

        // The command palette, over the page: the query box and the
        // commands the query leaves, the highlighted one marked.
        if (palette_open && !c.palette.is_empty()) {
            frame.fill_round_box(c.palette, t.address_corner_radius, t.border_width, t.popup_border, t.popup_background);
            frame.fill_round_box(c.palette_box, t.address_corner_radius, t.border_width, t.address_border_focus,
                t.address_background_focus);
            Rect const box_inner { c.palette_box.x + t.border_width, c.palette_box.y + t.border_width,
                c.palette_box.width - 2 * t.border_width, c.palette_box.height - 2 * t.border_width };
            Rect const box_text { box_inner.x + t.padding, box_inner.y, std::max(0, box_inner.width - 2 * t.padding), box_inner.height };
            if (!box_text.is_empty()) {
                frame.set_clip(box_text); // onto the box itself, as the address bar's text is
                std::u32string const query = decode_utf8(palette_query);
                std::size_t const caret_index = decode_utf8(palette_query.substr(0, palette_caret)).size();
                Rect const local { 0, 0, box_text.width, box_text.height };
                float const baseline = static_cast<float>(box_text.y) + centered_baseline(local, t.font_size);
                float const left = static_cast<float>(box_text.x);
                if (palette_select_all && !query.empty())
                    frame.fill_rect(Rect { box_text.x, box_text.y + 2, static_cast<int>(text_width(query, t.font_size) + 0.5f), box_text.height - 4 }, t.address_selection);
                if (query.empty())
                    draw_text(frame, U"Type a command", left, baseline, t.font_size, t.chrome_text_muted);
                else
                    draw_text(frame, ellipsize(query, static_cast<float>(box_text.width), t.font_size), left, baseline, t.font_size,
                        palette_select_all ? t.address_selection_text : t.address_text_focus);
                int const caret_x = static_cast<int>(text_width_to(query, caret_index, t.font_size) + 0.5f);
                frame.fill_rect(Rect { box_text.x + caret_x, box_text.y + 4, 1, box_text.height - 8 }, t.accent);
                frame.set_clip(std::nullopt);
            }
            std::size_t const first = palette_first_shown();
            for (std::size_t i = 0; i < c.palette_rows.size(); ++i) {
                Rect const row = c.palette_rows[i];
                std::size_t const match = first + i;
                if (match >= palette_matches.size())
                    break;
                bool const selected = match == palette_index;
                if (selected) {
                    frame.fill_round_rect(row, t.button_corner_radius, t.popup_highlight);
                    frame.fill_rect(Rect { row.x, row.y + 4, std::max(2, t.border_width * 3), row.height - 8 }, t.accent);
                }
                std::u32string const label = ellipsize(decode_utf8(palette_commands[palette_matches[match]].label),
                    static_cast<float>(row.width - 3 * t.padding), t.font_size);
                draw_text(frame, label, static_cast<float>(row.x + 2 * t.padding), centered_baseline(row, t.font_size),
                    t.font_size, selected ? t.popup_highlight_text : t.popup_text_muted);
            }
            if (palette_matches.empty()) {
                Rect const row { c.palette.x + t.padding, c.palette_box.bottom() + t.padding, c.palette.width - 2 * t.padding,
                    static_cast<int>(t.font_size * 2) };
                draw_text(frame, U"No command matches", static_cast<float>(row.x + 2 * t.padding),
                    centered_baseline(row, t.font_size), t.font_size, t.popup_text_muted);
            }
        }

        // What the shell has to say, over the page's bottom left corner: a
        // box bordered along its top and its right, the corner between them
        // round — a bordered box hung a corner's width past the window's
        // left and the content's foot, and shown through its own rectangle.
        if (!c.status.is_empty()) {
            int const r = t.button_corner_radius;
            frame.set_clip(c.status);
            frame.fill_round_box(Rect { c.status.x - r, c.status.y, c.status.width + r, c.status.height + r }, r,
                t.border_width, t.chrome_border, t.status_background);
            std::u32string const status = ellipsize(decode_utf8(status_text()),
                static_cast<float>(c.status.width - 2 * t.padding), t.status_font_size);
            draw_text(frame, status, static_cast<float>(c.status.x + t.padding),
                centered_baseline(c.status, t.status_font_size), t.status_font_size, t.status_text);
            frame.set_clip(std::nullopt);
        }

        paint_menus(c);

        dirty = false;
    }

    // The open menus, over everything else: a bordered box each, a row an
    // item — its check mark, its label, and at the right end its shortcut
    // or the mark of a submenu — the highlighted row lit, an item that
    // cannot be chosen dimmed, a separator a line.
    void paint_menus(ChromeLayout const& c)
    {
        Theme const& t = theme;
        float const advance = text_width(U"0", t.font_size); // the room between a menu's columns, as it is laid out
        for (std::size_t l = 0; l < c.menus.size() && l < menus.size(); ++l) {
            MenuBox const& box = c.menus[l];
            MenuLevel const& level = menus[l];
            frame.fill_round_box(box.box, t.button_corner_radius, t.border_width, t.popup_border, t.popup_background);
            bool any_checked = false;
            for (MenuItem const& item : level.items)
                any_checked = any_checked || item.checked;
            for (std::size_t i = 0; i < box.rows.size() && i < level.items.size(); ++i) {
                MenuItem const& item = level.items[i];
                Rect const row = box.rows[i];
                if (item.separator()) {
                    frame.fill_rect(Rect { row.x + t.padding, row.y + row.height / 2,
                                        std::max(0, row.width - 2 * t.padding), t.border_width },
                        t.popup_border);
                    continue;
                }
                bool const lit = level.highlighted == i && item.choosable();
                if (lit)
                    frame.fill_round_rect(row, t.button_corner_radius, t.popup_highlight);
                Color const color = !item.enabled ? t.popup_disabled_text : lit ? t.popup_highlight_text : t.popup_text;
                float const baseline = centered_baseline(row, t.font_size);
                float x = static_cast<float>(row.x + 2 * t.padding);
                float right = static_cast<float>(row.right() - 2 * t.padding);
                if (any_checked) {
                    if (item.checked)
                        draw_text(frame, std::u32string_view(&glyph_check, 1), x, baseline, t.font_size, color);
                    x += 2 * advance;
                }
                if (!item.children.empty()) {
                    draw_text(frame, std::u32string_view(&glyph_submenu, 1), right - advance, baseline, t.font_size, color);
                    right -= 2 * advance;
                } else if (!item.shortcut.empty()) {
                    std::u32string const shortcut = decode_utf8(item.shortcut);
                    float const shortcut_x = right - text_width(shortcut, t.font_size);
                    draw_text(frame, shortcut, shortcut_x, baseline, t.font_size,
                        item.enabled ? t.popup_text_muted : t.popup_disabled_text);
                    right = shortcut_x - advance;
                }
                draw_text(frame, ellipsize(decode_utf8(item.label), right - x, t.font_size), x, baseline,
                    t.font_size, color);
            }
        }
    }

    bool can_go(int delta) const
    {
        Tab const* const tab = active_tab();
        if (!tab)
            return false;
        auto const target = static_cast<std::ptrdiff_t>(tab->index) + delta;
        return target >= 0 && target < static_cast<std::ptrdiff_t>(tab->history.size());
    }

    // Whether what the shell has to say is worth a box over the page: where
    // a link leads, what is loading, a notice — not that a load is done,
    // which is what a page being there already says.
    static bool status_worth_showing(std::string const& said) { return !said.empty() && !said.starts_with("Done"); }

    std::string status_text() const
    {
        if (hover_link)
            return hover_link->serialize();
        Tab const* const tab = active_tab();
        return tab ? tab->status : std::string();
    }

    // The page as text: runs on one line concatenate (words and the spaces
    // between them are separate runs), a new baseline starts a new line.
    static void collect_text(layout::Fragment const& fragment, std::string& out,
        float& last_baseline)
    {
        for (layout::TextRun const& run : fragment.runs) {
            if (!out.empty() && run.baseline_y != last_baseline)
                out += '\n';
            last_baseline = run.baseline_y;
            out += to_utf8(run.text);
        }
        for (layout::Fragment const& child : fragment.children)
            collect_text(child, out, last_baseline);
    }
};

// --- The public surface --------------------------------------------------------

Browser::Browser(Loader& loader, Theme theme, int width, int height)
    : m_impl(std::make_unique<Impl>(loader, std::move(theme), width, height))
{
}

Browser::~Browser() = default;

void Browser::set_theme(Theme theme) { m_impl->set_base_theme(std::move(theme)); }

void Browser::open_palette(std::string const& query) { m_impl->open_palette(query); }
bool Browser::menu_open() const { return !m_impl->menus.empty(); }
std::string Browser::menu_text() const { return m_impl->menu_text(); }
bool Browser::choose_menu_item(std::string const& label) { return m_impl->choose_menu_item(label); }
void Browser::open_main_menu() { m_impl->open_main_menu(); }
bool Browser::palette_open() const { return m_impl->palette_open; }
std::string Browser::palette_selection() const { return m_impl->palette_selection(); }
void Browser::set_theme_presets(std::vector<ThemePreset> presets) { m_impl->theme_presets = std::move(presets); }
std::optional<std::string> Browser::take_theme_request()
{
    std::optional<std::string> request = std::move(m_impl->theme_request);
    m_impl->theme_request.reset();
    return request;
}

Theme const& Browser::theme() const { return m_impl->theme; }
std::vector<std::string> const& Browser::theme_problems() const { return m_impl->theme_problems; }

void Browser::set_scale(float scale)
{
    float const clamped = std::clamp(scale, 0.5f, 8.0f);
    if (!(scale > 0) || clamped == m_impl->scale)
        return;
    m_impl->close_menus(); // hung from a point of the old geometry
    m_impl->scale = clamped;
    m_impl->theme = m_impl->base_theme.scaled(clamped);
    // Nothing is laid out here: see resize().
    m_impl->dirty = true;
}

float Browser::scale() const { return m_impl->scale; }

void Browser::set_downloads_directory(std::string directory)
{
    m_impl->downloads_directory = std::move(directory);
}

void Browser::set_js_heap_limit(std::size_t bytes) { m_impl->js_heap_limit = bytes; }

void Browser::set_user_themes_directory(std::string directory)
{
    m_impl->user_themes_directory = std::move(directory);
}

void Browser::set_theme_gallery_source(std::string address) { m_impl->theme_gallery_api = std::move(address); }

void Browser::resize(int width, int height)
{
    // The size is taken, and that is all: no tab is laid out and no frame is
    // made. A window being dragged goes through dozens of sizes a second,
    // and a page laid out for each — every tab's, as this once did — is a
    // window that answers nothing for as long as the queue of sizes lasts.
    // The tab in front is laid out for the size there is when the next
    // frame is asked for (frame, ensure_fresh); the others when shown.
    if (std::max(width, 1) == m_impl->width && std::max(height, 1) == m_impl->height)
        return;
    m_impl->close_menus(); // hung from a point of the old geometry
    m_impl->width = std::max(width, 1);
    m_impl->height = std::max(height, 1);
    m_impl->dirty = true;
}

int Browser::width() const { return m_impl->width; }
int Browser::height() const { return m_impl->height; }

void Browser::mouse_move(int x, int y) { m_impl->mouse_move(x, y); }
void Browser::mouse_down(int x, int y, int button, platform::Modifiers modifiers)
{
    m_impl->mouse_down(x, y, button, modifiers);
}
void Browser::mouse_up(int, int, int button) { m_impl->mouse_up(button); }

void Browser::wheel(int x, int y, int notches)
{
    m_impl->update_hover(x, y);
    if (!m_impl->menus.empty())
        return; // an open menu holds the pointer: nothing under it moves
    if (m_impl->layout_chrome().content.contains(x, y))
        m_impl->wheel_at(x, y, notches);
    m_impl->refresh_hover();
}

void Browser::scroll_pixels(int x, int y, int dx, int dy)
{
    static_cast<void>(dx); // the page scrolls vertically only, as the wheel does
    m_impl->update_hover(x, y);
    if (!m_impl->menus.empty())
        return;
    if (m_impl->layout_chrome().content.contains(x, y))
        m_impl->scroll_pixels_at(x, y, dy);
    m_impl->refresh_hover();
}

void Browser::key_down(platform::KeyEvent const& key) { m_impl->key_down(key); }
void Browser::text_input(char32_t code_point) { m_impl->text_input(code_point); }
void Browser::preedit(std::string const& text) { m_impl->set_preedit(text); }
std::optional<Rect> Browser::text_input_area() const { return m_impl->text_input_area(); }

void Browser::set_window_controls(bool shown)
{
    if (m_impl->window_controls == shown)
        return;
    m_impl->window_controls = shown;
    m_impl->refresh_hover();
    m_impl->dirty = true;
}

bool Browser::window_controls() const { return m_impl->window_controls; }

Browser::WindowRequest Browser::take_window_request()
{
    WindowRequest const request = m_impl->window_request;
    m_impl->window_request = WindowRequest::None;
    return request;
}

void Browser::set_window_active(bool active)
{
    if (m_impl->window_active == active)
        return;
    m_impl->window_active = active;
    m_impl->dirty = true;
}

bool Browser::window_active() const { return m_impl->window_active; }

void Browser::navigate(std::string const& typed) { m_impl->navigate(typed); }
void Browser::open(net::Url const& url) { m_impl->open(url); }
void Browser::open_in_new_tab(net::Url const& url) { m_impl->open_in_new_tab(url); }
void Browser::back() { m_impl->go(-1); }
void Browser::forward() { m_impl->go(+1); }
void Browser::reload() { m_impl->reload(); }
void Browser::new_tab() { m_impl->new_tab(); }
void Browser::close_tab(std::size_t index) { m_impl->close_tab(index); }
void Browser::select_tab(std::size_t index) { m_impl->select_tab(index); }
void Browser::reopen_closed_tab() { m_impl->reopen_closed_tab(); }
void Browser::duplicate_tab(std::size_t index) { m_impl->duplicate_tab(index); }

bool Browser::has_pending_load() const
{
    if (!m_impl->pending.empty() || !m_impl->pending_windows.empty())
        return true;
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab && tab->images_owed;
}

bool Browser::tick()
{
    // A window a page asked for opens first: its tab, then its load queued
    // like any other.
    if (!m_impl->pending_windows.empty()) {
        Impl::PendingWindow const window = m_impl->pending_windows.front();
        m_impl->pending_windows.erase(m_impl->pending_windows.begin());
        // In front, beside the tab whose page asked.
        m_impl->open_tab_beside(window.container, window.url, window.opener, true);
    }
    if (m_impl->pending.empty()) {
        // The page shown takes the next of its pictures.
        if (Impl::Tab* const tab = m_impl->active_tab(); tab && tab->images_owed) {
            m_impl->continue_images(*tab);
            return true;
        }
        return false;
    }
    Impl::Pending const load = m_impl->pending.front();
    m_impl->pending.erase(m_impl->pending.begin());
    return m_impl->perform(load);
}

std::string Browser::session_json() const { return m_impl->session_json(); }
bool Browser::restore_session(std::string_view json) { return m_impl->restore_session(json); }

void Browser::set_containers(std::vector<Container> containers)
{
    m_impl->containers = std::move(containers);
    m_impl->dirty = true;
}
std::vector<Browser::Container> const& Browser::containers() const { return m_impl->containers; }
void Browser::new_tab_in(std::string const& container) { m_impl->new_tab_in(container); }
std::string const& Browser::active_container() const
{
    static std::string const none;
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? tab->container : none;
}

std::string Browser::storage_json() const { return m_impl->storage_json(); }
bool Browser::restore_storage(std::string_view json) { return m_impl->restore_storage(json); }
std::uint64_t Browser::storage_changes() const { return m_impl->storage_changes(); }

bool Browser::run_scripts()
{
    Impl& impl = *m_impl;
    bool ran = false;
    for (Impl::Tab& tab : impl.tabs) {
        if (!tab.realm)
            continue;
        impl.script_started = std::chrono::steady_clock::now();
        if (tab.realm->run_pending())
            ran = true;
    }
    if (Impl::Tab* const tab = impl.active_tab())
        impl.ensure_fresh(*tab);
    return ran;
}

std::optional<double> Browser::next_timer_ms() const
{
    std::optional<double> soonest;
    for (Impl::Tab const& tab : m_impl->tabs) {
        if (!tab.realm)
            continue;
        if (std::optional<double> const due = tab.realm->next_timer_due()) {
            if (!soonest || *due < *soonest)
                soonest = *due;
        }
    }
    if (!soonest)
        return std::nullopt;
    return std::max(0.0, *soonest - m_impl->script_now());
}

void Browser::set_clock(std::function<double()> now) { m_impl->clock = std::move(now); }
void Browser::set_wall_clock(std::function<WallTime()> now) { m_impl->wall_clock = std::move(now); }
std::size_t Browser::blocked_requests() const { return m_impl->loader.blocked_requests(); }

std::string Browser::console_text() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    if (!tab)
        return {};
    std::string text;
    for (std::string const& line : tab->console)
        text += line + "\n";
    return text;
}

Bitmap const& Browser::frame()
{
    // The frame is made at the window's size when one is asked for, and not
    // as the sizes come in (see resize); the tab in front is laid out for
    // that size here, once.
    if (m_impl->frame.width() != m_impl->width || m_impl->frame.height() != m_impl->height) {
        m_impl->frame = Bitmap(m_impl->width, m_impl->height, m_impl->theme.chrome_background);
        m_impl->dirty = true;
    }
    if (Impl::Tab* const tab = m_impl->active_tab())
        m_impl->ensure_fresh(*tab);
    if (m_impl->dirty)
        m_impl->paint();
    return m_impl->frame;
}

bool Browser::needs_paint() const { return m_impl->dirty; }
Profile const& Browser::profile() const { return m_impl->profile; }

std::size_t Browser::pictures() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    if (!tab)
        return 0;
    std::size_t decoded = 0;
    for (auto const& [element, image] : tab->images) {
        if (image.bitmap)
            ++decoded;
    }
    return decoded;
}

platform::Cursor Browser::cursor() const
{
    if (m_impl->hover == Impl::Hover::Content) {
        if (dom::Element const* const control = m_impl->control_at(m_impl->mouse_x, m_impl->mouse_y))
            return layout::is_text_kind(layout::control_kind(*control)) ? Cursor::Text
                                                                          : Cursor::Arrow;
        if (m_impl->hover_link)
            return Cursor::Hand;
    }
    if (m_impl->hover == Impl::Hover::Address)
        return Cursor::Text;
    return Cursor::Arrow;
}

std::string Browser::window_title() const
{
    std::string const title = page_title();
    return title.empty() ? std::string("Sashfold") : title + " - Sashfold";
}

std::size_t Browser::tab_count() const { return m_impl->tabs.size(); }
std::size_t Browser::active_tab() const { return m_impl->active; }
std::string Browser::tab_title(std::size_t index) const
{
    return index < m_impl->tabs.size() ? m_impl->tab_title(m_impl->tabs[index]) : std::string();
}
std::size_t Browser::closed_tab_count() const { return m_impl->closed_tabs.size(); }
bool Browser::tab_pinned(std::size_t index) const { return index < m_impl->tabs.size() && m_impl->tabs[index].pinned; }

net::Url const* Browser::current_url() const
{
    HistoryEntry const* const entry = current_entry();
    return entry ? &entry->final_url : nullptr;
}

HistoryEntry const* Browser::current_entry() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? tab->current() : nullptr;
}

bool Browser::can_go_back() const { return m_impl->can_go(-1); }
bool Browser::can_go_forward() const { return m_impl->can_go(+1); }
std::string const& Browser::address_text() const { return m_impl->address; }
bool Browser::address_focused() const { return m_impl->address_focus; }
std::string Browser::status_text() const { return m_impl->status_text(); }

std::string Browser::page_title() const
{
    HistoryEntry const* const entry = current_entry();
    return entry ? entry->title : std::string();
}

std::string Browser::page_text() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    std::string text;
    float last_baseline = 0;
    if (tab && tab->document) {
        Impl::collect_text(tab->layout.root, text, last_baseline);
        // The frames' documents' text after the page's, each frame's after
        // the one it is in.
        std::function<void(DrawnFrames const&)> const frames = [&](DrawnFrames const& drawn) {
            for (auto const& [element, frame] : drawn) {
                if (!frame.view)
                    continue;
                Impl::collect_text(frame.view->layout.root, text, last_baseline);
                frames(frame.view->frames);
            }
        };
        frames(tab->frames);
    }
    return text;
}

int Browser::scroll_y() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? tab->scroll_y : 0;
}

std::pair<int, int> Browser::box_scroll_at(int x, int y) const
{
    Impl::Tab* const tab = m_impl->active_tab();
    std::optional<std::pair<float, float>> const point = m_impl->page_point(x, y);
    if (!tab || !point)
        return { 0, 0 };
    // Inside a frame, the innermost scrolling thing under the point is a box
    // of the frame's document, else the document itself down its frame.
    std::vector<Impl::FrameStep> const chain = Impl::frames_at(*tab, point->first, point->second);
    if (!chain.empty()) {
        Impl::FrameStep const& step = chain.back();
        if (layout::Fragment const* const box = Impl::scrollport_at(step.view->layout.root, step.x, step.y)) {
            layout::ScrollOffset const at = layout::scroll_of(*box, &step.view->scrolls);
            return { static_cast<int>(at.x + 0.5f), static_cast<int>(at.y + 0.5f) };
        }
        return { 0, step.view->scroll_y };
    }
    layout::Fragment const* const box
        = Impl::scrollport_at(tab->layout.root, point->first, point->second);
    if (!box)
        return { 0, 0 };
    layout::ScrollOffset const at = layout::scroll_of(*box, &tab->scrolls);
    return { static_cast<int>(at.x + 0.5f), static_cast<int>(at.y + 0.5f) };
}

std::optional<net::Url> Browser::link_at(int x, int y) const { return m_impl->link_at(x, y); }

std::optional<std::pair<int, int>> Browser::find_text(std::string const& text) const
{
    Impl::Tab* const tab = m_impl->active_tab();
    if (!tab || !tab->document || text.empty())
        return std::nullopt;
    ChromeLayout const chrome = m_impl->layout_chrome();
    std::optional<Impl::TextHit> const hit = m_impl->find_text_in(tab->layout.root, text, *tab, chrome);
    if (hit)
        return std::make_pair(hit->x, hit->y);
    // Then the frames' documents, the found run's center moved by where its
    // frame's document sits on the page.
    std::function<std::optional<std::pair<int, int>>(DrawnFrames&)> const search
        = [&](DrawnFrames& drawn) -> std::optional<std::pair<int, int>> {
        for (auto& [element, frame] : drawn) {
            if (!frame.view)
                continue;
            if (std::optional<Impl::TextHit> const found = m_impl->find_text_in(frame.view->layout.root, text, *tab, chrome)) {
                auto const [ox, oy] = Impl::origin_of(Impl::frames_to(*tab, element));
                return std::make_pair(found->x + static_cast<int>(std::lround(ox)), found->y + static_cast<int>(std::lround(oy)));
            }
            if (std::optional<std::pair<int, int>> const inner = search(frame.view->frames))
                return inner;
        }
        return std::nullopt;
    };
    return search(tab->frames);
}

ChromeLayout Browser::chrome_layout() const { return m_impl->layout_chrome(); }

namespace {

// The first control with this name in the page's document, else in a
// frame's, each frame's document after the one it is in.
dom::Element const* control_named_anywhere(dom::Document const& document, DrawnFrames const& frames, std::string const& name)
{
    if (dom::Element const* const control = control_named(document, name))
        return control;
    for (auto const& [element, frame] : frames) {
        if (!frame.view)
            continue;
        if (dom::Element const* const control = control_named_anywhere(*frame.view->document, frame.view->frames, name))
            return control;
    }
    return nullptr;
}

}

bool Browser::focus_control(std::string const& name)
{
    Impl::Tab* const tab = m_impl->active_tab();
    if (!tab || !tab->document)
        return false;
    dom::Element const* const control = control_named_anywhere(*tab->document, tab->frames, name);
    if (!control || !layout::is_control(*control))
        return false;
    tab->controls.focused = control;
    m_impl->caret_to_end(*tab, *control);
    m_impl->relayout(*tab);
    m_impl->dirty = true;
    return true;
}

std::optional<std::string> Browser::control_value(std::string const& name) const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    if (!tab || !tab->document)
        return std::nullopt;
    dom::Element const* const control = control_named_anywhere(*tab->document, tab->frames, name);
    if (!control)
        return std::nullopt;
    return layout::control_value(*control, &tab->controls);
}

std::string Browser::focused_control_name() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    if (!tab || !tab->controls.focused)
        return "";
    dom::Attr const* const name = tab->controls.focused->find_attribute("name");
    return name ? name->value : "";
}

std::string Browser::selected_text() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? Impl::selected_text(*tab) : std::string();
}

bool Browser::select_text(std::string const& text)
{
    Impl::Tab* const tab = m_impl->active_tab();
    if (!tab || text.empty())
        return false;
    std::u32string const needle = decode_utf8(text);
    // A line is a stretch of runs sharing a baseline, in tree order; the
    // text may span several of its runs. The page's runs first, then each
    // frame's.
    auto const find_in = [&](std::vector<layout::TextRun const*> const& runs) -> std::optional<Impl::Selection> {
        std::size_t line_start = 0;
        while (line_start < runs.size()) {
            std::size_t line_end = line_start + 1;
            while (line_end < runs.size() && runs[line_end]->baseline_y == runs[line_start]->baseline_y)
                ++line_end;
            std::u32string line;
            std::vector<std::size_t> starts; // each run's offset within `line`
            for (std::size_t i = line_start; i < line_end; ++i) {
                starts.push_back(line.size());
                line += runs[i]->text;
            }
            std::size_t const at = line.find(needle);
            if (at != std::u32string::npos) {
                auto const locate = [&](std::size_t index) {
                    std::size_t run = 0;
                    for (std::size_t k = 0; k < starts.size(); ++k) {
                        if (starts[k] <= index)
                            run = k;
                    }
                    return Impl::TextPosition { line_start + run, index - starts[run] };
                };
                return Impl::Selection { locate(at), locate(at + needle.size()) };
            }
            line_start = line_end;
        }
        return std::nullopt;
    };
    m_impl->clear_selection(*tab);
    if (std::optional<Impl::Selection> const found = find_in(tab->runs)) {
        tab->selection = *found;
        m_impl->blur_address();
        m_impl->dirty = true;
        return true;
    }
    std::function<bool(DrawnFrames&)> const search = [&](DrawnFrames& drawn) {
        for (auto& [element, frame] : drawn) {
            if (!frame.view)
                continue;
            if (std::optional<Impl::Selection> const found = find_in(frame.view->runs)) {
                tab->selection = *found;
                tab->selection_frame = element;
                tab->selection_view = frame.view;
                m_impl->repaint_frames(*tab, Impl::frames_to(*tab, element));
                m_impl->blur_address();
                return true;
            }
            if (search(frame.view->frames))
                return true;
        }
        return false;
    };
    return search(tab->frames);
}

std::string Browser::find_status() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? m_impl->find_status(*tab) : std::string();
}

void Browser::toggle_reader() { m_impl->toggle_reader(); }

std::size_t Browser::hint_count() const { return m_impl->hints_active ? m_impl->hints.size() : 0; }

bool Browser::inspect_text(std::string const& text)
{
    Impl::Tab* const tab = m_impl->active_tab();
    if (!tab || text.empty())
        return false;
    std::u32string const needle = decode_utf8(text);
    for (layout::TextRun const* const run : tab->runs) {
        if (run->element && run->text.find(needle) != std::u32string::npos) {
            m_impl->inspect(*tab, run->element);
            return true;
        }
    }
    return false;
}

std::string Browser::inspected_summary() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? Impl::node_summary(tab->inspected) : std::string();
}

}
