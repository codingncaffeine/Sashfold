#include "platform/Window.h"

// The Win32 window: RegisterClass/CreateWindow, a PeekMessage
// pump translated into WindowEvents, MsgWaitForMultipleObjectsEx to sleep,
// and the frame blitted with SetDIBitsToDevice (the pixels are ours; GDI only
// copies them). user32 and gdi32 are OS interfaces under the pledge.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace sashfold::platform {

namespace {

wchar_t const* const window_class_name = L"SashfoldWindow";

// The Windows SDK's MAKEINTRESOURCE and SRCCOPY macros carry old-style casts
// our warnings forbid, so the few constants we need are spelled out.
LPCWSTR resource_id(unsigned id)
{
    return reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(static_cast<WORD>(id)));
}
constexpr unsigned idc_arrow = 32512;
constexpr unsigned idc_ibeam = 32513;
constexpr unsigned idc_hand = 32649;
constexpr DWORD rop_source_copy = 0x00CC0020;

std::wstring to_wide(std::string const& utf8)
{
    if (utf8.empty())
        return {};
    int const count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
        count);
    return wide;
}

int low_signed(LPARAM value)
{
    return static_cast<int>(static_cast<short>(static_cast<unsigned>(value) & 0xFFFFu));
}

int high_signed(LPARAM value)
{
    return static_cast<int>(static_cast<short>((static_cast<unsigned>(value) >> 16) & 0xFFFFu));
}

KeyEvent key_event_from(WPARAM virtual_key)
{
    KeyEvent event;
    event.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    switch (virtual_key) {
    case VK_RETURN: event.key = Key::Enter; break;
    case VK_ESCAPE: event.key = Key::Escape; break;
    case VK_BACK: event.key = Key::Backspace; break;
    case VK_DELETE: event.key = Key::Delete; break;
    case VK_TAB: event.key = Key::Tab; break;
    case VK_SPACE: event.key = Key::Space; break;
    case VK_LEFT: event.key = Key::Left; break;
    case VK_RIGHT: event.key = Key::Right; break;
    case VK_UP: event.key = Key::Up; break;
    case VK_DOWN: event.key = Key::Down; break;
    case VK_HOME: event.key = Key::Home; break;
    case VK_END: event.key = Key::End; break;
    case VK_PRIOR: event.key = Key::PageUp; break;
    case VK_NEXT: event.key = Key::PageDown; break;
    case VK_F5: event.key = Key::F5; break;
    case VK_F12: event.key = Key::F12; break;
    default:
        if ((virtual_key >= 'A' && virtual_key <= 'Z') || (virtual_key >= '0' && virtual_key <= '9')) {
            event.key = Key::Letter;
            event.letter = static_cast<char32_t>(virtual_key);
        }
        break;
    }
    return event;
}

} // namespace

class WindowWin32 final : public Window {
public:
    static std::unique_ptr<Window> open(std::string const& title, int width, int height);

    ~WindowWin32() override
    {
        if (m_hwnd) {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
    }

    bool poll(WindowEvent& event) override
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (m_events.empty())
            return false;
        event = m_events.front();
        m_events.pop_front();
        return true;
    }

    void wait(int timeout_ms) override
    {
        if (!m_events.empty())
            return;
        MsgWaitForMultipleObjectsEx(0, nullptr,
            timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms), QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
    }

    void present(Bitmap const& frame) override
    {
        m_frame_width = frame.width();
        m_frame_height = frame.height();
        std::vector<std::uint8_t> const& rgba = frame.pixels();
        m_bgra.resize(rgba.size());
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            m_bgra[i + 0] = rgba[i + 2];
            m_bgra[i + 1] = rgba[i + 1];
            m_bgra[i + 2] = rgba[i + 0];
            m_bgra[i + 3] = 255;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        UpdateWindow(m_hwnd);
    }

    void set_title(std::string const& title) override
    {
        SetWindowTextW(m_hwnd, to_wide(title).c_str());
    }

    void set_cursor(Cursor cursor) override
    {
        if (cursor == m_cursor)
            return;
        m_cursor = cursor;
        SetCursor(cursor_handle());
    }

    int width() const override { return m_width; }
    int height() const override { return m_height; }

private:
    WindowWin32() = default;

    static LRESULT CALLBACK window_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* const self = reinterpret_cast<WindowWin32*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, message, wparam, lparam);
        return self->handle(message, wparam, lparam);
    }

    HCURSOR cursor_handle() const
    {
        switch (m_cursor) {
        case Cursor::Hand: return LoadCursorW(nullptr, resource_id(idc_hand));
        case Cursor::Text: return LoadCursorW(nullptr, resource_id(idc_ibeam));
        case Cursor::Arrow: break;
        }
        return LoadCursorW(nullptr, resource_id(idc_arrow));
    }

    void push(WindowEvent event) { m_events.push_back(event); }

    void push_mouse(WindowEvent::Kind kind, LPARAM lparam, int button = 0)
    {
        WindowEvent event;
        event.kind = kind;
        event.x = low_signed(lparam);
        event.y = high_signed(lparam);
        event.button = button;
        push(event);
    }

    void paint()
    {
        PAINTSTRUCT ps;
        HDC const dc = BeginPaint(m_hwnd, &ps);
        if (dc && !m_bgra.empty() && m_frame_width > 0 && m_frame_height > 0) {
            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = m_frame_width;
            info.bmiHeader.biHeight = -m_frame_height; // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            if (m_frame_width == m_width && m_frame_height == m_height) {
                SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(m_frame_width),
                    static_cast<DWORD>(m_frame_height), 0, 0, 0,
                    static_cast<UINT>(m_frame_height), m_bgra.data(), &info, DIB_RGB_COLORS);
            } else {
                StretchDIBits(dc, 0, 0, m_width, m_height, 0, 0, m_frame_width, m_frame_height,
                    m_bgra.data(), &info, DIB_RGB_COLORS, rop_source_copy);
            }
        }
        EndPaint(m_hwnd, &ps);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message) {
        case WM_CLOSE: {
            WindowEvent event;
            event.kind = WindowEvent::Kind::Close;
            push(event);
            return 0; // the shell decides; the destructor destroys
        }
        case WM_SIZE: {
            m_width = static_cast<int>(static_cast<unsigned>(lparam) & 0xFFFFu);
            m_height = static_cast<int>((static_cast<unsigned>(lparam) >> 16) & 0xFFFFu);
            WindowEvent event;
            event.kind = WindowEvent::Kind::Resize;
            event.width = m_width;
            event.height = m_height;
            push(event);
            return 0;
        }
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if ((static_cast<unsigned>(lparam) & 0xFFFFu) == HTCLIENT) {
                SetCursor(cursor_handle());
                return 1;
            }
            break;
        case WM_MOUSEMOVE:
            push_mouse(WindowEvent::Kind::MouseMove, lparam);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
            SetCapture(m_hwnd);
            push_mouse(WindowEvent::Kind::MouseDown, lparam,
                message == WM_LBUTTONDOWN ? 1 : message == WM_MBUTTONDOWN ? 2 : 3);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
            ReleaseCapture();
            push_mouse(WindowEvent::Kind::MouseUp, lparam,
                message == WM_LBUTTONUP ? 1 : message == WM_MBUTTONUP ? 2 : 3);
            return 0;
        case WM_MOUSEWHEEL: {
            int const delta = static_cast<int>(
                static_cast<short>((static_cast<unsigned>(wparam) >> 16) & 0xFFFFu));
            m_wheel_remainder += delta;
            int const notches = m_wheel_remainder / WHEEL_DELTA;
            m_wheel_remainder -= notches * WHEEL_DELTA;
            if (notches != 0) {
                POINT point { low_signed(lparam), high_signed(lparam) };
                ScreenToClient(m_hwnd, &point);
                WindowEvent event;
                event.kind = WindowEvent::Kind::Wheel;
                event.x = point.x;
                event.y = point.y;
                event.wheel = notches;
                push(event);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            WindowEvent event;
            event.kind = WindowEvent::Kind::KeyDown;
            event.key = key_event_from(wparam);
            if (event.key.key != Key::None) {
                push(event);
                if (message == WM_SYSKEYDOWN)
                    return 0; // Alt+Left and friends: no menu-bar behavior
            }
            break;
        }
        case WM_SYSCHAR:
            return 0; // no Alt-menu beeps
        case WM_CHAR: {
            wchar_t const unit = static_cast<wchar_t>(wparam);
            char32_t code_point = 0;
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                m_high_surrogate = unit;
                return 0;
            }
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                if (m_high_surrogate == 0)
                    return 0;
                code_point = 0x10000
                    + ((static_cast<char32_t>(m_high_surrogate) - 0xD800) << 10)
                    + (static_cast<char32_t>(unit) - 0xDC00);
                m_high_surrogate = 0;
            } else {
                code_point = unit;
            }
            if (code_point >= 0x20 && code_point != 0x7F) {
                WindowEvent event;
                event.kind = WindowEvent::Kind::Text;
                event.text = code_point;
                push(event);
            }
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(m_hwnd, message, wparam, lparam);
    }

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::deque<WindowEvent> m_events;
    std::vector<std::uint8_t> m_bgra;
    int m_frame_width = 0;
    int m_frame_height = 0;
    Cursor m_cursor = Cursor::Arrow;
    int m_wheel_remainder = 0;
    wchar_t m_high_surrogate = 0;
};

std::unique_ptr<Window> WindowWin32::open(std::string const& title, int width, int height)
{
    static bool registered = false;
    HINSTANCE const instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW window_class {};
        window_class.cbSize = sizeof(WNDCLASSEXW);
        window_class.lpfnWndProc = window_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = nullptr; // WM_SETCURSOR applies ours
        window_class.hbrBackground = nullptr; // we paint every pixel
        window_class.lpszClassName = window_class_name;
        if (!RegisterClassExW(&window_class))
            return nullptr;
        registered = true;
    }

    RECT frame { 0, 0, width, height };
    DWORD const style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&frame, style, FALSE);
    int const outer_width = frame.right - frame.left;
    int const outer_height = frame.bottom - frame.top;
    int const screen_width = GetSystemMetrics(SM_CXSCREEN);
    int const screen_height = GetSystemMetrics(SM_CYSCREEN);
    int const x = std::max(0, (screen_width - outer_width) / 2);
    int const y = std::max(0, (screen_height - outer_height) / 2);

    std::unique_ptr<WindowWin32> window(new WindowWin32());
    window->m_width = width;
    window->m_height = height;
    HWND const hwnd = CreateWindowExW(0, window_class_name, to_wide(title).c_str(), style, x, y,
        outer_width, outer_height, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return nullptr;
    window->m_hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window.get()));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    // The WM_SIZE of creation went to DefWindowProc (our procedure attaches
    // after CreateWindow); the shell paints its first frame unprompted.
    return window;
}

std::unique_ptr<Window> Window::create(std::string const& title, int width, int height)
{
    return WindowWin32::open(title, width, height);
}

}
