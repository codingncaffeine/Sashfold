#include "ui/KeySheet.h"

#include "core/Unicode.h"
#include "ui/ChromeText.h"
#include "ui/Icons.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sashfold::ui {

namespace {

using View = KeySheetState::View;

// The longest PIN a key takes: 63 bytes of UTF-8 (CTAP 2.1 §6.5.5.5's
// newPin, at most 63 bytes before its padding to 64).
constexpr std::size_t max_pin_bytes = 63;
constexpr std::size_t accounts_shown = 4;

std::u32string u32(std::string_view text) { return decode_utf8(text); }

std::u32string number(long value) { return u32(std::to_string(value)); }

// The words cut into lines no wider than `width`, at spaces; a word wider
// than a line is left whole on a line of its own.
std::vector<std::u32string> wrap(std::u32string const& text, float width, float size)
{
    std::vector<std::u32string> lines;
    std::u32string line;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find(U' ', at);
        if (end == std::u32string::npos)
            end = text.size();
        std::u32string const word = text.substr(at, end - at);
        std::u32string const tried = line.empty() ? word : line + U" " + word;
        if (!line.empty() && chrome_text_width(tried, size) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
        at = end + 1;
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

bool ended_in_error(KeySheetState const& state) { return state.ended && state.view != View::Consent; }

char const* button_name(int button)
{
    static char const* const names[] = { "cancel", "continue", "close", "block", "allow" };
    return names[button];
}

}

void KeySheet::update(KeySheetState const& state)
{
    bool const fresh = state.id != m_state.id || state.question != m_question || state.view != m_state.view;
    m_state = state;
    if (!fresh)
        return;
    m_question = state.question;
    m_fields.assign(field_count(), std::u32string());
    m_error.clear();
    m_account = 0;
    m_hover.reset();
    m_hover_account.reset();
    // The focus goes to what is to be answered: the first field, the list,
    // else the default button (Allow, Close, or Cancel while it waits).
    std::vector<Focusable> const order = focusables();
    m_focus = 0;
    if (!order.empty() && order.front().kind == Focusable::Kind::Button) {
        std::vector<Button> const shown = buttons();
        m_focus = order.size() - 1;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i].kind == Focusable::Kind::Button && (shown[order[i].index] == Button::Allow || shown[order[i].index] == Button::Close))
                m_focus = i;
        }
    }
}

std::size_t KeySheet::field_count() const
{
    if (m_state.view != View::Pin || !m_state.pin)
        return 0;
    switch (m_state.pin->kind) {
    case auth::PinQuestion::Kind::Enter:
    case auth::PinQuestion::Kind::Retry: return 1;
    case auth::PinQuestion::Kind::Set: return 2;
    case auth::PinQuestion::Kind::Change: return 3;
    }
    return 1;
}

std::vector<std::u32string> KeySheet::field_labels() const
{
    switch (field_count()) {
    case 1: return { U"PIN" };
    case 2: return { U"New PIN", U"Confirm the new PIN" };
    case 3: return { U"Current PIN", U"New PIN", U"Confirm the new PIN" };
    default: return {};
    }
}

std::vector<KeySheet::Button> KeySheet::buttons() const
{
    if (m_state.view == View::Consent)
        return { Button::Cancel, Button::Block, Button::Allow };
    if (ended_in_error(m_state))
        return { Button::Close };
    if (m_state.view == View::Pin)
        return { Button::Cancel, Button::Continue };
    return { Button::Cancel };
}

std::vector<KeySheet::Focusable> KeySheet::focusables() const
{
    std::vector<Focusable> order;
    for (std::size_t i = 0; i < field_count(); ++i)
        order.push_back({ Focusable::Kind::Field, i });
    if (m_state.view == View::Account && !m_state.accounts.empty())
        order.push_back({ Focusable::Kind::Accounts, 0 });
    std::size_t const count = buttons().size();
    for (std::size_t i = 0; i < count; ++i)
        order.push_back({ Focusable::Kind::Button, i });
    return order;
}

std::u32string KeySheet::message() const
{
    std::size_t const min_length = m_state.pin ? m_state.pin->min_length : 4;
    switch (m_state.view) {
    case View::InsertKey: return U"Insert your security key and touch it.";
    case View::TouchKey:
        return m_state.keys_present > 1 ? U"Touch the security key you want to use." : U"Touch your security key.";
    case View::TouchKeyAgain: return U"Touch your security key again to finish.";
    case View::Processing: return U"Waiting for your security key…";
    case View::VerifyFingerprint: return U"Touch the fingerprint sensor on your security key.";
    case View::FingerprintRetry: return U"Your fingerprint was not recognized. Try again.";
    case View::Pin:
        if (!m_state.pin)
            return U"Enter the PIN of your security key.";
        switch (m_state.pin->kind) {
        case auth::PinQuestion::Kind::Enter: return U"Enter the PIN of your security key.";
        case auth::PinQuestion::Kind::Retry: return U"That PIN is not right. Enter it again.";
        case auth::PinQuestion::Kind::Set:
            return U"This site needs your security key to have a PIN. Choose one of at least " + number(static_cast<long>(min_length)) + U" characters.";
        case auth::PinQuestion::Kind::Change:
            return U"Your security key needs a new PIN. Enter its current PIN, then a new one of at least " + number(static_cast<long>(min_length)) + U" characters.";
        }
        return U"Enter the PIN of your security key.";
    case View::Account: return U"Your security key holds more than one account for this site. Choose the one to sign in with.";
    case View::AlreadyRegistered: return U"This security key is already registered with this site. You don't need to register it again.";
    case View::NotRegistered: return U"This security key is not registered with this site. Close this and try again with another key.";
    case View::Unsuitable: return U"This security key can't be used here: it does not support what this site asks for.";
    case View::PinBlocked:
        return U"Your security key is locked: its PIN was entered wrong too many times. It must be reset before it can be used again.";
    case View::PinAuthBlocked: return U"Too many wrong PINs in a row. Remove your security key, insert it again, and try again.";
    case View::UvBlocked:
        return m_state.ended ? U"Your security key's fingerprint check is locked, and the key has no PIN to use instead."
                             : U"Your security key's fingerprint check is locked. Its PIN is used instead.";
    case View::Failed:
        if (m_state.reason == auth::Reason::DeviceGone)
            return U"Your security key was removed before it finished.";
        if (m_state.reason == auth::Reason::PinPolicy)
            return U"Your security key did not accept that PIN. Try again with another one.";
        return U"Something went wrong with your security key. Try again.";
    case View::Consent: return U"This site wants to see the make and model of your security key, which could help it recognize you.";
    }
    return {};
}

void KeySheet::layout(Rect const& area, int anchor_x, KeySheetLook const& look)
{
    m_look = look;
    auto const px = [&](float css) { return static_cast<int>(std::lround(css * look.scale)); };
    float const size = look.font_size * look.scale;
    int const pad = px(16);
    int const margin = px(8);
    int const width = std::clamp(area.width - 2 * margin, std::min(px(280), area.width), px(384));
    int const x = std::clamp(anchor_x, area.x + margin, std::max(area.x + margin, area.right() - margin - width));
    int const y = area.y + margin;
    int const inner = width - 2 * pad;
    m_icon = Rect { x + pad, y + pad, px(32), px(32) };
    int const text_x = m_icon.right() + px(12);
    m_title = Rect { text_x, y + pad - px(1), x + width - pad - text_x, px(19) };
    m_subtitle = Rect { text_x, m_title.bottom(), m_title.width, px(17) };
    int cursor = std::max(m_icon.bottom(), m_subtitle.bottom()) + px(12);
    cursor += px(13); // the line under the header, and the space after it
    m_line_height = static_cast<int>(std::lround(size * 1.45f));
    m_lines = wrap(message(), static_cast<float>(inner), size);
    m_message_y = cursor;
    cursor += static_cast<int>(m_lines.size()) * m_line_height + px(4);

    m_field_boxes.clear();
    m_label_boxes.clear();
    for (std::size_t i = 0; i < field_count(); ++i) {
        cursor += px(6);
        m_label_boxes.push_back(Rect { x + pad, cursor, inner, px(18) });
        cursor += px(20);
        m_field_boxes.push_back(Rect { x + pad, cursor, inner, px(32) });
        cursor += px(32);
    }
    m_account_boxes.clear();
    if (m_state.view == View::Account) {
        cursor += px(6);
        int const row_height = px(44);
        std::size_t const first = m_account >= accounts_shown ? m_account + 1 - accounts_shown : 0;
        for (std::size_t i = 0; i < m_state.accounts.size(); ++i) {
            if (i < first || i >= first + accounts_shown) {
                m_account_boxes.push_back(Rect {});
                continue;
            }
            m_account_boxes.push_back(Rect { x + pad, cursor, inner, row_height });
            cursor += row_height;
        }
    }
    std::u32string note = m_error;
    if (note.empty() && m_state.retries >= 0
        && (m_state.view == View::FingerprintRetry || (m_state.pin && m_state.pin->kind == auth::PinQuestion::Kind::Retry)
            || (m_state.pin && m_state.pin->kind == auth::PinQuestion::Kind::Enter && m_state.retries <= 3)))
        note = m_state.retries == 1 ? U"1 try left before the key locks." : number(m_state.retries) + U" tries left before the key locks.";
    m_error_box = Rect {};
    if (!note.empty()) {
        cursor += px(8);
        m_error_box = Rect { x + pad, cursor, inner, m_line_height };
        cursor += m_line_height;
    }
    m_error_note = note;

    cursor += px(18);
    int const button_height = px(32);
    auto const button_width = [&](Button button) {
        std::u32string const label = button_label(button);
        return std::max(px(84), static_cast<int>(chrome_text_width(label, size)) + px(28));
    };
    m_button_boxes.clear();
    std::vector<Button> const shown = buttons();
    int right = x + width - pad;
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
        int const w = button_width(*it);
        // In the consent question, Cancel stands apart at the left.
        int const left = (*it == Button::Cancel && shown.size() == 3) ? x + pad : right - w;
        m_button_boxes.insert(m_button_boxes.begin(), { *it, Rect { left, cursor, w, button_height } });
        right = left - px(8);
    }
    cursor += button_height + pad;
    m_box = Rect { x, y, width, cursor - y };
}

std::u32string KeySheet::button_label(Button button)
{
    switch (button) {
    case Button::Cancel: return U"Cancel";
    case Button::Continue: return U"Continue";
    case Button::Close: return U"Close";
    case Button::Block: return U"Block";
    case Button::Allow: return U"Allow";
    }
    return {};
}

void KeySheet::paint(Bitmap& target) const
{
    KeySheetLook const& look = m_look;
    auto const px = [&](float css) { return static_cast<int>(std::lround(css * look.scale)); };
    float const size = look.font_size * look.scale;
    float const small = size * 0.92f;
    int const radius = px(10);
    int const hair = std::max(1, px(1));
    // A soft shadow, then the sheet.
    for (int i = 1; i <= 4; ++i)
        target.fill_round_rect(Rect { m_box.x - i, m_box.y - i + px(2), m_box.width + 2 * i, m_box.height + 2 * i }, radius + i,
            Color::rgba(0, 0, 0, static_cast<std::uint8_t>(0x18 - i * 4)));
    target.fill_round_box(m_box, radius, hair, look.border, look.background);

    // The header: the key in a disc, the site, what it wants.
    bool const failed = ended_in_error(m_state);
    target.fill_round_rect(m_icon, m_icon.width / 2, look.highlight);
    draw_icon(target, Icon::Key, m_icon, px(20), failed ? look.danger : look.accent);
    chrome_draw_text(target, chrome_ellipsize(u32(m_state.site), static_cast<float>(m_title.width), size * 1.08f), static_cast<float>(m_title.x),
        chrome_centered_baseline(m_title, size * 1.08f), size * 1.08f, look.text, true);
    std::u32string const wants = m_state.create ? U"wants to register a security key" : U"wants you to use a security key";
    chrome_draw_text(target, chrome_ellipsize(wants, static_cast<float>(m_subtitle.width), small), static_cast<float>(m_subtitle.x),
        chrome_centered_baseline(m_subtitle, small), small, look.muted);
    int const rule_y = m_message_y - px(13);
    target.fill_rect(Rect { m_box.x + hair, rule_y, m_box.width - 2 * hair, hair }, look.border);

    // What the ceremony needs now.
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        Rect const line { m_box.x + px(16), m_message_y + static_cast<int>(i) * m_line_height, m_box.width - px(32), m_line_height };
        chrome_draw_text(target, m_lines[i], static_cast<float>(line.x), chrome_centered_baseline(line, size), size, look.text);
    }

    std::vector<Focusable> const order = focusables();
    // (A pointer into `order`, which outlives it: GCC 13 reports an optional
    // made by a conditional as maybe uninitialized.)
    Focusable const* const focused = m_focus < order.size() ? &order[m_focus] : nullptr;
    auto const ring = [&](Rect const& box, int corner) {
        int const gap = px(2);
        target.fill_round_box(Rect { box.x - gap, box.y - gap, box.width + 2 * gap, box.height + 2 * gap }, corner + gap, px(2), look.accent,
            Color::rgba(0, 0, 0, 0));
    };

    // The PIN fields, masked: a dot a character.
    std::vector<std::u32string> const labels = field_labels();
    for (std::size_t i = 0; i < m_field_boxes.size(); ++i) {
        chrome_draw_text(target, labels[i], static_cast<float>(m_label_boxes[i].x), chrome_centered_baseline(m_label_boxes[i], small), small, look.muted);
        bool const here = focused && focused->kind == Focusable::Kind::Field && focused->index == i;
        Rect const& box = m_field_boxes[i];
        target.fill_round_box(box, px(6), here ? px(2) : hair, here ? look.accent : look.border, look.field_background);
        std::u32string const dots(i < m_fields.size() ? m_fields[i].size() : 0, U'•');
        Rect const text { box.x + px(10), box.y, box.width - px(20), box.height };
        chrome_draw_text(target, dots, static_cast<float>(text.x), chrome_centered_baseline(text, size), size, look.text);
        if (here) {
            float const caret_x = static_cast<float>(text.x) + chrome_text_width(dots, size);
            target.fill_rect(Rect { static_cast<int>(caret_x) + 1, box.y + px(8), hair, box.height - px(16) }, look.accent);
        }
    }

    // The accounts: the name, and under it the display name.
    if (!m_account_boxes.empty()) {
        Rect frame;
        for (Rect const& row : m_account_boxes) {
            if (row.is_empty())
                continue;
            frame = frame.is_empty() ? row : Rect { frame.x, frame.y, frame.width, row.bottom() - frame.y };
        }
        target.fill_round_box(frame, px(8), hair, look.border, look.field_background);
        bool const list_focused = focused && focused->kind == Focusable::Kind::Accounts;
        for (std::size_t i = 0; i < m_account_boxes.size(); ++i) {
            Rect const& row = m_account_boxes[i];
            if (row.is_empty())
                continue;
            Rect const inside { row.x + hair, row.y + hair, row.width - 2 * hair, row.height - 2 * hair };
            bool const lit = i == m_account || m_hover_account == i;
            if (lit)
                target.fill_round_rect(inside, px(6), look.highlight);
            auth::Account const& account = m_state.accounts[i];
            std::u32string const name = account.name.empty() ? std::u32string(U"An account with no name") : u32(account.name);
            Rect const top { row.x + px(12), row.y + px(4), row.width - px(24), row.height / 2 - px(2) };
            Rect const bottom { row.x + px(12), row.y + row.height / 2, row.width - px(24), row.height / 2 - px(4) };
            Color const ink = lit ? look.highlight_text : look.text;
            if (account.display_name.empty()) {
                chrome_draw_text(target, chrome_ellipsize(name, static_cast<float>(top.width), size), static_cast<float>(top.x),
                    chrome_centered_baseline(row, size), size, ink);
            } else {
                chrome_draw_text(target, chrome_ellipsize(u32(account.display_name), static_cast<float>(top.width), size), static_cast<float>(top.x),
                    chrome_centered_baseline(top, size), size, ink, true);
                chrome_draw_text(target, chrome_ellipsize(name, static_cast<float>(bottom.width), small), static_cast<float>(bottom.x),
                    chrome_centered_baseline(bottom, small), small, look.muted);
            }
            if (list_focused && i == m_account)
                ring(inside, px(6));
        }
    }

    if (!m_error_box.is_empty())
        chrome_draw_text(target, chrome_ellipsize(m_error_note, static_cast<float>(m_error_box.width), small), static_cast<float>(m_error_box.x),
            chrome_centered_baseline(m_error_box, small), small, look.danger);

    // The buttons: the last is the default, on the accent.
    int const accent_light = (2126 * look.accent.r + 7152 * look.accent.g + 722 * look.accent.b) / 10000;
    Color const on_accent = accent_light > 140 ? Color::rgb(0x10, 0x12, 0x16) : Color::rgb(0xff, 0xff, 0xff);
    for (std::size_t i = 0; i < m_button_boxes.size(); ++i) {
        auto const& [button, box] = m_button_boxes[i];
        bool const primary = i + 1 == m_button_boxes.size() && (button == Button::Continue || button == Button::Allow);
        bool const hovered = m_hover == button;
        Color const fill = primary ? look.accent : hovered ? look.highlight : look.field_background;
        target.fill_round_box(box, px(6), hair, primary ? look.accent : look.border, fill);
        if (primary && hovered)
            target.fill_round_rect(box, px(6), Color::rgba(0xff, 0xff, 0xff, 0x26));
        std::u32string const label = button_label(button);
        float const width = chrome_text_width(label, size);
        chrome_draw_text(target, label, static_cast<float>(box.x) + (static_cast<float>(box.width) - width) / 2, chrome_centered_baseline(box, size), size,
            primary ? on_accent : look.text);
        if (focused && focused->kind == Focusable::Kind::Button && focused->index == i)
            ring(box, px(6));
    }
}

void KeySheet::mouse_move(int x, int y)
{
    m_hover.reset();
    m_hover_account.reset();
    for (auto const& [button, box] : m_button_boxes) {
        if (box.contains(x, y))
            m_hover = button;
    }
    for (std::size_t i = 0; i < m_account_boxes.size(); ++i) {
        if (m_account_boxes[i].contains(x, y))
            m_hover_account = i;
    }
}

KeySheetAction KeySheet::mouse_down(int x, int y)
{
    for (std::size_t i = 0; i < m_button_boxes.size(); ++i) {
        if (m_button_boxes[i].second.contains(x, y))
            return press(m_button_boxes[i].first);
    }
    for (std::size_t i = 0; i < m_field_boxes.size(); ++i) {
        if (m_field_boxes[i].contains(x, y))
            m_focus = i;
    }
    for (std::size_t i = 0; i < m_account_boxes.size(); ++i) {
        if (m_account_boxes[i].contains(x, y)) {
            KeySheetAction action;
            action.kind = KeySheetAction::Kind::Account;
            action.account = i;
            return action;
        }
    }
    return {};
}

KeySheetAction KeySheet::press(Button button)
{
    KeySheetAction action;
    switch (button) {
    case Button::Cancel:
    case Button::Close: action.kind = KeySheetAction::Kind::Cancel; break;
    case Button::Continue: return submit_pin();
    case Button::Block:
    case Button::Allow:
        action.kind = KeySheetAction::Kind::Consent;
        action.allow = button == Button::Allow;
        break;
    }
    return action;
}

KeySheetAction KeySheet::submit_pin()
{
    if (!m_state.pin || m_fields.empty())
        return {};
    // A PIN goes to the key as UTF-8 of its NFC form (CTAP 2.1 §6.5.1), and
    // is measured in code points against the key's minimum.
    std::vector<std::u32string> normal;
    for (std::u32string const& field : m_fields)
        normal.push_back(nfc(field));
    std::size_t const min_length = std::max<std::size_t>(4, m_state.pin->min_length);
    auto const refuse = [&](std::u32string why, std::size_t field) {
        m_error = std::move(why);
        std::vector<Focusable> const order = focusables();
        m_focus = std::min(field, order.size() - 1);
        return KeySheetAction {};
    };
    auth::PinQuestion::Kind const kind = m_state.pin->kind;
    bool const choosing = kind == auth::PinQuestion::Kind::Set || kind == auth::PinQuestion::Kind::Change;
    std::size_t const new_index = kind == auth::PinQuestion::Kind::Change ? 1 : 0;
    if (!choosing) {
        if (normal[0].size() < 4)
            return refuse(U"A PIN has at least 4 characters.", 0);
    } else {
        if (kind == auth::PinQuestion::Kind::Change && normal[0].empty())
            return refuse(U"Enter the current PIN.", 0);
        if (normal[new_index].size() < min_length)
            return refuse(U"The new PIN needs at least " + number(static_cast<long>(min_length)) + U" characters.", new_index);
        if (to_utf8(normal[new_index]).size() > max_pin_bytes)
            return refuse(U"The new PIN is too long.", new_index);
        if (normal[new_index] != normal[new_index + 1])
            return refuse(U"The two new PINs are not the same.", new_index + 1);
    }
    KeySheetAction action;
    action.kind = KeySheetAction::Kind::Pin;
    if (kind == auth::PinQuestion::Kind::Change) {
        action.pin.pin = to_utf8(normal[0]);
        action.pin.new_pin = to_utf8(normal[1]);
    } else {
        action.pin.pin = to_utf8(normal[0]);
    }
    return action;
}

void KeySheet::move_focus(int by)
{
    std::size_t const count = focusables().size();
    if (count == 0)
        return;
    m_focus = static_cast<std::size_t>((static_cast<long>(m_focus) + by + static_cast<long>(count)) % static_cast<long>(count));
}

bool KeySheet::key(platform::KeyEvent const& key, KeySheetAction& action)
{
    using platform::Key;
    // The window's own chords go on to it.
    if (key.ctrl || key.alt)
        return false;
    std::vector<Focusable> const order = focusables();
    // (A pointer into `order`, which outlives it: GCC 13 reports an optional
    // made by a conditional as maybe uninitialized.)
    Focusable const* const focused = m_focus < order.size() ? &order[m_focus] : nullptr;
    switch (key.key) {
    case Key::Escape: action.kind = KeySheetAction::Kind::Cancel; return true;
    case Key::Tab: move_focus(key.shift ? -1 : 1); return true;
    case Key::Up:
    case Key::Down:
        if (m_state.view == View::Account && !m_state.accounts.empty()) {
            if (key.key == Key::Up && m_account > 0)
                --m_account;
            else if (key.key == Key::Down && m_account + 1 < m_state.accounts.size())
                ++m_account;
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (order[i].kind == Focusable::Kind::Accounts)
                    m_focus = i;
            }
        }
        return true;
    case Key::Enter:
    case Key::Space:
        if (!focused)
            return true;
        if (focused->kind == Focusable::Kind::Field) {
            if (key.key == Key::Enter) {
                // From a field, Enter goes to the next field, and from the
                // last it is Continue.
                if (focused->index + 1 < field_count())
                    m_focus = focused->index + 1;
                else
                    action = submit_pin();
            }
            return true;
        }
        if (focused->kind == Focusable::Kind::Accounts) {
            action.kind = KeySheetAction::Kind::Account;
            action.account = m_account;
            return true;
        }
        action = press(buttons()[focused->index]);
        return true;
    case Key::Backspace:
        if (focused && focused->kind == Focusable::Kind::Field && !m_fields[focused->index].empty()) {
            m_fields[focused->index].pop_back();
            m_error.clear();
        }
        return true;
    default:
        // Everything else the sheet keeps from the page under it; the
        // characters arrive as text.
        return true;
    }
}

void KeySheet::text(char32_t code_point)
{
    std::vector<Focusable> const order = focusables();
    if (m_focus >= order.size() || order[m_focus].kind != Focusable::Kind::Field || code_point < 0x20 || code_point == 0x7F)
        return;
    std::u32string& field = m_fields[order[m_focus].index];
    if (to_utf8(field).size() + 4 > 4 * max_pin_bytes)
        return;
    field.push_back(code_point);
    m_error.clear();
}

std::string KeySheet::shows() const
{
    std::string said = std::string(view_name(m_state)) + " " + m_state.site;
    if (m_state.pin && m_state.view == View::Pin)
        said += " | min " + std::to_string(m_state.pin->min_length);
    if (m_state.retries >= 0)
        said += " | retries " + std::to_string(m_state.retries);
    if (!m_state.accounts.empty()) {
        said += " |";
        for (std::size_t i = 0; i < m_state.accounts.size(); ++i)
            said += std::string(i ? ", " : " ") + m_state.accounts[i].name + " (" + m_state.accounts[i].display_name + ")";
    }
    if (!m_fields.empty()) {
        said += " | fields";
        for (std::size_t i = 0; i < m_fields.size(); ++i)
            said += std::string(i ? "," : " ") + std::to_string(m_fields[i].size());
    }
    if (!m_error.empty())
        said += " | " + to_utf8(m_error);
    if (m_state.held)
        said += " | held";
    return said;
}

std::string KeySheet::focus_name() const
{
    std::vector<Focusable> const order = focusables();
    if (m_focus >= order.size())
        return "none";
    Focusable const& focused = order[m_focus];
    switch (focused.kind) {
    case Focusable::Kind::Field: return "field " + std::to_string(focused.index);
    case Focusable::Kind::Accounts: return "account " + std::to_string(m_account);
    case Focusable::Kind::Button: return std::string("button ") + button_name(static_cast<int>(buttons()[focused.index]));
    }
    return "none";
}

std::optional<Rect> KeySheet::button(std::string_view name) const
{
    for (auto const& [button, box] : m_button_boxes) {
        if (name == button_name(static_cast<int>(button)))
            return box;
    }
    for (std::size_t i = 0; i < m_field_boxes.size(); ++i) {
        if (name == "field-" + std::to_string(i))
            return m_field_boxes[i];
    }
    return std::nullopt;
}

std::optional<Rect> KeySheet::account_row(std::size_t index) const
{
    if (index >= m_account_boxes.size() || m_account_boxes[index].is_empty())
        return std::nullopt;
    return m_account_boxes[index];
}

}
