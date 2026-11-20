#include "ui/KeySettings.h"

#include "core/Unicode.h"
#include "ui/ChromeText.h"
#include "ui/Icons.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace sashfold::ui {

namespace {

using Phase = auth::KeyManagementState::Phase;
using Outcome = auth::KeyManagementState::Outcome;
using Failure = auth::KeyManagementState::Failure;
using View = KeySettings::View;
using Button = KeySettings::Button;

// The longest PIN a key takes: 63 bytes of UTF-8 (CTAP 2.1 §6.5.5.5).
constexpr std::size_t max_pin_bytes = 63;
constexpr std::size_t rows_shown = 4;

std::u32string u32(std::string_view text) { return decode_utf8(text); }

std::u32string number(std::uint64_t value) { return u32(std::to_string(value)); }

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

std::size_t fields_of(View view)
{
    switch (view) {
    case View::Unlock: return 1;
    case View::SetPin: return 2;
    case View::ChangePin: return 3;
    default: return 0;
    }
}

char const* button_name(Button button)
{
    switch (button) {
    case Button::Cancel: return "cancel";
    case Button::Close: return "close";
    case Button::Back: return "back";
    case Button::Continue: return "continue";
    case Button::Save: return "save";
    case Button::SetPin: return "set-pin";
    case Button::ChangePin: return "change-pin";
    case Button::Passkeys: return "passkeys";
    case Button::Delete: return "delete";
    case Button::ConfirmDelete: return "confirm-delete";
    }
    return "";
}

std::u32string button_label(Button button)
{
    switch (button) {
    case Button::Cancel: return U"Cancel";
    case Button::Close: return U"Close";
    case Button::Back: return U"Back";
    case Button::Continue: return U"Continue";
    case Button::Save: return U"Save";
    case Button::SetPin: return U"Set a PIN";
    case Button::ChangePin: return U"Change PIN";
    case Button::Passkeys: return U"Passkeys";
    case Button::Delete: return U"Delete…";
    case Button::ConfirmDelete: return U"Delete";
    }
    return {};
}

std::u32string user_line(auth::UserEntity const& user)
{
    if (user.name.empty() && user.display_name.empty())
        return U"An account with no name";
    if (user.display_name.empty() || user.display_name == user.name)
        return u32(user.name.empty() ? user.display_name : user.name);
    if (user.name.empty())
        return u32(user.display_name);
    return u32(user.name) + U" · " + u32(user.display_name);
}

std::u32string tries_left(int retries)
{
    if (retries < 0)
        return {};
    return retries == 1 ? U" 1 try left before the key locks." : U" " + number(static_cast<std::uint64_t>(retries)) + U" tries left before the key locks.";
}

// Where there is no transport: no key ever comes.
class NoKeys final : public auth::Transport {
public:
    std::vector<auth::DeviceInfo> devices() override { return {}; }
    std::unique_ptr<auth::HidConnection> open(auth::DeviceInfo const&) override { return nullptr; }
    bool wait_for_change(std::chrono::milliseconds timeout) override
    {
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(100)));
        return false;
    }
};

}

KeySettings::KeySettings() = default;

KeySettings::~KeySettings()
{
    close();
    // The window is closing: the sessions still ending are waited for here,
    // as the ceremonies are.
    m_ending.clear();
}

void KeySettings::set_transport(std::shared_ptr<auth::Transport> transport, std::function<void()> wake)
{
    close();
    m_transport = std::move(transport);
    m_wake = std::move(wake);
}

void KeySettings::open()
{
    close();
    m_signal = std::make_shared<Signal>();
    m_signal->wake = m_wake;
    std::weak_ptr<Signal> const signal = m_signal;
    std::shared_ptr<auth::Transport> transport = m_transport ? m_transport : std::make_shared<NoKeys>();
    m_session = std::make_unique<auth::KeyManagement>(std::move(transport), [signal] {
        if (auto const alive = signal.lock()) {
            ++alive->generation;
            if (alive->wake)
                alive->wake();
        }
    });
    m_state = {};
    m_heard = ~std::uint64_t { 0 };
    m_commands_seen = 0;
    m_form = View::Home;
    m_shown = View::Waiting;
    m_fields.clear();
    m_error.clear();
    m_note.clear();
    m_selected = 0;
    m_deleting.reset();
    m_hover.reset();
    m_hover_row.reset();
    focus_default();
    m_session->start();
}

void KeySettings::close()
{
    if (!m_session)
        return;
    m_session->cancel();
    m_ending.push_back(std::move(m_session));
    m_signal.reset();
    // Nothing secret stays in the fields of a closed sheet.
    for (std::u32string& field : m_fields)
        std::fill(field.begin(), field.end(), U'\0');
    m_fields.clear();
    m_deleting.reset();
    m_state = {};
    reap();
}

void KeySettings::reap()
{
    std::erase_if(m_ending, [](std::unique_ptr<auth::KeyManagement> const& session) { return session->finished(); });
}

bool KeySettings::hear()
{
    reap();
    if (!m_session || !m_signal)
        return false;
    std::uint64_t const now = m_signal->generation.load();
    if (now == m_heard)
        return false;
    m_heard = now;
    m_state = m_session->state();
    take_outcome();
    if (view() != m_shown) {
        m_shown = view();
        focus_default();
    }
    return true;
}

View KeySettings::view() const
{
    switch (m_state.phase) {
    case Phase::InsertKey:
    case Phase::TouchKey: return View::Waiting;
    case Phase::Busy: return m_state.key ? View::Working : View::Waiting;
    case Phase::VerifyFingerprint: return View::Fingerprint;
    case Phase::Ready: return m_state.key ? m_form : View::Waiting;
    case Phase::Ended: return View::Ended;
    }
    return View::Waiting;
}

void KeySettings::go(View form)
{
    m_form = form;
    for (std::u32string& field : m_fields)
        std::fill(field.begin(), field.end(), U'\0');
    m_fields.assign(fields_of(form), std::u32string());
    m_error.clear();
    m_note.clear();
    m_hover.reset();
    m_hover_row.reset();
    if (form != View::Passkeys && form != View::ConfirmDelete)
        m_deleting.reset();
    if (m_selected >= m_state.passkeys.size())
        m_selected = m_state.passkeys.empty() ? 0 : m_state.passkeys.size() - 1;
    m_shown = view();
    focus_default();
}

// What the last command came to, once it is done: the form it leaves the
// reader on, and the word on it.
void KeySettings::take_outcome()
{
    if (m_state.commands_done == m_commands_seen)
        return;
    m_commands_seen = m_state.commands_done;
    View const form = m_form;
    switch (m_state.outcome) {
    case Outcome::None: break;
    case Outcome::PinSet:
        go(View::Home);
        m_note = U"The PIN is set.";
        break;
    case Outcome::PinChanged:
        go(View::Home);
        m_note = U"The PIN is changed.";
        break;
    case Outcome::PinWrong:
        go(form == View::ChangePin ? View::ChangePin : View::Unlock);
        m_error = U"That PIN is not right." + tries_left(m_state.retries);
        break;
    case Outcome::PinPolicy:
        go(form == View::ChangePin ? View::ChangePin : View::SetPin);
        m_error = U"Your security key did not accept that PIN. Choose another one.";
        break;
    case Outcome::PinNeedsChange:
        go(View::ChangePin);
        m_note = U"Your security key asks for a new PIN first.";
        break;
    case Outcome::Unlocked: go(View::Passkeys); break;
    case Outcome::TokenExpired:
        go(View::Unlock);
        m_note = U"Enter the PIN again to go on.";
        break;
    case Outcome::UvWrong:
        go(View::Home);
        m_error = U"Your fingerprint was not recognized." + tries_left(m_state.retries);
        break;
    case Outcome::UvBlocked:
        go(View::Home);
        m_error = U"The fingerprint check of your security key is locked.";
        break;
    case Outcome::Deleted:
        go(View::Passkeys);
        m_note = U"The passkey is deleted.";
        break;
    case Outcome::NotFound:
        go(View::Passkeys);
        m_note = U"That passkey was no longer on the key.";
        break;
    case Outcome::NotSupported:
        go(View::Home);
        m_error = U"Your security key can't do that.";
        break;
    }
}

std::size_t KeySettings::field_count() const { return fields_of(view()); }

std::vector<std::u32string> KeySettings::field_labels() const
{
    switch (view()) {
    case View::Unlock: return { U"PIN" };
    case View::SetPin: return { U"New PIN", U"Confirm the new PIN" };
    case View::ChangePin: return { U"Current PIN", U"New PIN", U"Confirm the new PIN" };
    default: return {};
    }
}

std::vector<Button> KeySettings::buttons() const
{
    switch (view()) {
    case View::Waiting:
    case View::Working:
    case View::Fingerprint: return { Button::Cancel };
    case View::Ended: return { Button::Close };
    case View::Home: {
        std::vector<Button> shown { Button::Close };
        auth::ManagedKey const& key = *m_state.key;
        if (key.pin_supported)
            shown.push_back(key.pin_set ? Button::ChangePin : Button::SetPin);
        if (key.credential_management != auth::cred_mgmt::Support::None && (key.pin_set || key.built_in_uv))
            shown.push_back(Button::Passkeys);
        return shown;
    }
    case View::Unlock: return { Button::Cancel, Button::Continue };
    case View::SetPin:
    case View::ChangePin: return { Button::Cancel, Button::Save };
    case View::Passkeys:
        if (m_state.passkeys.empty())
            return { Button::Back };
        return { Button::Back, Button::Delete };
    case View::ConfirmDelete: return { Button::Cancel, Button::ConfirmDelete };
    }
    return { Button::Cancel };
}

std::vector<KeySettings::Focusable> KeySettings::focusables() const
{
    std::vector<Focusable> order;
    for (std::size_t i = 0; i < field_count(); ++i)
        order.push_back({ Focusable::Kind::Field, i });
    if (view() == View::Passkeys && !m_state.passkeys.empty())
        order.push_back({ Focusable::Kind::List, 0 });
    std::size_t const count = buttons().size();
    for (std::size_t i = 0; i < count; ++i)
        order.push_back({ Focusable::Kind::Button, i });
    return order;
}

void KeySettings::focus_default()
{
    std::vector<Focusable> const order = focusables();
    // The first field or the list; else the last button, the one the view
    // is for (in the confirmation, Cancel: deleting is never the default).
    m_focus = order.empty() ? 0 : order.size() - 1;
    if (!order.empty() && order.front().kind != Focusable::Kind::Button)
        m_focus = 0;
    if (view() == View::ConfirmDelete)
        m_focus = 0;
}

std::u32string KeySettings::message() const
{
    auto const min_length = [&] { return number(m_state.key ? m_state.key->min_pin_length : 4); };
    switch (view()) {
    case View::Waiting:
        if (m_state.phase == Phase::InsertKey)
            return U"Insert your security key.";
        if (m_state.phase == Phase::TouchKey)
            return m_state.keys_present > 1 ? U"Touch the security key you want to manage." : U"Touch your security key to manage it.";
        return U"Reading your security key…";
    case View::Working: return U"Working with your security key…";
    case View::Fingerprint: return U"Touch the fingerprint sensor on your security key.";
    case View::Home: {
        auth::ManagedKey const& key = *m_state.key;
        if (!key.ctap2)
            return U"This security key has no PIN and keeps no passkeys: there is nothing to manage on it.";
        std::u32string said;
        if (!key.pin_supported)
            said = U"This security key has no PIN.";
        else if (key.force_pin_change)
            said = U"This security key needs a new PIN.";
        else if (key.pin_set)
            said = U"This security key is protected by a PIN.";
        else
            said = U"This security key has no PIN yet.";
        if (key.credential_management == auth::cred_mgmt::Support::None)
            said += U" It can't list the passkeys it holds.";
        else if (!key.pin_set && !key.built_in_uv)
            said += U" Set a PIN to see the passkeys it holds.";
        else
            said += U" You can see and delete the passkeys it holds.";
        return said;
    }
    case View::Unlock: return U"Enter the PIN of your security key to see its passkeys.";
    case View::SetPin: return U"Choose a PIN of at least " + min_length() + U" characters. Sites that verify you with this key will ask for it.";
    case View::ChangePin: return U"Enter the current PIN, then a new one of at least " + min_length() + U" characters.";
    case View::Passkeys: {
        std::uint64_t const held = m_state.passkeys.size();
        std::u32string const room = m_state.metadata ? number(m_state.metadata->remaining) : std::u32string(U"?");
        if (held == 0)
            return U"There are no passkeys on this security key. It has room for " + room + U".";
        return number(held) + (held == 1 ? U" passkey" : U" passkeys") + U" on this security key, and room for " + room + U" more.";
    }
    case View::ConfirmDelete: {
        if (!m_deleting)
            return {};
        std::u32string const who = m_deleting->user.name.empty() ? user_line(m_deleting->user) : u32(m_deleting->user.name);
        return U"Delete the passkey for " + who + U" on " + u32(m_deleting->rp_id) + U"? You won't be able to sign in to " + u32(m_deleting->rp_id)
            + U" with it anymore.";
    }
    case View::Ended:
        switch (m_state.failure) {
        case Failure::KeyGone: return U"Your security key was removed. Insert it and open Security keys again to go on.";
        case Failure::PinBlocked:
            return U"Your security key is locked: its PIN was entered wrong too many times. Only a reset of the key, which erases everything on it, makes it usable again.";
        case Failure::PinAuthBlocked: return U"Too many wrong PINs in a row. Remove your security key and insert it again to try again.";
        case Failure::DeviceError: return U"Something went wrong with your security key. Remove it, insert it again, and try again.";
        case Failure::Cancelled:
        case Failure::None: return U"Closed.";
        }
        return {};
    }
    return {};
}

void KeySettings::layout(Rect const& area, int anchor_right, KeySheetLook const& look)
{
    m_look = look;
    auto const px = [&](float css) { return static_cast<int>(std::lround(css * look.scale)); };
    float const size = look.font_size * look.scale;
    int const pad = px(16);
    int const margin = px(8);
    int const width = std::clamp(area.width - 2 * margin, std::min(px(280), area.width), px(400));
    int const x = std::clamp(anchor_right - width, area.x + margin, std::max(area.x + margin, area.right() - margin - width));
    int const y = area.y + margin;
    int const inner = width - 2 * pad;
    m_icon = Rect { x + pad, y + pad, px(32), px(32) };
    int const text_x = m_icon.right() + px(12);
    m_title = Rect { text_x, y + pad - px(1), x + width - pad - text_x, px(19) };
    m_subtitle = Rect { text_x, m_title.bottom(), m_title.width, px(17) };
    int cursor = std::max(m_icon.bottom(), m_subtitle.bottom()) + px(12);
    cursor += px(13);
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
    m_row_boxes.clear();
    if (view() == View::Passkeys && !m_state.passkeys.empty()) {
        cursor += px(6);
        int const row_height = px(44);
        std::size_t const first = m_selected >= rows_shown ? m_selected + 1 - rows_shown : 0;
        for (std::size_t i = 0; i < m_state.passkeys.size(); ++i) {
            if (i < first || i >= first + rows_shown) {
                m_row_boxes.push_back(Rect {});
                continue;
            }
            m_row_boxes.push_back(Rect { x + pad, cursor, inner, row_height });
            cursor += row_height;
        }
    }
    m_note_danger = !m_error.empty();
    m_note_lines.clear();
    if (std::u32string const& note = m_error.empty() ? m_note : m_error; !note.empty()) {
        cursor += px(8);
        m_note_lines = wrap(note, static_cast<float>(inner), size * 0.92f);
        m_note_y = cursor;
        cursor += static_cast<int>(m_note_lines.size()) * m_line_height;
    }

    cursor += px(18);
    int const button_height = px(32);
    m_button_boxes.clear();
    std::vector<Button> const shown = buttons();
    int right = x + width - pad;
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
        int const w = std::max(px(84), static_cast<int>(chrome_text_width(button_label(*it), size)) + px(28));
        // Close and Back stand apart at the left when there is more than one.
        bool const apart = (*it == Button::Close || *it == Button::Back) && shown.size() > 1;
        int const left = apart ? x + pad : right - w;
        m_button_boxes.insert(m_button_boxes.begin(), { *it, Rect { left, cursor, w, button_height } });
        if (!apart)
            right = left - px(8);
    }
    cursor += button_height + pad;
    m_box = Rect { x, y, width, cursor - y };
}

void KeySettings::paint(Bitmap& target) const
{
    KeySheetLook const& look = m_look;
    auto const px = [&](float css) { return static_cast<int>(std::lround(css * look.scale)); };
    float const size = look.font_size * look.scale;
    float const small = size * 0.92f;
    int const radius = px(10);
    int const hair = std::max(1, px(1));
    for (int i = 1; i <= 4; ++i)
        target.fill_round_rect(Rect { m_box.x - i, m_box.y - i + px(2), m_box.width + 2 * i, m_box.height + 2 * i }, radius + i,
            Color::rgba(0, 0, 0, static_cast<std::uint8_t>(0x18 - i * 4)));
    target.fill_round_box(m_box, radius, hair, look.border, look.background);

    // The header: the key in a disc, what this is, which key.
    bool const failed = view() == View::Ended;
    target.fill_round_rect(m_icon, m_icon.width / 2, look.highlight);
    draw_icon(target, Icon::Key, m_icon, px(20), failed ? look.danger : look.accent);
    chrome_draw_text(target, chrome_ellipsize(U"Security keys", static_cast<float>(m_title.width), size * 1.08f), static_cast<float>(m_title.x),
        chrome_centered_baseline(m_title, size * 1.08f), size * 1.08f, look.text, true);
    std::u32string const which = m_state.key ? (m_state.key->device.empty() ? std::u32string(U"Your security key") : u32(m_state.key->device))
                                             : std::u32string(U"Set a PIN, see and delete passkeys");
    chrome_draw_text(target, chrome_ellipsize(which, static_cast<float>(m_subtitle.width), small), static_cast<float>(m_subtitle.x),
        chrome_centered_baseline(m_subtitle, small), small, look.muted);
    int const rule_y = m_message_y - px(13);
    target.fill_rect(Rect { m_box.x + hair, rule_y, m_box.width - 2 * hair, hair }, look.border);

    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        Rect const line { m_box.x + px(16), m_message_y + static_cast<int>(i) * m_line_height, m_box.width - px(32), m_line_height };
        chrome_draw_text(target, m_lines[i], static_cast<float>(line.x), chrome_centered_baseline(line, size), size, look.text);
    }

    std::vector<Focusable> const order = focusables();
    std::optional<Focusable> const focused = m_focus < order.size() ? std::optional<Focusable>(order[m_focus]) : std::nullopt;
    auto const ring = [&](Rect const& box, int corner) {
        int const gap = px(2);
        target.fill_round_box(Rect { box.x - gap, box.y - gap, box.width + 2 * gap, box.height + 2 * gap }, corner + gap, px(2), look.accent,
            Color::rgba(0, 0, 0, 0));
    };

    // The PIN fields, masked: a dot a character.
    std::vector<std::u32string> const labels = field_labels();
    for (std::size_t i = 0; i < m_field_boxes.size() && i < labels.size(); ++i) {
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

    // The passkeys: the site, and under it the user's names.
    if (!m_row_boxes.empty()) {
        Rect frame;
        for (Rect const& row : m_row_boxes) {
            if (row.is_empty())
                continue;
            frame = frame.is_empty() ? row : Rect { frame.x, frame.y, frame.width, row.bottom() - frame.y };
        }
        target.fill_round_box(frame, px(8), hair, look.border, look.field_background);
        bool const list_focused = focused && focused->kind == Focusable::Kind::List;
        for (std::size_t i = 0; i < m_row_boxes.size() && i < m_state.passkeys.size(); ++i) {
            Rect const& row = m_row_boxes[i];
            if (row.is_empty())
                continue;
            Rect const inside { row.x + hair, row.y + hair, row.width - 2 * hair, row.height - 2 * hair };
            bool const lit = i == m_selected || m_hover_row == i;
            if (lit)
                target.fill_round_rect(inside, px(6), look.highlight);
            auth::Passkey const& passkey = m_state.passkeys[i];
            Rect const top { row.x + px(12), row.y + px(4), row.width - px(24), row.height / 2 - px(2) };
            Rect const bottom { row.x + px(12), row.y + row.height / 2, row.width - px(24), row.height / 2 - px(4) };
            chrome_draw_text(target, chrome_ellipsize(u32(passkey.rp_id), static_cast<float>(top.width), size), static_cast<float>(top.x),
                chrome_centered_baseline(top, size), size, lit ? look.highlight_text : look.text, true);
            chrome_draw_text(target, chrome_ellipsize(user_line(passkey.user), static_cast<float>(bottom.width), small), static_cast<float>(bottom.x),
                chrome_centered_baseline(bottom, small), small, look.muted);
            if (list_focused && i == m_selected)
                ring(inside, px(6));
        }
    }

    for (std::size_t i = 0; i < m_note_lines.size(); ++i) {
        Rect const line { m_box.x + px(16), m_note_y + static_cast<int>(i) * m_line_height, m_box.width - px(32), m_line_height };
        chrome_draw_text(target, m_note_lines[i], static_cast<float>(line.x), chrome_centered_baseline(line, small), small,
            m_note_danger ? look.danger : look.muted);
    }

    // The buttons: the last is the default, on the accent; deleting is on
    // the danger color.
    auto const ink_on = [](Color fill) {
        int const light = (2126 * fill.r + 7152 * fill.g + 722 * fill.b) / 10000;
        return light > 140 ? Color::rgb(0x10, 0x12, 0x16) : Color::rgb(0xff, 0xff, 0xff);
    };
    for (std::size_t i = 0; i < m_button_boxes.size(); ++i) {
        auto const& [button, box] = m_button_boxes[i];
        bool const danger = button == Button::ConfirmDelete;
        bool const primary = !danger && i + 1 == m_button_boxes.size() && button != Button::Cancel && button != Button::Close && button != Button::Back;
        bool const hovered = m_hover == button;
        Color const fill = danger ? look.danger : primary ? look.accent : hovered ? look.highlight : look.field_background;
        target.fill_round_box(box, px(6), hair, danger || primary ? fill : look.border, fill);
        if ((primary || danger) && hovered)
            target.fill_round_rect(box, px(6), Color::rgba(0xff, 0xff, 0xff, 0x26));
        std::u32string const label = button_label(button);
        float const label_width = chrome_text_width(label, size);
        chrome_draw_text(target, label, static_cast<float>(box.x) + (static_cast<float>(box.width) - label_width) / 2, chrome_centered_baseline(box, size),
            size, primary || danger ? ink_on(fill) : look.text);
        if (focused && focused->kind == Focusable::Kind::Button && focused->index == i)
            ring(box, px(6));
    }
}

void KeySettings::mouse_move(int x, int y)
{
    m_hover.reset();
    m_hover_row.reset();
    for (auto const& [button, box] : m_button_boxes) {
        if (box.contains(x, y))
            m_hover = button;
    }
    for (std::size_t i = 0; i < m_row_boxes.size(); ++i) {
        if (m_row_boxes[i].contains(x, y))
            m_hover_row = i;
    }
}

void KeySettings::mouse_down(int x, int y)
{
    for (std::size_t i = 0; i < m_button_boxes.size(); ++i) {
        if (m_button_boxes[i].second.contains(x, y)) {
            press(m_button_boxes[i].first);
            return;
        }
    }
    std::vector<Focusable> const order = focusables();
    for (std::size_t i = 0; i < m_field_boxes.size(); ++i) {
        if (m_field_boxes[i].contains(x, y))
            m_focus = i;
    }
    for (std::size_t i = 0; i < m_row_boxes.size(); ++i) {
        if (!m_row_boxes[i].contains(x, y))
            continue;
        m_selected = i;
        for (std::size_t j = 0; j < order.size(); ++j) {
            if (order[j].kind == Focusable::Kind::List)
                m_focus = j;
        }
    }
}

void KeySettings::press(Button button)
{
    if (!m_session)
        return;
    switch (button) {
    case Button::Close: close(); return;
    case Button::Cancel:
        if (view() == View::ConfirmDelete) {
            go(View::Passkeys);
            return;
        }
        if (view() == View::Unlock || view() == View::SetPin || view() == View::ChangePin) {
            go(View::Home);
            return;
        }
        close();
        return;
    case Button::Back: go(View::Home); return;
    case Button::Continue:
    case Button::Save: submit(); return;
    case Button::SetPin: go(View::SetPin); return;
    case Button::ChangePin: go(View::ChangePin); return;
    case Button::Passkeys:
        if (!m_state.key)
            return;
        if (m_state.unlocked) {
            go(View::Passkeys);
            m_session->refresh();
        } else if (m_state.key->pin_set) {
            go(View::Unlock);
        } else if (m_state.key->built_in_uv) {
            m_session->unlock(std::nullopt);
        }
        return;
    case Button::Delete:
        if (m_selected < m_state.passkeys.size()) {
            m_deleting = m_state.passkeys[m_selected];
            go(View::ConfirmDelete);
        }
        return;
    case Button::ConfirmDelete:
        if (m_deleting) {
            m_session->remove(m_deleting->credential);
            m_deleting.reset();
        }
        return;
    }
}

void KeySettings::submit()
{
    View const form = view();
    std::size_t const count = fields_of(form);
    if (count == 0 || m_fields.size() != count || !m_state.key)
        return;
    // A PIN goes to the key as the UTF-8 of its NFC form (CTAP 2.1 §6.5.1),
    // measured in code points against the key's minimum.
    std::vector<std::u32string> normal;
    for (std::u32string const& field : m_fields)
        normal.push_back(nfc(field));
    std::size_t const min_length = std::max<std::size_t>(4, m_state.key->min_pin_length);
    auto const refuse = [&](std::u32string why, std::size_t field) {
        m_error = std::move(why);
        m_note.clear();
        m_focus = std::min(field, focusables().size() - 1);
    };
    if (form == View::Unlock) {
        if (normal[0].size() < 4)
            return refuse(U"A PIN has at least 4 characters.", 0);
        m_session->unlock(to_utf8(normal[0]));
    } else {
        std::size_t const new_index = form == View::ChangePin ? 1 : 0;
        if (form == View::ChangePin && normal[0].size() < 4)
            return refuse(U"Enter the current PIN.", 0);
        if (normal[new_index].size() < min_length)
            return refuse(U"The new PIN needs at least " + number(min_length) + U" characters.", new_index);
        if (to_utf8(normal[new_index]).size() > max_pin_bytes)
            return refuse(U"The new PIN is too long.", new_index);
        if (normal[new_index] != normal[new_index + 1])
            return refuse(U"The two new PINs are not the same.", new_index + 1);
        if (form == View::ChangePin)
            m_session->change_pin(to_utf8(normal[0]), to_utf8(normal[1]));
        else
            m_session->set_pin(to_utf8(normal[0]));
    }
    // The PIN is the key's now; the fields keep nothing of it.
    for (std::u32string& field : m_fields)
        std::fill(field.begin(), field.end(), U'\0');
    for (std::u32string& field : normal)
        std::fill(field.begin(), field.end(), U'\0');
    m_fields.assign(count, std::u32string());
    m_error.clear();
    m_note.clear();
}

void KeySettings::move_focus(int by)
{
    std::size_t const count = focusables().size();
    if (count == 0)
        return;
    m_focus = static_cast<std::size_t>((static_cast<long>(m_focus) + by + static_cast<long>(count)) % static_cast<long>(count));
}

bool KeySettings::key(platform::KeyEvent const& key)
{
    using platform::Key;
    if (!m_session)
        return false;
    if (key.ctrl || key.alt)
        return false;
    std::vector<Focusable> const order = focusables();
    std::optional<Focusable> const focused = m_focus < order.size() ? std::optional<Focusable>(order[m_focus]) : std::nullopt;
    switch (key.key) {
    case Key::Escape: close(); return true;
    case Key::Tab: move_focus(key.shift ? -1 : 1); return true;
    case Key::Up:
    case Key::Down:
        if (view() == View::Passkeys && !m_state.passkeys.empty()) {
            if (key.key == Key::Up && m_selected > 0)
                --m_selected;
            else if (key.key == Key::Down && m_selected + 1 < m_state.passkeys.size())
                ++m_selected;
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (order[i].kind == Focusable::Kind::List)
                    m_focus = i;
            }
        }
        return true;
    case Key::Delete:
        if (focused && focused->kind == Focusable::Kind::List)
            press(Button::Delete);
        return true;
    case Key::Enter:
    case Key::Space:
        if (!focused)
            return true;
        if (focused->kind == Focusable::Kind::Field) {
            if (key.key == Key::Enter) {
                if (focused->index + 1 < field_count())
                    m_focus = focused->index + 1;
                else
                    submit();
            }
            return true;
        }
        if (focused->kind == Focusable::Kind::List) {
            if (key.key == Key::Enter)
                press(Button::Delete);
            return true;
        }
        press(buttons()[focused->index]);
        return true;
    case Key::Backspace:
        if (focused && focused->kind == Focusable::Kind::Field && focused->index < m_fields.size() && !m_fields[focused->index].empty()) {
            m_fields[focused->index].pop_back();
            m_error.clear();
        }
        return true;
    default: return true;
    }
}

void KeySettings::text(char32_t code_point)
{
    std::vector<Focusable> const order = focusables();
    if (m_focus >= order.size() || order[m_focus].kind != Focusable::Kind::Field || code_point < 0x20 || code_point == 0x7F)
        return;
    std::size_t const index = order[m_focus].index;
    if (index >= m_fields.size())
        return;
    std::u32string& field = m_fields[index];
    if (to_utf8(field).size() + 4 > 4 * max_pin_bytes)
        return;
    field.push_back(code_point);
    m_error.clear();
}

std::string KeySettings::shows() const
{
    if (!m_session)
        return {};
    std::string said;
    auto const fields = [&] {
        std::string out = " | fields";
        for (std::size_t i = 0; i < m_fields.size(); ++i)
            out += std::string(i ? "," : " ") + std::to_string(m_fields[i].size());
        return out;
    };
    std::string const min = m_state.key ? " | min " + std::to_string(m_state.key->min_pin_length) : std::string();
    switch (view()) {
    case View::Waiting:
        said = m_state.phase == Phase::InsertKey ? "insert-key"
            : m_state.phase == Phase::TouchKey   ? "touch-key " + std::to_string(m_state.keys_present)
                                                 : "reading";
        break;
    case View::Working: said = "working"; break;
    case View::Fingerprint: said = "fingerprint"; break;
    case View::Home: {
        auth::ManagedKey const& key = *m_state.key;
        said = "home | pin ";
        said += !key.pin_supported ? "none" : key.force_pin_change ? "change" : key.pin_set ? "set" : "unset";
        said += " | passkeys ";
        said += key.credential_management == auth::cred_mgmt::Support::Full ? "full"
            : key.credential_management == auth::cred_mgmt::Support::Preview ? "preview"
                                                                              : "none";
        break;
    }
    case View::Unlock: said = "unlock" + fields(); break;
    case View::SetPin: said = "set-pin" + min + fields(); break;
    case View::ChangePin: said = "change-pin" + min + fields(); break;
    case View::Passkeys: {
        said = "passkeys " + std::to_string(m_state.passkeys.size());
        if (m_state.metadata)
            said += " of " + std::to_string(m_state.metadata->existing + m_state.metadata->remaining);
        if (!m_state.passkeys.empty()) {
            said += " |";
            for (std::size_t i = 0; i < m_state.passkeys.size(); ++i) {
                auth::Passkey const& p = m_state.passkeys[i];
                said += std::string(i ? ", " : " ") + p.rp_id + " " + p.user.name + " (" + p.user.display_name + ")";
            }
            said += " | selected " + std::to_string(m_selected);
        }
        break;
    }
    case View::ConfirmDelete: said = m_deleting ? "confirm-delete " + m_deleting->rp_id + " " + m_deleting->user.name : "confirm-delete"; break;
    case View::Ended:
        said = "ended ";
        switch (m_state.failure) {
        case Failure::KeyGone: said += "key-gone"; break;
        case Failure::PinBlocked: said += "pin-blocked"; break;
        case Failure::PinAuthBlocked: said += "pin-auth-blocked"; break;
        case Failure::DeviceError: said += "device-error"; break;
        case Failure::Cancelled: said += "cancelled"; break;
        case Failure::None: said += "none"; break;
        }
        break;
    }
    std::u32string const note = m_error.empty() ? m_note : m_error;
    if (!note.empty())
        said += " | " + to_utf8(note);
    return said;
}

std::string KeySettings::focus_name() const
{
    if (!m_session)
        return "none";
    std::vector<Focusable> const order = focusables();
    if (m_focus >= order.size())
        return "none";
    Focusable const& focused = order[m_focus];
    switch (focused.kind) {
    case Focusable::Kind::Field: return "field " + std::to_string(focused.index);
    case Focusable::Kind::List: return "passkey " + std::to_string(m_selected);
    case Focusable::Kind::Button: return std::string("button ") + button_name(buttons()[focused.index]);
    }
    return "none";
}

std::optional<Rect> KeySettings::button(std::string_view name) const
{
    for (auto const& [shown, box] : m_button_boxes) {
        if (name == button_name(shown))
            return box;
    }
    for (std::size_t i = 0; i < m_field_boxes.size(); ++i) {
        if (name == "field-" + std::to_string(i))
            return m_field_boxes[i];
    }
    return std::nullopt;
}

std::optional<Rect> KeySettings::row(std::size_t index) const
{
    if (index >= m_row_boxes.size() || m_row_boxes[index].is_empty())
        return std::nullopt;
    return m_row_boxes[index];
}

}
