// gen-font: writes Sashfold Mono as a TrueType file. The regular face is the
// fixture the reader's tests read back (tests/fixtures/fonts/SashfoldMono.ttf);
// test_truetype regenerates it in memory, so the committed bytes can never
// drift from the face. With --seed it writes a small face for the fuzzing
// corpus instead.
//
//   gen_font <out.ttf> [--bold] [--italic] [--seed]

#include "text/SashfoldMono.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int usage()
{
    std::cerr << "usage: gen_font <out.ttf> [--bold] [--italic] [--seed]\n";
    return 2;
}

int main(int argc, char** argv)
{
    std::string output;
    sashfold::text::TrueTypeOptions options;
    bool seed = false;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--bold")
            options.bold = true;
        else if (arg == "--italic")
            options.italic = true;
        else if (arg == "--seed")
            seed = true;
        else if (arg.starts_with("-"))
            return usage();
        else
            output = arg;
    }
    if (output.empty())
        return usage();

    // The seed keeps one of everything: a capital, a lowercase, a composed
    // letter over each, an alias, the box, and the space.
    std::u32string seed_set = U" AEae-";
    for (char32_t const c : { 0x00C9u, 0x00E9u, 0x2010u, 0xFF21u, 0xFFFDu })
        seed_set.push_back(c);
    if (seed)
        options.only = seed_set;

    std::vector<std::uint8_t> const bytes = sashfold::text::SashfoldMono::instance().to_truetype(options);
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << " (" << bytes.size() << " bytes)\n";
    return 0;
}
