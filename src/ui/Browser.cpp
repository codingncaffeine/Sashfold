#include "ui/Browser.h"
#include "ui/Forms.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "ui/PageImages.h"
#include "ui/Downloads.h"
#include "ui/InternalPages.h"

#include <algorithm>
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
        || scheme == "view-source";
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
    struct Tab {
        std::vector<HistoryEntry> history;
        std::size_t index = 0;
        std::unique_ptr<dom::Document> document;
        std::vector<css::SheetSource> sheets; // the page's stylesheets, kept so a resize can restyle
        std::optional<css::StyleSet> style_set; // the sheets compiled for style_media
        css::MediaContext style_media;
        css::StyleMap styles;
        layout::ImageMap images; // the page's pictures, decoded
        layout::ControlStates controls; // what the user typed and toggled in the page's forms
        layout::LayoutResult layout;
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

    enum class Hover { None, Back, Forward, Reload, NewTab, Tab, TabClose, Address, Content };

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
        int const content_top = t.tab_strip_height + t.toolbar_height;
        c.status = Rect { 0, height - t.status_height, width, t.status_height };
        c.content = Rect { 0, content_top, width,
            std::max(0, height - content_top - t.status_height) };

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
        c.address = Rect { address_x, c.toolbar.y + (t.toolbar_height - t.address_height) / 2,
            std::max(0, width - address_x - t.padding), t.address_height };
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
        css::MediaContext const media = media_context();
        if (!tab.style_set || tab.style_media.width != media.width
            || tab.style_media.height != media.height) {
            tab.style_set.emplace(tab.sheets, media);
            tab.style_media = media;
        }
        tab.styles = css::resolve_styles(*tab.document, *tab.style_set);
    }

    void relayout(Tab& tab)
    {
        if (!tab.document)
            return;
        ChromeLayout const c = layout_chrome();
        tab.layout = layout::layout_document(*tab.document, tab.styles,
            static_cast<float>(std::max(1, c.content.width)), &tab.images, &tab.controls);
        tab.scroll_y = std::clamp(tab.scroll_y, 0, max_scroll(tab));
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
        tab.style_set.reset();
        restyle(tab);
        auto const fetch_image = [&](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
            net::FetchResult result = loader.load_subresource(url, page_url, referrer_for(&page_url, url));
            if (!result.response || result.response->status != 200)
                return std::nullopt;
            return std::move(result.response->body);
        };
        tab.images = collect_images(*tab.document, &page_url, fetch_image, media_context());
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
            else if (c.address.contains(x, y))
                next = Hover::Address;
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
            case Hover::Address: focus_address(true); break;
            case Hover::Content:
                blur_address();
                if (dom::Element const* const control = control_at(x, y)) {
                    activate_control(*control);
                } else {
                    blur_control();
                    if (hover_link)
                        open(*hover_link);
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
                }
                return;
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
        if (address_focus) {
            edit_address(key);
            return;
        }
        if (edit_control(key))
            return;
        Tab* const tab = active_tab();
        if (!tab)
            return;
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
        if (code_point < 0x20 || code_point == 0x7F)
            return;
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

        // Content.
        frame.fill_rect(c.content, t.content_background);
        if (tab && tab->document && !c.content.is_empty()) {
            Bitmap content(c.content.width, c.content.height, t.content_background);
            paint::paint_page(content, tab->layout, 0, -static_cast<float>(tab->scroll_y));
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

void Browser::mouse_move(int x, int y) { m_impl->update_hover(x, y); }
void Browser::mouse_down(int x, int y, int button) { m_impl->mouse_down(x, y, button); }
void Browser::mouse_up(int, int, int) { }

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

}
