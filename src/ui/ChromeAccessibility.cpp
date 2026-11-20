#include "ui/ChromeAccessibility.h"

#include "ui/Browser.h"

#include <algorithm>

namespace sashfold::ui {

namespace {

using a11y::NodeData;
using a11y::NodeId;
using a11y::Role;
using a11y::State;

constexpr a11y::States bit(State s) { return a11y::state_bit(s); }

// UTF-16 code units in the first `bytes` bytes of UTF-8 text: the model's
// offsets.
std::uint32_t utf16_of_bytes(std::string_view text, std::size_t bytes)
{
    std::uint32_t units = 0;
    for (std::size_t i = 0; i < std::min(bytes, text.size()); ++i) {
        auto const byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xc0) == 0x80)
            continue;
        units += byte >= 0xf0 ? 2 : 1;
    }
    return units;
}

void click(Browser& browser, Rect const& rect)
{
    int const x = rect.x + rect.width / 2;
    int const y = rect.y + rect.height / 2;
    browser.mouse_move(x, y);
    browser.mouse_down(x, y, 1);
    browser.mouse_up(x, y, 1);
}

std::vector<std::string> split_items(std::string const& text)
{
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= text.size() && !text.empty()) {
        std::size_t const end = text.find(" | ", start);
        items.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 3;
    }
    return items;
}

}

NodeId ChromeAccessibility::id_for(std::string const& key)
{
    auto const [at, made] = m_ids.try_emplace(key, m_next);
    if (made)
        ++m_next;
    return at->second;
}

platform::WindowState ChromeAccessibility::build(Browser& browser, bool window_active)
{
    ChromeLayout const c = browser.chrome_layout();
    auto snapshot = std::make_shared<a11y::Snapshot>();
    m_actions.clear();
    std::string signature;
    auto const add = [&](std::string const& key, NodeId parent, Role role, std::string name, Rect const& rect,
                         std::function<void(Browser&, platform::WindowAction const&)> act = {}, a11y::States states = 0,
                         std::string action_name = "press") -> NodeData& {
        auto node = std::make_shared<NodeData>();
        node->id = id_for(key);
        node->parent = parent;
        node->role = role;
        node->name = std::move(name);
        node->states = states;
        node->bounds = a11y::Rect { static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
            static_cast<float>(rect.height) };
        if (act) {
            node->actions.push_back(std::move(action_name));
            m_actions[node->id] = std::move(act);
        }
        if (parent != 0) {
            auto const found = snapshot->nodes.find(parent);
            if (found != snapshot->nodes.end())
                const_cast<NodeData&>(*found->second).children.push_back(node->id);
        }
        NodeData& made = *node;
        snapshot->nodes.emplace(node->id, std::move(node));
        return made;
    };
    auto const press_at = [](Rect rect) { return [rect](Browser& b, platform::WindowAction const&) { click(b, rect); }; };

    // The frame.
    NodeId const frame = id_for("frame");
    snapshot->root = frame;
    add("frame", 0, Role::Window, browser.window_title(), Rect { 0, 0, browser.width(), browser.height() });

    // The tab strip.
    NodeId const strip = add("tabs", frame, Role::TabList, "Tabs", c.tab_strip).id;
    std::size_t const tabs = browser.tab_count();
    for (std::size_t i = 0; i < tabs && i < c.tabs.size(); ++i) {
        bool const active = i == browser.active_tab();
        NodeData& tab = add("tab:" + std::to_string(i), strip, Role::Tab, browser.tab_title(i), c.tabs[i],
            [i](Browser& b, platform::WindowAction const&) { b.select_tab(i); },
            bit(State::Selectable) | (active ? bit(State::Selected) : 0), "select");
        tab.pos_in_set = static_cast<int>(i + 1);
        tab.set_size = static_cast<int>(tabs);
        if (i < c.tab_close_buttons.size() && !browser.tab_pinned(i) && c.tab_close_buttons[i].width > 0)
            add("tab-close:" + std::to_string(i), tab.id, Role::Button, "Close tab", c.tab_close_buttons[i],
                [i](Browser& b, platform::WindowAction const&) { b.close_tab(i); });
    }
    add("new-tab", strip, Role::Button, "New tab", c.new_tab_button, [](Browser& b, platform::WindowAction const&) { b.new_tab(); })
        .key_shortcuts = "Control+T";
    if (c.window_controls) {
        add("minimize", frame, Role::Button, "Minimize", c.minimize_button, press_at(c.minimize_button));
        add("maximize", frame, Role::Button, "Maximize", c.maximize_button, press_at(c.maximize_button));
        add("close", frame, Role::Button, "Close", c.close_button, press_at(c.close_button));
    }

    // The toolbar.
    NodeId const toolbar = add("toolbar", frame, Role::Toolbar, "Navigation", c.toolbar).id;
    add("back", toolbar, Role::Button, "Back", c.back_button, [](Browser& b, platform::WindowAction const&) { b.back(); },
        browser.can_go_back() ? 0 : bit(State::Disabled))
        .key_shortcuts
        = "Alt+ArrowLeft";
    add("forward", toolbar, Role::Button, "Forward", c.forward_button, [](Browser& b, platform::WindowAction const&) { b.forward(); },
        browser.can_go_forward() ? 0 : bit(State::Disabled))
        .key_shortcuts
        = "Alt+ArrowRight";
    add("reload", toolbar, Role::Button, "Reload", c.reload_button, [](Browser& b, platform::WindowAction const&) { b.reload(); })
        .key_shortcuts = "F5";
    {
        std::string const& text = browser.address_text();
        auto const [from, to] = browser.address_selection();
        NodeData& address = add("address", toolbar, Role::TextBox, "Address and search bar", c.address,
            [rect = c.address](Browser& b, platform::WindowAction const& action) {
                // Focus is a click into the bar, which selects what it holds.
                if (!b.address_focused())
                    click(b, rect);
                if (action.action == "set-text") {
                    platform::KeyEvent all;
                    all.key = platform::Key::Letter;
                    all.letter = 'A';
                    all.ctrl = true;
                    b.key_down(all);
                    b.commit_text(action.text);
                } else if (action.action == "set-caret" || action.action == "set-selection") {
                    platform::KeyEvent home;
                    home.key = platform::Key::Home;
                    b.key_down(home);
                    std::string const& now = b.address_text();
                    std::size_t const steps = std::min<std::size_t>(action.start, now.size());
                    for (std::size_t i = 0; i < steps; ++i) {
                        platform::KeyEvent right;
                        right.key = platform::Key::Right;
                        b.key_down(right);
                    }
                    for (std::size_t i = action.start; i < action.end && i < now.size(); ++i) {
                        platform::KeyEvent extend;
                        extend.key = platform::Key::Right;
                        extend.shift = true;
                        b.key_down(extend);
                    }
                }
            },
            bit(State::Focusable) | bit(State::Editable) | (browser.address_focused() ? bit(State::Focused) : 0), "focus");
        address.text = text;
        address.value_text = text;
        address.key_shortcuts = "Control+L";
        address.autocomplete = "list";
        address.caret = utf16_of_bytes(text, to);
        address.text_selection = std::make_pair(utf16_of_bytes(text, from), utf16_of_bytes(text, to));
    }
    add("reader", toolbar, Role::Button, "Reader view", c.reader_button, [](Browser& b, platform::WindowAction const&) { b.toggle_reader(); });
    add("star", toolbar, Role::Button, "Bookmark this page", c.star_button, [](Browser& b, platform::WindowAction const&) { b.bookmark_page(); })
        .key_shortcuts = "Control+D";
    {
        NodeData& menu = add("menu-button", toolbar, Role::Button, "Main menu", c.menu_button,
            [](Browser& b, platform::WindowAction const&) { b.open_main_menu(); },
            bit(State::HasPopup) | bit(State::Expandable) | (browser.menu_open() ? bit(State::Expanded) : 0));
        menu.has_popup = "menu";
    }

    // The bars that are open.
    if (c.permission_bar.width > 0) {
        std::string const site = browser.notification_question();
        NodeId const bar = add("permission", frame, Role::Alert, site + " wants to show notifications", c.permission_bar).id;
        add("permission-allow", bar, Role::Button, "Allow", c.permission_allow,
            [](Browser& b, platform::WindowAction const&) { b.answer_notification_question(true); });
        add("permission-block", bar, Role::Button, "Block", c.permission_block,
            [](Browser& b, platform::WindowAction const&) { b.answer_notification_question(false); });
        add("permission-close", bar, Role::Button, "Not now", c.permission_close,
            [](Browser& b, platform::WindowAction const&) { b.answer_notification_question(std::nullopt); });
    }
    if (c.find_bar.width > 0) {
        NodeId const bar = add("find", frame, Role::Toolbar, "Find in page", c.find_bar).id;
        add("find-box", bar, Role::SearchBox, "Find", c.find_box, press_at(c.find_box), bit(State::Focusable) | bit(State::Editable), "focus");
        if (std::string const status = browser.find_status(); !status.empty()) {
            NodeData& said = add("find-status", bar, Role::Status, status, c.find_bar);
            said.live = "polite";
        }
    }
    if (c.bookmarks_bar.width > 0) {
        NodeId const bar = add("bookmarks", frame, Role::Toolbar, "Bookmarks", c.bookmarks_bar).id;
        std::vector<std::string> const titles = browser.bookmarks_bar_titles();
        for (std::size_t i = 0; i < c.bookmark_items.size() && i < titles.size(); ++i) {
            std::string title = titles[i];
            bool const folder = title.size() >= 2 && title.front() == '[' && title.back() == ']';
            if (folder)
                title = title.substr(1, title.size() - 2);
            NodeData& item = add("bookmark:" + std::to_string(i < c.bookmark_ids.size() ? c.bookmark_ids[i] : i), bar,
                folder ? Role::Button : Role::Link, title, c.bookmark_items[i], press_at(c.bookmark_items[i]),
                folder ? bit(State::HasPopup) : a11y::States { 0 }, folder ? "press" : "jump");
            if (folder)
                item.has_popup = "menu";
        }
        if (c.bookmarks_overflow.width > 0)
            add("bookmarks-more", bar, Role::Button, "More bookmarks", c.bookmarks_overflow, press_at(c.bookmarks_overflow),
                bit(State::HasPopup))
                .has_popup
                = "menu";
    }

    // The content area: the documents go under it.
    NodeId const content = add("content", frame, Role::Group, std::string(), c.content).id;

    // The menus that are open, the innermost with its items.
    if (!c.menus.empty()) {
        std::vector<std::string> const items = split_items(browser.menu_text());
        NodeId parent = frame;
        for (std::size_t level = 0; level < c.menus.size(); ++level) {
            NodeId const menu = add("menu:" + std::to_string(level), parent, Role::Menu, std::string(), c.menus[level].box).id;
            parent = menu;
            if (level + 1 != c.menus.size())
                continue;
            for (std::size_t i = 0; i < items.size() && i < c.menus[level].rows.size(); ++i) {
                std::string label = items[i];
                std::string const key = "menu:" + std::to_string(level) + ":" + std::to_string(i);
                if (label == "-") {
                    add(key, menu, Role::Separator, std::string(), c.menus[level].rows[i]);
                    continue;
                }
                // Browser::menu_text: "(label)" disabled, "*" ahead of a
                // checked one, " >" after one with a submenu.
                bool const disabled = label.size() >= 2 && label.front() == '(' && label.back() == ')';
                if (disabled)
                    label = label.substr(1, label.size() - 2);
                bool const checked = label.starts_with('*');
                if (checked)
                    label.erase(0, 1);
                bool const submenu = label.ends_with(" >");
                if (submenu)
                    label.erase(label.size() - 2);
                a11y::States states = disabled ? bit(State::Disabled) : 0;
                if (checked)
                    states |= bit(State::Checkable) | bit(State::Checked);
                if (submenu)
                    states |= bit(State::HasPopup) | bit(State::Expandable);
                NodeData& item = add(key, menu, checked ? Role::MenuItemCheckbox : Role::MenuItem, label, c.menus[level].rows[i],
                    disabled ? std::function<void(Browser&, platform::WindowAction const&)>()
                             : [label](Browser& b, platform::WindowAction const&) { b.choose_menu_item(label); },
                    states, "click");
                if (submenu)
                    item.has_popup = "menu";
            }
        }
    }
    // The command palette.
    if (c.palette.width > 0) {
        NodeId const dialog = add("palette", frame, Role::Dialog, "Command palette", c.palette).id;
        add("palette-box", dialog, Role::Combobox, "Command", c.palette_box, press_at(c.palette_box),
            bit(State::Focusable) | bit(State::Focused) | bit(State::Editable) | bit(State::Expanded) | bit(State::Expandable), "focus");
        NodeId const list = add("palette-list", dialog, Role::ListBox, "Commands", c.palette).id;
        std::string const highlighted = browser.palette_selection();
        for (std::size_t i = 0; i < c.palette_rows.size(); ++i) {
            // Only the highlighted command's label is known to this side:
            // it names the first row, which the highlight sits on.
            add("palette-row:" + std::to_string(i), list, Role::Option, i == 0 ? highlighted : std::string(), c.palette_rows[i],
                press_at(c.palette_rows[i]), bit(State::Selectable) | (i == 0 ? bit(State::Selected) : 0), "select");
        }
    }
    // The window's own file chooser.
    if (std::optional<std::string> const shows = browser.builtin_chooser_shows()) {
        std::vector<std::string> rows = split_items(*shows);
        std::string const folder = rows.empty() ? std::string() : rows.front();
        NodeId const dialog = add("chooser", frame, Role::Dialog, "Open file: " + folder, c.content, {}, bit(State::Modal)).id;
        NodeId const list = add("chooser-list", dialog, Role::ListBox, folder, c.content).id;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            std::string name = rows[i];
            bool const chosen = name.size() >= 2 && name.front() == '[' && name.back() == ']';
            if (chosen)
                name = name.substr(1, name.size() - 2);
            add("chooser-row:" + std::to_string(i), list, Role::Option, name, c.content, {},
                bit(State::Selectable) | (chosen ? bit(State::Selected) : 0));
        }
    }
    if (std::string const status = browser.status_text(); !status.empty() && c.status.width > 0 && c.status.height > 0) {
        NodeData& said = add("status", frame, Role::Status, status, c.status);
        said.live = "polite";
    }

    // What changed since the last build: the events the chrome says itself.
    platform::WindowState state;
    signature = (window_active ? "active;" : "inactive;") + std::to_string(c.content.x) + "," + std::to_string(c.content.y) + ";";
    for (auto const& [id, node] : snapshot->nodes) {
        signature += std::to_string(id) + node->name + node->text + std::to_string(node->states) + std::to_string(node->children.size())
            + std::to_string(node->bounds ? node->bounds->x + node->bounds->y * 7 + node->bounds->width * 13 : 0)
            + (node->caret ? std::to_string(*node->caret) : std::string()) + ";";
        if (!m_last)
            continue;
        NodeData const* const old = m_last->find(id);
        if (!old)
            continue;
        if (old->name != node->name)
            state.events.push_back({ a11y::Event::Type::NameChanged, id });
        for (State const s : { State::Focused, State::Selected, State::Checked, State::Expanded, State::Disabled }) {
            bool const was = (old->states & bit(s)) != 0;
            bool const is = (node->states & bit(s)) != 0;
            if (was == is)
                continue;
            a11y::Event event { a11y::Event::Type::StateChanged, id };
            event.state = s;
            event.on = is;
            state.events.push_back(event);
            if (s == State::Focused && is)
                state.events.push_back({ a11y::Event::Type::Focus, id });
            if (s == State::Selected && node->parent != 0)
                state.events.push_back({ a11y::Event::Type::SelectionChanged, node->parent });
        }
        if (old->text != node->text)
            state.events.push_back({ a11y::Event::Type::TextInserted, id });
        if (old->caret != node->caret && node->caret && (node->states & bit(State::Focused)) != 0) {
            a11y::Event moved { a11y::Event::Type::CaretMoved, id };
            moved.offset = *node->caret;
            state.events.push_back(moved);
        }
    }
    m_signature = std::move(signature);
    m_last = snapshot;
    state.chrome = std::move(snapshot);
    state.content = content;
    state.title = browser.window_title();
    state.active = window_active;
    state.width = browser.width();
    state.height = browser.height();
    state.content_x = static_cast<float>(c.content.x);
    state.content_y = static_cast<float>(c.content.y);
    state.content_width = static_cast<float>(c.content.width);
    state.content_height = static_cast<float>(c.content.height);
    return state;
}

void ChromeAccessibility::perform(Browser& browser, platform::WindowAction const& action)
{
    auto const found = m_actions.find(action.node);
    if (found != m_actions.end())
        found->second(browser, action);
}

void ChromeAccessibility::turn(Browser& browser, platform::Accessibility& bridge, bool window_active)
{
    if (!bridge.wanted())
        return;
    for (platform::WindowAction const& action : bridge.take_window_actions())
        perform(browser, action);
    std::string const before = m_signature;
    bool const first = m_last == nullptr;
    platform::WindowState state = build(browser, window_active);
    if (first || m_signature != before || !state.events.empty())
        bridge.set_window(std::move(state));
}

void WindowAccessibility::open(std::string const& application_name, std::function<void()> wake_window)
{
    close();
    m_bridge = platform::open_accessibility(application_name, std::move(wake_window));
    if (m_bridge)
        platform::set_accessibility(m_bridge.get());
}

void WindowAccessibility::close()
{
    if (!m_bridge)
        return;
    // Out of the pages' reach first: once this returns no page thread
    // reaches the bridge, which can then go.
    platform::set_accessibility(nullptr);
    m_bridge.reset();
    m_chrome = ChromeAccessibility();
}

void WindowAccessibility::turn(Browser& browser, bool window_active)
{
    if (m_bridge)
        m_chrome.turn(browser, *m_bridge, window_active);
}

}
