#include "platform/Window.h"

// The Wayland window: the wire client in Wayland.cpp carries the requests
// and events; this file knows the interfaces. wl_shm buffers in a memfd
// carry the frame (the pixels are ours; the compositor only composites
// them), xdg-shell places the toplevel, xdg-decoration asks the compositor
// for its own title bar (KDE, sway and friends draw one; a compositor that
// insists on client-side decorations gets a bare surface for now), the
// seat's pointer and keyboard become WindowEvents with the keymap parsed
// in Xkb.cpp, cursor-shape-v1 names the cursor instead of us loading a
// cursor theme, toplevel-icon-v1 hands over the brand icon, and the data
// device carries the clipboard. Opcodes and enum values are read off the
// protocol XML files, named here exactly as the protocol names them.

#include "platform/linux/Wayland.h"
#include "platform/linux/WindowWayland.h"
#include "platform/linux/Xkb.h"
#include "platform/Touch.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace sashfold::platform {

namespace {

using wayland::Connection;
using wayland::Message;
using wayland::Request;

// --- Protocol vocabulary ----------------------------------------------------

namespace wl_display {
    constexpr std::uint16_t get_registry = 1;
}
namespace wl_registry {
    constexpr std::uint16_t bind = 0;
    constexpr std::uint16_t event_global = 0;
}
namespace wl_compositor {
    constexpr std::uint16_t create_surface = 0;
}
namespace wl_shm {
    constexpr std::uint16_t create_pool = 0;
    constexpr std::uint32_t format_argb8888 = 0;
    constexpr std::uint32_t format_xrgb8888 = 1;
}
namespace wl_shm_pool {
    constexpr std::uint16_t create_buffer = 0;
    constexpr std::uint16_t destroy = 1;
}
namespace wl_buffer {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t event_release = 0;
}
namespace wl_surface {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t attach = 1;
    constexpr std::uint16_t damage = 2;
    constexpr std::uint16_t frame = 3;
    constexpr std::uint16_t commit = 6;
    constexpr std::uint16_t set_buffer_scale = 8; // since 3
    constexpr std::uint16_t damage_buffer = 9; // since 4
    constexpr std::uint16_t event_enter = 0;
    constexpr std::uint16_t event_leave = 1;
    constexpr std::uint16_t event_preferred_buffer_scale = 2; // since 6
}
namespace wl_output {
    constexpr std::uint16_t event_scale = 3; // since 2
}
namespace wp_viewporter {
    constexpr std::uint16_t get_viewport = 1;
}
namespace wp_viewport {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t set_destination = 2;
}
namespace wp_fractional_scale_manager_v1 {
    constexpr std::uint16_t get_fractional_scale = 1;
}
namespace wp_fractional_scale_v1 {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t event_preferred_scale = 0; // the scale, times 120
}
namespace wl_seat {
    constexpr std::uint16_t get_pointer = 0;
    constexpr std::uint16_t get_keyboard = 1;
    constexpr std::uint16_t get_touch = 2;
    constexpr std::uint16_t event_capabilities = 0;
    constexpr std::uint32_t capability_pointer = 1;
    constexpr std::uint32_t capability_keyboard = 2;
    constexpr std::uint32_t capability_touch = 4;
}
namespace wl_touch {
    constexpr std::uint16_t event_down = 0;
    constexpr std::uint16_t event_up = 1;
    constexpr std::uint16_t event_motion = 2;
    constexpr std::uint16_t event_cancel = 4;
}
namespace zwp_text_input_manager_v3 {
    constexpr std::uint16_t get_text_input = 1;
}
namespace zwp_text_input_v3 {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t enable = 1;
    constexpr std::uint16_t disable = 2;
    constexpr std::uint16_t set_content_type = 5;
    constexpr std::uint16_t set_cursor_rectangle = 6;
    constexpr std::uint16_t commit = 7;
    constexpr std::uint16_t event_enter = 0;
    constexpr std::uint16_t event_leave = 1;
    constexpr std::uint16_t event_preedit_string = 2;
    constexpr std::uint16_t event_commit_string = 3;
    constexpr std::uint16_t event_delete_surrounding_text = 4;
    constexpr std::uint16_t event_done = 5;
    constexpr std::uint32_t content_hint_none = 0;
    constexpr std::uint32_t content_purpose_normal = 0;
}
namespace wl_pointer {
    constexpr std::uint16_t event_enter = 0;
    constexpr std::uint16_t event_leave = 1;
    constexpr std::uint16_t event_motion = 2;
    constexpr std::uint16_t event_button = 3;
    constexpr std::uint16_t event_axis = 4;
    constexpr std::uint16_t event_frame = 5;
    constexpr std::uint16_t event_axis_discrete = 8;
    constexpr std::uint16_t event_axis_value120 = 9;
    constexpr std::uint32_t axis_vertical_scroll = 0;
    constexpr std::uint32_t button_state_pressed = 1;
    constexpr std::uint32_t btn_left = 0x110; // linux/input-event-codes.h
    constexpr std::uint32_t btn_right = 0x111;
    constexpr std::uint32_t btn_middle = 0x112;
}
namespace wl_keyboard {
    constexpr std::uint16_t event_keymap = 0;
    constexpr std::uint16_t event_enter = 1;
    constexpr std::uint16_t event_leave = 2;
    constexpr std::uint16_t event_key = 3;
    constexpr std::uint16_t event_modifiers = 4;
    constexpr std::uint16_t event_repeat_info = 5;
    constexpr std::uint32_t keymap_format_xkb_v1 = 1;
    constexpr std::uint32_t key_state_pressed = 1;
}
namespace wl_data_device_manager {
    constexpr std::uint16_t create_data_source = 0;
    constexpr std::uint16_t get_data_device = 1;
}
namespace wl_data_device {
    constexpr std::uint16_t set_selection = 1;
    constexpr std::uint16_t event_data_offer = 0;
    constexpr std::uint16_t event_enter = 1;
    constexpr std::uint16_t event_selection = 5;
}
namespace wl_data_offer {
    constexpr std::uint16_t receive = 1;
    constexpr std::uint16_t destroy = 2;
    constexpr std::uint16_t event_offer = 0;
}
namespace wl_data_source {
    constexpr std::uint16_t offer = 0;
    constexpr std::uint16_t destroy = 1;
    constexpr std::uint16_t event_send = 1;
    constexpr std::uint16_t event_cancelled = 2;
}
namespace xdg_wm_base {
    constexpr std::uint16_t get_xdg_surface = 2;
    constexpr std::uint16_t pong = 3;
    constexpr std::uint16_t event_ping = 0;
}
namespace xdg_surface {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t get_toplevel = 1;
    constexpr std::uint16_t ack_configure = 4;
    constexpr std::uint16_t event_configure = 0;
}
namespace xdg_toplevel {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t set_title = 2;
    constexpr std::uint16_t set_app_id = 3;
    constexpr std::uint16_t move = 5;
    constexpr std::uint16_t resize = 6;
    constexpr std::uint16_t set_min_size = 8;
    constexpr std::uint16_t set_maximized = 9;
    constexpr std::uint16_t unset_maximized = 10;
    constexpr std::uint16_t set_minimized = 12;
    constexpr std::uint16_t event_configure = 0;
    constexpr std::uint16_t event_close = 1;
    constexpr std::uint16_t event_configure_bounds = 2; // since 4
    constexpr std::uint32_t state_maximized = 1;
    constexpr std::uint32_t edge_top = 1;
    constexpr std::uint32_t edge_bottom = 2;
    constexpr std::uint32_t edge_left = 4;
    constexpr std::uint32_t edge_right = 8;
}
namespace zxdg_decoration_manager_v1 {
    constexpr std::uint16_t get_toplevel_decoration = 1;
}
namespace zxdg_toplevel_decoration_v1 {
    constexpr std::uint16_t set_mode = 1;
    constexpr std::uint16_t event_configure = 0;
    constexpr std::uint32_t mode_client_side = 1;
    constexpr std::uint32_t mode_server_side = 2;
}
namespace wp_cursor_shape_manager_v1 {
    constexpr std::uint16_t get_pointer = 1;
}
namespace wp_cursor_shape_device_v1 {
    constexpr std::uint16_t set_shape = 1;
    constexpr std::uint32_t shape_default = 1;
    constexpr std::uint32_t shape_pointer = 4;
    constexpr std::uint32_t shape_text = 9;
}
namespace xdg_toplevel_icon_manager_v1 {
    constexpr std::uint16_t create_icon = 1;
    constexpr std::uint16_t set_icon = 2;
    constexpr std::uint16_t event_icon_size = 0;
}
namespace xdg_toplevel_icon_v1 {
    constexpr std::uint16_t destroy = 0;
    constexpr std::uint16_t set_name = 1;
    constexpr std::uint16_t add_buffer = 2;
}

constexpr char const* app_id = "sashfold"; // matches sashfold.desktop: the icon and name the shell shows
constexpr int icon_fallback_sizes[] = { 16, 32, 48, 64, 128, 256 };

bool debug_enabled()
{
    static bool const enabled = [] {
        char const* value = std::getenv("SASHFOLD_WAYLAND_DEBUG");
        return value && *value && *value != '0';
    }();
    return enabled;
}

template<typename... Args>
void debug(char const* format, Args... args)
{
    if (debug_enabled()) {
        std::fprintf(stderr, "wayland: ");
        if constexpr (sizeof...(Args) == 0)
            std::fputs(format, stderr); // a line with nothing to format is printed as it is
        else
            std::fprintf(stderr, format, args...);
        std::fputc('\n', stderr);
    }
}

void* const mmap_failed = reinterpret_cast<void*>(static_cast<std::intptr_t>(-1));

// A memfd the compositor maps too.
struct SharedMemory {
    int fd = -1;
    std::uint8_t* map = nullptr;
    std::size_t size = 0;

    bool create(std::size_t bytes)
    {
        fd = memfd_create("sashfold-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0)
            return false;
        if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            release();
            return false;
        }
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL); // a pool the compositor can trust not to shrink
        void* const mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == mmap_failed) {
            release();
            return false;
        }
        map = static_cast<std::uint8_t*>(mapped);
        size = bytes;
        return true;
    }

    void release()
    {
        if (map)
            munmap(map, size);
        if (fd >= 0)
            ::close(fd);
        map = nullptr;
        size = 0;
        fd = -1;
    }
};

Key named_key(std::uint32_t keysym)
{
    switch (keysym) {
    case 0xff0d: // Return
    case 0xff8d: // KP_Enter
        return Key::Enter;
    case 0xff1b: return Key::Escape;
    case 0xff08: return Key::Backspace;
    case 0xffff: // Delete
    case 0xff9f: // KP_Delete
        return Key::Delete;
    case 0xff09: // Tab
    case 0xfe20: // ISO_Left_Tab, what Shift+Tab produces
        return Key::Tab;
    case 0x0020: // space
    case 0xff80: // KP_Space
        return Key::Space;
    case 0xff51: case 0xff96: return Key::Left;
    case 0xff52: case 0xff97: return Key::Up;
    case 0xff53: case 0xff98: return Key::Right;
    case 0xff54: case 0xff99: return Key::Down;
    case 0xff50: case 0xff95: return Key::Home;
    case 0xff57: case 0xff9c: return Key::End;
    case 0xff55: case 0xff9a: return Key::PageUp;
    case 0xff56: case 0xff9b: return Key::PageDown;
    case 0xffc2: return Key::F5;
    case 0xffc9: return Key::F12;
    default: return Key::None;
    }
}

// The letter a shortcut is named by: the key's unshifted symbol when that
// is a Latin letter or digit (Ctrl+L on any layout that has an L).
char32_t shortcut_letter(std::uint32_t keysym)
{
    if (keysym >= 'a' && keysym <= 'z')
        return static_cast<char32_t>(keysym - 'a' + 'A');
    if ((keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9'))
        return static_cast<char32_t>(keysym);
    return 0;
}

// The icon squared and box-filtered to `size`, premultiplied ARGB8888 as
// wl_shm wants it (bytes B, G, R, A on a little-endian machine).
std::vector<std::uint8_t> scaled_icon(Bitmap const& icon, int size)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    int const side = std::max(icon.width(), icon.height());
    if (side <= 0 || size <= 0)
        return out;
    int const offset_x = (side - icon.width()) / 2;
    int const offset_y = (side - icon.height()) / 2;
    std::vector<std::uint8_t> const& src = icon.pixels();
    for (int y = 0; y < size; ++y) {
        int const sy0 = y * side / size;
        int const sy1 = std::max(sy0 + 1, (y + 1) * side / size);
        for (int x = 0; x < size; ++x) {
            int const sx0 = x * side / size;
            int const sx1 = std::max(sx0 + 1, (x + 1) * side / size);
            std::uint64_t r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    ++count;
                    int const ix = sx - offset_x;
                    int const iy = sy - offset_y;
                    if (ix < 0 || iy < 0 || ix >= icon.width() || iy >= icon.height())
                        continue; // the padding: transparent
                    std::size_t const at = (static_cast<std::size_t>(iy) * static_cast<std::size_t>(icon.width())
                                               + static_cast<std::size_t>(ix))
                        * 4;
                    std::uint64_t const alpha = src[at + 3];
                    r += src[at + 0] * alpha;
                    g += src[at + 1] * alpha;
                    b += src[at + 2] * alpha;
                    a += alpha * 255;
                }
            }
            std::uint64_t const divisor = count * 255;
            std::size_t const at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size)
                                       + static_cast<std::size_t>(x))
                * 4;
            out[at + 0] = static_cast<std::uint8_t>(b / divisor);
            out[at + 1] = static_cast<std::uint8_t>(g / divisor);
            out[at + 2] = static_cast<std::uint8_t>(r / divisor);
            out[at + 3] = static_cast<std::uint8_t>(a / divisor);
        }
    }
    return out;
}

} // namespace

class WaylandWindow final : public Window {
public:
    static std::unique_ptr<Window> open(std::string const& title, int width, int height, Bitmap const* icon);
    ~WaylandWindow() override;

    bool poll(WindowEvent& event) override;
    void wait(int timeout_ms) override;
    void present(Bitmap const& frame) override;
    void set_title(std::string const& title) override;
    void set_cursor(Cursor cursor) override;
    void set_text_input(std::optional<Rect> const& caret) override;
    bool wants_client_decorations() const override { return m_client_decorations; }
    void begin_move() override;
    void begin_resize(WindowEdge edge) override;
    void minimize() override;
    void toggle_maximize() override;
    // The buffer's size: the logical size the compositor configured, at the
    // display's scale.
    int width() const override { return m_buffer_width; }
    int height() const override { return m_buffer_height; }
    float scale() const override { return static_cast<float>(m_scale); }

    bool write_clipboard(std::string const& utf8);
    std::optional<std::string> read_clipboard();

private:
    struct Global {
        std::uint32_t name = 0;
        std::uint32_t version = 0;
    };
    struct FrameBuffer {
        SharedMemory memory;
        std::uint32_t pool = 0;
        std::uint32_t buffer = 0;
        int width = 0;
        int height = 0;
        bool busy = false; // attached, and the compositor has not released it
    };

    WaylandWindow(std::unique_ptr<Connection> connection, int width, int height);
    bool setup(std::string const& title, Bitmap const* icon, std::string& error);
    std::uint32_t bind(std::string const& interface, std::uint32_t version);
    void send(Request& request) { m_connection->send(request); }
    void pump(int timeout_ms);
    void fail();
    void push(WindowEvent const& event) { m_events.push_back(event); }

    void listen_seat();
    void listen_pointer();
    void listen_keyboard();
    void listen_touch();
    void listen_data_device();
    void listen_text_input();
    void sync_text_input();
    void apply_text_input_done();
    void listen_surface();
    void bind_outputs();
    void output_scale_changed();
    void apply_scale(double scale);
    void size_changed();
    void upload_icon(Bitmap const& icon);
    bool ensure_frame_buffer(FrameBuffer& frame, int width, int height);
    void release_frame_buffer(FrameBuffer& frame);
    void apply_cursor();
    void emit_key(std::uint32_t keycode);
    void repeat_keys();
    void push_wheel();
    void destroy_offer(std::uint32_t id);

    std::unique_ptr<Connection> m_connection;
    std::map<std::string, Global> m_globals;
    std::uint32_t m_registry = 0;
    std::uint32_t m_compositor = 0;
    std::uint32_t m_compositor_version = 0;
    std::uint32_t m_shm = 0;
    std::uint32_t m_wm_base = 0;
    std::uint32_t m_seat = 0;
    std::uint32_t m_decoration_manager = 0;
    std::uint32_t m_cursor_shape_manager = 0;
    std::uint32_t m_icon_manager = 0;
    std::uint32_t m_data_device_manager = 0;
    std::uint32_t m_surface = 0;
    std::uint32_t m_xdg_surface = 0;
    std::uint32_t m_toplevel = 0;
    std::uint32_t m_decoration = 0;
    std::uint32_t m_pointer = 0;
    std::uint32_t m_keyboard = 0;
    std::uint32_t m_touch = 0;
    TouchGestures m_fingers; // a touchscreen's fingers, as pointer and scroll events
    std::uint32_t m_cursor_device = 0;
    // The input method (text-input-v3): enabled while the shell has a
    // caret to compose at, told where that caret is; what the method sends
    // is gathered and applied at its `done`.
    std::uint32_t m_text_input_manager = 0;
    std::uint32_t m_text_input = 0;
    bool m_text_input_entered = false; // the method's focus is on this surface
    bool m_text_input_enabled = false;
    std::optional<Rect> m_text_caret; // buffer pixels
    std::uint32_t m_text_input_serial = 0; // commits sent, which `done` echoes
    std::string m_pending_preedit;
    bool m_pending_preedit_set = false;
    std::string m_pending_commit;
    std::uint32_t m_data_device = 0;
    std::uint32_t m_viewporter = 0;
    std::uint32_t m_viewport = 0;
    std::uint32_t m_fractional_scale_manager = 0;
    std::uint32_t m_fractional_scale = 0;
    std::vector<std::uint32_t> m_output_names; // every wl_output the registry announced
    std::map<std::uint32_t, int> m_output_scales; // by bound object: the output's whole-number scale
    std::uint32_t m_entered_output = 0; // the output the surface was last told it is on

    // The logical size the compositor configured, in surface units, and the
    // display's scale: the buffer is the one times the other, and every
    // pointer position is scaled the same way before the shell sees it.
    // With the viewporter the buffer may be any size and the surface stays
    // at the logical one (a fractional scale needs that); without it the
    // whole-number scale goes on the surface and the buffer is that many
    // times the logical size.
    int m_width;
    int m_height;
    double m_scale = 1;
    int m_buffer_width;
    int m_buffer_height;
    int m_buffer_scale = 1; // the whole-number scale on the surface, without a viewport
    int m_pending_width = 0;
    int m_pending_height = 0;
    bool m_configured = false;
    bool m_closed = false;
    // The frame: the compositor's through xdg-decoration where it offers
    // one and agrees to draw it, else the shell's.
    bool m_client_decorations = false;
    bool m_maximized = false; // from the toplevel's configure states
    std::deque<WindowEvent> m_events;

    FrameBuffer m_frames[2];
    bool m_frame_callback_pending = false;
    SharedMemory m_icon_memory;
    std::vector<std::uint32_t> m_icon_buffers;
    std::vector<int> m_icon_sizes;

    // Pointer
    int m_pointer_x = 0;
    int m_pointer_y = 0;
    std::uint32_t m_pointer_enter_serial = 0;
    bool m_pointer_inside = false;
    Cursor m_cursor = Cursor::Arrow;
    double m_axis_value = 0;
    int m_axis_discrete = 0;
    int m_axis_value120 = 0;
    int m_wheel_remainder120 = 0;
    double m_wheel_remainder = 0;

    // Keyboard
    std::optional<xkb::Keymap> m_keymap;
    std::uint32_t m_alt_mask = xkb::mod1_mask;
    std::uint32_t m_mods = 0;
    std::uint32_t m_group = 0;
    bool m_keyboard_focus = false;
    int m_repeat_rate = 25;
    int m_repeat_delay_ms = 600;
    std::uint32_t m_repeat_keycode = 0;
    std::chrono::steady_clock::time_point m_repeat_due;
    std::uint32_t m_input_serial = 0; // the latest key or button: what set_selection wants

    // Clipboard
    std::uint32_t m_data_source = 0;
    std::string m_clipboard_text;
    bool m_selection_is_ours = false;
    std::uint32_t m_selection_offer = 0;
    std::map<std::uint32_t, std::vector<std::string>> m_offers; // id -> mime types
};

namespace {
WaylandWindow* g_window = nullptr;
}

WaylandWindow* wayland_window()
{
    return g_window;
}

bool wayland_write_clipboard_text(WaylandWindow& window, std::string const& utf8)
{
    return window.write_clipboard(utf8);
}

std::optional<std::string> wayland_read_clipboard_text(WaylandWindow& window)
{
    return window.read_clipboard();
}

// --- Setup ------------------------------------------------------------------

WaylandWindow::WaylandWindow(std::unique_ptr<Connection> connection, int width, int height)
    : m_connection(std::move(connection))
    , m_width(width)
    , m_height(height)
    , m_buffer_width(width)
    , m_buffer_height(height)
{
}

WaylandWindow::~WaylandWindow()
{
    if (g_window == this)
        g_window = nullptr;
    for (FrameBuffer& frame : m_frames)
        release_frame_buffer(frame);
    m_icon_memory.release();
    if (!m_connection->failed()) {
        if (m_text_input) {
            Request destroy(m_text_input, zwp_text_input_v3::destroy);
            send(destroy);
        }
        if (m_fractional_scale) {
            Request destroy(m_fractional_scale, wp_fractional_scale_v1::destroy);
            send(destroy);
        }
        if (m_viewport) {
            Request destroy(m_viewport, wp_viewport::destroy);
            send(destroy);
        }
        if (m_toplevel) {
            Request destroy(m_toplevel, xdg_toplevel::destroy);
            send(destroy);
        }
        if (m_xdg_surface) {
            Request destroy(m_xdg_surface, xdg_surface::destroy);
            send(destroy);
        }
        if (m_surface) {
            Request destroy(m_surface, wl_surface::destroy);
            send(destroy);
        }
        m_connection->flush();
    }
}

std::unique_ptr<Window> WaylandWindow::open(std::string const& title, int width, int height, Bitmap const* icon)
{
    std::string error;
    std::unique_ptr<Connection> connection = Connection::connect(error);
    if (!connection) {
        std::fprintf(stderr, "sashfold: no Wayland display: %s\n", error.c_str());
        return nullptr;
    }
    std::unique_ptr<WaylandWindow> window(new WaylandWindow(std::move(connection), width, height));
    if (!window->setup(title, icon, error)) {
        std::fprintf(stderr, "sashfold: cannot open a Wayland window: %s\n", error.c_str());
        return nullptr;
    }
    g_window = window.get();
    return window;
}

std::uint32_t WaylandWindow::bind(std::string const& interface, std::uint32_t version)
{
    auto const found = m_globals.find(interface);
    if (found == m_globals.end())
        return 0;
    std::uint32_t const chosen = std::min(version, found->second.version);
    std::uint32_t const id = m_connection->allocate_id();
    Request request(m_registry, wl_registry::bind);
    request.uint(found->second.name).string(interface).uint(chosen).new_id(id);
    send(request);
    debug("bound %s version %u as object %u", interface.c_str(), chosen, id);
    return id;
}

bool WaylandWindow::setup(std::string const& title, Bitmap const* icon, std::string& error)
{
    m_registry = m_connection->allocate_id();
    m_connection->listen(m_registry, [this](std::uint16_t opcode, Message& message) {
        if (opcode == wl_registry::event_global) {
            std::uint32_t const name = message.uint();
            std::string const interface = message.string();
            std::uint32_t const version = message.uint();
            if (!m_globals.contains(interface)) // the first of a kind is ours
                m_globals[interface] = { name, version };
            if (interface == "wl_output" && version >= 2)
                m_output_names.push_back(name); // every output: the window may land on any
        }
    });
    Request get_registry(Connection::display_id, wl_display::get_registry);
    get_registry.new_id(m_registry);
    send(get_registry);
    if (!m_connection->roundtrip()) {
        error = m_connection->error();
        return false;
    }

    m_compositor = bind("wl_compositor", 6);
    m_compositor_version = std::min(6u, m_globals["wl_compositor"].version);
    m_shm = bind("wl_shm", 1);
    m_wm_base = bind("xdg_wm_base", 6);
    for (auto const& [required, id] : { std::pair("wl_compositor", m_compositor), std::pair("wl_shm", m_shm),
             std::pair("xdg_wm_base", m_wm_base) }) {
        if (id == 0) {
            error = std::string("the compositor does not offer ") + required;
            return false;
        }
    }
    m_connection->listen(m_wm_base, [this](std::uint16_t opcode, Message& message) {
        if (opcode == xdg_wm_base::event_ping) {
            Request pong(m_wm_base, xdg_wm_base::pong);
            pong.uint(message.uint());
            send(pong);
        }
    });
    m_seat = bind("wl_seat", 9);
    if (m_seat)
        listen_seat();
    m_text_input_manager = bind("zwp_text_input_manager_v3", 1);
    if (m_text_input_manager && m_seat) {
        m_text_input = m_connection->allocate_id();
        Request get_text_input(m_text_input_manager, zwp_text_input_manager_v3::get_text_input);
        get_text_input.new_id(m_text_input).object(m_seat);
        send(get_text_input);
        listen_text_input();
    }
    // SASHFOLD_WAYLAND_CSD leaves the compositor's title bar unasked for, to
    // see the shell's own frame on a compositor that would draw one.
    char const* const force_client_frame = std::getenv("SASHFOLD_WAYLAND_CSD");
    bool const client_frame_wanted = force_client_frame && *force_client_frame && *force_client_frame != '0';
    m_decoration_manager = client_frame_wanted ? 0 : bind("zxdg_decoration_manager_v1", 1);
    m_cursor_shape_manager = bind("wp_cursor_shape_manager_v1", 1);
    m_data_device_manager = bind("wl_data_device_manager", 3);
    if (m_data_device_manager && m_seat)
        listen_data_device();
    m_icon_manager = bind("xdg_toplevel_icon_manager_v1", 1);
    if (m_icon_manager) {
        m_connection->listen(m_icon_manager, [this](std::uint16_t opcode, Message& message) {
            if (opcode == xdg_toplevel_icon_manager_v1::event_icon_size)
                m_icon_sizes.push_back(message.int_());
        });
    }
    // The display's scale: fractional-scale-v1 with the viewporter names it
    // exactly (1.5 on a laptop panel, say); a compositor without them but
    // with wl_compositor 6 says a whole number through the surface; older
    // ones leave the window at 1.
    m_viewporter = bind("wp_viewporter", 1);
    m_fractional_scale_manager = m_viewporter ? bind("wp_fractional_scale_manager_v1", 1) : 0;
    bind_outputs();

    m_surface = m_connection->allocate_id();
    Request create_surface(m_compositor, wl_compositor::create_surface);
    create_surface.new_id(m_surface);
    send(create_surface);
    listen_surface();
    if (m_viewporter) {
        m_viewport = m_connection->allocate_id();
        Request get_viewport(m_viewporter, wp_viewporter::get_viewport);
        get_viewport.new_id(m_viewport).object(m_surface);
        send(get_viewport);
    }
    if (m_fractional_scale_manager) {
        m_fractional_scale = m_connection->allocate_id();
        Request get_scale(m_fractional_scale_manager, wp_fractional_scale_manager_v1::get_fractional_scale);
        get_scale.new_id(m_fractional_scale).object(m_surface);
        send(get_scale);
        m_connection->listen(m_fractional_scale, [this](std::uint16_t opcode, Message& message) {
            if (opcode == wp_fractional_scale_v1::event_preferred_scale)
                apply_scale(static_cast<double>(message.uint()) / 120.0);
        });
    }

    m_xdg_surface = m_connection->allocate_id();
    Request get_xdg_surface(m_wm_base, xdg_wm_base::get_xdg_surface);
    get_xdg_surface.new_id(m_xdg_surface).object(m_surface);
    send(get_xdg_surface);
    m_connection->listen(m_xdg_surface, [this](std::uint16_t opcode, Message& message) {
        if (opcode != xdg_surface::event_configure)
            return;
        std::uint32_t const serial = message.uint();
        Request ack(m_xdg_surface, xdg_surface::ack_configure);
        ack.uint(serial);
        send(ack);
        if (m_pending_width > 0 && m_pending_height > 0
            && (m_pending_width != m_width || m_pending_height != m_height)) {
            m_width = m_pending_width;
            m_height = m_pending_height;
            size_changed();
        }
        m_pending_width = 0;
        m_pending_height = 0;
        debug("configure serial %u: %d x %d at scale %.3f (buffer %d x %d)", serial, m_width, m_height, m_scale,
            m_buffer_width, m_buffer_height);
        m_configured = true;
    });

    m_toplevel = m_connection->allocate_id();
    Request get_toplevel(m_xdg_surface, xdg_surface::get_toplevel);
    get_toplevel.new_id(m_toplevel);
    send(get_toplevel);
    m_connection->listen(m_toplevel, [this](std::uint16_t opcode, Message& message) {
        switch (opcode) {
        case xdg_toplevel::event_configure: {
            m_pending_width = message.int_();
            m_pending_height = message.int_();
            // The states: whether the window is maximized is what the
            // shell's own maximize button toggles against.
            std::span<std::uint8_t const> const states = message.array();
            bool maximized = false;
            for (std::size_t i = 0; i + 4 <= states.size(); i += 4) {
                std::uint32_t const state = static_cast<std::uint32_t>(states[i]) | (static_cast<std::uint32_t>(states[i + 1]) << 8)
                    | (static_cast<std::uint32_t>(states[i + 2]) << 16) | (static_cast<std::uint32_t>(states[i + 3]) << 24);
                if (state == xdg_toplevel::state_maximized)
                    maximized = true;
            }
            m_maximized = maximized;
            break;
        }
        case xdg_toplevel::event_close: {
            WindowEvent event;
            event.kind = WindowEvent::Kind::Close;
            push(event);
            break;
        }
        case xdg_toplevel::event_configure_bounds: {
            int const bound_width = message.int_();
            int const bound_height = message.int_();
            if (!m_configured && bound_width > 0 && bound_height > 0
                && (bound_width < m_width || bound_height < m_height)) {
                m_width = std::min(m_width, bound_width);
                m_height = std::min(m_height, bound_height);
                size_changed(); // the buffer and the viewport follow the smaller window
            }
            break;
        }
        default:
            break;
        }
    });
    set_title(title);
    Request set_app_id(m_toplevel, xdg_toplevel::set_app_id);
    set_app_id.string(app_id);
    send(set_app_id);
    Request set_min_size(m_toplevel, xdg_toplevel::set_min_size);
    set_min_size.int_(320).int_(240);
    send(set_min_size);

    // The frame: the compositor's where it offers xdg-decoration and agrees
    // to draw it; the shell's on a compositor without the protocol (GNOME),
    // or one that answers client-side.
    m_client_decorations = m_decoration_manager == 0;
    if (m_decoration_manager) {
        m_decoration = m_connection->allocate_id();
        Request get_decoration(m_decoration_manager, zxdg_decoration_manager_v1::get_toplevel_decoration);
        get_decoration.new_id(m_decoration).object(m_toplevel);
        send(get_decoration);
        m_connection->listen(m_decoration, [this](std::uint16_t opcode, Message& message) {
            if (opcode != zxdg_toplevel_decoration_v1::event_configure)
                return;
            std::uint32_t const mode = message.uint();
            m_client_decorations = mode == zxdg_toplevel_decoration_v1::mode_client_side;
            debug("decoration mode %u (2 = the compositor's, 1 = the shell's)", mode);
        });
        Request set_mode(m_decoration, zxdg_toplevel_decoration_v1::set_mode);
        set_mode.uint(zxdg_toplevel_decoration_v1::mode_server_side);
        send(set_mode);
    }

    // The seat's capabilities and the icon sizes arrive in this roundtrip.
    if (!m_connection->roundtrip()) {
        error = m_connection->error();
        return false;
    }
    if (m_icon_manager && icon)
        upload_icon(*icon);

    // The initial commit carries no buffer; the compositor answers with the
    // first configure, and only then may a frame be attached.
    Request commit(m_surface, wl_surface::commit);
    send(commit);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_configured && !m_connection->failed() && std::chrono::steady_clock::now() < deadline)
        m_connection->dispatch(100);
    if (m_connection->failed()) {
        error = m_connection->error();
        return false;
    }
    if (!m_configured) {
        error = "the compositor never configured the window";
        return false;
    }
    return true;
}

// The surface's own events: a whole-number preferred scale (wl_compositor
// 6) stands in when the fractional protocol is not offered, and the output
// the surface enters is the last resort, its own scale.
void WaylandWindow::listen_surface()
{
    m_connection->listen(m_surface, [this](std::uint16_t opcode, Message& message) {
        if (opcode == wl_surface::event_preferred_buffer_scale) {
            int const preferred = message.int_();
            debug("the compositor prefers buffer scale %d", preferred);
            if (!m_fractional_scale)
                apply_scale(static_cast<double>(preferred));
        } else if (opcode == wl_surface::event_enter) {
            m_entered_output = message.object();
            auto const known = m_output_scales.find(m_entered_output);
            debug("entered output %u (scale %d)", m_entered_output, known == m_output_scales.end() ? 0 : known->second);
            output_scale_changed();
        } else if (opcode == wl_surface::event_leave) {
            if (message.object() == m_entered_output)
                m_entered_output = 0;
        }
    });
}

// Every output the registry announced, bound for its scale event: what
// the window is scaled by when the compositor offers neither the
// fractional protocol nor a preferred buffer scale, and what the debug
// log says about the display either way.
void WaylandWindow::bind_outputs()
{
    for (std::uint32_t const name : m_output_names) {
        std::uint32_t const id = m_connection->allocate_id();
        Request request(m_registry, wl_registry::bind);
        request.uint(name).string("wl_output").uint(2).new_id(id);
        send(request);
        m_output_scales[id] = 1;
        m_connection->listen(id, [this, id](std::uint16_t opcode, Message& message) {
            if (opcode != wl_output::event_scale)
                return;
            m_output_scales[id] = message.int_();
            debug("output %u has scale %d", id, m_output_scales[id]);
            if (m_entered_output == id)
                output_scale_changed();
        });
    }
}

void WaylandWindow::output_scale_changed()
{
    if (m_fractional_scale || m_compositor_version >= 6)
        return; // a better source speaks for the display
    auto const known = m_output_scales.find(m_entered_output);
    if (known != m_output_scales.end() && known->second >= 1)
        apply_scale(static_cast<double>(known->second));
}

// The display's scale from here on: the buffer follows the logical size
// at it, the shell hears the new scale and then the new size.
void WaylandWindow::apply_scale(double scale)
{
    if (!(scale > 0) || scale > 8 || scale == m_scale)
        return;
    m_scale = scale;
    debug("scale %.3f", m_scale);
    m_fingers.set_slop(static_cast<int>(std::lround(10 * m_scale)));
    WindowEvent event;
    event.kind = WindowEvent::Kind::Scale;
    event.scale = static_cast<float>(m_scale);
    push(event);
    size_changed();
}

// The logical size or the scale changed: the buffer size follows, the
// surface is told how the buffer maps onto it, and the shell hears the
// size the next frame must have.
void WaylandWindow::size_changed()
{
    if (m_viewport) {
        m_buffer_width = std::max(1, static_cast<int>(std::lround(m_width * m_scale)));
        m_buffer_height = std::max(1, static_cast<int>(std::lround(m_height * m_scale)));
        Request destination(m_viewport, wp_viewport::set_destination);
        destination.int_(m_width).int_(m_height);
        send(destination);
    } else {
        int const whole = std::clamp(static_cast<int>(std::lround(m_scale)), 1, 8);
        if (whole != m_buffer_scale && m_compositor_version >= 3) {
            m_buffer_scale = whole;
            Request set_scale(m_surface, wl_surface::set_buffer_scale);
            set_scale.int_(whole);
            send(set_scale);
        }
        m_buffer_width = m_width * m_buffer_scale;
        m_buffer_height = m_height * m_buffer_scale;
    }
    WindowEvent event;
    event.kind = WindowEvent::Kind::Resize;
    event.width = m_buffer_width;
    event.height = m_buffer_height;
    push(event);
}

void WaylandWindow::listen_seat()
{
    m_connection->listen(m_seat, [this](std::uint16_t opcode, Message& message) {
        if (opcode != wl_seat::event_capabilities)
            return;
        std::uint32_t const capabilities = message.uint();
        if ((capabilities & wl_seat::capability_pointer) && !m_pointer) {
            m_pointer = m_connection->allocate_id();
            Request get_pointer(m_seat, wl_seat::get_pointer);
            get_pointer.new_id(m_pointer);
            send(get_pointer);
            listen_pointer();
            if (m_cursor_shape_manager) {
                m_cursor_device = m_connection->allocate_id();
                Request get_device(m_cursor_shape_manager, wp_cursor_shape_manager_v1::get_pointer);
                get_device.new_id(m_cursor_device).object(m_pointer);
                send(get_device);
            }
        }
        if ((capabilities & wl_seat::capability_keyboard) && !m_keyboard) {
            m_keyboard = m_connection->allocate_id();
            Request get_keyboard(m_seat, wl_seat::get_keyboard);
            get_keyboard.new_id(m_keyboard);
            send(get_keyboard);
            listen_keyboard();
        }
        if ((capabilities & wl_seat::capability_touch) && !m_touch) {
            m_touch = m_connection->allocate_id();
            Request get_touch(m_seat, wl_seat::get_touch);
            get_touch.new_id(m_touch);
            send(get_touch);
            listen_touch();
        }
    });
}

// A touchscreen: each finger's position in surface units, scaled to buffer
// pixels and told to the gesture reader, whose events go out at the frame.
void WaylandWindow::listen_touch()
{
    m_fingers.set_slop(static_cast<int>(std::lround(10 * m_scale)));
    m_connection->listen(m_touch, [this](std::uint16_t opcode, Message& message) {
        std::vector<WindowEvent> out;
        switch (opcode) {
        case wl_touch::event_down: {
            m_input_serial = message.uint();
            message.uint(); // time
            message.object(); // the surface: ours, the only one
            int const id = message.int_();
            int const x = static_cast<int>(std::floor(message.fixed() * m_scale));
            int const y = static_cast<int>(std::floor(message.fixed() * m_scale));
            m_fingers.down(id, x, y, out);
            break;
        }
        case wl_touch::event_up: {
            m_input_serial = message.uint();
            message.uint(); // time
            m_fingers.up(message.int_(), out);
            break;
        }
        case wl_touch::event_motion: {
            message.uint(); // time
            int const id = message.int_();
            int const x = static_cast<int>(std::floor(message.fixed() * m_scale));
            int const y = static_cast<int>(std::floor(message.fixed() * m_scale));
            m_fingers.motion(id, x, y, out);
            break;
        }
        case wl_touch::event_cancel:
            m_fingers.cancel(out);
            break;
        default:
            break; // frame, shape, orientation
        }
        for (WindowEvent const& event : out)
            push(event);
    });
}

// The input method, text-input-v3: while the shell has a caret to compose
// at, the method is enabled and told where the caret is; what it sends —
// composing text, text to commit — is gathered and applied at its `done`.
// Surrounding text is never offered, so a request to delete some has
// nothing to refer to and is read past.
void WaylandWindow::listen_text_input()
{
    m_connection->listen(m_text_input, [this](std::uint16_t opcode, Message& message) {
        switch (opcode) {
        case zwp_text_input_v3::event_enter:
            message.object(); // the surface: ours
            m_text_input_entered = true;
            m_text_input_enabled = false;
            debug("text input: the input method is here");
            sync_text_input();
            break;
        case zwp_text_input_v3::event_leave:
            message.object();
            m_text_input_entered = false;
            m_text_input_enabled = false;
            debug("text input: the input method left");
            break;
        case zwp_text_input_v3::event_preedit_string:
            m_pending_preedit = message.string();
            message.int_(); // cursor_begin
            message.int_(); // cursor_end
            m_pending_preedit_set = true;
            break;
        case zwp_text_input_v3::event_commit_string:
            m_pending_commit = message.string();
            break;
        case zwp_text_input_v3::event_delete_surrounding_text:
            message.uint(); // before
            message.uint(); // after: nothing was offered to delete
            break;
        case zwp_text_input_v3::event_done: {
            std::uint32_t const serial = message.uint();
            debug("text input: done %u (commit %zu bytes, preedit %s)", serial, m_pending_commit.size(),
                m_pending_preedit_set ? "set" : "unchanged");
            apply_text_input_done();
            break;
        }
        default:
            break;
        }
    });
}

// The method's answer, in the order the protocol names: the text to
// commit becomes typed text, then the composing text is shown (or, when
// the method sent none, cleared, since a commit ends a composition).
void WaylandWindow::apply_text_input_done()
{
    if (!m_pending_commit.empty()) {
        std::string const& text = m_pending_commit;
        std::size_t i = 0;
        while (i < text.size()) {
            unsigned char const lead = static_cast<unsigned char>(text[i]);
            std::size_t const length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 1;
            char32_t code_point = length == 1 ? lead : static_cast<char32_t>(lead & (0xff >> (length + 1)));
            for (std::size_t k = 1; k < length && i + k < text.size(); ++k)
                code_point = (code_point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
            i += length;
            if (code_point < 0x20 || code_point == 0x7f)
                continue;
            WindowEvent typed;
            typed.kind = WindowEvent::Kind::Text;
            typed.text = code_point;
            push(typed);
        }
    }
    if (m_pending_preedit_set || !m_pending_commit.empty()) {
        WindowEvent composing;
        composing.kind = WindowEvent::Kind::Preedit;
        composing.preedit = m_pending_preedit_set ? m_pending_preedit : std::string();
        push(composing);
    }
    m_pending_commit.clear();
    m_pending_preedit.clear();
    m_pending_preedit_set = false;
}

// Tells the method whether there is a caret and where: enabled with its
// content type and rectangle when a field has focus, disabled when none
// has; every change is one commit, whose serial `done` echoes.
void WaylandWindow::sync_text_input()
{
    if (!m_text_input || !m_text_input_entered)
        return;
    if (m_text_caret) {
        if (!m_text_input_enabled) {
            Request enable(m_text_input, zwp_text_input_v3::enable);
            send(enable);
            Request content(m_text_input, zwp_text_input_v3::set_content_type);
            content.uint(zwp_text_input_v3::content_hint_none).uint(zwp_text_input_v3::content_purpose_normal);
            send(content);
            m_text_input_enabled = true;
        }
        // Buffer pixels to surface units.
        double const s = m_scale > 0 ? m_scale : 1;
        Request rectangle(m_text_input, zwp_text_input_v3::set_cursor_rectangle);
        rectangle.int_(static_cast<std::int32_t>(std::lround(m_text_caret->x / s)))
            .int_(static_cast<std::int32_t>(std::lround(m_text_caret->y / s)))
            .int_(std::max(1, static_cast<int>(std::lround(m_text_caret->width / s))))
            .int_(std::max(1, static_cast<int>(std::lround(m_text_caret->height / s))));
        send(rectangle);
    } else {
        if (!m_text_input_enabled)
            return;
        Request disable(m_text_input, zwp_text_input_v3::disable);
        send(disable);
        m_text_input_enabled = false;
    }
    Request commit(m_text_input, zwp_text_input_v3::commit);
    send(commit);
    ++m_text_input_serial;
    m_connection->flush();
}

void WaylandWindow::set_text_input(std::optional<Rect> const& caret)
{
    if (caret == m_text_caret)
        return;
    m_text_caret = caret;
    sync_text_input();
}

// The frame's requests, each on the serial of the press that made it: the
// compositor moves or resizes the window along with the pointer from here.
void WaylandWindow::begin_move()
{
    if (!m_toplevel || !m_seat || m_input_serial == 0)
        return;
    debug("move on serial %u", m_input_serial);
    Request move(m_toplevel, xdg_toplevel::move);
    move.object(m_seat).uint(m_input_serial);
    send(move);
    m_connection->flush();
}

void WaylandWindow::begin_resize(WindowEdge edge)
{
    if (!m_toplevel || !m_seat || m_input_serial == 0)
        return;
    std::uint32_t edges = 0;
    switch (edge) {
    case WindowEdge::Top: edges = xdg_toplevel::edge_top; break;
    case WindowEdge::Bottom: edges = xdg_toplevel::edge_bottom; break;
    case WindowEdge::Left: edges = xdg_toplevel::edge_left; break;
    case WindowEdge::Right: edges = xdg_toplevel::edge_right; break;
    case WindowEdge::TopLeft: edges = xdg_toplevel::edge_top | xdg_toplevel::edge_left; break;
    case WindowEdge::TopRight: edges = xdg_toplevel::edge_top | xdg_toplevel::edge_right; break;
    case WindowEdge::BottomLeft: edges = xdg_toplevel::edge_bottom | xdg_toplevel::edge_left; break;
    case WindowEdge::BottomRight: edges = xdg_toplevel::edge_bottom | xdg_toplevel::edge_right; break;
    }
    debug("resize edges %u on serial %u", edges, m_input_serial);
    Request resize(m_toplevel, xdg_toplevel::resize);
    resize.object(m_seat).uint(m_input_serial).uint(edges);
    send(resize);
    m_connection->flush();
}

void WaylandWindow::minimize()
{
    if (!m_toplevel)
        return;
    debug("minimize");
    Request request(m_toplevel, xdg_toplevel::set_minimized);
    send(request);
    m_connection->flush();
}

void WaylandWindow::toggle_maximize()
{
    if (!m_toplevel)
        return;
    debug("%s", m_maximized ? "unmaximize" : "maximize");
    Request request(m_toplevel, m_maximized ? xdg_toplevel::unset_maximized : xdg_toplevel::set_maximized);
    send(request);
    m_connection->flush();
}

void WaylandWindow::listen_pointer()
{
    m_connection->listen(m_pointer, [this](std::uint16_t opcode, Message& message) {
        switch (opcode) {
        case wl_pointer::event_enter: {
            m_pointer_enter_serial = message.uint();
            message.object();
            m_pointer_x = static_cast<int>(std::floor(message.fixed() * m_scale));
            m_pointer_y = static_cast<int>(std::floor(message.fixed() * m_scale));
            m_pointer_inside = true;
            apply_cursor();
            WindowEvent event;
            event.kind = WindowEvent::Kind::MouseMove;
            event.x = m_pointer_x;
            event.y = m_pointer_y;
            push(event);
            break;
        }
        case wl_pointer::event_leave:
            m_pointer_inside = false;
            break;
        case wl_pointer::event_motion: {
            message.uint(); // time
            // Surface units to buffer pixels: the shell's coordinates.
            m_pointer_x = static_cast<int>(std::floor(message.fixed() * m_scale));
            m_pointer_y = static_cast<int>(std::floor(message.fixed() * m_scale));
            WindowEvent event;
            event.kind = WindowEvent::Kind::MouseMove;
            event.x = m_pointer_x;
            event.y = m_pointer_y;
            push(event);
            break;
        }
        case wl_pointer::event_button: {
            m_input_serial = message.uint();
            message.uint(); // time
            std::uint32_t const button = message.uint();
            std::uint32_t const state = message.uint();
            int number = 0;
            if (button == wl_pointer::btn_left)
                number = 1;
            else if (button == wl_pointer::btn_middle)
                number = 2;
            else if (button == wl_pointer::btn_right)
                number = 3;
            if (number == 0)
                break;
            WindowEvent event;
            event.kind = state == wl_pointer::button_state_pressed ? WindowEvent::Kind::MouseDown
                                                                   : WindowEvent::Kind::MouseUp;
            event.x = m_pointer_x;
            event.y = m_pointer_y;
            event.button = number;
            push(event);
            break;
        }
        case wl_pointer::event_axis: {
            message.uint(); // time
            std::uint32_t const axis = message.uint();
            double const value = message.fixed();
            if (axis == wl_pointer::axis_vertical_scroll)
                m_axis_value += value;
            break;
        }
        case wl_pointer::event_axis_discrete: {
            std::uint32_t const axis = message.uint();
            std::int32_t const discrete = message.int_();
            if (axis == wl_pointer::axis_vertical_scroll)
                m_axis_discrete += discrete;
            break;
        }
        case wl_pointer::event_axis_value120: {
            std::uint32_t const axis = message.uint();
            std::int32_t const value120 = message.int_();
            if (axis == wl_pointer::axis_vertical_scroll)
                m_axis_value120 += value120;
            break;
        }
        case wl_pointer::event_frame:
            push_wheel();
            break;
        default:
            break; // axis_source, axis_stop, axis_relative_direction
        }
    });
}

// A wheel notch is 120 in value120 terms, one in discrete terms and 15
// surface units on a continuous axis. Wayland's positive is toward the
// bottom of the page; the shell's positive rolls away from the user.
void WaylandWindow::push_wheel()
{
    int notches = 0;
    if (m_axis_value120 != 0) {
        m_wheel_remainder120 += m_axis_value120;
        notches = m_wheel_remainder120 / 120;
        m_wheel_remainder120 -= notches * 120;
    } else if (m_axis_discrete != 0) {
        notches = m_axis_discrete;
    } else if (m_axis_value != 0) {
        m_wheel_remainder += m_axis_value / 15.0;
        notches = static_cast<int>(m_wheel_remainder);
        m_wheel_remainder -= notches;
    }
    m_axis_value = 0;
    m_axis_discrete = 0;
    m_axis_value120 = 0;
    if (notches == 0)
        return;
    WindowEvent event;
    event.kind = WindowEvent::Kind::Wheel;
    event.x = m_pointer_x;
    event.y = m_pointer_y;
    event.wheel = -notches;
    push(event);
}

void WaylandWindow::listen_keyboard()
{
    m_connection->listen(
        m_keyboard,
        [this](std::uint16_t opcode, Message& message) {
            switch (opcode) {
            case wl_keyboard::event_keymap: {
                std::uint32_t const format = message.uint();
                int const fd = message.fd();
                std::uint32_t const size = message.uint();
                if (fd < 0)
                    break;
                debug("keymap: format %u, %u bytes", format, size);
                if (format == wl_keyboard::keymap_format_xkb_v1 && size > 0) {
                    void* const mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (mapped != mmap_failed) {
                        char const* const text = static_cast<char const*>(mapped);
                        std::string_view const keymap_text(text, strnlen(text, size));
                        m_keymap = xkb::Keymap::parse(keymap_text);
                        if (debug_enabled()) {
                            // The instrument for a keymap the parser refuses: the text itself.
                            char const* runtime = std::getenv("XDG_RUNTIME_DIR");
                            std::string const path = std::string(runtime ? runtime : "/tmp") + "/sashfold-keymap.xkb";
                            if (FILE* dump = std::fopen(path.c_str(), "wb")) {
                                std::fwrite(keymap_text.data(), 1, keymap_text.size(), dump);
                                std::fclose(dump);
                                debug("keymap: text written to %s (parsed: %s)", path.c_str(), m_keymap ? "yes" : "NO");
                            }
                        }
                        munmap(mapped, size);
                    } else {
                        debug("keymap: mmap failed: %s", std::strerror(errno));
                    }
                }
                ::close(fd);
                if (m_keymap) {
                    m_alt_mask = xkb::mod1_mask | m_keymap->modifier_mask("Alt");
                    debug("keymap: %zu keys, %zu types, alt mask 0x%x", m_keymap->key_count(),
                        m_keymap->type_count(), m_alt_mask);
                } else {
                    std::fprintf(stderr, "sashfold: the compositor's keymap could not be read; keys will not type\n");
                }
                break;
            }
            case wl_keyboard::event_enter:
                m_input_serial = message.uint();
                m_keyboard_focus = true;
                break;
            case wl_keyboard::event_leave:
                m_keyboard_focus = false;
                m_repeat_keycode = 0;
                break;
            case wl_keyboard::event_key: {
                m_input_serial = message.uint();
                message.uint(); // time
                std::uint32_t const keycode = message.uint() + 8; // evdev to xkb
                std::uint32_t const state = message.uint();
                if (state == wl_keyboard::key_state_pressed) {
                    emit_key(keycode);
                } else if (keycode == m_repeat_keycode) {
                    m_repeat_keycode = 0;
                }
                break;
            }
            case wl_keyboard::event_modifiers: {
                message.uint(); // serial
                std::uint32_t const depressed = message.uint();
                std::uint32_t const latched = message.uint();
                std::uint32_t const locked = message.uint();
                m_group = message.uint();
                m_mods = depressed | latched | locked;
                break;
            }
            case wl_keyboard::event_repeat_info:
                m_repeat_rate = message.int_();
                m_repeat_delay_ms = message.int_();
                break;
            default:
                break;
            }
        },
        { wl_keyboard::event_keymap });
}

void WaylandWindow::emit_key(std::uint32_t keycode)
{
    if (!m_keymap)
        return;
    std::uint32_t const keysym = m_keymap->keysym(keycode, m_mods, m_group);
    WindowEvent key_event;
    key_event.kind = WindowEvent::Kind::KeyDown;
    key_event.key.ctrl = (m_mods & xkb::control_mask) != 0;
    key_event.key.shift = (m_mods & xkb::shift_mask) != 0;
    key_event.key.alt = (m_mods & m_alt_mask) != 0;
    key_event.key.key = named_key(keysym);
    if (key_event.key.key == Key::None) {
        char32_t letter = shortcut_letter(m_keymap->keysym(keycode, 0, m_group));
        if (letter == 0)
            letter = shortcut_letter(m_keymap->keysym(keycode, 0, 0)); // the Latin group of a Cyrillic layout
        if (letter != 0) {
            key_event.key.key = Key::Letter;
            key_event.key.letter = letter;
        }
    }
    bool produced = false;
    if (key_event.key.key != Key::None) {
        push(key_event);
        produced = true;
    }
    char32_t const code_point = xkb::keysym_code_point(keysym);
    if (code_point >= 0x20 && code_point != 0x7f && !key_event.key.ctrl && !key_event.key.alt) {
        WindowEvent text;
        text.kind = WindowEvent::Kind::Text;
        text.text = code_point;
        push(text);
        produced = true;
    }
    debug("key %u -> keysym 0x%x, mods 0x%x, group %u%s", keycode, keysym, m_mods, m_group,
        produced ? "" : " (nothing for the shell)");
    if (m_repeat_keycode != keycode) {
        // A held key repeats after the delay, at the rate: the compositor
        // tells both, the client does the repeating.
        if (produced && !xkb::keysym_is_modifier(keysym) && m_repeat_rate > 0) {
            m_repeat_keycode = keycode;
            m_repeat_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_repeat_delay_ms);
        } else {
            m_repeat_keycode = 0;
        }
    }
}

void WaylandWindow::repeat_keys()
{
    if (m_repeat_keycode == 0 || !m_keyboard_focus || m_repeat_rate <= 0)
        return;
    auto const now = std::chrono::steady_clock::now();
    if (now < m_repeat_due)
        return;
    std::uint32_t const keycode = m_repeat_keycode;
    emit_key(keycode);
    auto const interval = std::chrono::milliseconds(std::max(1, 1000 / m_repeat_rate));
    m_repeat_due += interval;
    if (m_repeat_due < now) // a long block: one repeat, then back on the clock
        m_repeat_due = now + interval;
}

void WaylandWindow::listen_data_device()
{
    m_data_device = m_connection->allocate_id();
    Request get_device(m_data_device_manager, wl_data_device_manager::get_data_device);
    get_device.new_id(m_data_device).object(m_seat);
    send(get_device);
    m_connection->listen(m_data_device, [this](std::uint16_t opcode, Message& message) {
        switch (opcode) {
        case wl_data_device::event_data_offer: {
            std::uint32_t const id = message.new_id();
            m_offers[id] = {};
            m_connection->listen(id, [this, id](std::uint16_t offer_opcode, Message& offer_message) {
                if (offer_opcode == wl_data_offer::event_offer)
                    m_offers[id].push_back(offer_message.string());
            });
            break;
        }
        case wl_data_device::event_enter: {
            // A drag over the window: not accepted, so its offer goes at once.
            message.uint(); // serial
            message.object(); // surface
            message.fixed();
            message.fixed();
            if (std::uint32_t const id = message.object())
                destroy_offer(id);
            break;
        }
        case wl_data_device::event_selection: {
            std::uint32_t const id = message.object();
            if (m_selection_offer && m_selection_offer != id)
                destroy_offer(m_selection_offer);
            m_selection_offer = id;
            break;
        }
        default:
            break;
        }
    });
}

void WaylandWindow::destroy_offer(std::uint32_t id)
{
    Request destroy(id, wl_data_offer::destroy);
    send(destroy);
    m_connection->forget(id);
    m_offers.erase(id);
    if (m_selection_offer == id)
        m_selection_offer = 0;
}

bool WaylandWindow::write_clipboard(std::string const& utf8)
{
    if (!m_data_device || m_input_serial == 0 || m_connection->failed())
        return false;
    if (m_data_source) {
        Request destroy(m_data_source, wl_data_source::destroy);
        send(destroy);
        m_connection->forget(m_data_source);
    }
    m_data_source = m_connection->allocate_id();
    Request create(m_data_device_manager, wl_data_device_manager::create_data_source);
    create.new_id(m_data_source);
    send(create);
    std::uint32_t const source = m_data_source;
    m_connection->listen(
        source,
        [this, source](std::uint16_t opcode, Message& message) {
            if (opcode == wl_data_source::event_send) {
                message.string(); // the mime type: every one offered is text
                int const fd = message.fd();
                if (fd < 0)
                    return;
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                std::size_t written = 0;
                while (written < m_clipboard_text.size()) {
                    ssize_t const count = ::write(fd, m_clipboard_text.data() + written, m_clipboard_text.size() - written);
                    if (count > 0) {
                        written += static_cast<std::size_t>(count);
                        continue;
                    }
                    if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                        pollfd waiter { fd, POLLOUT, 0 };
                        if (::poll(&waiter, 1, 1000) <= 0)
                            break; // a reader that stalled: give up on it
                        continue;
                    }
                    break;
                }
                ::close(fd);
            } else if (opcode == wl_data_source::event_cancelled) {
                Request destroy(source, wl_data_source::destroy);
                send(destroy);
                m_connection->forget(source);
                if (m_data_source == source) {
                    m_data_source = 0;
                    m_selection_is_ours = false;
                }
            }
        },
        { wl_data_source::event_send });
    for (char const* mime : { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" }) {
        Request offer(m_data_source, wl_data_source::offer);
        offer.string(mime);
        send(offer);
    }
    Request set_selection(m_data_device, wl_data_device::set_selection);
    set_selection.object(m_data_source).uint(m_input_serial);
    send(set_selection);
    m_clipboard_text = utf8;
    m_selection_is_ours = true;
    return m_connection->flush();
}

std::optional<std::string> WaylandWindow::read_clipboard()
{
    if (m_selection_is_ours)
        return m_clipboard_text;
    if (!m_selection_offer || m_connection->failed())
        return std::nullopt;
    std::vector<std::string> const& mimes = m_offers[m_selection_offer];
    std::string chosen;
    for (char const* wanted : { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING" }) {
        if (std::find(mimes.begin(), mimes.end(), wanted) != mimes.end()) {
            chosen = wanted;
            break;
        }
    }
    if (chosen.empty())
        return std::nullopt;
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0)
        return std::nullopt;
    Request receive(m_selection_offer, wl_data_offer::receive);
    receive.string(chosen).fd(pipe_fds[1]);
    send(receive);
    ::close(pipe_fds[1]);
    m_connection->roundtrip(); // the owner has the request now
    std::string text;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        auto const now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        int const remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd waiter { pipe_fds[0], POLLIN, 0 };
        if (::poll(&waiter, 1, remaining) <= 0)
            break;
        char chunk[4096];
        ssize_t const count = ::read(pipe_fds[0], chunk, sizeof chunk);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        text.append(chunk, static_cast<std::size_t>(count));
    }
    ::close(pipe_fds[0]);
    return text;
}

// --- Buffers ----------------------------------------------------------------

bool WaylandWindow::ensure_frame_buffer(FrameBuffer& frame, int width, int height)
{
    if (frame.buffer && frame.width == width && frame.height == height)
        return true;
    release_frame_buffer(frame);
    std::size_t const stride = static_cast<std::size_t>(width) * 4;
    std::size_t const size = stride * static_cast<std::size_t>(height);
    if (!frame.memory.create(size))
        return false;
    frame.pool = m_connection->allocate_id();
    Request create_pool(m_shm, wl_shm::create_pool);
    create_pool.new_id(frame.pool).fd(frame.memory.fd).int_(static_cast<std::int32_t>(size));
    send(create_pool);
    frame.buffer = m_connection->allocate_id();
    Request create_buffer(frame.pool, wl_shm_pool::create_buffer);
    create_buffer.new_id(frame.buffer)
        .int_(0)
        .int_(width)
        .int_(height)
        .int_(static_cast<std::int32_t>(stride))
        .uint(wl_shm::format_xrgb8888);
    send(create_buffer);
    FrameBuffer* const slot = &frame;
    m_connection->listen(frame.buffer, [slot](std::uint16_t opcode, Message&) {
        if (opcode == wl_buffer::event_release)
            slot->busy = false;
    });
    frame.width = width;
    frame.height = height;
    frame.busy = false;
    return true;
}

void WaylandWindow::release_frame_buffer(FrameBuffer& frame)
{
    if (frame.buffer) {
        Request destroy(frame.buffer, wl_buffer::destroy);
        send(destroy);
        m_connection->forget(frame.buffer);
        frame.buffer = 0;
    }
    if (frame.pool) {
        Request destroy(frame.pool, wl_shm_pool::destroy);
        send(destroy);
        m_connection->forget(frame.pool);
        frame.pool = 0;
    }
    frame.memory.release();
    frame.width = 0;
    frame.height = 0;
    frame.busy = false;
}

void WaylandWindow::upload_icon(Bitmap const& icon)
{
    std::vector<int> sizes = m_icon_sizes;
    if (sizes.empty())
        sizes.assign(std::begin(icon_fallback_sizes), std::end(icon_fallback_sizes));
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    std::size_t total = 0;
    for (int const size : sizes)
        total += static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4;
    if (total == 0 || !m_icon_memory.create(total))
        return;
    std::uint32_t const pool = m_connection->allocate_id();
    Request create_pool(m_shm, wl_shm::create_pool);
    create_pool.new_id(pool).fd(m_icon_memory.fd).int_(static_cast<std::int32_t>(total));
    send(create_pool);
    std::uint32_t const icon_object = m_connection->allocate_id();
    Request create_icon(m_icon_manager, xdg_toplevel_icon_manager_v1::create_icon);
    create_icon.new_id(icon_object);
    send(create_icon);
    Request set_name(icon_object, xdg_toplevel_icon_v1::set_name);
    set_name.string(app_id); // the installed theme icon, when there is one
    send(set_name);
    std::size_t offset = 0;
    for (int const size : sizes) {
        std::vector<std::uint8_t> const pixels = scaled_icon(icon, size);
        std::memcpy(m_icon_memory.map + offset, pixels.data(), pixels.size());
        std::uint32_t const buffer = m_connection->allocate_id();
        Request create_buffer(pool, wl_shm_pool::create_buffer);
        create_buffer.new_id(buffer)
            .int_(static_cast<std::int32_t>(offset))
            .int_(size)
            .int_(size)
            .int_(size * 4)
            .uint(wl_shm::format_argb8888);
        send(create_buffer);
        Request add_buffer(icon_object, xdg_toplevel_icon_v1::add_buffer);
        add_buffer.object(buffer).int_(1);
        send(add_buffer);
        m_icon_buffers.push_back(buffer); // kept for the window's life: the icon holds them
        offset += pixels.size();
    }
    Request set_icon(m_icon_manager, xdg_toplevel_icon_manager_v1::set_icon);
    set_icon.object(m_toplevel).object(icon_object);
    send(set_icon);
    Request destroy_icon(icon_object, xdg_toplevel_icon_v1::destroy);
    send(destroy_icon);
    Request destroy_pool(pool, wl_shm_pool::destroy);
    send(destroy_pool); // the buffers keep the pool's pages alive
    debug("icon: %zu sizes uploaded (%d .. %d)", sizes.size(), sizes.front(), sizes.back());
}

// --- The Window interface ---------------------------------------------------

void WaylandWindow::pump(int timeout_ms)
{
    int const dispatched = m_connection->dispatch(timeout_ms);
    if (dispatched < 0)
        fail();
    else if (dispatched > 0)
        debug("dispatched %d events (waited up to %d ms)", dispatched, timeout_ms);
    repeat_keys();
}

void WaylandWindow::fail()
{
    if (m_closed)
        return;
    m_closed = true;
    std::fprintf(stderr, "sashfold: %s\n", m_connection->error().c_str());
    WindowEvent event;
    event.kind = WindowEvent::Kind::Close;
    push(event);
}

bool WaylandWindow::poll(WindowEvent& event)
{
    if (!m_closed)
        pump(0);
    if (m_events.empty())
        return false;
    event = m_events.front();
    m_events.pop_front();
    return true;
}

void WaylandWindow::wait(int timeout_ms)
{
    if (!m_events.empty() || m_closed)
        return;
    int timeout = timeout_ms;
    if (m_repeat_keycode && m_keyboard_focus) {
        auto const now = std::chrono::steady_clock::now();
        int const until_repeat = m_repeat_due <= now
            ? 0
            : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(m_repeat_due - now).count()) + 1;
        timeout = timeout < 0 ? until_repeat : std::min(timeout, until_repeat);
    }
    pump(timeout);
}

void WaylandWindow::present(Bitmap const& frame)
{
    if (m_closed || frame.width() <= 0 || frame.height() <= 0)
        return;
    debug("present %d x %d", frame.width(), frame.height());
    // One frame per display refresh: the callback of the last commit says
    // the compositor has shown it. A window nobody can see gets no
    // callbacks, so a short cap keeps the shell responsive there too.
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (m_frame_callback_pending && !m_closed && std::chrono::steady_clock::now() < deadline)
        pump(5);
    FrameBuffer* chosen = nullptr;
    for (auto attempt = 0; attempt < 20 && !chosen && !m_closed; ++attempt) {
        for (FrameBuffer& candidate : m_frames) {
            if (!candidate.busy) {
                chosen = &candidate;
                break;
            }
        }
        if (!chosen)
            pump(5); // both attached: the compositor releases one soon
    }
    if (!chosen)
        chosen = &m_frames[0]; // a compositor that stopped releasing: draw anyway
    if (!ensure_frame_buffer(*chosen, frame.width(), frame.height()))
        return;
    std::vector<std::uint8_t> const& rgba = frame.pixels();
    std::uint8_t* out = chosen->memory.map;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out[i + 0] = rgba[i + 2];
        out[i + 1] = rgba[i + 1];
        out[i + 2] = rgba[i + 0];
        out[i + 3] = 0xff;
    }
    Request attach(m_surface, wl_surface::attach);
    attach.object(chosen->buffer).int_(0).int_(0);
    send(attach);
    if (m_compositor_version >= 4) {
        Request damage(m_surface, wl_surface::damage_buffer);
        damage.int_(0).int_(0).int_(frame.width()).int_(frame.height());
        send(damage);
    } else {
        // Surface coordinates: the whole surface, whatever the buffer's size.
        Request damage(m_surface, wl_surface::damage);
        damage.int_(0).int_(0).int_(m_width).int_(m_height);
        send(damage);
    }
    std::uint32_t const callback = m_connection->allocate_id();
    Request frame_request(m_surface, wl_surface::frame);
    frame_request.new_id(callback);
    send(frame_request);
    m_frame_callback_pending = true;
    m_connection->listen(callback, [this, callback](std::uint16_t, Message&) {
        m_frame_callback_pending = false;
        m_connection->forget(callback);
    });
    Request commit(m_surface, wl_surface::commit);
    send(commit);
    chosen->busy = true;
    if (!m_connection->flush())
        fail();
}

void WaylandWindow::set_title(std::string const& title)
{
    Request request(m_toplevel, xdg_toplevel::set_title);
    request.string(title);
    send(request);
    m_connection->flush();
}

void WaylandWindow::set_cursor(Cursor cursor)
{
    if (cursor == m_cursor)
        return;
    m_cursor = cursor;
    if (m_pointer_inside) {
        apply_cursor();
        m_connection->flush();
    }
}

void WaylandWindow::apply_cursor()
{
    if (!m_cursor_device || !m_pointer_inside)
        return;
    std::uint32_t shape = wp_cursor_shape_device_v1::shape_default;
    switch (m_cursor) {
    case Cursor::Hand: shape = wp_cursor_shape_device_v1::shape_pointer; break;
    case Cursor::Text: shape = wp_cursor_shape_device_v1::shape_text; break;
    case Cursor::Arrow: break;
    }
    Request set_shape(m_cursor_device, wp_cursor_shape_device_v1::set_shape);
    set_shape.uint(m_pointer_enter_serial).uint(shape);
    send(set_shape);
}

std::unique_ptr<Window> Window::create(std::string const& title, int width, int height, Bitmap const* icon)
{
    return WaylandWindow::open(title, width, height, icon);
}

}
