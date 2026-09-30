#include "Test.h"
#include "TestDecoder.h"

#include "core/Bitmap.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "media/PlaybackClock.h"
#include "media/StreamBuffer.h"
#include "media/VideoPipeline.h"
#include "media/Vp9Accelerator.h"
#include "media/WebM.h"
#include "paint/Painter.h"
#include "platform/SoundClock.h"
#include "text/FontManager.h"
#include "ui/PageCompositor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// A video shown apart from its page's painting: the clock that says where
// playback is between the page's own looks at it, the pipeline that shows
// its pictures by that clock, the page's painting that leaves the video's
// place open, and the compositor that puts the pictures there while the
// page's thread does something else. What a reader is promised is time: a
// picture at the moment it is due, whoever is busy — so the checks are
// bands on the wall clock.

using namespace sashfold;

namespace {

using steady = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;

double seconds_since(steady::time_point from)
{
    return std::chrono::duration<double>(steady::now() - from).count();
}

// (Every frame shown is the same flat white picture: what these tests ask
// of a picture is which frame it is and when it was shown, and the
// pipeline says both.)
using FlatAccelerator = test::TestAccelerator;

// The 144p stream's thirty blocks: real frames, so the pipeline's reading
// of their headers is the real one.
std::vector<media::WebmFrame> fixture_blocks()
{
    auto const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "vp9-144p.webm";
    std::ifstream in(fixture, std::ios::binary);
    Bytes const stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    media::WebmParser parser;
    CHECK(parser.append(stream));
    return parser.take_frames();
}

Bytes fixture_bytes(char const* name)
{
    auto const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / name;
    std::ifstream in(fixture, std::ios::binary);
    return Bytes { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

media::PlaybackClock::State running_from(double position, double limit, double rate = 1)
{
    media::PlaybackClock::State state;
    state.advancing = true;
    state.position = position;
    state.at = steady::now();
    state.rate = rate;
    state.limit = limit;
    return state;
}

media::PlaybackClock::State standing_at(double position)
{
    media::PlaybackClock::State state;
    state.position = position;
    state.at = steady::now();
    state.limit = position;
    return state;
}

// The sound clock, against a sound server made of arithmetic: a stream that
// plays at the machine's pace, and a server that says where it has played
// to in whole blocks, asked at the uneven intervals the device asks at. No
// time passes in this test — the clock is handed every moment it is asked
// about — so every figure is exact.
void test_the_sound_clock()
{
    using namespace std::chrono;
    steady::time_point const epoch = steady::time_point {} + hours(1);
    auto const at = [epoch](double seconds) { return epoch + duration_cast<steady::duration>(duration<double>(seconds)); };
    // The stream begins to be heard 0.2 s in, and plays on from there.
    auto const truth = [](double seconds) { return std::max(0.0, seconds - 0.2); };
    double block = 2048.0 / 48000.0; // what the server counts in: 42.7 ms
    auto const server = [&](double seconds) { return std::floor(truth(seconds) / block) * block; };
    static constexpr int intervals_ms[] = { 17, 23, 29, 37, 41 };

    platform::SoundClock clock(epoch);
    double now = 0;
    double next_answer = 0;
    std::size_t answers = 0;
    // Runs to `until`, a millisecond at a time, the server answering when
    // it is asked; says the clock's worst distance from the truth, and the
    // least and the most it moved in a millisecond.
    struct Seen {
        double worst = 0;
        double least_step = 1e9;
        double most_step = 0;
    };
    bool held = false; // the stream held at the server: its count stands
    double held_at = 0;
    auto const run = [&](double until, bool judge) {
        Seen seen;
        double before = clock.played_by(at(now));
        while (now < until) {
            now += 0.001;
            if (now >= next_answer) {
                clock.take_answer(held ? held_at : server(now), at(now));
                next_answer = now + intervals_ms[answers++ % std::size(intervals_ms)] / 1000.0;
            }
            double const reading = clock.played_by(at(now));
            if (judge) {
                seen.worst = std::max(seen.worst, std::abs(reading - truth(now)));
                seen.least_step = std::min(seen.least_step, reading - before);
                seen.most_step = std::max(seen.most_step, reading - before);
            }
            before = reading;
        }
        return seen;
    };

    // Before anything is heard the clock stands at nothing.
    run(0.15, false);
    CHECK_EQ(clock.played_by(at(now)), 0.0);
    CHECK(!clock.running());
    // Three seconds in, it has the stream's pace and place: within three
    // milliseconds of the truth at every millisecond of the next five
    // seconds, moving in every one of them by a millisecond, three parts in
    // a hundred more or less — where the server's own word is up to a
    // whole block of 42.7 ms behind and stands for a block at a time.
    run(3.0, false);
    CHECK(clock.running());
    Seen const steady_state = run(8.0, true);
    CHECK(steady_state.worst < 0.003);
    CHECK(steady_state.least_step > 0.00096 && steady_state.most_step < 0.00104);
    // (The server's word itself, for the difference.)
    double server_worst = 0;
    for (double t = 3.0; t < 8.0; t += 0.001)
        server_worst = std::max(server_worst, truth(t) - server(t));
    CHECK(server_worst > 0.040);

    // The server takes to counting in blocks half the size: nothing to see.
    block = 1024.0 / 48000.0;
    Seen const after_change = run(12.0, true);
    CHECK(after_change.worst < 0.003);
    CHECK(after_change.least_step > 0.00096 && after_change.most_step < 0.00104);

    // A pause stands the clock at once, where it had reached, whatever the
    // server still says; let go, it runs again once the server's count
    // moves, and is right again.
    clock.set_paused(true, at(now));
    double const paused_at = clock.played_by(at(now));
    CHECK(std::abs(paused_at - truth(now)) < 0.003);
    held = true;
    held_at = server(now);
    run(now + 0.7, false);
    CHECK_EQ(clock.played_by(at(now)), paused_at);
    CHECK(!clock.running());
    clock.set_paused(false, at(now));
    CHECK_EQ(clock.played_by(at(now)), paused_at);
    // (The stream takes up where it was held: the truth is that much later.)
    double const lost = truth(now) - paused_at;
    held = false;
    auto const resumed_truth = [&](double seconds) { return truth(seconds) - lost; };
    double resumed_worst = 0;
    double resumed_now = now;
    double back = 0;
    double before = paused_at;
    while (resumed_now < now + 3.0) {
        resumed_now += 0.001;
        if (resumed_now >= next_answer) {
            clock.take_answer(std::floor(resumed_truth(resumed_now) / block) * block, at(resumed_now));
            next_answer = resumed_now + intervals_ms[answers++ % std::size(intervals_ms)] / 1000.0;
        }
        double const reading = clock.played_by(at(resumed_now));
        back = std::min(back, reading - before);
        before = reading;
        if (resumed_now > now + 2.5)
            resumed_worst = std::max(resumed_worst, std::abs(reading - resumed_truth(resumed_now)));
    }
    CHECK(clock.running());
    CHECK(resumed_worst < 0.003);
    CHECK(back > -0.0008); // a clock may run slow while it gives way; it does not go back by a block

    // A stream that stops by itself — the speakers ran dry — stands the
    // clock once the server's count has stood longer than any block, at
    // what the server says.
    double const dry_at = std::floor(resumed_truth(resumed_now) / block) * block;
    for (int i = 0; i < 400; ++i) {
        resumed_now += 0.001;
        if (i % 20 == 0)
            clock.take_answer(dry_at, at(resumed_now));
    }
    CHECK(!clock.running());
    CHECK_EQ(clock.played_by(at(resumed_now)), dry_at);

    // Emptied for a seek, the count begins again from nothing.
    clock.reset();
    CHECK_EQ(clock.played_by(at(resumed_now)), 0.0);
    CHECK(!clock.running());
}

// The clock: it stands where it was told while playback stands, runs with
// the machine's clock while it plays, goes by what was heard when there are
// speakers, and never passes the end of what is buffered.
void test_the_clock()
{
    media::PlaybackClock clock;
    clock.set(standing_at(1.5));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(clock.seconds(), 1.5);
    CHECK(!clock.advancing());

    clock.set(running_from(2.0, 100.0));
    auto const started = steady::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    double const after = clock.seconds();
    double const passed = seconds_since(started);
    // As far on as the time that passed: within the few milliseconds either
    // side that reading two clocks one after the other costs.
    CHECK(after >= 2.0 + 0.080 - 0.002);
    CHECK(after <= 2.0 + passed + 0.002);
    CHECK(clock.advancing());

    // Twice as fast at twice the rate.
    clock.set(running_from(2.0, 100.0, 2.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(clock.seconds() >= 2.0 + 0.100 - 0.002);

    // Not past the end of what is buffered.
    clock.set(running_from(2.0, 2.02));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(clock.seconds(), 2.02);

    // By what was heard, not by the time since — and never back before
    // where the element said it stood.
    media::PlaybackClock::State heard = running_from(2.0, 100.0);
    double said = 2.25;
    heard.heard = [&said]() -> std::optional<double> { return said; };
    clock.set(heard);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK_EQ(clock.seconds(), 2.25);
    said = 1.0;
    CHECK_EQ(clock.seconds(), 2.0);
    // Speakers that have not answered yet leave the clock to the time.
    media::PlaybackClock::State unheard = running_from(2.0, 100.0);
    unheard.heard = []() -> std::optional<double> { return std::nullopt; };
    clock.set(unheard);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(clock.seconds() >= 2.0 + 0.030 - 0.002);
}

// A pipeline that follows a clock shows each picture when the clock
// reaches it — asked from a thread that is not the one that feeds it — and
// nothing while the clock stands.
void test_a_pipeline_shows_pictures_by_its_clock()
{
    std::vector<media::WebmFrame> const blocks = fixture_blocks();
    CHECK_EQ(blocks.size(), 30u);
    if (blocks.size() != 30)
        return;
    auto const clock = std::make_shared<media::PlaybackClock>();
    media::VideoPipeline pipeline(std::make_unique<FlatAccelerator>());
    clock->set(standing_at(0));
    pipeline.follow(clock, 250'000'000);
    CHECK(pipeline.follows());
    for (media::WebmFrame const& block : blocks)
        pipeline.push(block.time_ns, block.data);

    // Standing at the start: the first frame's picture, and no other
    // however long it stands.
    std::shared_ptr<media::ShownPicture const> first;
    steady::time_point first_asked;
    for (int i = 0; i < 2000 && !first; ++i) {
        first_asked = steady::now();
        first = pipeline.show();
        if (!first)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    steady::time_point const first_had = steady::now();
    CHECK(first != nullptr);
    if (!first)
        return;
    CHECK_EQ(first->time_ns, blocks[0].time_ns);
    CHECK(first->bitmap.width() == 32 && first->bitmap.height() == 18);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK_EQ(pipeline.show()->serial, first->serial);
    CHECK(!pipeline.next_picture_in().has_value()); // a clock that stands brings nothing due

    // Running: every picture, in order, none before its time and none
    // long after it.
    auto const started = steady::now();
    media::PlaybackClock::State running = running_from(0, 100.0);
    running.at = started;
    clock->set(running);
    pipeline.clock_changed();
    double const stream_seconds = static_cast<double>(blocks.back().time_ns) / 1e9;
    std::vector<std::pair<std::int64_t, double>> shown; // the frame's time, and when it was asked for
    std::vector<double> had; // when the asking came back with it
    std::uint64_t serial = first->serial;
    while (seconds_since(started) < stream_seconds + 0.4) {
        double const at = seconds_since(started);
        std::shared_ptr<media::ShownPicture const> const picture = pipeline.show();
        if (picture->serial != serial) {
            serial = picture->serial;
            shown.emplace_back(picture->time_ns, at);
            had.push_back(seconds_since(started));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    // (A machine busy with other tests may be late to ask and find two
    // frames due at once: the older of the two is then rightly passed over.)
    CHECK(shown.size() >= blocks.size() - 4);
    CHECK(shown.size() <= blocks.size() - 1);
    double latest = 0;
    bool in_order = true;
    bool none_early = true;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        double const due = static_cast<double>(shown[i].first) / 1e9;
        // `at` was read before the picture was asked for, so a picture is
        // never seen earlier than `at`.
        none_early = none_early && shown[i].second + 0.003 >= due - 0.003;
        latest = std::max(latest, shown[i].second - due);
        in_order = in_order && (i == 0 || shown[i].first > shown[i - 1].first);
    }
    CHECK(in_order);
    CHECK(none_early);
    CHECK(latest < 0.150);
    CHECK(!shown.empty() && shown.back().first == blocks.back().time_ns);
    media::VideoPipeline::Counts const counts = pipeline.counts();
    CHECK_EQ(counts.failed, 0u);
    CHECK_EQ(counts.shown, serial);
    // What it says of the waits between pictures is what was measured here.
    // A picture was shown between the asking for it and the having of it,
    // so the longest wait is no shorter than the longest from one picture
    // had to the next asked for, and no longer than the longest from one
    // asked for to the next had — on a machine of any speed.
    double const longest_wait = pipeline.take_longest_wait_ms();
    double at_least = 0;
    double at_most = 0;
    double asked_before = -std::chrono::duration<double>(started - first_asked).count();
    double had_before = -std::chrono::duration<double>(started - first_had).count();
    for (std::size_t i = 0; i < shown.size(); ++i) {
        at_least = std::max(at_least, (shown[i].second - had_before) * 1000.0);
        at_most = std::max(at_most, (had[i] - asked_before) * 1000.0);
        asked_before = shown[i].second;
        had_before = had[i];
    }
    std::cout << "a pipeline by its clock: " << shown.size() << " pictures, the latest " << latest * 1000.0
              << " ms after its time, the longest wait " << longest_wait << " ms (between " << at_least << " and "
              << at_most << ")\n";
    CHECK(longest_wait >= at_least && longest_wait <= at_most);

    // A clock that may not pass a moment shows nothing from after it. (As
    // for a seek: the clock is told where playback stands before the
    // frames from there are handed over.)
    clock->set(standing_at(0));
    pipeline.clock_changed();
    pipeline.flush();
    for (media::WebmFrame const& block : blocks)
        pipeline.push(block.time_ns, block.data);
    clock->set(running_from(0, 0.3));
    pipeline.clock_changed();
    std::shared_ptr<media::ShownPicture const> held;
    for (int i = 0; i < 300; ++i) {
        held = pipeline.show();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(held && held->time_ns <= 300'000'000 && held->time_ns >= 250'000'000);
}

// Frames the page replaced are taken back where the decoder has not had
// them, and not where it has.
void test_frames_are_taken_back()
{
    std::vector<media::WebmFrame> const blocks = fixture_blocks();
    if (blocks.size() != 30)
        return;
    auto const clock = std::make_shared<media::PlaybackClock>();
    media::VideoPipeline pipeline(std::make_unique<FlatAccelerator>());
    clock->set(standing_at(0));
    pipeline.follow(clock, 250'000'000);
    for (media::WebmFrame const& block : blocks)
        pipeline.push(block.time_ns, block.data);
    // Standing at the start, the quarter second ahead is decoded (as far
    // as there is room for its pictures) and the rest waits.
    for (int i = 0; i < 2000 && !pipeline.show(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::size_t const waiting = pipeline.queued();
    CHECK(waiting >= 20u && waiting < 30u);
    // From two thirds of a second on: not decoded yet, so taken back.
    CHECK(pipeline.drop_from(blocks[20].time_ns));
    CHECK_EQ(pipeline.queued(), waiting - 10u);
    // Again, and nothing more goes.
    CHECK(pipeline.drop_from(blocks[20].time_ns));
    CHECK_EQ(pipeline.queued(), waiting - 10u);
    // From the second frame on: the decoder has had it.
    CHECK(!pipeline.drop_from(blocks[1].time_ns));
    CHECK_EQ(pipeline.queued(), waiting - 10u);
    // After a flush nothing has been decoded, and anything can go.
    pipeline.flush();
    CHECK(pipeline.drop_from(0));
}

// A track says from where, and to where, frames were taken out of it.
void test_a_track_says_what_changed()
{
    Bytes const stream = fixture_bytes("vp9-144p.webm");
    media::StreamBuffer buffer;
    CHECK(buffer.append(stream).ok);
    media::StreamBuffer::Track const* track = buffer.track_of(media::WebmTrack::Kind::Video);
    CHECK(track != nullptr);
    if (!track)
        return;
    CHECK_EQ(track->frames.size(), 30u);
    // Filed once, nothing was replaced.
    auto [from, to] = track->take_changed();
    CHECK(from > to);
    // The same stream appended over itself replaces every frame.
    buffer.reset_parser();
    CHECK(buffer.append(stream).ok);
    track = buffer.track_of(media::WebmTrack::Kind::Video);
    std::tie(from, to) = track->take_changed();
    CHECK_EQ(from, track->frames.begin()->first);
    CHECK_EQ(to, track->frames.rbegin()->first);
    // Asked again, nothing has changed since.
    std::tie(from, to) = track->take_changed();
    CHECK(from > to);
    // A removal from the middle to the end says where it began and ended.
    std::int64_t const last = track->frames.rbegin()->first;
    std::int64_t const first = track->frames.begin()->first;
    buffer.remove(0.5, 10.0);
    track = buffer.track_of(media::WebmTrack::Kind::Video);
    std::tie(from, to) = track->take_changed();
    CHECK(from >= 500'000'000 && from < 540'000'000);
    CHECK_EQ(to, last);
    CHECK_EQ(track->frames.begin()->first, first);
}

dom::Element* find_by_id(dom::Node& node, std::string_view id)
{
    if (node.is_element()) {
        auto& element = static_cast<dom::Element&>(node);
        if (dom::Attr const* const attribute = element.find_attribute("id"); attribute && attribute->value == id)
            return &element;
    }
    for (dom::Node* child : node.children()) {
        if (dom::Element* const found = find_by_id(*child, id))
            return found;
    }
    return nullptr;
}

// A picture with something to see in every part of it.
std::shared_ptr<Bitmap> patterned(int width, int height, int seed)
{
    auto picture = std::make_shared<Bitmap>(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            picture->set_pixel(x, y,
                Color::rgb(static_cast<std::uint8_t>((x * 7 + seed) & 255), static_cast<std::uint8_t>((y * 11 + seed * 3) & 255),
                    static_cast<std::uint8_t>((x * 3 + y * 5) & 255)));
        }
    }
    return picture;
}

// The page's painting with a video's place left open, and the video put
// there by the compositor's own arithmetic, is the picture the page paints
// when it draws the video itself: the same pixels under a rounded clip,
// under what the page lays over the video — opaque and translucent — and
// inside a group that is itself translucent.
void test_a_video_shown_apart_looks_the_same()
{
    text::FontManager::instance().set_system_fonts(false);
    std::string const html = R"HTML(<!DOCTYPE html>
<body style="margin:0;background:#246">
<div style="position:relative;width:200px;height:120px;border-radius:24px;overflow:hidden;margin:10px">
  <video id="one" style="width:200px;height:120px;display:block"></video>
  <div style="position:absolute;left:0;top:80px;width:200px;height:40px;background:rgba(0,0,0,0.5)"></div>
  <div style="position:absolute;left:20px;top:20px;width:40px;height:40px;background:#f80"></div>
  <div style="position:absolute;left:10px;top:90px;width:80px;height:10px;background:rgba(255,255,255,0.4)"></div>
</div>
<div style="opacity:0.6;margin:10px;background:#a31;padding:6px;width:160px">
  <video id="two" style="width:100px;height:60px;display:block"></video>
  <div style="width:150px;height:8px;margin-top:-20px;background:rgba(0,200,0,0.7)"></div>
</div>
<div style="width:120px;height:40px;overflow:hidden;margin:10px">
  <video id="three" style="width:160px;height:90px;display:block;margin-left:-20px;margin-top:-10px"></video>
</div>
</body>)HTML";
    auto document = std::make_unique<dom::Document>();
    html::parse_document_bytes_into(*document, html, nullptr);
    dom::Element* const one = find_by_id(*document, "one");
    dom::Element* const two = find_by_id(*document, "two");
    dom::Element* const three = find_by_id(*document, "three");
    CHECK(one && two && three);
    if (!one || !two || !three)
        return;
    std::shared_ptr<Bitmap> const first = patterned(64, 36, 40);
    std::shared_ptr<Bitmap> const second = patterned(48, 28, 170);
    std::shared_ptr<Bitmap> const third = patterned(32, 18, 90);
    layout::ImageMap images;
    images[one] = layout::PageImage { first, 1 };
    images[two] = layout::PageImage { second, 1 };
    images[three] = layout::PageImage { third, 1 };
    css::StyleMap const styles = css::resolve_styles(*document);
    layout::LayoutResult const page = layout::layout_document(*document, styles, 260.0f, &images);
    int const height = std::max(1, static_cast<int>(page.page_height + 0.5f));

    // As the page paints it, the videos drawn where they are.
    Bitmap painted(260, height, page.canvas_background);
    std::vector<paint::PaintedPicture> drawn_where;
    paint::paint_page(painted, page, 0, 0, nullptr, nullptr, &drawn_where);
    CHECK_EQ(drawn_where.size(), 3u);

    // With their places left open.
    std::vector<Bitmap const*> const apart { first.get(), second.get(), third.get() };
    Bitmap open(260, height, page.canvas_background);
    std::vector<paint::PaintedPicture> places;
    paint::paint_page(open, page, 0, 0, nullptr, nullptr, &places, &apart);
    CHECK_EQ(places.size(), 3u);
    if (places.size() != 3 || drawn_where.size() != 3)
        return;
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(places[i].bitmap == drawn_where[i].bitmap);
        CHECK(places[i].rect == drawn_where[i].rect);
        CHECK(places[i].drawn == drawn_where[i].drawn);
    }
    // (In the painter's order, not the document's.)
    auto const place_of = [&places](Bitmap const* bitmap) {
        return *std::find_if(places.begin(), places.end(), [bitmap](paint::PaintedPicture const& place) { return place.bitmap == bitmap; });
    };
    paint::PaintedPicture const place_one = place_of(first.get());
    paint::PaintedPicture const place_two = place_of(second.get());
    paint::PaintedPicture const place_three = place_of(third.get());
    // The first video's picture is fitted into its box, keeping its shape;
    // the third is cut by the box it overflows, and says both what shows
    // of it and where the whole of it would go.
    CHECK((place_one.rect == Rect { 10, 14, 200, 112 }));
    CHECK((place_one.drawn == Rect { 10, 14, 200, 112 }));
    CHECK((place_three.rect == Rect { 10, 210, 120, 40 }));
    CHECK((place_three.drawn == Rect { -10, 200, 160, 90 }));
    // Open means open: nothing of the page in the middle of the first
    // video, the page whole outside the rounded corner that cuts it, and
    // the opaque box laid over the video whole too.
    CHECK_EQ(static_cast<int>(open.pixel(110, 60).a), 0);
    CHECK_EQ(static_cast<int>(open.pixel(11, 15).a), 255);
    CHECK((open.pixel(11, 15) == painted.pixel(11, 15)));
    CHECK((open.pixel(40, 44) == Color::rgb(255, 136, 0)));
    // Under the translucent bar the page has a part of the pixel only.
    CHECK(open.pixel(150, 110).a > 100 && open.pixel(150, 110).a < 160);
    // In the translucent group, what shows of the video is its share.
    Color const in_group = open.pixel(place_two.rect.x + 50, place_two.rect.y + 10);
    CHECK(in_group.a > 80 && in_group.a < 125);

    // Put together as the compositor does it.
    Bitmap composed = open;
    ui::PicturePatches patches;
    for (paint::PaintedPicture const& place : places) {
        auto patch = std::make_shared<Bitmap>(place.rect.width, place.rect.height, Color::rgb(9, 9, 9));
        ui::compose_patch(*patch, open, place.rect, place.drawn, place.bitmap);
        patches.push_back({ place.rect, patch });
    }
    ui::put_patches(composed, patches, 0, 0);
    int worst = 0;
    std::size_t differing = 0;
    std::size_t not_opaque = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 260; ++x) {
            Color const a = painted.pixel(x, y);
            Color const b = composed.pixel(x, y);
            int const off = std::max({ std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b) });
            worst = std::max(worst, off);
            differing += off != 0;
            not_opaque += b.a != 255;
        }
    }
    CHECK_EQ(not_opaque, 0u);
    // A colour laid over the video in two steps instead of one may round
    // the other way: by one in 255, and nowhere by more.
    CHECK(worst <= 1);
    if (worst > 1)
        std::cerr << "test_video_layer: the worst pixel is off by " << worst << " (" << differing << " pixels differ)\n";
    // Where no two things that cover a pixel in part lie one on the other,
    // nothing is rounded twice, and the bytes are the same: the third
    // video whole; the first above the dark bar, its rounded corners and
    // the opaque box over it included; and under the dark bar, away from
    // the corners and from the pale bar that lies on it.
    Rect const above_the_bar { place_one.rect.x, place_one.rect.y, place_one.rect.width, 90 - place_one.rect.y };
    Rect const under_the_bar { 110, 92, 70, 30 };
    for (Rect const rect : { above_the_bar, under_the_bar, place_three.rect }) {
        std::size_t off = 0;
        for (int y = rect.y; y < rect.bottom(); ++y) {
            for (int x = rect.x; x < rect.right(); ++x)
                off += !(painted.pixel(x, y) == composed.pixel(x, y));
        }
        CHECK_EQ(off, 0u);
    }

    // Without a frame yet the place is black under what the page laid over.
    Bitmap before(place_one.rect.width, place_one.rect.height, Color::rgb(9, 9, 9));
    ui::compose_patch(before, open, place_one.rect, place_one.drawn, nullptr);
    CHECK((before.pixel(100, 40) == Color::rgb(0, 0, 0)));
    CHECK((before.pixel(30, 30) == Color::rgb(255, 136, 0)));
}

// The compositor puts a video's pictures on the page as they come due with
// nobody else asking for them: the page's thread — here, this test — does
// nothing at all for the whole second.
void test_the_compositor_shows_pictures_while_the_page_is_busy()
{
    std::vector<media::WebmFrame> const blocks = fixture_blocks();
    if (blocks.size() != 30)
        return;
    auto const clock = std::make_shared<media::PlaybackClock>();
    auto const video = std::make_shared<media::VideoPipeline>(std::make_unique<FlatAccelerator>());
    clock->set(standing_at(0));
    video->follow(clock, 250'000'000);
    for (media::WebmFrame const& block : blocks)
        video->push(block.time_ns, block.data);

    // A page 64 by 48, blue, with the video's place open in the middle of
    // it and a translucent red band painted over the top of the place.
    auto page = std::make_shared<Bitmap>(64, 48, Color::rgb(0, 0, 200));
    Rect const place { 8, 8, 32, 24 };
    page->punch(place);
    page->fill_rect(Rect { 8, 8, 32, 4 }, Color::rgba(255, 0, 0, 128));

    struct Said {
        double at = 0;
        std::int64_t frame_ns = -1;
        bool page_is_new = false;
        bool whole = false;
        std::shared_ptr<Bitmap const> patch;
        std::shared_ptr<Bitmap const> page;
    };
    std::mutex mutex;
    std::vector<Said> said;
    steady::time_point started {};
    {
        ui::PageCompositor compositor([&](std::shared_ptr<Bitmap const> the_page, std::shared_ptr<ui::PicturePatches const> patches,
                                          bool page_is_new, bool whole) {
            std::shared_ptr<media::ShownPicture const> const frame = video->shown();
            std::lock_guard const lock(mutex);
            said.push_back({ seconds_since(started), frame ? frame->time_ns : -1, page_is_new, whole,
                patches->empty() ? nullptr : patches->front().bitmap, std::move(the_page) });
        });
        started = steady::now();
        media::PlaybackClock::State running = running_from(0, 100.0);
        running.at = started;
        clock->set(running);
        video->clock_changed();
        compositor.submit(page, { ui::VideoLayer { place, place, video } }, true);
        // The page is busy: nothing here looks at the video for the whole
        // stream and a little more.
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        ui::PageCompositor::Counts const counts = compositor.counts();
        // (Thirty on a machine that wakes a thread when it asked to be
        // woken. A shared runner wakes it tens of milliseconds late now and
        // then, and the picture that came due meanwhile is rightly passed
        // over for the one after: half of them is still a video playing,
        // and far from the handful a compositor that did not wake by
        // itself would show.)
        CHECK(counts.published >= 15u);
        CHECK(counts.composed >= counts.published);
    }
    std::lock_guard const lock(mutex);
    CHECK(said.size() >= 15u);
    if (said.empty())
        return;
    // The page's picture is said first, as it was handed over, and only
    // that once: after it only the patch is new.
    CHECK(said.front().page_is_new && said.front().whole && said.front().page == page);
    CHECK(std::none_of(said.begin() + 1, said.end(), [](Said const& each) { return each.page_is_new; }));
    // Every picture at its time: a new frame in each saying after the
    // first and none before it is due. How late is held two ways, because
    // a shared runner holds a thread back a tenth of a second now and then:
    // the middle one of them within a twentieth of a second of its time
    // (a compositor late by habit is late in the middle too), and none a
    // quarter of a second late nor two sayings a quarter of a second apart.
    double latest = 0;
    double longest_gap = 0;
    bool none_early = true;
    bool in_order = true;
    std::vector<double> late;
    for (std::size_t i = 1; i < said.size(); ++i) {
        double const due = static_cast<double>(said[i].frame_ns) / 1e9;
        none_early = none_early && said[i].at >= due - 0.003;
        latest = std::max(latest, said[i].at - due);
        late.push_back(said[i].at - due);
        longest_gap = std::max(longest_gap, said[i].at - said[i - 1].at);
        in_order = in_order && said[i].frame_ns > said[i - 1].frame_ns;
    }
    std::sort(late.begin(), late.end());
    double const late_in_the_middle = late.empty() ? 0.0 : late[late.size() / 2];
    std::cout << "a compositor by itself: " << said.size() << " sayings, the latest " << latest * 1000.0
              << " ms after its time, the middle one " << late_in_the_middle * 1000.0 << " ms, the longest gap "
              << longest_gap * 1000.0 << " ms\n";
    CHECK(in_order);
    CHECK(none_early);
    CHECK(late_in_the_middle < 0.050);
    CHECK(latest < 0.250);
    CHECK(longest_gap < 0.250);
    CHECK_EQ(said.back().frame_ns, blocks.back().time_ns);
    // The patch is the video's place: the picture where the place is open,
    // and the page's band over the picture where the page painted one.
    std::shared_ptr<Bitmap const> const patch = said.back().patch;
    CHECK(patch && patch->width() == 32 && patch->height() == 24);
    if (patch) {
        std::shared_ptr<media::ShownPicture const> const frame = video->shown();
        Color const white = frame->bitmap.pixel(0, 0);
        CHECK(white.r > 240 && white.g > 240 && white.b > 240);
        CHECK((patch->pixel(16, 12) == white));
        Color const band = patch->pixel(16, 2);
        CHECK(band.a == 255 && band.r == 255 && band.g > 110 && band.g < 135 && band.b == band.g);
    }
}

}

int main()
{
    test_the_sound_clock();
    test_the_clock();
    test_a_pipeline_shows_pictures_by_its_clock();
    test_frames_are_taken_back();
    test_a_track_says_what_changed();
    test_a_video_shown_apart_looks_the_same();
    test_the_compositor_shows_pictures_while_the_page_is_busy();
    return test::report("test_video_layer");
}
