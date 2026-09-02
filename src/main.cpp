#include "core/Bitmap.h"
#include "core/Png.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"
#include "layout/Layout.h"
#include "net/Http.h"
#include "paint/Painter.h"
#include "platform/Window.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "text/TrueType.h"
#include "ui/Browser.h"
#include "ui/Script.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

int usage(char const* program)
{
    std::cerr << "usage: " << program << " [url] [--theme <file.json>] [--downloads <dir>]\n"
              << "       " << program << " --script <file> [--update-goldens] [--width N] [--height N]\n"
              << "       " << program << " --render <file.html|url> [-o out.png] [--width N]\n"
              << "       " << program << " --bench <file.html|url> [--runs N] [--width N]\n"
              << "       " << program << " --fetch <url>\n"
              << "       " << program << " --dump-dom <file.html>\n"
              << "       " << program << " --font-sampler <output.png> [--font <file.ttf>]\n"
              << "       " << program << " --font-info <file.ttf|file.ttc>\n"
              << "       " << program << " --font-list\n"
              << "       any mode: --fonts system|builtin   (system, except --script)\n"
              << "       " << program << " --smoke [-o output.png]\n"
              << "\n"
              << "  With a URL or nothing, opens the browser window.\n"
              << "  --theme applies a theme file to the window and to --script; the default is\n"
              << "          themes/default.json beside the executable or its parent, reloaded\n"
              << "          whenever the file changes while the window is open.\n"
              << "  --downloads is where downloads are saved (the window defaults to your\n"
              << "          Downloads folder; --script saves nothing unless told where).\n"
              << "  --script replays a shell script headlessly and checks its assertions.\n"
              << "  --render lays out the page (local file or live URL) and writes a PNG.\n"
              << "  --bench times parse, style, layout and paint of a page, several runs.\n"
              << "  --fetch prints the response head through the fetch choke point.\n"
              << "  --dump-dom parses the file and prints the document tree.\n"
              << "  --font-sampler draws the Sashfold Mono QA sheet.\n"
              << "  --smoke renders the paint smoke scene.\n";
    return 2;
}

int font_info(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "error: cannot read " << path << "\n";
        return 1;
    }
    std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    std::size_t const faces = text::TrueTypeFont::face_count(bytes);
    if (faces == 0) {
        std::cerr << "error: not a TrueType font: " << path << "\n";
        return 1;
    }
    for (std::size_t face = 0; face < faces; ++face) {
        std::optional<text::TrueTypeFont> const font = text::TrueTypeFont::parse(bytes, face);
        if (!font) {
            std::cout << "face " << face << ": malformed\n";
            continue;
        }
        std::size_t outlined = 0;
        std::size_t empty = 0;
        std::size_t malformed = 0;
        std::size_t points = 0;
        for (std::uint32_t glyph = 0; glyph < font->glyph_count(); ++glyph) {
            auto const outline = font->outline(static_cast<std::uint16_t>(glyph));
            if (!outline)
                ++malformed;
            else if (outline->points.empty())
                ++empty;
            else {
                ++outlined;
                points += outline->points.size();
            }
        }
        std::uint16_t const a = font->glyph_index(U'A');
        std::cout << "face " << face << ": " << font->family_name() << " / "
                  << font->subfamily_name() << "\n"
                  << "  units/em " << font->units_per_em() << ", ascender " << font->ascender()
                  << ", descender " << font->descender() << ", line gap " << font->line_gap()
                  << ", x-height " << font->x_height() << ", cap height " << font->cap_height()
                  << "\n"
                  << "  weight " << font->weight_class() << (font->is_italic() ? ", italic" : "")
                  << (font->has_cff() ? ", CFF outlines (declined)" : "") << "\n"
                  << "  glyphs " << font->glyph_count() << ": " << outlined << " with outlines, "
                  << empty << " empty, " << malformed << " malformed; " << points << " points\n"
                  << "  cmap: " << font->mapped_code_points() << " code points; 'A' -> glyph " << a
                  << ", advance " << font->advance_width(a) << ", lsb "
                  << font->left_side_bearing(a) << "\n";
    }
    return 0;
}

// Every face the font manager finds on this machine, and how long the
// catalogue took to build.
int font_list()
{
    auto const started = std::chrono::steady_clock::now();
    std::vector<text::FaceInfo> const& faces = text::FontManager::instance().catalogue();
    auto const elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started);
    for (text::FaceInfo const& face : faces) {
        std::cout << face.family << " / " << face.subfamily << "  (" << face.weight_class
                  << (face.italic ? ", italic" : "") << ")  " << face.path;
        if (face.face_index != 0)
            std::cout << " #" << face.face_index;
        std::cout << "\n";
    }
    std::cout << faces.size() << " faces catalogued in " << elapsed.count() << " ms\n";
    return 0;
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

struct LoadedPage {
    std::string bytes;
    net::Url url; // where it landed: the base for the page's references
    std::unique_ptr<ui::ShellLoader> loader; // fetches the page's stylesheets with the same session
};

// Loads a --render / --bench input through the shell's loader: a URL as
// typed, anything else as a local file.
std::optional<LoadedPage> load_page(std::string const& source)
{
    std::optional<net::Url> url;
    if (source.starts_with("http://") || source.starts_with("https://") || source.starts_with("file:")
        || source.starts_with("data:")) {
        url = net::parse_url(source);
    } else {
        std::error_code error;
        std::string generic = std::filesystem::absolute(source, error).generic_string();
        if (!generic.starts_with("/"))
            generic = "/" + generic;
        url = net::parse_url("file://" + generic);
    }
    if (!url) {
        std::cerr << "error: unparseable input " << source << "\n";
        return std::nullopt;
    }
    auto loader = std::make_unique<ui::ShellLoader>();
    net::FetchResult result = loader->load(*url, "", false);
    if (!result.response) {
        std::cerr << "error: " << result.error << "\n";
        return std::nullopt;
    }
    if (url->scheme != "file")
        std::cerr << "fetched " << result.response->final_url.serialize() << " ("
                  << result.response->status << ", " << result.response->body.size() << " bytes)\n";
    return LoadedPage { std::string(result.response->body.begin(), result.response->body.end()),
        result.response->final_url, std::move(loader) };
}

// The page's stylesheets, fetched through its own session.
css::SheetFetcher sheet_fetcher(LoadedPage const& page)
{
    return [&page](net::Url const& url) -> std::optional<css::FetchedSheet> {
        net::FetchResult result = page.loader->load_subresource(url, page.url, "");
        if (!result.response || result.response->status != 200)
            return std::nullopt;
        std::string const* type = net::find_header(result.response->headers, "content-type");
        return css::FetchedSheet { std::move(result.response->body), type ? *type : "" };
    };
}

int render_page(std::string const& path, std::string const& output, int viewport_width,
    int viewport_height)
{
    std::optional<LoadedPage> const loaded = load_page(path);
    if (!loaded)
        return 1;
    css::MediaContext const media { static_cast<float>(viewport_width),
        static_cast<float>(viewport_height) };
    auto document = html::parse_document_bytes(loaded->bytes);
    css::StyleMap const styles = css::resolve_styles(*document,
        css::collect_stylesheets(*document, &loaded->url, sheet_fetcher(*loaded), media), media);
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
// The QA sheet for a face: the built-in one by default, or any TrueType
// file, drawn through the same Face interface the page renderer uses. Code
// points the file lacks fall back to the built-in face, so a gap shows as
// a Sashfold Mono glyph rather than nothing.
int font_sampler(std::string const& output, std::string const& font_path)
{
    std::unique_ptr<text::Face> loaded;
    if (!font_path.empty()) {
        std::ifstream file(font_path, std::ios::binary);
        std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        std::optional<text::TrueTypeFont> parsed = text::TrueTypeFont::parse(bytes);
        if (!parsed) {
            std::cerr << "error: cannot read " << font_path << " as a TrueType font\n";
            return 1;
        }
        loaded = text::make_truetype_face(std::move(*parsed));
    }
    text::Face const& builtin = text::builtin_face();
    text::Face const& font = loaded ? *loaded : builtin;
    auto const draw = [&](Bitmap& canvas, char32_t c, float x, float y, float size, bool bold,
                          bool italic) {
        std::uint32_t const glyph = font.glyph_index(c);
        if (glyph != 0) {
            font.draw_glyph(canvas, glyph, x, y, size, Color::rgb(20, 20, 24), bold, italic);
            return font.advance(glyph, size);
        }
        builtin.draw_glyph(canvas, c, x, y, size, Color::rgb(20, 20, 24), bold, italic);
        return builtin.advance(c, size);
    };
    Bitmap canvas(980, 760, Color::rgb(252, 252, 250));
    std::u32string const rows[] = {
        U"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        U"abcdefghijklmnopqrstuvwxyz",
        U"0123456789 !\"#$%&'()*+,-./",
        U":;<=>?@[\\]^_`{|}~ •–—‘’“”…�",
        U"← → ↑ ↓ ↔ ↻ × ✓ † ‡ ™ chrome glyphs",
        U"café naïve Straße Ångström Ærø œuvre Łódź Đại þorn Việt ếch ÉCOLE ÑANDÚ",
        U"© ® · ° ± ÷ « » ‹ › ¡ ¿ £ € ¢ ¥ § ¶ µ ¬ ² ³ ¹ ½ ¼ ¾ ¤ ¦ ª º Ｆｕｌｌ",
        U"The quick brown fox jumps over the lazy dog.",
        U"int main() { return \"hi\"; } /* 0xFF */",
    };
    float y = 40;
    for (float size : { 16.0f, 24.0f }) {
        for (std::u32string const& row : rows) {
            float x = 16;
            for (char32_t const c : row)
                x += draw(canvas, c, x, y, size, false, false);
            y += size * 1.4f;
        }
        y += 12;
    }
    // Bold and italic rows.
    for (int variant = 0; variant < 3; ++variant) {
        std::u32string const sample = U"Weight and slant: Hamburgefonstiv 017";
        float x = 16;
        for (char32_t const c : sample)
            x += draw(canvas, c, x, y, 22.0f, variant == 1, variant == 2);
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

// The engine's stages timed separately, best and median of several runs,
// painting a viewport-sized slice the way the shell does each frame. The
// perf budgets are checked against these numbers.
int bench(std::string const& input, int runs, int viewport_width, int viewport_height)
{
    std::optional<LoadedPage> const loaded = load_page(input);
    if (!loaded)
        return 1;
    css::MediaContext const media { static_cast<float>(viewport_width),
        static_cast<float>(viewport_height) };
    // The sheets are fetched once, outside the timed runs: the network is
    // not what is being measured.
    std::vector<css::SheetSource> const sheets = [&] {
        auto const first = html::parse_document_bytes(loaded->bytes);
        return css::collect_stylesheets(*first, &loaded->url, sheet_fetcher(*loaded), media);
    }();
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    struct Sample {
        double parse = 0;
        double sheets = 0;
        double style = 0;
        double layout = 0;
        double paint = 0;
    };
    std::vector<Sample> samples;
    float page_height = 0;
    std::size_t rule_count = 0;
    std::size_t universal_count = 0;
    for (int run = 0; run < runs; ++run) {
        Sample sample;
        auto const t0 = clock::now();
        auto document = html::parse_document_bytes(loaded->bytes);
        auto const t1 = clock::now();
        css::StyleSet const style_set(sheets, media);
        auto const t1b = clock::now();
        css::StyleMap const styles = css::resolve_styles(*document, style_set);
        auto const t2 = clock::now();
        rule_count = style_set.rule_count();
        universal_count = style_set.universal_count();
        layout::LayoutResult const page = layout::layout_document(*document, styles,
            static_cast<float>(viewport_width));
        auto const t3 = clock::now();
        Bitmap canvas(viewport_width, 1000, page.canvas_background);
        paint::paint_page(canvas, page);
        auto const t4 = clock::now();
        sample.parse = ms(t1 - t0).count();
        sample.sheets = ms(t1b - t1).count();
        sample.style = ms(t2 - t1b).count();
        sample.layout = ms(t3 - t2).count();
        sample.paint = ms(t4 - t3).count();
        samples.push_back(sample);
        page_height = page.page_height;
    }
    auto const report = [&](char const* name, double Sample::*member) {
        std::vector<double> values;
        for (Sample const& sample : samples)
            values.push_back(sample.*member);
        std::sort(values.begin(), values.end());
        std::printf("  %-7s min %8.2f ms   median %8.2f ms\n", name, values.front(),
            values[values.size() / 2]);
    };
    std::printf("bench: %zu bytes, %zu sheet(s) with %zu rules (%zu universal), %d run(s), "
                "viewport %d px wide, page %d px tall\n",
        loaded->bytes.size(), sheets.size(), rule_count, universal_count, runs, viewport_width,
        static_cast<int>(page_height + 0.5f));
    report("parse", &Sample::parse);
    report("sheets", &Sample::sheets);
    report("style", &Sample::style);
    report("layout", &Sample::layout);
    report("paint", &Sample::paint);
    std::vector<double> totals;
    for (Sample const& sample : samples)
        totals.push_back(sample.parse + sample.sheets + sample.style + sample.layout + sample.paint);
    std::sort(totals.begin(), totals.end());
    std::printf("  %-7s min %8.2f ms   median %8.2f ms\n", "total", totals.front(),
        totals[totals.size() / 2]);
    return 0;
}

// The user's Downloads folder, when the OS has the convention and it exists.
std::string default_downloads_directory()
{
#ifdef _WIN32
    char const* const home = std::getenv("USERPROFILE");
#else
    char const* const home = std::getenv("HOME");
#endif
    if (!home || !*home)
        return {};
    std::error_code error;
    std::filesystem::path const downloads = std::filesystem::path(home) / "Downloads";
    return std::filesystem::is_directory(downloads, error) ? downloads.string() : std::string();
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
    std::string const& theme_path, std::string const& downloads)
{
    ui::ShellLoader loader;
    ui::Browser browser(loader, load_theme(theme_path), width, height);
    browser.set_downloads_directory(downloads);
    ui::ScriptResult const result = ui::run_script(browser, script, update_goldens, std::cout);
    return result.ok() ? 0 : 1;
}

int run_window(std::string const& start_url, std::string const& theme_path,
    std::string const& downloads)
{
    std::unique_ptr<platform::Window> window = platform::Window::create("Sashfold", 1100, 760);
    if (!window) {
        std::cerr << "error: no window backend on this OS yet (the Wayland and AppKit shells are not written);\n"
                     "       --render, --fetch, and --script work everywhere\n";
        return 1;
    }
    ui::ShellLoader loader;
    ui::Browser browser(loader, load_theme(theme_path), window->width(), window->height());
    browser.set_downloads_directory(downloads);
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
    std::optional<std::string> downloads;
    std::string font_path;
    std::string fonts_mode; // "system" or "builtin"; empty picks per mode
    int width = 0;
    int height = 0;
    int runs = 5;
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
        } else if (arg == "--downloads") {
            std::string directory;
            if (!value_after(i, directory))
                return usage(argv[0]);
            downloads = directory;
        } else if (arg == "--script" || arg == "--render" || arg == "--fetch" || arg == "--dump-dom"
            || arg == "--font-sampler" || arg == "--font-info" || arg == "--bench") {
            mode = arg;
            if (!value_after(i, input))
                return usage(argv[0]);
        } else if (arg == "--font") {
            if (!value_after(i, font_path))
                return usage(argv[0]);
        } else if (arg == "--fonts") {
            if (!value_after(i, fonts_mode))
                return usage(argv[0]);
            if (fonts_mode != "system" && fonts_mode != "builtin") {
                std::cerr << "error: --fonts takes system or builtin\n";
                return usage(argv[0]);
            }
        } else if (arg == "--smoke" || arg == "--font-list") {
            mode = arg;
        } else if (arg == "--update-goldens") {
            update_goldens = true;
        } else if (arg == "--runs") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            runs = std::clamp(std::atoi(text.c_str()), 1, 1000);
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

    // The machine's fonts serve the window, --render and --bench. The script
    // harness renders the built-in face alone unless told otherwise: its
    // goldens must match on every OS.
    bool const system_fonts = fonts_mode.empty() ? mode != "--script" : fonts_mode == "system";
    text::FontManager::instance().set_system_fonts(system_fonts);

    if (mode == "--script")
        return run_script_mode(input, update_goldens, width ? width : 1024, height ? height : 720,
            theme_path, downloads.value_or(""));
    if (mode == "--render")
        return render_page(input, output, width ? width : 800, height ? height : 720);
    if (mode == "--bench")
        return bench(input, runs, width ? width : 800, height ? height : 720);
    if (mode == "--fetch")
        return fetch_url(input);
    if (mode == "--dump-dom")
        return dump_dom(input);
    if (mode == "--font-sampler")
        return font_sampler(input, font_path);
    if (mode == "--font-info")
        return font_info(input);
    if (mode == "--font-list")
        return font_list();
    if (mode == "--smoke")
        return smoke_scene(output);
    return run_window(start_url, theme_path, downloads.value_or(default_downloads_directory()));
}
