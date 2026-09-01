#include "core/Bitmap.h"
#include "core/Png.h"
#include "css/StyleResolver.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "text/SashfoldMono.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

int usage(char const* program)
{
    std::cerr << "usage: " << program << " --render <file.html> [-o output.png] [--width N]\n"
              << "       " << program << " --dump-dom <file.html>\n"
              << "       " << program << " --font-sampler <output.png>\n"
              << "       " << program << " [-o output.png]\n"
              << "\n"
              << "  --render lays out the page and writes it as a PNG.\n"
              << "  --dump-dom parses the file and prints the document tree.\n"
              << "  --font-sampler draws the Sashfold Mono QA sheet.\n"
              << "  With no mode, renders the paint smoke scene.\n";
    return 2;
}

int render_page(std::string const& path, std::string const& output, int viewport_width)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "error: cannot read " << path << "\n";
        return 1;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    auto document = html::parse_document_bytes(std::move(stream).str());
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
    Bitmap canvas(980, 720, Color::rgb(252, 252, 250));
    std::u32string const rows[] = {
        U"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        U"abcdefghijklmnopqrstuvwxyz",
        U"0123456789 !\"#$%&'()*+,-./",
        U":;<=>?@[\\]^_`{|}~ •–—‘’“”…�",
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
Bitmap render_smoke_scene()
{
    Bitmap canvas(320, 200, Color::rgb(250, 250, 248));

    canvas.fill_rect(Rect { 0, 0, 320, 44 }, Color::rgb(32, 38, 52));
    canvas.fill_rect(Rect { 16, 68, 120, 90 }, Color::rgb(214, 84, 72));
    canvas.fill_rect(Rect { 96, 100, 120, 90 }, Color::rgba(60, 120, 216, 128));
    canvas.fill_rect(Rect { -20, 168, 80, 60 }, Color::rgb(96, 176, 120));
    canvas.fill_rect(Rect { 268, -10, 80, 40 }, Color::rgba(240, 200, 64, 200));

    return canvas;
}

}

int main(int argc, char** argv)
{
    std::vector<std::string> const args(argv + 1, argv + argc);
    std::string output = "sashfold-out.png";

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--dump-dom") {
            if (i + 1 >= args.size()) {
                std::cerr << "error: --dump-dom needs a path\n";
                return usage(argv[0]);
            }
            return dump_dom(args[i + 1]);
        }
        if (args[i] == "--font-sampler") {
            if (i + 1 >= args.size()) {
                std::cerr << "error: --font-sampler needs an output path\n";
                return usage(argv[0]);
            }
            return font_sampler(args[i + 1]);
        }
        if (args[i] == "--render") {
            if (i + 1 >= args.size()) {
                std::cerr << "error: --render needs a path\n";
                return usage(argv[0]);
            }
            std::string const input = args[i + 1];
            std::string render_output = "sashfold-out.png";
            int viewport_width = 800;
            for (std::size_t j = i + 2; j < args.size(); ++j) {
                if ((args[j] == "-o" || args[j] == "--output") && j + 1 < args.size()) {
                    render_output = args[++j];
                } else if (args[j] == "--width" && j + 1 < args.size()) {
                    viewport_width = std::max(64, std::atoi(args[++j].c_str()));
                } else {
                    std::cerr << "error: unrecognised argument '" << args[j] << "'\n";
                    return usage(argv[0]);
                }
            }
            return render_page(input, render_output, viewport_width);
        }
        if (args[i] == "-o" || args[i] == "--output") {
            if (i + 1 >= args.size()) {
                std::cerr << "error: " << args[i] << " needs a path\n";
                return usage(argv[0]);
            }
            output = args[++i];
        } else if (args[i] == "-h" || args[i] == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "error: unrecognised argument '" << args[i] << "'\n";
            return usage(argv[0]);
        }
    }

    Bitmap const canvas = render_smoke_scene();
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }

    std::cout << "wrote " << output << " (" << canvas.width() << "x" << canvas.height() << ")\n";
    return 0;
}
