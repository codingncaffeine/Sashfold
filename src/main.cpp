#include "core/Bitmap.h"
#include "core/Png.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

int usage(char const* program)
{
    std::cerr << "usage: " << program << " [-o output.png]\n"
              << "       " << program << " --dump-dom <file.html>\n"
              << "\n"
              << "  --dump-dom parses the file and prints the document tree.\n"
              << "  Without it, renders the paint smoke scene to a PNG\n"
              << "  (CSS and layout are not wired up yet).\n";
    return 2;
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
    auto document = html::parse_document(std::move(stream).str());
    std::cout << html::dump_document(*document);
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
