#include "core/Bitmap.h"
#include "core/Png.h"
#include "css/StyleResolver.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"
#include "layout/Layout.h"
#include "net/Http.h"
#include "paint/Painter.h"
#include "platform/Window.h"
#include "text/SashfoldMono.h"
#include "ui/Browser.h"
#include "ui/Script.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

int usage(char const* program)
{
    std::cerr << "usage: " << program << " [url] [--theme <file.json>]\n"
              << "       " << program << " --script <file> [--update-goldens] [--width N] [--height N]\n"
              << "       " << program << " --render <file.html|url> [-o out.png] [--width N]\n"
              << "       " << program << " --fetch <url>\n"
              << "       " << program << " --dump-dom <file.html>\n"
              << "       " << program << " --font-sampler <output.png>\n"
              << "       " << program << " --smoke [-o output.png]\n"
              << "\n"
              << "  With a URL or nothing, opens the browser window.\n"
              << "  --theme applies a theme file to the window and to --script; the default is\n"
              << "          themes/default.json beside the executable or its parent, reloaded\n"
              << "          whenever the file changes while the window is open.\n"
              << "  --script replays a shell script headlessly and checks its assertions.\n"
              << "  --render lays out the page (local file or live URL) and writes a PNG.\n"
              << "  --fetch prints the response head through the fetch choke point.\n"
              << "  --dump-dom parses the file and prints the document tree.\n"
              << "  --font-sampler draws the Sashfold Mono QA sheet.\n"
              << "  --smoke renders the paint smoke scene.\n";
    return 2;
}

// Loads a --render / --dump-dom input: an http URL through the fetch choke
// point, anything else as a local file.
std::optional<std::string> load_input(std::string const& source)
{
    if (source.starts_with("http://") || source.starts_with("https://")) {
        auto const url = net::parse_url(source);
        if (!url) {
            std::cerr << "error: unparseable URL " << source << "\n";
            return std::nullopt;
        }
        net::FetchResult result = net::fetch(*url);
        if (!result.response) {
            std::cerr << "error: " << result.error << "\n";
            return std::nullopt;
        }
        std::cerr << "fetched " << result.response->final_url.serialize() << " ("
                  << result.response->status << ", " << result.response->body.size()
                  << " bytes)\n";
        return std::string(result.response->body.begin(), result.response->body.end());
    }
    std::ifstream file(source, std::ios::binary);
    if (!file) {
        std::cerr << "error: cannot read " << source << "\n";
        return std::nullopt;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return std::move(stream).str();
}

int fetch_url(std::string const& input)
{
    auto const url = net::parse_url(input);
    if (!url) {
        std::cerr << "error: unparseable URL " << input << "\n";
        return 1;
    }
    net::FetchResult result = net::fetch(*url);
    if (!result.response) {
        std::cerr << "error: " << result.error << "\n";
        return 1;
    }
    std::cout << result.response->status << " " << result.response->status_text << "  ("
              << result.response->final_url.serialize() << ")\n";
    for (net::Header const& header : result.response->headers)
        std::cout << header.name << ": " << header.value << "\n";
    std::cout << "\n[" << result.response->body.size() << " bytes of body]\n";
    return 0;
}

int render_page(std::string const& path, std::string const& output, int viewport_width)
{
    std::optional<std::string> const bytes = load_input(path);
    if (!bytes)
        return 1;
    auto document = html::parse_document_bytes(*bytes);
    css::StyleMap const styles = css::resolve_styles(*document);
    layout::LayoutResult const page = layout::layout_document(*document, styles,
        static_cast<float>(viewport_width));

    int const height = std::max(1, static_cast<int>(page.page_height + 0.5f));
    Bitmap canvas(viewport_width, height, page.canvas_background);
    paint::paint_page(canvas, page);
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << " (" << canvas.width() << "x" << canvas.height() << ")\n";
    return 0;
}

int dump_dom(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "error: cannot read " << path << "\n";
        return 1;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    auto document = html::parse_document_bytes(std::move(stream).str());
    std::cout << html::dump_document(*document);
    return 0;
}

// Renders every glyph of Sashfold Mono at several sizes — the standing QA
// sheet for the face.
int font_sampler(std::string const& output)
{
    text::SashfoldMono const& font = text::SashfoldMono::instance();
    Bitmap canvas(980, 760, Color::rgb(252, 252, 250));
    std::u32string const rows[] = {
        U"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        U"abcdefghijklmnopqrstuvwxyz",
        U"0123456789 !\"#$%&'()*+,-./",
        U":;<=>?@[\\]^_`{|}~ •–—‘’“”…�",
        U"← → ↻ × chrome glyphs",
        U"The quick brown fox jumps over the lazy dog.",
        U"int main() { return \"hi\"; } /* 0xFF */",
    };
    float y = 40;
    for (float size : { 16.0f, 24.0f }) {
        for (std::u32string const& row : rows) {
            float x = 16;
            for (char32_t const c : row) {
                font.draw_glyph(canvas, c, x, y, size, Color::rgb(20, 20, 24), false, false);
                x += text::SashfoldMono::advance(size);
            }
            y += size * 1.4f;
        }
        y += 12;
    }
    // Bold and italic rows.
    for (int variant = 0; variant < 3; ++variant) {
        std::u32string const sample = U"Weight and slant: Hamburgefonstiv 017";
        float x = 16;
        for (char32_t const c : sample) {
            font.draw_glyph(canvas, c, x, y, 22.0f, Color::rgb(20, 20, 24), variant == 1,
                variant == 2);
            x += text::SashfoldMono::advance(22.0f);
        }
        y += 34;
    }
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << "\n";
    return 0;
}

// Exercises the paint path end to end: opaque fills, clipping at every edge,
// and alpha compositing over both opaque and transparent ground.
int smoke_scene(std::string const& output)
{
    Bitmap canvas(320, 200, Color::rgb(250, 250, 248));
    canvas.fill_rect(Rect { 0, 0, 320, 44 }, Color::rgb(32, 38, 52));
    canvas.fill_rect(Rect { 16, 68, 120, 90 }, Color::rgb(214, 84, 72));
    canvas.fill_rect(Rect { 96, 100, 120, 90 }, Color::rgba(60, 120, 216, 128));
    canvas.fill_rect(Rect { -20, 168, 80, 60 }, Color::rgb(96, 176, 120));
    canvas.fill_rect(Rect { 268, -10, 80, 40 }, Color::rgba(240, 200, 64, 200));
    canvas.fill_round_rect(Rect { 200, 60, 100, 60 }, 14, Color::rgb(60, 60, 70));
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << " (" << canvas.width() << "x" << canvas.height() << ")\n";
    return 0;
}

// themes/default.json beside the executable, or beside its parent directory
// (a build tree inside the repository), else the built-in defaults.
std::string default_theme_path(char const* program)
{
    std::error_code error;
    std::filesystem::path const exe = std::filesystem::absolute(program, error);
    if (error)
        return {};
    std::filesystem::path const dir = exe.parent_path();
    for (std::filesystem::path const& base : { dir, dir.parent_path() }) {
        std::filesystem::path const candidate = base / "themes" / "default.json";
        if (std::filesystem::exists(candidate, error))
            return candidate.string();
    }
    return {};
}

ui::Theme load_theme(std::string const& path)
{
    if (path.empty())
        return ui::Theme {};
    std::vector<std::string> problems;
    std::optional<ui::Theme> const theme = ui::Theme::load(path, &problems);
    for (std::string const& problem : problems)
        std::cerr << problem << "\n";
    return theme.value_or(ui::Theme {});
}

int run_script_mode(std::string const& script, bool update_goldens, int width, int height,
    std::string const& theme_path)
{
    ui::ShellLoader loader;
    ui::Browser browser(loader, load_theme(theme_path), width, height);
    ui::ScriptResult const result = ui::run_script(browser, script, update_goldens, std::cout);
    return result.ok() ? 0 : 1;
}

int run_window(std::string const& start_url, std::string const& theme_path)
{
    std::unique_ptr<platform::Window> window = platform::Window::create("Sashfold", 1100, 760);
    if (!window) {
        std::cerr << "error: no window backend on this OS yet (Wayland lands at M3.5, macOS at M5);\n"
                     "       --render, --fetch, and --script work everywhere\n";
        return 1;
    }
    ui::ShellLoader loader;
    ui::Browser browser(loader, load_theme(theme_path), window->width(), window->height());
    browser.navigate(start_url.empty() ? "about:sashfold" : start_url);

    std::error_code error;
    std::filesystem::file_time_type theme_stamp;
    if (!theme_path.empty())
        theme_stamp = std::filesystem::last_write_time(theme_path, error);
    auto last_theme_check = std::chrono::steady_clock::now();
    std::string last_title;

    bool running = true;
    while (running) {
        platform::WindowEvent event;
        while (window->poll(event)) {
            using Kind = platform::WindowEvent::Kind;
            switch (event.kind) {
            case Kind::Close: running = false; break;
            case Kind::Resize: browser.resize(event.width, event.height); break;
            case Kind::MouseMove: browser.mouse_move(event.x, event.y); break;
            case Kind::MouseDown: browser.mouse_down(event.x, event.y, event.button); break;
            case Kind::MouseUp: browser.mouse_up(event.x, event.y, event.button); break;
            case Kind::Wheel: browser.wheel(event.x, event.y, event.wheel); break;
            case Kind::KeyDown: browser.key_down(event.key); break;
            case Kind::Text: browser.text_input(event.text); break;
            case Kind::None: break;
            }
        }
        if (!running)
            break;

        if (browser.has_pending_load()) {
            window->present(browser.frame()); // the "Loading" frame, before the synchronous fetch
            browser.tick();
        }
        if (browser.needs_paint())
            window->present(browser.frame());
        std::string const title = browser.window_title();
        if (title != last_title) {
            window->set_title(title);
            last_title = title;
        }
        window->set_cursor(browser.cursor());

        // Themes are data: edit the file and the window follows.
        if (!theme_path.empty()) {
            auto const now = std::chrono::steady_clock::now();
            if (now - last_theme_check > std::chrono::milliseconds(500)) {
                last_theme_check = now;
                std::filesystem::file_time_type const stamp
                    = std::filesystem::last_write_time(theme_path, error);
                if (!error && stamp != theme_stamp) {
                    theme_stamp = stamp;
                    browser.set_theme(load_theme(theme_path));
                }
            }
        }
        if (!browser.has_pending_load())
            window->wait(theme_path.empty() ? -1 : 500);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> const args(argv + 1, argv + argc);
    std::string mode;
    std::string input;
    std::string output = "sashfold-out.png";
    std::string start_url;
    std::string theme_path = default_theme_path(argv[0]);
    int width = 0;
    int height = 0;
    bool update_goldens = false;

    auto const value_after = [&](std::size_t& i, std::string& into) {
        if (i + 1 >= args.size()) {
            std::cerr << "error: " << args[i] << " needs a value\n";
            return false;
        }
        into = args[++i];
        return true;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string const& arg = args[i];
        if (arg == "--theme") {
            if (!value_after(i, theme_path))
                return usage(argv[0]);
        } else if (arg == "--script" || arg == "--render" || arg == "--fetch" || arg == "--dump-dom"
            || arg == "--font-sampler") {
            mode = arg;
            if (!value_after(i, input))
                return usage(argv[0]);
        } else if (arg == "--smoke") {
            mode = arg;
        } else if (arg == "--update-goldens") {
            update_goldens = true;
        } else if (arg == "--width" || arg == "--height") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            (arg == "--width" ? width : height) = std::max(64, std::atoi(text.c_str()));
        } else if (arg == "-o" || arg == "--output") {
            if (!value_after(i, output))
                return usage(argv[0]);
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (arg.starts_with("-")) {
            std::cerr << "error: unrecognised argument '" << arg << "'\n";
            return usage(argv[0]);
        } else {
            start_url = arg;
        }
    }

    if (mode == "--script")
        return run_script_mode(input, update_goldens, width ? width : 1024, height ? height : 720,
            theme_path);
    if (mode == "--render")
        return render_page(input, output, width ? width : 800);
    if (mode == "--fetch")
        return fetch_url(input);
    if (mode == "--dump-dom")
        return dump_dom(input);
    if (mode == "--font-sampler")
        return font_sampler(input);
    if (mode == "--smoke")
        return smoke_scene(output);
    return run_window(start_url, theme_path);
}
