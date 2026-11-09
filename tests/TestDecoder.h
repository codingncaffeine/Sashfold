#pragma once

// A video decoder for tests: it decodes nothing, and every frame shown is a
// flat grey picture, 32 by 18 — white, or with `counting` a grey that is
// another with every frame shown, so that a test can see one picture give
// way to the next. What becomes of a video's pictures once they are made —
// which is shown when, by whom, and where it is put — is then tested on a
// machine with no video hardware as well as on one with it.

#include "media/Vp9Accelerator.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace sashfold::test {

class TestAccelerator final : public media::Vp9Accelerator {
public:
    // `first_decode`: how long the first frame after the start or a reset
    // takes, as a slow decoder's would.
    explicit TestAccelerator(bool counting = false, std::chrono::milliseconds first_decode = {})
        : m_counting(counting)
        , m_first_decode(first_decode)
    {
    }
    std::string const& device() const override { return m_name; }
    bool decode(std::span<std::uint8_t const>, media::Vp9FrameHeader const&) override
    {
        if (m_fresh && m_first_decode.count() > 0)
            std::this_thread::sleep_for(m_first_decode);
        m_fresh = false;
        return true;
    }
    std::optional<media::Nv12Picture> read_last() override { return picture(); }
    std::optional<media::Nv12Picture> read_slot(int) override { return picture(); }
    void reset() override { m_fresh = true; }

private:
    media::Nv12Picture picture()
    {
        media::Nv12Picture made;
        made.width = 32;
        made.height = 18;
        made.luma.assign(32u * 18u, m_counting ? static_cast<std::uint8_t>(40 + 6 * (m_shown++ % 30)) : std::uint8_t { 255 });
        made.chroma.assign(16u * 2u * 9u, 128);
        return made;
    }

    bool m_counting;
    std::chrono::milliseconds m_first_decode;
    bool m_fresh = true;
    unsigned m_shown = 0;
    std::string m_name = "a decoder of the test's own";
};

}
