#include "ui/Browser.h"
#include "ui/Forms.h"

#include "core/Ascii.h"
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
#include "ui/Downloads.h"
#include "ui/InternalPages.h"
#include "ui/Reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace sashfold::ui {

using platform::Cursor;
using platform::Key;
using platform::KeyEvent;

namespace {

constexpr char32_t glyph_back = 0x2190;
constexpr char32_t glyph_forward = 0x2192;
constexpr char32_t glyph_reload = 0x21BB;
constexpr char32_t glyph_close = 0x00D7;
constexpr char32_t glyph_plus = U'+';
constexpr char32_t glyph_ellipsis = 0x2026;
constexpr char32_t glyph_reader = 0x00B6; // the pilcrow: reader mode

constexpr float font_ascent_ratio = 25.0f / 32.0f;
constexpr float font_descent_ratio = 7.0f / 32.0f;

float text_width(std::u32string_view text, float size)
{
    return text::SashfoldMono::measure(text, size);
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
    return run.fonts ? run.fonts->measure(prefix, size) : text_width(prefix, size);
}

void draw_text(Bitmap& target, std::u32string_view text, float x, float baseline, float size,
    Color color, bool bold = false)
{
    text::SashfoldMono const& font = text::SashfoldMono::instance();
    float const advance = text::SashfoldMono::advance(size);
    for (char32_t const c : text) {
        font.draw_glyph(target, c, x, baseline, size, color, bold, false);
        x += advance;
    }
}

// The baseline that centers the em box (ascent 25, descent 7 of 32) in a rect.
float centered_baseline(Rect const& rect, float size)
{
    float const ascent = size * font_ascent_ratio;
    float const descent = size * font_descent_ratio;
    return static_cast<float>(rect.y)
        + (static_cast<float>(rect.height) - ascent - descent) / 2.0f + ascent;
}

void draw_glyph_centered(Bitmap& target, char32_t glyph, Rect const& rect, float size, Color color)
{
    float const x = static_cast<float>(rect.x)
        + (static_cast<float>(rect.width) - text::SashfoldMono::advance(size)) / 2.0f;
    draw_text(target, std::u32string_view(&glyph, 1), x, centered_baseline(rect, size), size,
        color);
}

std::u32string ellipsize(std::u32string text, float max_width, float size)
{
    float const advance = text::SashfoldMono::advance(size);
    if (advance <= 0 || max_width <= 0)
        return {};
    auto const fit = static_cast<std::size_t>(max_width / advance);
    if (text.size() <= fit)
        return text;
    if (fit == 0)
        return {};
    text.resize(fit - 1);
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
        std::unique_ptr<dom::Document> document;
        std::vector<css::SheetSource> sheets; // the page's stylesheets, kept so a resize can restyle
        std::vector<text::PageFont> fonts; // the fonts its @font-face rules brought along
        std::optional<css::StyleSet> style_set; // the sheets compiled for style_media
        css::MediaContext style_media;
        css::StyleMap styles;
        layout::ImageMap images; // the page's pictures, decoded
        layout::BackgroundImages backgrounds; // the pictures its styles name as backgrounds
        layout::ControlStates controls; // what the user typed and toggled in the page's forms
        layout::LayoutResult layout;
        std::vector<layout::TextRun const*> runs; // the layout's runs in tree order
        std::optional<Selection> selection; // positions into `runs`; dropped with the layout
        std::vector<Match> matches; // the find bar's query in this tab, refreshed with the layout
        std::size_t current_match = 0;
        dom::Node const* inspected = nullptr; // devtools: the node under inspection
        int tree_scroll = 0; // devtools: the first tree line shown
        int scroll_y = 0;
        std::string status;

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
        Content
    };

    Loader& loader;
    Theme theme;
    std::string downloads_directory;
    int width;
    int height;
    std::vector<Tab> tabs;
    std::size_t active = 0;
    std::vector<Pending> pending;

    std::string address;
    bool address_focus = false;
    bool select_all = false;
    std::size_t caret = 0;

    int mouse_x = -1;
    int mouse_y = -1;
    Hover hover = Hover::None;
    std::size_t hover_index = 0;
    std::optional<net::Url> hover_link;
    bool selecting = false; // the left button went down on page text and is still held

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

    Bitmap frame;
    bool dirty = true;

    Impl(Loader& the_loader, Theme the_theme, int the_width, int the_height)
        : loader(the_loader)
        , theme(std::move(the_theme))
        , width(std::max(the_width, 1))
        , height(std::max(the_height, 1))
        , frame(width, height, theme.chrome_background)
    {
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

    void add_blank_tab()
    {
        Tab tab;
        tab.history.push_back(blank_entry());
        tabs.push_back(std::move(tab));
        active = tabs.size() - 1;
        render(tabs[active]);
        sync_address();
        dirty = true;
    }

    std::string display_url(HistoryEntry const& entry) const
    {
        if (is_about_blank(entry.url))
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
        c.status = Rect { 0, height - t.status_height, width, t.status_height };
        c.content = Rect { 0, content_top, width,
            std::max(0, height - content_top - t.status_height) };
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

        int const count = static_cast<int>(tabs.size());
        int const available = width - 3 * t.padding - t.button_size;
        int tab_width = count > 0 ? (available - (count - 1) * t.tab_gap) / count : t.tab_max_width;
        tab_width = std::clamp(tab_width, t.tab_min_width, t.tab_max_width);
        int const tab_y = t.tab_strip_height - t.tab_height;
        int const close_size = std::max(10, t.tab_height - 12);
        int x = t.padding;
        for (int i = 0; i < count; ++i) {
            Rect const tab { x, tab_y, tab_width, t.tab_height };
            c.tabs.push_back(tab);
            c.tab_close_buttons.push_back(Rect { tab.right() - t.padding - close_size,
                tab.y + (tab.height - close_size) / 2, close_size, close_size });
            x += tab_width + t.tab_gap;
        }
        int const new_tab_x = std::min(x + t.padding / 2, width - t.padding - t.button_size);
        c.new_tab_button = Rect { new_tab_x, tab_y + (t.tab_height - t.button_size) / 2,
            t.button_size, t.button_size };

        int const button_y = c.toolbar.y + (t.toolbar_height - t.button_size) / 2;
        int const step = t.button_size + t.padding;
        c.back_button = Rect { t.padding, button_y, t.button_size, t.button_size };
        c.forward_button = Rect { t.padding + step, button_y, t.button_size, t.button_size };
        c.reload_button = Rect { t.padding + 2 * step, button_y, t.button_size, t.button_size };
        int const address_x = c.reload_button.right() + 2 * t.padding;
        // The reader button sits at the toolbar's right end; the address
        // bar takes what lies between.
        c.reader_button = Rect { std::max(address_x, width - t.padding - t.button_size), button_y,
            t.button_size, t.button_size };
        c.address = Rect { address_x, c.toolbar.y + (t.toolbar_height - t.address_height) / 2,
            std::max(0, c.reader_button.x - 2 * t.padding - address_x), t.address_height };
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
        dirty = true;
    }

    // --- Rendering a history entry ------------------------------------------------

    // What media queries see: the content area.
    css::MediaContext media_context()
    {
        ChromeLayout const c = layout_chrome();
        return css::MediaContext { static_cast<float>(std::max(1, c.content.width)),
            static_cast<float>(std::max(1, c.content.height)) };
    }

    // Styles depend on the viewport through media queries: computed when a
    // page arrives and again when the content area changes size.
    void restyle(Tab& tab)
    {
        if (!tab.document)
            return;
        text::FontManager::instance().set_page_fonts(tab.fonts);
        css::MediaContext const media = media_context();
        if (!tab.style_set || tab.style_media.width != media.width
            || tab.style_media.height != media.height) {
            net::Url const* const page_url
                = tab.index < tab.history.size() ? &tab.history[tab.index].final_url : nullptr;
            tab.style_set.emplace(tab.sheets, media, page_url);
            tab.style_media = media;
        }
        tab.styles = css::resolve_styles(*tab.document, *tab.style_set);
    }

    void relayout(Tab& tab)
    {
        if (!tab.document)
            return;
        // The page's own fonts answer this tab's families; another tab may
        // have set its own since.
        text::FontManager::instance().set_page_fonts(tab.fonts);
        ChromeLayout const c = layout_chrome();
        tab.layout = layout::layout_document(*tab.document, tab.styles,
            static_cast<float>(std::max(1, c.content.width)), &tab.images, &tab.controls,
            static_cast<float>(std::max(1, c.content.height)));
        tab.scroll_y = std::clamp(tab.scroll_y, 0, max_scroll(tab));
        // The selection pointed into the old layout's runs.
        tab.runs.clear();
        gather_runs(tab.layout.root, tab.runs);
        tab.selection.reset();
        selecting = false;
        update_matches(tab);
        stop_hints(); // the labels pointed into the old layout
    }

    void render(Tab& tab)
    {
        HistoryEntry* const entry = tab.current();
        if (!entry) {
            tab.document.reset();
            return;
        }
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
        tab.document = html::parse_document_bytes(source);
        tab.controls = {}; // a new document: nothing typed into it yet
        tab.inspected = nullptr;
        tab.tree_scroll = 0;
        // The page's stylesheets come through the loader with the page as
        // first party and the usual referrer policy; a sheet that fails to
        // load is simply absent.
        net::Url const& page_url = entry->final_url;
        auto const fetch_sheet = [&](net::Url const& url) -> std::optional<css::FetchedSheet> {
            net::FetchResult result = loader.load_subresource(url, page_url,
                referrer_for(&page_url, url));
            if (!result.response || result.response->status != 200)
                return std::nullopt;
            std::string const* header = net::find_header(result.response->headers, "content-type");
            return css::FetchedSheet { std::move(result.response->body), header ? *header : "" };
        };
        tab.sheets = css::collect_stylesheets(*tab.document, &page_url, fetch_sheet, media_context());
        tab.fonts = css::collect_page_fonts(tab.sheets, fetch_sheet, media_context());
        tab.style_set.reset();
        restyle(tab);
        auto const fetch_image = [&](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
            net::FetchResult result = loader.load_subresource(url, page_url, referrer_for(&page_url, url));
            if (!result.response || result.response->status != 200)
                return std::nullopt;
            return std::move(result.response->body);
        };
        tab.images = collect_images(*tab.document, &page_url, fetch_image, media_context());
        tab.backgrounds = collect_background_images(tab.styles, fetch_image);
        entry->title = find_title(*tab.document);
        tab.scroll_y = entry->scroll_y;
        relayout(tab);
        if (entry->scroll_y == 0 && entry->final_url.fragment && !entry->final_url.fragment->empty())
            scroll_to_fragment(tab, *entry->final_url.fragment);
        dirty = true;
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
        } else if (load.url.scheme == "view-source") {
            std::optional<net::Url> const inner = net::parse_url(load.url.serialize_path());
            if (!inner) {
                fail_entry(entry, load.url, "view-source: needs a URL after it");
            } else {
                net::FetchResult result = loader.load(*inner, "", load.mode == Mode::Reload);
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
                net::FetchResult result = loader.load(*inner, "", load.mode == Mode::Reload);
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
            net::FetchResult result = loader.load(load.url, referrer, load.mode == Mode::Reload);
            if (!result.response && load.https_first
                && result.error.find("could not connect") != std::string::npos) {
                // HTTPS-first: a host that does not answer on 443 gets one
                // plain-HTTP try, and the address bar says so.
                net::Url http = load.url;
                http.scheme = "http";
                net::FetchResult retry = loader.load(http, referrer, load.mode == Mode::Reload);
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
                std::string const* const disposition
                    = net::find_header(response.headers, "content-disposition");
                bool const attachment = disposition && starts_with_ci(trim(*disposition), "attachment");
                if (attachment || !is_renderable_content_type(entry.content_type))
                    download_status = receive_download(entry, response, disposition, referrer);
                else
                    entry.bytes = std::move(response.body);
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

    void new_tab()
    {
        blur_address();
        add_blank_tab();
        focus_address(false);
        refresh_hover();
    }

    void close_tab(std::size_t index)
    {
        if (index >= tabs.size())
            return;
        tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(index));
        for (Pending& load : pending) {
            if (load.tab > index)
                --load.tab;
        }
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                          [&](Pending const& load) { return load.tab == index; }),
            pending.end());
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
        active = index;
        blur_address();
        sync_address();
        if (find_open)
            update_matches(tabs[index]); // the query may have changed while another tab showed
        refresh_hover();
        dirty = true;
    }

    // --- Hit testing --------------------------------------------------------------

    static dom::Element const* hit_run(layout::Fragment const& fragment, float x, float y)
    {
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
    dom::Element const* control_at(int x, int y) const
    {
        Tab const* const tab = active_tab();
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!tab || !point)
            return nullptr;
        if (dom::Element const* const control
            = hit_control(tab->layout.root, point->first, point->second))
            return control;
        dom::Element const* const hit = hit_run(tab->layout.root, point->first, point->second);
        for (dom::Node const* node = hit; node; node = node->parent()) {
            if (!node->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*node);
            if (!element.is_html("label"))
                continue;
            if (dom::Attr const* const target = element.find_attribute("for")) {
                dom::Element const* const named = element_by_id(*tab->document, target->value);
                return named && layout::is_control(*named) ? named : nullptr;
            }
            return first_control_within(element);
        }
        return nullptr;
    }

    std::optional<net::Url> link_at(int x, int y) const
    {
        ChromeLayout const c = layout_chrome();
        Tab const* const tab = active_tab();
        HistoryEntry const* const entry = tab ? tab->current() : nullptr;
        if (!tab || !tab->document || !entry || !c.content.contains(x, y))
            return std::nullopt;
        float const px = static_cast<float>(x - c.content.x);
        float const py = static_cast<float>(y - c.content.y + tab->scroll_y);
        dom::Element const* const hit = hit_run(tab->layout.root, px, py);
        for (dom::Node const* node = hit; node; node = node->parent()) {
            if (!node->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*node);
            if (!element.is_html("a"))
                continue;
            dom::Attr const* const href = element.find_attribute("href");
            if (!href)
                continue;
            return net::parse_url(href->value, &entry->final_url);
        }
        return std::nullopt;
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
        mouse_x = x;
        mouse_y = y;
        ChromeLayout const c = layout_chrome();
        Hover next = Hover::None;
        std::size_t index = 0;
        std::optional<net::Url> link;
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
            else if (c.back_button.contains(x, y))
                next = Hover::Back;
            else if (c.forward_button.contains(x, y))
                next = Hover::Forward;
            else if (c.reload_button.contains(x, y))
                next = Hover::Reload;
            else if (c.reader_button.contains(x, y))
                next = Hover::Reader;
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
                link = link_at(x, y);
            }
        }
        if (next != hover || index != hover_index || !same_url(link, hover_link)) {
            hover = next;
            hover_index = index;
            hover_link = std::move(link);
            dirty = true;
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
    static std::optional<TextPosition> position_at(Tab const& tab, float px, float py)
    {
        if (tab.runs.empty())
            return std::nullopt;
        std::optional<std::size_t> same_line;
        float same_line_distance = 0;
        std::optional<std::size_t> above;
        float above_bottom = 0;
        for (std::size_t i = 0; i < tab.runs.size(); ++i) {
            layout::TextRun const& run = *tab.runs[i];
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
            layout::TextRun const& run = *tab.runs[*same_line];
            return TextPosition { *same_line, px < run.x ? 0 : run.text.size() };
        }
        if (above)
            return TextPosition { *above, tab.runs[*above]->text.size() };
        return TextPosition { 0, 0 };
    }

    static std::pair<TextPosition, TextPosition> ordered(Selection const& selection)
    {
        if (selection.focus < selection.anchor)
            return { selection.focus, selection.anchor };
        return { selection.anchor, selection.focus };
    }

    // The selected text: each run's slice, lines separated by newlines.
    static std::string selected_text(Tab const& tab)
    {
        if (!tab.selection || tab.runs.empty())
            return {};
        auto const [start, end] = ordered(*tab.selection);
        std::string out;
        std::optional<float> last_baseline;
        for (std::size_t i = start.run; i <= end.run && i < tab.runs.size(); ++i) {
            layout::TextRun const& run = *tab.runs[i];
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
        std::size_t const size = tab.runs[position.run]->text.size();
        if (direction > 0) {
            if (position.offset < size) {
                ++position.offset;
            } else if (position.run + 1 < tab.runs.size()) {
                ++position.run;
                position.offset = std::min<std::size_t>(1, tab.runs[position.run]->text.size());
            }
        } else {
            if (position.offset > 0) {
                --position.offset;
            } else if (position.run > 0) {
                --position.run;
                std::size_t const previous = tab.runs[position.run]->text.size();
                position.offset = previous > 0 ? previous - 1 : 0;
            }
        }
        return position;
    }

    // The position on the line above or below, at the same x.
    static std::optional<TextPosition> line_step(Tab const& tab, TextPosition position, int direction)
    {
        layout::TextRun const& run = *tab.runs[position.run];
        float const x = run.x + prefix_width(run, position.offset);
        std::optional<float> target;
        for (layout::TextRun const* const other : tab.runs) {
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
        return position_at(tab, x, *target);
    }

    // The first or last position on the line a position sits on.
    static TextPosition line_end(Tab const& tab, TextPosition position, bool end)
    {
        float const baseline = tab.runs[position.run]->baseline_y;
        std::size_t index = position.run;
        if (end) {
            while (index + 1 < tab.runs.size() && tab.runs[index + 1]->baseline_y == baseline)
                ++index;
            return TextPosition { index, tab.runs[index]->text.size() };
        }
        while (index > 0 && tab.runs[index - 1]->baseline_y == baseline)
            --index;
        return TextPosition { index, 0 };
    }

    void start_selection(Tab& tab, int x, int y)
    {
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        std::optional<TextPosition> const position
            = point ? position_at(tab, point->first, point->second) : std::nullopt;
        if (!position) {
            tab.selection.reset();
            selecting = false;
            return;
        }
        tab.selection = Selection { *position, *position };
        selecting = true;
        dirty = true;
    }

    void mouse_move(int x, int y)
    {
        update_hover(x, y);
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
        if (std::optional<TextPosition> const focus = position_at(*tab, px, py);
            focus && !(*focus == tab->selection->focus)) {
            tab->selection->focus = *focus;
            dirty = true;
        }
    }

    void mouse_up(int button)
    {
        if (button == 1)
            selecting = false;
    }

    void select_all_text(Tab& tab)
    {
        if (tab.runs.empty())
            return;
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
        if (s.overflow != css::Overflow::Visible)
            display += "  overflow hidden";
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
        std::optional<std::pair<float, float>> const point = page_point(x, y);
        if (!point)
            return;
        dom::Element const* element = hit_run(tab.layout.root, point->first, point->second);
        if (!element)
            element = hit_control(tab.layout.root, point->first, point->second);
        if (!element)
            element = element_at_point(tab.layout.root, point->first, point->second);
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
    static void paint_bands(Bitmap& content, Tab const& tab, TextPosition start, TextPosition end,
        Color color)
    {
        for (std::size_t i = start.run; i <= end.run && i < tab.runs.size(); ++i) {
            layout::TextRun const& run = *tab.runs[i];
            std::size_t const from = i == start.run ? std::min(start.offset, run.text.size()) : 0;
            std::size_t const to = i == end.run ? std::min(end.offset, run.text.size()) : run.text.size();
            if (to <= from)
                continue;
            text::FaceMetrics const metrics = run_metrics(run);
            float const x1 = run.x + prefix_width(run, from);
            float const x2 = run.x + prefix_width(run, to);
            float const top = run.baseline_y - metrics.ascent - static_cast<float>(tab.scroll_y);
            content.fill_rect(Rect { static_cast<int>(x1 + 0.5f), static_cast<int>(top + 0.5f),
                                  static_cast<int>(x2 - x1 + 0.5f),
                                  static_cast<int>(metrics.ascent + metrics.descent + 0.5f) },
                color);
        }
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
        int const top = static_cast<int>(run.baseline_y - metrics.ascent);
        int const bottom = static_cast<int>(run.baseline_y + metrics.descent + 1);
        ChromeLayout const c = layout_chrome();
        if (top >= tab.scroll_y && bottom <= tab.scroll_y + c.content.height)
            return;
        set_scroll(tab, top - c.content.height / 3);
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
        dom::Element const* const form = form_owner(control, *tab.document);
        if (!form || !entry)
            return;
        dom::Element const* const submitter
            = layout::control_kind(control) == layout::ControlKind::Submit ? &control
                                                                            : default_submitter(*form);
        std::optional<net::Url> const url
            = get_submission_url(*form, submitter, &tab.controls, entry->final_url);
        if (!url) {
            tab.status = "This form posts; only GET forms are written yet";
            dirty = true;
            return;
        }
        queue(active, *url, Mode::Push);
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

    void mouse_down(int x, int y, int button)
    {
        update_hover(x, y);
        if (button == 1) {
            switch (hover) {
            case Hover::TabClose: close_tab(hover_index); break;
            case Hover::Tab: select_tab(hover_index); break;
            case Hover::NewTab: new_tab(); break;
            case Hover::Back: go(-1); break;
            case Hover::Forward: go(+1); break;
            case Hover::Reload: reload(); break;
            case Hover::Reader: toggle_reader(); break;
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
                if (devtools_open) {
                    // With the panel open a click inspects instead of navigating.
                    if (Tab* const tab = active_tab())
                        inspect_at(*tab, x, y);
                    break;
                }
                if (dom::Element const* const control = control_at(x, y)) {
                    activate_control(*control);
                } else {
                    blur_control();
                    if (hover_link) {
                        open(*hover_link);
                    } else if (Tab* const tab = active_tab()) {
                        start_selection(*tab, x, y);
                    }
                }
                break;
            case Hover::None: blur_address(); break;
            }
        } else if (button == 2) {
            if (hover == Hover::Content && hover_link)
                open_in_new_tab(*hover_link);
            else if (hover == Hover::Tab || hover == Hover::TabClose)
                close_tab(hover_index);
        }
        dirty = true;
    }

    void open_in_new_tab(net::Url const& url)
    {
        blur_address();
        add_blank_tab();
        queue(active, url, Mode::Push);
    }

    void scroll_by(int delta)
    {
        if (Tab* const tab = active_tab())
            set_scroll(*tab, tab->scroll_y + delta);
    }

    void key_down(KeyEvent const& key)
    {
        if (hints_active) {
            hint_key(key);
            return;
        }
        if (key.key == Key::F12
            || (key.ctrl && key.shift && key.key == Key::Letter && key.letter == U'I')) {
            toggle_devtools();
            return;
        }
        if (key.ctrl && key.key == Key::Letter) {
            switch (key.letter) {
            case U'L': focus_address(true); return;
            case U'T': new_tab(); return;
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
            tab->selection.reset();
            dirty = true;
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
        case Key::Home: set_scroll(*tab, 0); break;
        case Key::End: set_scroll(*tab, max_scroll(*tab)); break;
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

    void text_input(char32_t code_point)
    {
        if (code_point < 0x20 || code_point == 0x7F || hints_active)
            return; // a hint's letters are keys, not text
        if (find_focus) {
            type_into_find(code_point);
            return;
        }
        if (!address_focus) {
            type_into_control(code_point);
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

    void paint_button(Rect const& rect, char32_t glyph, bool enabled, bool hovered)
    {
        if (hovered && enabled)
            frame.fill_round_rect(rect, theme.button_corner_radius, theme.button_hover_background);
        draw_glyph_centered(frame, glyph, rect, theme.font_size * 1.15f,
            enabled ? theme.chrome_text : theme.button_disabled_text);
    }

    void paint()
    {
        Theme const& t = theme;
        ChromeLayout const c = layout_chrome();
        Tab const* const tab = active_tab();
        net::Url const* const url = tab && tab->current() ? &tab->current()->final_url : nullptr;

        // Tab strip.
        frame.fill_rect(c.tab_strip, t.chrome_background);
        for (std::size_t i = 0; i < c.tabs.size(); ++i) {
            Rect const rect = c.tabs[i];
            bool const is_active = i == active;
            bool const hovered = (hover == Hover::Tab || hover == Hover::TabClose) && hover_index == i;
            Color const background = is_active ? t.tab_active_background
                : hovered                      ? t.tab_hover_background
                                               : t.tab_inactive_background;
            if (!(background == t.chrome_background)) {
                // Only the top corners round: the toolbar paints over the rest.
                frame.fill_round_rect(Rect { rect.x, rect.y, rect.width, rect.height + t.tab_corner_radius },
                    t.tab_corner_radius, background);
            }
            Rect const close = c.tab_close_buttons[i];
            float const text_x = static_cast<float>(rect.x + t.padding + 2);
            float const max_width = static_cast<float>(close.x - t.padding) - text_x;
            std::u32string const title
                = ellipsize(decode_utf8(tab_title(tabs[i])), max_width, t.tab_font_size);
            draw_text(frame, title, text_x, centered_baseline(rect, t.tab_font_size), t.tab_font_size,
                is_active ? t.chrome_text : t.chrome_text_muted);
            bool const close_hover = hover == Hover::TabClose && hover_index == i;
            if (close_hover)
                frame.fill_round_rect(close, close.width / 2, t.button_hover_background);
            draw_glyph_centered(frame, glyph_close, close, t.tab_font_size,
                is_active || close_hover ? t.chrome_text : t.chrome_text_muted);
        }
        if (hover == Hover::NewTab)
            frame.fill_round_rect(c.new_tab_button, t.button_corner_radius, t.button_hover_background);
        draw_glyph_centered(frame, glyph_plus, c.new_tab_button, t.font_size * 1.15f, t.chrome_text);

        // Toolbar.
        frame.fill_rect(c.toolbar, t.tab_active_background);
        frame.fill_rect(Rect { 0, c.toolbar.bottom() - t.border_width, width, t.border_width },
            t.chrome_border);
        paint_button(c.back_button, glyph_back, can_go(-1), hover == Hover::Back);
        paint_button(c.forward_button, glyph_forward, can_go(+1), hover == Hover::Forward);
        paint_button(c.reload_button, glyph_reload, tab && tab->current() != nullptr,
            hover == Hover::Reload);
        paint_button(c.reader_button, glyph_reader, reader_available(), hover == Hover::Reader);

        // Address bar.
        frame.fill_round_rect(c.address, t.address_corner_radius,
            address_focus ? t.accent : t.address_border);
        Rect const inner { c.address.x + t.border_width, c.address.y + t.border_width,
            c.address.width - 2 * t.border_width, c.address.height - 2 * t.border_width };
        frame.fill_round_rect(inner, std::max(0, t.address_corner_radius - t.border_width),
            t.address_background);
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
            Bitmap strip(text_area.width, text_area.height, t.address_background);
            std::u32string const text = decode_utf8(address);
            Rect const local { 0, 0, text_area.width, text_area.height };
            float const baseline = centered_baseline(local, t.font_size);
            if (address_focus && select_all && !text.empty())
                strip.fill_rect(Rect { 0, 2, static_cast<int>(text_width(text, t.font_size) + 0.5f),
                                    text_area.height - 4 },
                    t.selection);
            draw_text(strip, text, 0, baseline, t.font_size, t.address_text);
            if (address_focus) {
                std::size_t const index = decode_utf8(address.substr(0, caret)).size();
                int const caret_x = static_cast<int>(
                    static_cast<float>(index) * text::SashfoldMono::advance(t.font_size) + 0.5f);
                strip.fill_rect(Rect { caret_x, 4, 1, text_area.height - 8 }, t.accent);
            }
            frame.blit(strip, text_area.x, text_area.y);
        }

        // Find bar: the query box and the count of matches.
        if (find_open) {
            frame.fill_rect(c.find_bar, t.chrome_background);
            frame.fill_rect(Rect { 0, c.find_bar.bottom() - t.border_width, width, t.border_width },
                t.chrome_border);
            frame.fill_round_rect(c.find_box, t.address_corner_radius,
                find_focus ? t.accent : t.address_border);
            Rect const box_inner { c.find_box.x + t.border_width, c.find_box.y + t.border_width,
                c.find_box.width - 2 * t.border_width, c.find_box.height - 2 * t.border_width };
            frame.fill_round_rect(box_inner, std::max(0, t.address_corner_radius - t.border_width),
                t.address_background);
            Rect const box_text { box_inner.x + t.padding, box_inner.y,
                std::max(0, box_inner.width - 2 * t.padding), box_inner.height };
            if (!box_text.is_empty()) {
                Bitmap strip(box_text.width, box_text.height, t.address_background);
                std::u32string const query = decode_utf8(find_query);
                Rect const local { 0, 0, box_text.width, box_text.height };
                float const baseline = centered_baseline(local, t.font_size);
                if (find_focus && find_select_all && !query.empty())
                    strip.fill_rect(Rect { 0, 2, static_cast<int>(text_width(query, t.font_size) + 0.5f),
                                        box_text.height - 4 },
                        t.selection);
                draw_text(strip, ellipsize(query, static_cast<float>(box_text.width), t.font_size), 0,
                    baseline, t.font_size, t.address_text);
                if (find_focus) {
                    std::size_t const index = decode_utf8(find_query.substr(0, find_caret)).size();
                    int const caret_x = static_cast<int>(
                        static_cast<float>(index) * text::SashfoldMono::advance(t.font_size) + 0.5f);
                    strip.fill_rect(Rect { caret_x, 4, 1, box_text.height - 8 }, t.accent);
                }
                frame.blit(strip, box_text.x, box_text.y);
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
            paint::paint_page(content, tab->layout, 0, -static_cast<float>(tab->scroll_y), &tab->backgrounds);
            // The find bar's matches, the current one stronger; then the
            // selection over them, all as translucent bands.
            if (find_open) {
                for (std::size_t m = 0; m < tab->matches.size(); ++m) {
                    Match const& match = tab->matches[m];
                    paint_bands(content, *tab, match.start, match.end,
                        m == tab->current_match ? t.find_current : t.find_highlight);
                }
            }
            if (tab->selection) {
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

        // Status bar.
        frame.fill_rect(c.status, t.status_background);
        frame.fill_rect(Rect { 0, c.status.y, width, t.border_width }, t.chrome_border);
        std::u32string const status = ellipsize(decode_utf8(status_text()),
            static_cast<float>(width - 2 * t.padding), t.status_font_size);
        draw_text(frame, status, static_cast<float>(t.padding),
            centered_baseline(c.status, t.status_font_size), t.status_font_size, t.status_text);

        dirty = false;
    }

    bool can_go(int delta) const
    {
        Tab const* const tab = active_tab();
        if (!tab)
            return false;
        auto const target = static_cast<std::ptrdiff_t>(tab->index) + delta;
        return target >= 0 && target < static_cast<std::ptrdiff_t>(tab->history.size());
    }

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

void Browser::set_theme(Theme theme)
{
    m_impl->theme = std::move(theme);
    for (Impl::Tab& tab : m_impl->tabs)
        m_impl->relayout(tab);
    m_impl->refresh_hover();
    m_impl->dirty = true;
}

Theme const& Browser::theme() const { return m_impl->theme; }

void Browser::set_downloads_directory(std::string directory)
{
    m_impl->downloads_directory = std::move(directory);
}

void Browser::resize(int width, int height)
{
    m_impl->width = std::max(width, 1);
    m_impl->height = std::max(height, 1);
    m_impl->frame = Bitmap(m_impl->width, m_impl->height, m_impl->theme.chrome_background);
    for (Impl::Tab& tab : m_impl->tabs) {
        m_impl->restyle(tab);
        m_impl->relayout(tab);
    }
    m_impl->refresh_hover();
    m_impl->dirty = true;
}

int Browser::width() const { return m_impl->width; }
int Browser::height() const { return m_impl->height; }

void Browser::mouse_move(int x, int y) { m_impl->mouse_move(x, y); }
void Browser::mouse_down(int x, int y, int button) { m_impl->mouse_down(x, y, button); }
void Browser::mouse_up(int, int, int button) { m_impl->mouse_up(button); }

void Browser::wheel(int x, int y, int notches)
{
    m_impl->update_hover(x, y);
    if (m_impl->layout_chrome().content.contains(x, y))
        m_impl->scroll_by(-notches * m_impl->theme.scroll_step);
    m_impl->refresh_hover();
}

void Browser::key_down(platform::KeyEvent const& key) { m_impl->key_down(key); }
void Browser::text_input(char32_t code_point) { m_impl->text_input(code_point); }

void Browser::navigate(std::string const& typed) { m_impl->navigate(typed); }
void Browser::open(net::Url const& url) { m_impl->open(url); }
void Browser::open_in_new_tab(net::Url const& url) { m_impl->open_in_new_tab(url); }
void Browser::back() { m_impl->go(-1); }
void Browser::forward() { m_impl->go(+1); }
void Browser::reload() { m_impl->reload(); }
void Browser::new_tab() { m_impl->new_tab(); }
void Browser::close_tab(std::size_t index) { m_impl->close_tab(index); }
void Browser::select_tab(std::size_t index) { m_impl->select_tab(index); }

bool Browser::has_pending_load() const { return !m_impl->pending.empty(); }

bool Browser::tick()
{
    if (m_impl->pending.empty())
        return false;
    Impl::Pending const load = m_impl->pending.front();
    m_impl->pending.erase(m_impl->pending.begin());
    return m_impl->perform(load);
}

Bitmap const& Browser::frame()
{
    if (m_impl->dirty)
        m_impl->paint();
    return m_impl->frame;
}

bool Browser::needs_paint() const { return m_impl->dirty; }

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
    if (tab && tab->document)
        Impl::collect_text(tab->layout.root, text, last_baseline);
    return text;
}

int Browser::scroll_y() const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    return tab ? tab->scroll_y : 0;
}

std::optional<net::Url> Browser::link_at(int x, int y) const { return m_impl->link_at(x, y); }

std::optional<std::pair<int, int>> Browser::find_text(std::string const& text) const
{
    Impl::Tab const* const tab = m_impl->active_tab();
    if (!tab || !tab->document || text.empty())
        return std::nullopt;
    std::optional<Impl::TextHit> const hit
        = m_impl->find_text_in(tab->layout.root, text, *tab, m_impl->layout_chrome());
    if (!hit)
        return std::nullopt;
    return std::make_pair(hit->x, hit->y);
}

ChromeLayout Browser::chrome_layout() const { return m_impl->layout_chrome(); }

bool Browser::focus_control(std::string const& name)
{
    Impl::Tab* const tab = m_impl->active_tab();
    if (!tab || !tab->document)
        return false;
    dom::Element const* const control = control_named(*tab->document, name);
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
    dom::Element const* const control = control_named(*tab->document, name);
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
    // text may span several of its runs.
    std::size_t line_start = 0;
    while (line_start < tab->runs.size()) {
        std::size_t line_end = line_start + 1;
        while (line_end < tab->runs.size()
            && tab->runs[line_end]->baseline_y == tab->runs[line_start]->baseline_y)
            ++line_end;
        std::u32string line;
        std::vector<std::size_t> starts; // each run's offset within `line`
        for (std::size_t i = line_start; i < line_end; ++i) {
            starts.push_back(line.size());
            line += tab->runs[i]->text;
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
            tab->selection = Impl::Selection { locate(at), locate(at + needle.size()) };
            m_impl->blur_address();
            m_impl->dirty = true;
            return true;
        }
        line_start = line_end;
    }
    return false;
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
