// tools/shape-probe.cpp — shapes each line of standard input with the
// engine's shaper (src/text/Shaper.h) and prints one line per input line:
// "glyph:cluster:x_advance:y_advance:x_offset:y_offset ..." in visual
// order, font units. What a comparison with another shaping engine reads.
//
//   shape_probe <font> <face-index> <script (ISO 15924, e.g. Arab)> <language> <ltr|rtl> [features]
//
// Features are comma-separated: "liga" or "+liga" on, "-kern" off,
// "aalt=2" a value, "smcp[3:5]" a range of the line's characters.
// --time prints the lines per second instead, over every line repeated
// until a second has passed; --time-unshaped the same for measuring the
// lines glyph by glyph with pair kerning, as text was measured before the
// shaper.

#include "core/Unicode.h"
#include "text/Face.h"
#include "text/Shaper.h"
#include "text/TrueType.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::text;

namespace {

std::vector<FeatureSetting> parse_features(std::string const& list)
{
    std::vector<FeatureSetting> out;
    std::size_t at = 0;
    while (at < list.size()) {
        std::size_t const comma = list.find(',', at);
        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? list.size() : comma + 1;
        if (item.empty())
            continue;
        FeatureSetting f;
        if (item[0] == '-') {
            f.value = 0;
            item.erase(0, 1);
        } else if (item[0] == '+') {
            item.erase(0, 1);
        }
        if (std::size_t const eq = item.find('='); eq != std::string::npos) {
            f.value = static_cast<std::uint32_t>(std::stoul(item.substr(eq + 1)));
            item.erase(eq);
        }
        if (std::size_t const open = item.find('['); open != std::string::npos) {
            std::size_t const colon = item.find(':', open);
            std::size_t const close = item.find(']', open);
            if (colon != std::string::npos && close != std::string::npos) {
                f.start = std::stoul(item.substr(open + 1, colon - open - 1));
                f.end = std::stoul(item.substr(colon + 1, close - colon - 1));
            }
            item.erase(open);
        }
        while (item.size() < 4)
            item.push_back(' ');
        f.tag = feature_tag(item[0], item[1], item[2], item[3]);
        out.push_back(f);
    }
    return out;
}

}

int main(int argc, char** argv)
{
    bool timing = false;
    bool baseline = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--time")
            timing = true;
        else if (std::string(argv[i]) == "--time-unshaped")
            timing = baseline = true;
        else
            args.emplace_back(argv[i]);
    }
    if (args.size() < 5) {
        std::cerr << "usage: shape_probe [--time] font face script language ltr|rtl [features]\n";
        return 2;
    }
    std::ifstream file(args[0], std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto font = TrueTypeFont::parse(std::move(bytes), static_cast<std::size_t>(std::stoul(args[1])));
    if (!font) {
        std::cerr << "shape_probe: not a font\n";
        return 1;
    }
    std::unique_ptr<Face> const face = make_truetype_face(std::move(*font));
    std::string const& code = args[2];
    ShapeRequest request;
    request.script = code.size() == 4 ? script_code(code[0], code[1], code[2], code[3]) : scripts::common;
    std::string const language = args[3];
    request.language = language;
    request.direction = args[4] == "rtl" ? TextDirection::RightToLeft : TextDirection::LeftToRight;
    std::vector<FeatureSetting> const features = args.size() > 5 ? parse_features(args[5]) : std::vector<FeatureSetting> {};
    request.features = features;

    std::vector<std::u32string> lines;
    for (std::string line; std::getline(std::cin, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(decode_utf8(line));
    }
    ShapedRun run;
    float sink = 0;
    if (timing) {
        auto const start = std::chrono::steady_clock::now();
        std::size_t words = 0;
        double seconds = 0;
        do {
            for (std::u32string const& line : lines) {
                if (baseline) {
                    // What measuring costs without shaping: each code
                    // point's glyph and advance, each pair's kerning.
                    float width = 0;
                    std::uint32_t previous = 0;
                    for (char32_t c : line) {
                        std::uint32_t const glyph = face->glyph_index(c);
                        width += face->advance(glyph, 16.0f);
                        if (previous)
                            width += face->kerning(previous, glyph, 16.0f);
                        previous = glyph;
                    }
                    sink += width;
                    continue;
                }
                request.text = line;
                shape(*face, request, run);
            }
            words += lines.size();
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        } while (seconds < 1.0);
        std::printf("%zu lines in %.3f s: %.0f lines per second%s\n", words, seconds, static_cast<double>(words) / seconds,
            sink < 0 ? " " : "");
        return 0;
    }
    std::string out;
    for (std::u32string const& line : lines) {
        request.text = line;
        shape(*face, request, run);
        out.clear();
        for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
            ShapedGlyph const& g = run.glyphs[i];
            char item[96];
            std::snprintf(item, sizeof item, "%s%u:%u:%d:%d:%d:%d", i ? " " : "", g.glyph, g.cluster, g.x_advance,
                g.y_advance, g.x_offset, g.y_offset);
            out += item;
        }
        out += '\n';
        std::fputs(out.c_str(), stdout);
    }
    return 0;
}
