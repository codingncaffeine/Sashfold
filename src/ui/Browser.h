#pragma once

// The browser shell, OS-free: tabs, history, the address bar,
// chrome painted through ThemeTokens, the page painted through a scrolled
// viewport, link hit-testing — everything a window needs except the window.
// The platform layer feeds it input and blits the frame it paints; the
// --script replay drives it headlessly the same way, so shell tests run on
// every OS in CI and chrome screenshots are byte-identical across them.
//
// Loads are synchronous and queued: a navigation is performed by tick(), so
// the caller can present the "loading" frame first. The event-loop
// integration of fetch arrives with the multi-process split.

#include "core/Bitmap.h"
#include "net/Csp.h"
#include "net/FetchPool.h"
#include "net/Filters.h"
#include "net/Http.h"
#include "net/Url.h"
#include "platform/Input.h"
#include "ui/Theme.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

// Where documents come from: the shell's loader goes through the fetch
// choke point with the session's cookie jar and cache; tests inject canned
// pages. This is also the seam a renderer-process boundary would cut at.
// Every request names the container its tab is in (empty for the
// default): a container has a cookie jar of its own, so a site in one
// never sees the cookies it set in another.
class Loader {
public:
    virtual ~Loader() = default;
    // `referrer` is already policy-shaped (empty = send none); bypass_cache
    // is a user reload.
    virtual net::FetchResult load(net::Url const& url, std::string const& referrer,
        bool bypass_cache, std::string_view container = {})
        = 0;
    // A resource a page asks for — a stylesheet, a picture, a script, a
    // font — fetched on the page's behalf: the same session, with
    // `first_party` (the page's URL) keeping third-party cookies out,
    // `kind` for the content-blocking rules to judge by, and the page's
    // own `guard` — its Content Security Policy's say on the URL and on
    // every redirect hop, honoured before any request leaves. A loader
    // that serves only documents says so.
    virtual net::FetchResult load_subresource(net::Url const& url, net::Url const& first_party,
        std::string const& referrer, net::ResourceKind kind = net::ResourceKind::Other,
        net::RequestGuard const& guard = {}, std::string_view container = {})
    {
        (void)url;
        (void)first_party;
        (void)referrer;
        (void)kind;
        (void)guard;
        (void)container;
        return { std::nullopt, "this loader serves documents only" };
    }
    // The same resource asked for AHEAD of the page's asking — named by its
    // markup, or chosen for one of its pictures — so that it is on its way,
    // or here, by the time load_subresource is called for it, which then
    // answers with what came. Nothing is waited for; a loader that fetches
    // nothing ahead does nothing, and the page's own asking still judges the
    // request by its guard.
    // The ticket says when it has come, for whoever holds a page back until
    // what its parse will wait for is here; null when nothing was asked for.
    virtual std::shared_ptr<net::FetchTicket> prefetch(net::Url const& url, net::Url const& first_party,
        std::string const& referrer, net::ResourceKind kind, std::string_view container = {})
    {
        (void)url;
        (void)first_party;
        (void)referrer;
        (void)kind;
        (void)container;
        return nullptr;
    }
    // load(), begun on another thread: a ticket to ask after the document
    // by, so that the window is not held still while it comes. Null from a
    // loader that fetches where it is asked — and load() is then the way.
    virtual std::shared_ptr<net::FetchTicket> load_ahead(net::Url const& url, std::string const& referrer,
        bool bypass_cache, std::string_view container = {})
    {
        (void)url;
        (void)referrer;
        (void)bypass_cache;
        (void)container;
        return nullptr;
    }
    // A request a page's script makes — fetch(), XMLHttpRequest — carried
    // out on the page's behalf through the same session: the method, the
    // headers and the body as given, cookies only when the request allows
    // them, a redirect followed only when it asks, the page's guard
    // honoured as above.
    virtual net::FetchResult load_resource(net::Url const& url, net::Url const& first_party,
        std::string const& referrer, net::ResourceRequest const& request,
        net::RequestGuard const& guard = {}, std::string_view container = {})
    {
        (void)url;
        (void)first_party;
        (void)referrer;
        (void)request;
        (void)guard;
        (void)container;
        return { std::nullopt, "this loader serves documents only" };
    }
    // document.cookie: the Cookie header the page would send, and a
    // Set-Cookie line a script wrote. A loader without a jar keeps none.
    virtual std::string cookies_for(net::Url const& url, std::string_view container = {})
    {
        (void)url;
        (void)container;
        return {};
    }
    virtual void set_cookie(net::Url const& url, std::string_view set_cookie_line, std::string_view container = {})
    {
        (void)url;
        (void)set_cookie_line;
        (void)container;
    }
    // How many requests the session's blocklists have refused so far.
    virtual std::size_t blocked_requests() const { return 0; }
    // The session's lists, for their element-hiding rules; null for a
    // loader without any.
    virtual net::Blocklists const* content_lists() const { return nullptr; }
};

struct HistoryEntry {
    net::Url url; // as navigated
    net::Url final_url; // where it landed: the base for relative links
    std::string title;
    std::vector<std::uint8_t> bytes; // the document itself: Back never refetches
    std::string content_type;
    // The document's Content-Security-Policy and -Report-Only header
    // values, kept with the bytes: a policy travels with its page through
    // Back and Forward.
    std::vector<std::string> csp_headers;
    std::vector<std::string> csp_report_only_headers;
    int status = 0;
    bool from_cache = false;
    bool internal = false; // an error page or about: page generated by the shell
    std::string error; // the loader's error, when internal
    int scroll_y = 0;
    // An entry a session restored: its title and scroll position are
    // known, its page is not — it is fetched again the first time the
    // entry is shown, and this replaces it.
    bool unloaded = false;
    // An entry the page's script pushed (history.pushState). While the
    // reader stays it is the document it was pushed from, under the address
    // the script gave it, and its bytes are that document's. Come back to by
    // Back or Forward, the script's state is gone: its own address is
    // fetched, as a restored entry's is.
    bool pushed = false;
};

// One open menu as the chrome lays it out: its box, and a row for every
// item in order, a separator's thin row included.
struct MenuBox {
    Rect box;
    std::vector<Rect> rows;
};

struct ChromeLayout {
    Rect tab_strip;
    Rect toolbar;
    Rect content;
    Rect status;
    Rect back_button;
    Rect forward_button;
    Rect reload_button;
    Rect reader_button;
    Rect star_button; // bookmark the page in front, before the reader's button
    // The bookmarks bar under the toolbar — empty while it is not shown —
    // with the bookmarks of it that fit, left to right, by their ids, and
    // the chevrons at its right end when some did not.
    Rect bookmarks_bar;
    std::vector<Rect> bookmark_items;
    std::vector<std::uint64_t> bookmark_ids;
    Rect bookmarks_overflow;
    Rect menu_button; // the main menu, at the toolbar's right end
    Rect new_tab_button;
    Rect address;
    Rect find_bar; // empty unless the find bar is open
    Rect find_box;
    Rect devtools; // empty unless the devtools panel is open
    Rect devtools_tree;
    Rect devtools_styles;
    Rect palette; // empty unless the command palette is open
    Rect palette_box; // its query box
    std::vector<Rect> palette_rows; // the commands shown, top to bottom
    std::vector<MenuBox> menus; // the open menu, then each submenu open from it; empty when none is
    std::vector<Rect> tabs;
    std::vector<Rect> tab_close_buttons;
    // The window's own controls at the tab strip's right end, drawn only
    // where the compositor leaves the frame to the client.
    bool window_controls = false;
    Rect minimize_button;
    Rect maximize_button;
    Rect close_button;
};

// A moment of local time, as the new-tab page shows it.
struct WallTime {
    int year = 1970;
    int month = 1; // 1 to 12
    int day = 1; // 1 to 31
    int weekday = 4; // 0 is Sunday
    int hour = 0;
    int minute = 0;
    int second = 0;
};

// What the shell has done for its pages since it started, for the
// instruments: counts and milliseconds, read by --bench and by a script's
// assertions and never reset — a script marks a moment and reads the
// difference. A restyle is the page's styles resolved whole, a relayout
// the page laid out whole, however either was asked for; a paint is the
// window's frame drawn again, its area in device pixels. The milliseconds
// include what a phase fetched: the sheets and the pictures come through
// the loader inside their phase.
struct Profile {
    std::uint64_t restyles = 0;
    std::uint64_t relayouts = 0;
    std::uint64_t paints = 0;
    // The frames for which the header alone was drawn again — a theme's
    // picture moved and nothing else had changed — and the page was not.
    std::uint64_t header_paints = 0;
    std::uint64_t painted_pixels = 0;
    double sheets_ms = 0; // stylesheets and fonts collected
    double images_ms = 0; // pictures collected and decoded
    double restyle_ms = 0;
    double relayout_ms = 0; // the page's own layout
    double frames_ms = 0; // its frames' documents laid out and drawn
    double paint_ms = 0;
    double last_paint_ms = 0; // the most recent frame alone
};

class Browser {
public:
    Browser(Loader& loader, Theme theme, int width, int height);
    ~Browser();
    Browser(Browser const&) = delete;
    Browser& operator=(Browser const&) = delete;

    void set_theme(Theme theme);
    Theme const& theme() const;
    // What stood in the way of the pictures the theme in use names — a file
    // that could not be read, or is no picture in a format read here — each
    // by the token that names it. Such a picture is left out and the rest of
    // the theme stands.
    std::vector<std::string> const& theme_problems() const;
    // Where downloads land. Empty disables downloading: a response the
    // engine cannot render then shows the unsupported-content page instead.
    void set_downloads_directory(std::string directory);
    // The ceiling on each page's script heap, in bytes (0: none). A page
    // whose scripts hold more has them stopped and its console says so; the
    // page stays as it is, and every other tab goes on. For the pages opened
    // from now on.
    void set_js_heap_limit(std::size_t bytes);
    // The reader's own themes folder. With one set, a download that is a
    // Firefox or Chrome theme (an .xpi, a .crx) is converted into it, offered
    // among the themes from then on, and put on at once; the file itself is
    // saved like any download. Empty (as unsaid): a download is a download.
    void set_user_themes_directory(std::string directory);
    // Where about:themes reads Firefox's themes from: the add-ons site's
    // search endpoint, unless a harness names a file of the same shape.
    void set_theme_gallery_source(std::string address);
    void resize(int width, int height);
    int width() const;
    int height() const;
    // The display's device px per CSS px. The window's sizes and every
    // coordinate the shell takes are device px; the chrome draws from the
    // theme scaled by this and pages lay out with it. 1 until the window
    // says otherwise.
    void set_scale(float scale);
    float scale() const;

    // --- Input, window coordinates ------------------------------------------
    void mouse_move(int x, int y);
    // 1 left, 2 middle, 3 right; with the keys held as it went down — Ctrl
    // with a click on a link opens it in a new tab, Shift with a right
    // click shows the shell's menu whatever the page says.
    void mouse_down(int x, int y, int button, platform::Modifiers modifiers = {});
    void mouse_up(int x, int y, int button);
    void wheel(int x, int y, int notches); // positive scrolls the content up
    // The content under (x, y) moves by dx, dy device px: a finger's drag,
    // a touchpad's swipe. Positive dy brings what is below into view.
    void scroll_pixels(int x, int y, int dx, int dy);
    void key_down(platform::KeyEvent const& key);
    void text_input(char32_t code_point);
    // An input method's composing text (UTF-8): shown at the caret of the
    // focused field, underlined, until text arrives or focus moves; empty
    // clears it.
    void preedit(std::string const& text);
    // The caret's box in the focused field, window coordinates, for an
    // input method to compose beside; nothing when no field has focus.
    std::optional<Rect> text_input_area() const;

    // --- The window's frame, where the shell draws it ---------------------
    // On a display whose compositor draws no title bar (GNOME's, say) the
    // shell carries the window's controls itself: minimize, maximize and
    // close at the tab strip's right end, the empty tab strip as the
    // handle that moves the window, and a band along the edges that
    // resizes it. What the reader asks of the window comes out here for
    // the window to do, one request at a time.
    enum class WindowRequest {
        None,
        Move,
        Minimize,
        ToggleMaximize,
        Close,
        ResizeTop,
        ResizeBottom,
        ResizeLeft,
        ResizeRight,
        ResizeTopLeft,
        ResizeTopRight,
        ResizeBottomLeft,
        ResizeBottomRight,
    };
    void set_window_controls(bool shown);
    bool window_controls() const;
    WindowRequest take_window_request();
    // Whether this window is the one in front, as the system says: a theme
    // may give the frame of one that is not a color of its own
    // (chrome-background-inactive). In front until told otherwise.
    void set_window_active(bool active);
    bool window_active() const;
    // Whether any of the window can be seen, as far as the system says —
    // not minimized, not wholly covered, not on another desktop. What only
    // moves to be looked at, a theme's pictures, stops while it cannot.
    void set_window_visible(bool visible);
    bool window_visible() const;

    // --- Bookmarks ----------------------------------------------------------
    // The reader's bookmarks as the profile keeps them (bookmarks.json), and
    // a number that moves with every change to them: what the window's loop
    // saves by. restore_bookmarks is false, nothing changed, for a text that
    // is not that file.
    std::string bookmarks_json() const;
    bool restore_bookmarks(std::string_view json);
    std::uint64_t bookmarks_changes() const;
    // Ctrl+D and the star: the page in front goes onto the bar, or, already
    // bookmarked, has its bookmark taken away.
    void bookmark_page();
    // What a bookmarks file of another browser holds, added: how many came.
    std::size_t import_bookmarks_html(std::string_view html);
    // For the page that manages them: the folder the dated copies of the
    // bookmarks are kept in (the profile's), which it lists and puts back
    // from; and the home folder other browsers' own bookmarks, and bookmarks
    // files, are looked for under when the reader opens the page's import
    // view. Unset, as in a script, nothing of the reader's is looked at.
    void set_bookmark_backups_directory(std::string directory);
    void set_bookmark_sources_home(std::string home);
    // The bar as it is shown now: each title in order, a folder's in [ ],
    // then » when some did not fit; empty while the bar is not shown.
    std::vector<std::string> bookmarks_bar_titles() const;

    // --- Navigation ---------------------------------------------------------
    // Address-bar semantics: a URL, or a bare host that tries https first.
    void navigate(std::string const& typed);
    void open(net::Url const& url);
    void open_in_new_tab(net::Url const& url);
    void back();
    void forward();
    void reload();
    void new_tab();
    void close_tab(std::size_t index);
    void select_tab(std::size_t index);
    // The tab closed last comes back where it stood, with its history; a
    // copy of a tab opens beside it. Neither fetches anything.
    void reopen_closed_tab();
    void duplicate_tab(std::size_t index);

    // Whether a load is under way: queued, on its way from the network, or
    // owing pictures. tick() until it is not is how a caller without a
    // window loads a page.
    bool has_pending_load() const;
    // Whether there is a step of loading to be done NOW, without waiting —
    // what the window's loop asks, so that it sleeps on its events while a
    // fetch is on its way rather than turning over and over.
    bool load_ready() const;
    // Performs one step of loading; true while there is loading under way.
    bool tick();
    // Whether the tab in front has a load on its way that has not reached
    // it yet: it still shows what it showed.
    bool navigating() const;

    // --- Containers ---------------------------------------------------------
    // A container keeps a set of tabs apart from the rest: its own cookie
    // jar (the loader's) and its own web storage, so a site opened in one
    // never sees what it set in another. The default container has no
    // name. The shell offers the containers it was given, in order; a tab
    // shows its container's colour along its top edge.
    struct Container {
        std::string name;
        Color color;
    };
    void set_containers(std::vector<Container> containers);
    std::vector<Container> const& containers() const;
    // A new tab in the container named (the default when the name is
    // empty); Ctrl+Shift+N opens one in the container after the active
    // tab's, round to the default.
    void new_tab_in(std::string const& container);
    std::string const& active_container() const;

    // --- The command palette ------------------------------------------------
    // Ctrl+Shift+P: every command the shell has — the tabs to switch to,
    // the containers to open a tab in, the themes to put on — in one
    // list over the page, narrowed word by word as the reader types, the
    // arrow keys moving along it, Enter running the highlighted one and
    // Escape closing it with nothing done.
    void open_palette(std::string const& query = {});
    bool palette_open() const;
    // The highlighted command's label; "" when the palette is closed or
    // nothing matches what was typed.
    std::string palette_selection() const;
    // The themes the palette offers, each by name and file: choosing one
    // puts it on at once and leaves its file for the host to take, so the
    // window can keep it for the next start.
    struct ThemePreset {
        std::string name;
        std::string path;
    };
    void set_theme_presets(std::vector<ThemePreset> presets);
    std::optional<std::string> take_theme_request();

    // --- Menus --------------------------------------------------------------
    // A right click, the Menu key or Shift+F10 opens a menu for what is
    // under the pointer — a link, a picture, a selection, a field, the
    // page; a tab; the address bar — and the button at the toolbar's right
    // end opens the window's main menu. The page hears `contextmenu`
    // first and may keep the shell's menu away; Shift with the click shows
    // it regardless. The arrow keys move along a menu and into a submenu,
    // Enter runs the highlighted item, Escape closes the innermost menu,
    // and a press anywhere else closes them all and does nothing more.
    bool menu_open() const;
    // The innermost open menu's items in order, joined by " | ": a
    // separator as "-", an item that cannot be chosen in parentheses, a
    // checked one after "*", one that opens a submenu before ">". Empty
    // when no menu is open.
    std::string menu_text() const;
    // Chooses the first item with this label that can be chosen, innermost
    // menu first: runs it, or opens its submenu. False when there is none.
    bool choose_menu_item(std::string const& label);
    void open_main_menu();

    // --- Storage ------------------------------------------------------------
    // Every page's localStorage — an area per container and origin, kept
    // by the shell across the pages that share it — as JSON, the way the
    // profile keeps it between runs, and read back; and a count that moves
    // with every change a page makes, so a host writes it when it has.
    std::string storage_json() const;
    bool restore_storage(std::string_view json);
    std::uint64_t storage_changes() const;

    // --- Sessions -----------------------------------------------------------
    // The open tabs — each history entry's URL, title and scroll position,
    // which entry each tab is on, and which tab is active — as JSON, the
    // way the shell keeps a session between runs. Deterministic: the same
    // session serializes the same, so a host writes it when it changes.
    std::string session_json() const;
    // Opens the tabs a session_json() described in place of the current
    // ones. Every entry comes back with its title and scroll position; a
    // page is fetched again the first time its tab is shown (the active
    // tab's at once, queued like a navigation), never all at start. False
    // when the text is not a session, and nothing changes then.
    bool restore_session(std::string_view json);

    // --- Scripts ------------------------------------------------------------
    // Runs every timer due on any tab's page and brings the active tab's
    // styles and layout up to date with what its scripts changed; true
    // when something ran. The window loop and the replay call it after
    // input, and again when next_timer_ms has elapsed.
    bool run_scripts();
    // Milliseconds until the earliest pending timer of any tab; nullopt
    // when none is pending.
    std::optional<double> next_timer_ms() const;
    // The clock the pages' timers run on, in ms; wall time unless set. The
    // replay gives a virtual one so a timer fires when the script says.
    void set_clock(std::function<double()> now);
    // The local time the new-tab page opens on; the OS's unless set. The
    // replay gives a fixed one so the page is the same on every machine.
    void set_wall_clock(std::function<WallTime()> now);
    // The active page's console output, one line per call, oldest first.
    std::string console_text() const;

    // --- Output -------------------------------------------------------------
    Bitmap const& frame(); // paints when something changed
    bool needs_paint() const;
    Profile const& profile() const; // what the shell has done so far, counted and timed
    platform::Cursor cursor() const;
    std::string window_title() const;

    // --- Inspection (tests and --script) ------------------------------------
    std::size_t tab_count() const;
    std::size_t active_tab() const;
    std::string tab_title(std::size_t index) const; // as the strip shows it, before any cutting
    std::size_t closed_tab_count() const; // how many Ctrl+Shift+T could bring back
    bool tab_pinned(std::size_t index) const; // an icon alone, among the strip's first
    net::Url const* current_url() const;
    HistoryEntry const* current_entry() const;
    bool can_go_back() const;
    bool can_go_forward() const;
    std::string const& address_text() const;
    bool address_focused() const;
    std::string status_text() const;
    std::string page_title() const;
    std::string page_text() const; // the laid-out text, runs joined by spaces
    std::size_t blocked_requests() const; // refused by the loader's blocklists, this session
    std::size_t pictures() const; // the active page's pictures decoded so far
    int scroll_y() const;
    // How far the innermost box that scrolls under a window point has had
    // its content moved, in CSS px; zero when the point is in no such box.
    std::pair<int, int> box_scroll_at(int x, int y) const;
    std::optional<net::Url> link_at(int x, int y) const;
    // The window-coordinate center of the first text run containing `text`,
    // or, with `nth`, of the one that many such runs after it.
    std::optional<std::pair<int, int>> find_text(std::string const& text, std::size_t nth = 0) const;
    ChromeLayout chrome_layout() const;

    // --- Forms (tests and --script) -----------------------------------------
    // Focuses the first control with this name; false when there is none.
    bool focus_control(std::string const& name);
    // The current value of the first control with this name.
    std::optional<std::string> control_value(std::string const& name) const;
    // The focused control's name, or "" when focus is elsewhere.
    std::string focused_control_name() const;

    // --- Selection (tests and --script) -------------------------------------
    // The selected page text, lines separated by newlines; "" when none.
    std::string selected_text() const;
    // Selects the first occurrence of `text` within one run; false when none.
    bool select_text(std::string const& text);

    // --- Find in page (tests and --script) ----------------------------------
    // "3 of 12", "No matches", or "" when the find bar is closed or empty.
    std::string find_status() const;
    // Shows the current page in reader mode, or leaves it.
    void toggle_reader();
    // Keyboard link-hints: how many labels are showing (0 when they are not).
    std::size_t hint_count() const;
    // Devtools: inspects the element of the first run containing text; the inspected
    // element as "tag#id.class" ("" when none).
    bool inspect_text(std::string const& text);
    std::string inspected_summary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
