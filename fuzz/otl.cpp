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
            // The Indic shaper: reph, half forms with joiners, pre-base
            // matras, broken clusters, a prohibited vowel sequence.
            { U"कर्मक्षत्रिय र्‍क क्‌ष ि◌ं अा ्ि", script_code('D', 'e', 'v', 'a'), TextDirection::LeftToRight, "hi" },
            { U"নির্বাচিত ক্যৌ ৼি", script_code('B', 'e', 'n', 'g'), TextDirection::LeftToRight, "bn" },
            { U"ਕਂੀ ਪ੍ਰੇ", script_code('G', 'u', 'r', 'u'), TextDirection::LeftToRight, "pa" },
            { U"ಕ್ರೋ ರ್‍ಜ ಕೊ", script_code('K', 'n', 'd', 'a'), TextDirection::LeftToRight, "kn" },
            { U"ക്രൈ ൎക ൺ", script_code('M', 'l', 'y', 'm'), TextDirection::LeftToRight, "ml" },
            { U"ర్‍క ై్ౖ", script_code('T', 'e', 'l', 'u'), TextDirection::LeftToRight, "te" },
            { U"ශ්‍රී ෙකා", script_code('S', 'i', 'n', 'h'), TextDirection::LeftToRight, "si" },
            { U"ꦏꦺꦴꦤ꧀ꦠꦿ ‌ꦺ ꦏ꧀‍ꦏ", script_code('J', 'a', 'v', 'a'), TextDirection::LeftToRight, "jv" },
            { U"ᬓᬾᬲ᭄ᬭ᭫ ᭡᭫ ᬵᬶ", script_code('B', 'a', 'l', 'i'), TextDirection::LeftToRight, "ban" },
            { U"བསྒྲུབས་ ༿ཀ ྐ", script_code('T', 'i', 'b', 't'), TextDirection::LeftToRight, "bo" },
            { U"ក្រោម ៉ិ ក៉៉៉ ្", script_code('K', 'h', 'm', 'r'), TextDirection::LeftToRight, "km" },
            { U"င်္ကြွေို့ ေ ၼꧤ", script_code('M', 'y', 'm', 'r'), TextDirection::LeftToRight, "my" },
            { U"ߒߞߏ ߫ ߊ߲", script_code('N', 'k', 'o', 'o'), TextDirection::RightToLeft, "nqo" },
            { U"ᨣᩮᨶ᩠ᨴ ᩰ", script_code('L', 'a', 'n', 'a'), TextDirection::LeftToRight, "" },
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
