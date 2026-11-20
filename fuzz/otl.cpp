// libFuzzer harness for the text shaper: a font from the input, its GDEF,
// GSUB, GPOS, kern and cmap tables parsed and applied by shaping fixed
// strings in several scripts and directions, with features asked for —
// no crash, no sanitizer finding, no endless loop.

#include "text/Face.h"
#include "text/Shaper.h"
#include "text/TrueType.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using namespace sashfold::text;
    std::vector<std::uint8_t> const bytes(data, data + size);
    std::size_t const faces = std::min<std::size_t>(TrueTypeFont::face_count(bytes), 2);
    for (std::size_t index = 0; index < faces; ++index) {
        auto font = TrueTypeFont::parse(bytes, index);
        if (!font)
            continue;
        std::unique_ptr<Face> const face = make_truetype_face(std::move(*font));
        struct Sample {
            char32_t const* text;
            ScriptCode script;
            TextDirection direction;
            char const* language;
        };
        static constexpr Sample samples[] = {
            { U"office fi ́á̂ ÄV 1⁄2 x️­", scripts::latin, TextDirection::LeftToRight, "" },
            { U"بِسْمِ اللَّهِ لا (ی‌ک)",
                scripts::arabic, TextDirection::RightToLeft, "fa" },
            { U"שָּׁלוֹם", scripts::hebrew, TextDirection::RightToLeft, "he" },
            { U"น้ำกิน ຫຳ", scripts::thai, TextDirection::LeftToRight, "th" },
            { U"각가〮ᄒᆞᆫ", scripts::hangul, TextDirection::LeftToRight, "ko" },
            { U"ܐܒܐܕܐ̈", scripts::syriac, TextDirection::RightToLeft, "" },
            { U"ᠠ᠋ᠡ ᠢ", scripts::mongolian, TextDirection::LeftToRight, "" },
            { U"ᾀἆ abc", scripts::greek, TextDirection::RightToLeft, "" },
        };
        FeatureSetting const features[] = {
            { feature_tag('s', 'm', 'c', 'p'), 1, 2, 6 },
            { feature_tag('a', 'a', 'l', 't'), 3 },
            { feature_tag('l', 'i', 'g', 'a'), 0, 0, 3 },
            { feature_tag('r', 'a', 'n', 'd'), 255 },
        };
        ShapedRun run;
        for (Sample const& s : samples) {
            std::u32string const text = s.text;
            for (int with_features = 0; with_features < 2; ++with_features) {
                ShapeRequest request;
                request.text = text;
                request.before = U"ب";
                request.script = s.script;
                request.direction = s.direction;
                request.language = s.language;
                request.beginning_of_text = true;
                if (with_features)
                    request.features = features;
                shape(*face, request, run);
            }
        }
    }
    return 0;
}
