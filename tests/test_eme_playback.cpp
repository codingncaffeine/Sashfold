#include "JsTest.h"

#include "bindings/Realm.h"
#include "core/Base64.h"
#include "core/Bitmap.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "platform/Audio.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Encrypted Media Extensions end to end, as a page uses it: Clear Key asked
// for, a MediaKeys set on a video, an MP4 with sound and picture encrypted
// under 'cenc' appended through Media Source Extensions, the "encrypted"
// event's initialization data turned into a license request, the license
// written by the page's own script. Before the key comes the element waits
// (waitingforkey, HAVE_METADATA, no picture, no sound); once it comes, the
// pictures shown are the clear clip's to the byte, and so is every sample
// of sound heard. With a wrong key the pictures are not the clear clip's.
//
// Silent and windowless: sound goes to a virtual device the test listens
// to, the video is decoded by the processor's H.264 decoder and read back
// from the element's picture.

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::filesystem::path const fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "eme";

Bytes read_file(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The fixture with the "cenc" initialization data a packager writes and
// ffmpeg does not: a version 1 'pssh' of the common system ID naming the
// key ID, at the end of the moov.
Bytes with_pssh(Bytes file)
{
    auto const be32_at = [&](std::size_t at) {
        return (std::uint32_t(file[at]) << 24) | (std::uint32_t(file[at + 1]) << 16) | (std::uint32_t(file[at + 2]) << 8) | file[at + 3];
    };
    std::size_t const moov = be32_at(0);
    std::size_t const moov_end = moov + be32_at(moov);
    Bytes const pssh = { 0x00, 0x00, 0x00, 0x34, 'p', 's', 's', 'h', 0x01, 0x00, 0x00, 0x00, 0x10, 0x77, 0xEF, 0xEC, 0xC0, 0xB2, 0x4D, 0x02,
        0xAC, 0xE3, 0x3C, 0x1E, 0x52, 0xE2, 0xFB, 0x4B, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96,
        0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0, 0x00, 0x00, 0x00, 0x00 };
    file.insert(file.begin() + static_cast<std::ptrdiff_t>(moov_end), pssh.begin(), pssh.end());
    std::uint32_t const size = static_cast<std::uint32_t>(moov_end - moov + pssh.size());
    for (int i = 0; i < 4; ++i)
        file[moov + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (24 - 8 * i));
    return file;
}

struct Page {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    std::string console;
    double clock = 1000;
    std::vector<float> heard;

    Page()
    {
        bindings::HostHooks hooks;
        hooks.console = [this](std::string_view level, std::string_view message) { console += std::string(level) + ":" + std::string(message) + "|"; };
        hooks.now = [this] { return clock; };
        // The sound goes nowhere but here.
        hooks.open_audio = [this](platform::AudioFormat const& format, std::string&) {
            return platform::open_virtual_audio(format, [this] { return clock; },
                [this](std::span<float const> samples) { heard.insert(heard.end(), samples.begin(), samples.end()); });
        };
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url("https://example.test/eme.html"), std::move(hooks));
        realm->interpreter().heap().set_stress(true);
        html::parse_document_bytes_into(*document, "<!DOCTYPE html><body><video id=v></video></body>", realm.get());
        realm->document_parsed();
    }

    void pump()
    {
        for (int round = 0; round < 50; ++round) {
            for (int i = 0; i < 100 && realm->run_pending(); ++i) { }
            realm->perform_microtask_checkpoint();
        }
    }
    test::JsRun eval(std::string_view source)
    {
        js::Outcome const outcome = realm->run(source, "<test>");
        test::JsRun run;
        run.ok = outcome.ok;
        run.value = outcome.value;
        if (!outcome.ok)
            run.thrown = realm->interpreter().describe(outcome.value);
        if (!run.ok)
            test::fail("threw " + run.thrown, __FILE__, __LINE__);
        return run;
    }
    std::string string(std::string_view source)
    {
        test::JsRun const run = eval(source);
        return run.ok && run.value.is_string() ? run.value.as_string()->to_utf8() : std::string("<not a string>");
    }
    double number(std::string_view source)
    {
        test::JsRun const run = eval(source);
        return run.ok && run.value.is_number() ? run.value.as_number() : -1;
    }
    // The element's picture as it is now, the decoder given time to catch up.
    std::optional<Bytes> picture()
    {
        realm->settle_video(5000);
        std::vector<bindings::VideoFrame> const frames = realm->video_frames();
        if (frames.size() != 1 || !frames[0].bitmap || frames[0].frames == 0)
            return std::nullopt;
        return frames[0].bitmap->pixels();
    }
};

// The page's script: the clip appended whole through a SourceBuffer once
// the MediaKeys is set, and a license made from each request with `key`.
std::string script(Bytes const& clip, bool encrypted, std::string_view key)
{
    std::string out = "var CLIP = '" + base64_encode(clip) + "'; var KEY = '" + std::string(key) + "'; var ENCRYPTED = " + (encrypted ? "true" : "false") + ";\n";
    out += R"JS(
        var log = [];
        var v = document.getElementById('v');
        v.addEventListener('encrypted', function (e) {
            log.push('encrypted ' + e.initDataType + ' ' + e.initData.byteLength);
            var session = v.mediaKeys.createSession('temporary');
            session.addEventListener('message', function (m) {
                var request = JSON.parse(new TextDecoder().decode(m.message));
                log.push('message ' + m.messageType + ' ' + request.kids.join(',') + ' ' + request.type);
                var license = { keys: request.kids.map(function (kid) { return { kty: 'oct', kid: kid, k: KEY }; }) };
                session.update(new TextEncoder().encode(JSON.stringify(license))).then(function () {
                    var statuses = [];
                    session.keyStatuses.forEach(function (status) { statuses.push(status); });
                    log.push('updated ' + statuses.join(','));
                }, function (error) { log.push('update failed ' + error.name); });
            });
            session.generateRequest(e.initDataType, e.initData).catch(function (error) { log.push('request failed ' + error.name); });
        });
        v.addEventListener('waitingforkey', function () { log.push('waitingforkey ' + v.readyState); });
        v.addEventListener('error', function () { log.push('error ' + v.error.code); });
        function append() {
            var ms = new MediaSource();
            ms.addEventListener('sourceopen', function () {
                var sb = ms.addSourceBuffer('video/mp4; codecs="avc1.64000b, mp4a.40.2"');
                // The clip's first picture is at 1/12 s (the edit list of a
                // stream with B pictures): playback begins there.
                sb.addEventListener('updateend', function () {
                    ms.endOfStream();
                    v.currentTime = v.buffered.start(0);
                    log.push('appended');
                }, { once: true });
                sb.appendBuffer(Uint8Array.from(atob(CLIP), function (c) { return c.charCodeAt(0); }));
            }, { once: true });
            v.src = URL.createObjectURL(ms);
        }
        if (!ENCRYPTED) {
            append();
        } else {
            navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{
                initDataTypes: ['cenc'],
                videoCapabilities: [{ contentType: 'video/mp4; codecs="avc1.64000b"' }],
                audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }],
            }]).then(function (access) { return access.createMediaKeys(); })
              .then(function (keys) { return v.setMediaKeys(keys); })
              .then(function () { log.push('keys set'); append(); }, function (error) { log.push('failed ' + error.name + ' ' + error.message); });
        }
    )JS";
    return out;
}

// The fixtures' key, base64url, and the same with one bit changed.
constexpr std::string_view right_key = "Oi9bTG1-j5AaKzxNXm9wgQ";
constexpr std::string_view wrong_key = "Oi9bTG1-j5AaKzxNXm9wgA";

struct Seen {
    std::vector<std::optional<Bytes>> pictures; // at each of the times below
    std::vector<float> heard; // the sound of the first half second
    std::string log;
};
constexpr double times[] = { 0.0, 0.5, 0.9 };

// The clip played half a second from its start, then shown paused at each
// of the times after its start.
Seen watch(Page& page)
{
    Seen seen;
    page.heard.clear();
    page.eval("v.currentTime = v.buffered.start(0); v.play();");
    page.pump();
    page.clock += 500;
    page.pump();
    seen.heard = page.heard;
    page.eval("v.pause();");
    page.pump();
    for (double const time : times) {
        page.eval("v.currentTime = v.buffered.start(0) + " + std::to_string(time) + ";");
        page.pump();
        page.clock += 20;
        page.pump();
        seen.pictures.push_back(page.picture());
    }
    seen.log = page.string("log.join(' | ')");
    return seen;
}

void test_clear_key_playback()
{
    Bytes const clear = read_file(fixtures / "av-clear-frag.mp4");
    Bytes const encrypted = with_pssh(read_file(fixtures / "av-cenc-frag.mp4"));
    CHECK(clear.size() > 1000 && encrypted.size() > 1000);

    // The clear clip: what the encrypted one must show and sound like.
    Page reference;
    reference.eval(script(clear, false, ""));
    reference.pump();
    CHECK_EQ(reference.string("log.join(' | ') + ' ' + v.readyState"), "appended 4");
    // Played as the encrypted one is below: 300 ms, then watched.
    reference.eval("v.play();");
    reference.pump();
    reference.clock += 300;
    reference.pump();
    reference.clock += 10;
    reference.pump();
    reference.eval("v.pause();");
    reference.pump();
    Seen const expected = watch(reference);
    for (std::optional<Bytes> const& picture : expected.pictures)
        CHECK(picture.has_value());
    CHECK(expected.heard.size() > 48000u * 2u / 4u);

    // The encrypted clip, before its key comes: the encrypted event's
    // request goes out, and the page has not answered it yet when play()
    // is called — the element waits.
    Page page;
    page.eval("var hold = true;");
    page.eval(script(encrypted, true, right_key));
    // Answers held back: the license is written only after the element
    // has been seen waiting.
    page.eval("var realUpdate = MediaKeySession.prototype.update; var held = [];"
              "MediaKeySession.prototype.update = function (r) { var s = this; if (hold) return new Promise(function (ok) { held.push(function () { realUpdate.call(s, r).then(ok); }); }); return realUpdate.call(this, r); };");
    page.pump();
    // The encrypted event comes before the append's end (MSE's parser loop
    // queues it first), and the license request names the pssh's key ID.
    CHECK_EQ(page.string("log.join(' | ')"),
        "keys set | encrypted cenc 52 | appended | message license-request Dx4tPEtaaXiHlqW0w9Lh8A temporary | waitingforkey 1");
    double const start = page.number("v.currentTime");
    CHECK(start > 0.08 && start < 0.09);
    page.eval("v.play();");
    page.pump();
    page.clock += 500;
    page.pump();
    // Waiting where it began: HAVE_METADATA (the first picture cannot be
    // decrypted), not paused, not moving, no picture.
    CHECK_EQ(page.string("log.slice(5).join(' | ') + ' ' + v.readyState + ' ' + v.paused + ' ' + (v.currentTime === " + std::to_string(start) + ")"), " 1 false false");
    CHECK_EQ(page.number("v.currentTime"), start);
    CHECK(!page.picture().has_value());
    // Nothing heard but what a device plays while it waits: silence.
    bool silent = true;
    for (float const sample : page.heard)
        silent = silent && sample == 0.0f;
    CHECK(silent);

    // The key comes: playback goes on from the start, and what is shown and
    // heard is the clear clip's.
    page.eval("hold = false; held.forEach(function (f) { f(); });");
    page.pump();
    CHECK_EQ(page.string("log.slice(5).join(' | ')"), "updated usable");
    // It goes on by itself ("Attempt to Resume Playback If Necessary").
    page.clock += 300;
    page.pump();
    page.clock += 10;
    page.pump();
    CHECK(page.number("v.readyState") >= 3);
    CHECK(page.number("v.currentTime") > start + 0.2);
    page.eval("v.pause();");
    page.pump();
    Seen const seen = watch(page);
    CHECK_EQ(seen.pictures.size(), expected.pictures.size());
    for (std::size_t i = 0; i < seen.pictures.size() && i < expected.pictures.size(); ++i) {
        CHECK(seen.pictures[i].has_value());
        if (seen.pictures[i] && expected.pictures[i])
            CHECK(*seen.pictures[i] == *expected.pictures[i]);
    }
    std::size_t const compared = std::min(seen.heard.size(), expected.heard.size());
    CHECK(compared > 48000u * 2u / 4u);
    if (auto const differ = std::mismatch(seen.heard.begin(), seen.heard.begin() + static_cast<std::ptrdiff_t>(compared), expected.heard.begin());
        differ.first != seen.heard.begin() + static_cast<std::ptrdiff_t>(compared))
        std::cerr << "test_eme_playback: the sound differs from sample " << (differ.first - seen.heard.begin()) << " of " << compared << " ("
                  << seen.heard.size() << " heard, " << expected.heard.size() << " in the clear)\n";
    CHECK(std::equal(seen.heard.begin(), seen.heard.begin() + static_cast<std::ptrdiff_t>(compared), expected.heard.begin()));
    CHECK_EQ(page.realm->stats().uncaught_errors, 0);

    // The control: a wrong key. Clear Key cannot tell a wrong key from the
    // right one (a license has no check of its own), so the frames are
    // decrypted to noise; what the decoder makes of it is not the clip.
    Page wrong;
    wrong.eval("var hold = false;");
    wrong.eval(script(encrypted, true, wrong_key));
    wrong.pump();
    Seen const garbled = watch(wrong);
    std::size_t shown = 0;
    for (std::optional<Bytes> const& picture : garbled.pictures)
        shown += picture.has_value() ? 1 : 0;
    std::cerr << "test_eme_playback: with a wrong key: " << garbled.log << "; " << shown << " of " << garbled.pictures.size()
              << " pictures shown, error " << wrong.string("String(v.error && v.error.code)") << "\n";
    for (std::size_t i = 0; i < garbled.pictures.size() && i < expected.pictures.size(); ++i) {
        if (garbled.pictures[i] && expected.pictures[i])
            CHECK(*garbled.pictures[i] != *expected.pictures[i]);
    }
    std::size_t const wrong_compared = std::min(garbled.heard.size(), expected.heard.size());
    CHECK(wrong_compared == 0
        || !std::equal(garbled.heard.begin(), garbled.heard.begin() + static_cast<std::ptrdiff_t>(wrong_compared), expected.heard.begin()));
}

}

int main()
{
    // The processor's H.264 decoder: the same pictures on every machine.
#if defined(_WIN32)
    _putenv_s("SASHFOLD_VIDEO_DECODE", "software");
#else
    setenv("SASHFOLD_VIDEO_DECODE", "software", 1);
#endif
    test_clear_key_playback();
    return test::report("test_eme_playback");
}
