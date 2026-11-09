// H.264 decoded on the processor, for a machine whose graphics have no
// H.264 decoder (or none we can reach): the same accelerator interface
// the hardware paths implement, so the host's buffer bookkeeping
// (media/H264Decoder.h) drives it unchanged. Written from ITU-T H.264
// alone; held to ffmpeg's decoding of the same files bit for bit.
#include "media/H264Software.h"

#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sashfold::media {

namespace h264sw {

void Picture::allocate(int mb_w, int mb_h)
{
    mb_width = mb_w;
    mb_height = mb_h;
    width = mb_w * 16;
    height = mb_h * 16;
    std::size_t const luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    y.assign(luma, 128);
    cb.assign(luma / 4, 128);
    cr.assign(luma / 4, 128);
    std::size_t const blocks = static_cast<std::size_t>(mb_w) * static_cast<std::size_t>(mb_h) * 16;
    for (int l = 0; l < 2; ++l) {
        mv[static_cast<std::size_t>(l)].assign(blocks, Mv {});
        ref_idx[static_cast<std::size_t>(l)].assign(blocks, -1);
        ref_pic[static_cast<std::size_t>(l)].assign(blocks, -1);
    }
    mb_intra.assign(static_cast<std::size_t>(mb_w) * static_cast<std::size_t>(mb_h), 1);
}

}

namespace {

using namespace h264sw;

constexpr int slot_count = 17;

// How many threads decode a picture: SASHFOLD_H264_THREADS, or the
// machine's cores up to eight (past that the parsing, which is one
// thread's, is the limit).
int picture_thread_count()
{
    if (char const* const forced = std::getenv("SASHFOLD_H264_THREADS"))
        return std::clamp(std::atoi(forced), 1, 64);
    unsigned const cores = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(cores), 1, 8);
}

// Threads kept for the decoder's life, each running the job it is given
// once per picture.
class WorkerPool {
public:
    explicit WorkerPool(int count)
    {
        for (int i = 0; i < count; ++i)
            m_threads.emplace_back([this] { work(); });
    }
    ~WorkerPool()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        for (std::thread& t : m_threads)
            t.join();
    }
    WorkerPool(WorkerPool const&) = delete;
    WorkerPool& operator=(WorkerPool const&) = delete;

    // Every thread runs `job` once; wait() returns when all have.
    void start(std::function<void()> job)
    {
        {
            std::lock_guard lock(m_mutex);
            m_job = std::move(job);
            m_running = static_cast<int>(m_threads.size());
            ++m_generation;
        }
        m_wake.notify_all();
    }
    void wait()
    {
        std::unique_lock lock(m_mutex);
        m_finished.wait(lock, [this] { return m_running == 0; });
    }

private:
    void work()
    {
        std::uint64_t seen = 0;
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
                if (m_stop)
                    return;
                seen = m_generation;
                job = m_job;
            }
            job();
            {
                std::lock_guard lock(m_mutex);
                --m_running;
            }
            m_finished.notify_all();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_finished;
    std::function<void()> m_job;
    std::uint64_t m_generation = 0;
    int m_running = 0;
    bool m_stop = false;
};

// A spinning thread's hint to the core that shares it (SMT) to go on.
inline void spin_pause()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// A count the picture's threads wait on, with how many sleep on it.
struct Counter {
    std::atomic<int> value { 0 };
    std::atomic<int> sleepers { 0 };
};

// Waiting on counters: a short spin first (a neighbor's macroblock is
// microseconds away), then asleep on one condition, woken by a store only
// when someone sleeps on that counter. (std::atomic's own wait lost a
// wakeup here under libstdc++ 16: the counter held what the sleeper
// waited for and nothing woke it.)
class Progress {
public:
    void wait_at_least(Counter& counter, int least)
    {
        for (int spin = 0; spin < 200; ++spin) {
            if (counter.value.load(std::memory_order_acquire) >= least)
                return;
            spin_pause();
        }
        std::unique_lock lock(m_mutex);
        // The sleeper is counted before it looks; a store is made before
        // its maker looks for sleepers: one of the two sees the other.
        counter.sleepers.fetch_add(1, std::memory_order_seq_cst);
        m_wake.wait(lock, [&] { return counter.value.load(std::memory_order_seq_cst) >= least; });
        counter.sleepers.fetch_sub(1, std::memory_order_relaxed);
    }

    void publish(Counter& counter, int now)
    {
        counter.value.store(now, std::memory_order_seq_cst);
        if (counter.sleepers.load(std::memory_order_seq_cst) > 0) {
            {
                std::lock_guard lock(m_mutex);
            }
            m_wake.notify_all();
        }
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
};

// One thread: each macroblock reconstructed as soon as it is parsed, the
// picture deblocked after its last slice.
class DirectSink final : public MacroblockSink {
public:
    explicit DirectSink(Frame& frame)
        : m_frame(frame)
    {
    }
    MbSyntax& begin(int) override
    {
        m_mb = MbSyntax {};
        return m_mb;
    }
    void end(Slice const& slice, int mb_addr) override { reconstruct_macroblock(m_frame, slice, mb_addr, m_mb); }

private:
    Frame& m_frame;
    MbSyntax m_mb;
};

// Several threads: the caller parses into a ring of macroblock rows while
// the others take rows in order, each reconstructing its row two
// macroblocks behind the row above it (what intra prediction reads: the
// left, upper-left, upper and upper-right neighbors, before filtering)
// and then deblocking the row above it two macroblocks behind that row's
// own filtering (8.7's raster order: a macroblock's upper edge after the
// upper macroblock's and its right neighbor's left edge). A row is
// filtered only once the row under it is reconstructed, so no prediction
// reads a filtered sample. Slices must come in raster order.
class WavefrontSink final : public MacroblockSink {
public:
    WavefrontSink(Frame& frame, int ring_rows)
        : m_frame(frame)
        , m_width(frame.picture->mb_width)
        , m_height(frame.picture->mb_height)
        , m_ring_rows(std::min(ring_rows, frame.picture->mb_height))
        , m_ring(static_cast<std::size_t>(m_ring_rows) * static_cast<std::size_t>(m_width))
        , m_slice_of(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height), nullptr)
        , m_parsed(std::make_unique<Counter[]>(static_cast<std::size_t>(m_height)))
        , m_reconstructed(std::make_unique<Counter[]>(static_cast<std::size_t>(m_height)))
        , m_deblocked(std::make_unique<Counter[]>(static_cast<std::size_t>(m_height)))
    {
    }

    MbSyntax& begin(int mb_addr) override
    {
        // A macroblock at or before one already begun is a broken stream's: parsed, not shown.
        if (mb_addr < m_next) {
            m_dropping = true;
            m_scratch = MbSyntax {};
            return m_scratch;
        }
        m_dropping = false;
        if (mb_addr > m_next)
            parsed_up_to(mb_addr); // a gap: the macroblocks skipped are as they will stay
        m_next = mb_addr + 1;
        int const row = mb_addr / m_width;
        if (row >= m_ring_rows)
            m_progress.wait_at_least(m_reconstructed[static_cast<std::size_t>(row - m_ring_rows)], m_width);
        MbSyntax& mb = slot(mb_addr);
        mb = MbSyntax {};
        return mb;
    }

    void end(Slice const& slice, int mb_addr) override
    {
        if (m_dropping)
            return;
        m_slice_of[static_cast<std::size_t>(mb_addr)] = &slice;
        // Said eight macroblocks at a time and at each row's end: the
        // parsing is the slowest stage, and every word of it may wake the
        // thread waiting on it.
        if ((mb_addr + 1) % 8 == 0 || (mb_addr + 1) % m_width == 0)
            parsed_up_to(mb_addr + 1);
    }

    // After the last slice: what was not parsed never will be.
    void parsed_all() { parsed_up_to(m_width * m_height); }

    // Rows until none is left; run by every thread.
    void work()
    {
        for (;;) {
            int const job = m_next_job.fetch_add(1, std::memory_order_relaxed);
            if (job > m_height)
                return;
            if (job < m_height)
                reconstruct_row(job);
            else
                m_progress.wait_at_least(m_reconstructed[static_cast<std::size_t>(m_height - 1)], m_width);
            if (job > 0)
                deblock_row(job - 1);
        }
    }

private:
    MbSyntax& slot(int mb_addr)
    {
        int const row = mb_addr / m_width, x = mb_addr % m_width;
        return m_ring[static_cast<std::size_t>((row % m_ring_rows) * m_width + x)];
    }

    // Each row's parsing said on its own counter, so a store wakes only
    // the thread on that row.
    void parsed_up_to(int mb_addr)
    {
        while (m_said < mb_addr) {
            int const row = m_said / m_width;
            int const upto = std::min(mb_addr, (row + 1) * m_width);
            m_progress.publish(m_parsed[static_cast<std::size_t>(row)], upto - row * m_width);
            m_said = upto;
        }
    }

    void reconstruct_row(int row)
    {
        for (int x = 0; x < m_width; ++x) {
            int const addr = row * m_width + x;
            if (row > 0)
                m_progress.wait_at_least(m_reconstructed[static_cast<std::size_t>(row - 1)], std::min(x + 2, m_width));
            m_progress.wait_at_least(m_parsed[static_cast<std::size_t>(row)], x + 1);
            if (Slice const* const slice = m_slice_of[static_cast<std::size_t>(addr)])
                reconstruct_macroblock(m_frame, *slice, addr, slot(addr));
            m_progress.publish(m_reconstructed[static_cast<std::size_t>(row)], x + 1);
        }
    }

    void deblock_row(int row)
    {
        for (int x = 0; x < m_width; ++x) {
            if (row > 0)
                m_progress.wait_at_least(m_deblocked[static_cast<std::size_t>(row - 1)], std::min(x + 2, m_width));
            deblock_macroblock(m_frame, x, row);
            m_progress.publish(m_deblocked[static_cast<std::size_t>(row)], x + 1);
        }
    }

    Frame& m_frame;
    int m_width;
    int m_height;
    int m_ring_rows;
    std::vector<MbSyntax> m_ring;
    std::vector<Slice const*> m_slice_of; // per macroblock: its slice once parsed whole
    MbSyntax m_scratch;
    bool m_dropping = false;
    int m_next = 0; // the parser's: the first address it may still begin
    int m_said = 0; // the parser's: macroblocks before it said to be as they will stay
    std::unique_ptr<Counter[]> m_parsed; // per row: macroblocks as they will stay
    std::unique_ptr<Counter[]> m_reconstructed; // per row: macroblocks done
    std::unique_ptr<Counter[]> m_deblocked;
    std::atomic<int> m_next_job { 0 };
    Progress m_progress;
};

class SoftwareH264 final : public H264Accelerator {
public:
    SoftwareH264(int max_macroblocks, int threads)
        : m_max_macroblocks(max_macroblocks)
        , m_thread_count(threads > 0 ? threads : picture_thread_count())
    {
        m_pictures.resize(slot_count);
    }

    std::string const& device() const override { return m_device; }
    bool is_software() const override { return true; }
    int slots() const override { return slot_count; }

    bool set_parameter_sets(std::vector<H264Sps> const& sps, std::vector<H264Pps> const& pps) override
    {
        m_sps.clear();
        m_pps.clear();
        for (H264Sps const& s : sps)
            m_sps[s.id] = s;
        for (H264Pps const& p : pps)
            m_pps[p.id] = p;
        m_scales.clear();
        return true;
    }

    bool decode(H264PictureInfo const& info) override
    {
        if (!info.sps || !info.pps || info.slot < 0 || info.slot >= slot_count)
            return false;
        H264Sps const& sps = *info.sps;
        H264Pps const& pps = *info.pps;
        // What is written here: frames, 4:2:0, 8 bits, one slice group.
        if (!sps.frame_mbs_only || sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8
            || pps.num_slice_groups != 1 || sps.pic_width_in_mbs <= 0 || sps.pic_height_in_map_units <= 0
            || static_cast<long long>(sps.pic_width_in_mbs) * sps.frame_height_in_mbs() > m_max_macroblocks)
            return false;
        int const mb_w = sps.pic_width_in_mbs, mb_h = sps.frame_height_in_mbs();
        Picture& pic = m_pictures[static_cast<std::size_t>(info.slot)];
        if (pic.mb_width != mb_w || pic.mb_height != mb_h)
            pic.allocate(mb_w, mb_h);
        pic.serial = ++m_serial;
        pic.poc = std::min(info.poc_top, info.poc_bottom);
        pic.valid = true;
        // what read() shows: the sequence's cropping (7.4.2.1.1, frames, 4:2:0)
        pic.crop_x = sps.frame_cropping ? sps.crop_left * 2 : 0;
        pic.crop_y = sps.frame_cropping ? sps.crop_top * 2 : 0;
        pic.shown_width = sps.width();
        pic.shown_height = sps.height();
        std::fill(pic.mb_intra.begin(), pic.mb_intra.end(), std::uint8_t { 1 });

        Frame frame;
        frame.picture = &pic;
        frame.slice_alpha.assign(info.slices.size(), 0);
        frame.slice_beta.assign(info.slices.size(), 0);
        frame.slice_disable.assign(info.slices.size(), 0);
        frame.mbs.assign(static_cast<std::size_t>(mb_w) * static_cast<std::size_t>(mb_h), MbState {});
        frame.chroma_qp_offset[0] = pps.chroma_qp_index_offset;
        frame.chroma_qp_offset[1] = pps.second_chroma_qp_index_offset; // the first's value where the set gives none

        LevelScales const& scales = scales_for(pps);
        int const max_frame_num = 1 << sps.log2_max_frame_num;
        std::vector<H264ListPicture> const references = h264_list_pictures(info, max_frame_num);
        int const current_poc = std::min(info.poc_top, info.poc_bottom);

        // Threads where the picture has rows enough and its slices come in raster order.
        bool in_order = true;
        for (std::size_t i = 1; i < info.headers.size(); ++i)
            in_order = in_order && info.headers[i].first_mb_in_slice > info.headers[i - 1].first_mb_in_slice;
        std::optional<WavefrontSink> wavefront;
        std::optional<DirectSink> direct;
        if (m_thread_count > 1 && in_order && mb_h >= 4) {
            if (!m_pool)
                m_pool = std::make_unique<WorkerPool>(m_thread_count - 1);
            wavefront.emplace(frame, m_thread_count + 2);
            frame.sink = &*wavefront;
            m_pool->start([&wavefront] { wavefront->work(); });
        } else {
            direct.emplace(frame);
            frame.sink = &*direct;
        }

        // Each slice kept until the picture is done: the threads reconstruct from it.
        std::vector<Slice> slices;
        slices.reserve(info.slices.size());
        bool any = false;
        for (std::size_t i = 0; i < info.slices.size() && i < info.headers.size(); ++i) {
            H264SliceHeader const& h = info.headers[i];
            Slice& slice = slices.emplace_back();
            slice.sps = &sps;
            slice.pps = &pps;
            slice.header = &h;
            slice.number = static_cast<int>(i);
            slice.type = h.slice_type == H264SliceType::SP ? H264SliceType::P : (h.slice_type == H264SliceType::SI ? H264SliceType::I : h.slice_type);
            if (h.slice_type == H264SliceType::SP || h.slice_type == H264SliceType::SI)
                continue; // the Extended profile's switching slices are not written
            slice.qp = std::clamp(pps.pic_init_qp + h.slice_qp_delta, 0, 51); // 7.4.3's range, held for a broken stream
            slice.current_poc = current_poc;
            slice.scales = &scales;
            bool const is_b = slice.type == H264SliceType::B;
            if (slice.type != H264SliceType::I) {
                for (std::size_t list = 0; list < (is_b ? 2u : 1u); ++list) {
                    for (H264ListPicture const& e : h264_reference_list(references, h, list, is_b, current_poc, max_frame_num, info.frame_num)) {
                        RefEntry r;
                        if (e.slot >= 0 && e.slot < slot_count && e.slot != info.slot && m_pictures[static_cast<std::size_t>(e.slot)].valid
                            && m_pictures[static_cast<std::size_t>(e.slot)].mb_width == mb_w && m_pictures[static_cast<std::size_t>(e.slot)].mb_height == mb_h)
                            r.picture = &m_pictures[static_cast<std::size_t>(e.slot)];
                        r.long_term = e.long_term;
                        r.poc = e.poc;
                        slice.list[list].push_back(r);
                    }
                }
            }
            if (slice.type == H264SliceType::P)
                slice.weighted_mode = pps.weighted_pred ? 1 : 0;
            else if (is_b)
                slice.weighted_mode = pps.weighted_bipred_idc;
            if (slice.weighted_mode == 2)
                implicit_weights(slice);
            frame.slice_alpha[i] = h.slice_alpha_c0_offset_div2 * 2;
            frame.slice_beta[i] = h.slice_beta_offset_div2 * 2;
            frame.slice_disable[i] = h.disable_deblocking_filter_idc;

            std::vector<std::uint8_t> const rbsp = h264_rbsp(info.slices[i]);
            if (h.header_bits < 8)
                continue;
            BitReader reader(rbsp.data(), rbsp.size(), static_cast<std::size_t>(h.header_bits - 8));
            if (decode_slice_data(frame, slice, reader))
                any = true;
        }
        if (wavefront) {
            wavefront->parsed_all();
            wavefront->work();
            m_pool->wait();
        } else {
            deblock_picture(frame);
        }
        return any;
    }

    std::optional<Nv12Picture> read(int slot) override
    {
        if (slot < 0 || slot >= slot_count || !m_pictures[static_cast<std::size_t>(slot)].valid)
            return std::nullopt;
        Picture const& p = m_pictures[static_cast<std::size_t>(slot)];
        int const crop_x = p.crop_x, crop_y = p.crop_y;
        Nv12Picture out;
        out.width = std::min(p.shown_width, p.width - crop_x);
        out.height = std::min(p.shown_height, p.height - crop_y);
        if (out.width <= 0 || out.height <= 0)
            return std::nullopt;
        out.luma.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
        for (int y = 0; y < out.height; ++y)
            std::copy_n(&p.y[static_cast<std::size_t>((y + crop_y) * p.width + crop_x)], out.width, &out.luma[static_cast<std::size_t>(y * out.width)]);
        int const cw = out.chroma_width(), ch = out.chroma_height();
        out.chroma.resize(static_cast<std::size_t>(cw) * 2 * static_cast<std::size_t>(ch));
        int const pcw = p.width / 2;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                std::size_t const src = static_cast<std::size_t>((y + crop_y / 2) * pcw + x + crop_x / 2);
                out.chroma[static_cast<std::size_t>((y * cw + x) * 2)] = p.cb[src];
                out.chroma[static_cast<std::size_t>((y * cw + x) * 2 + 1)] = p.cr[src];
            }
        return out;
    }

    void reset() override
    {
        for (Picture& p : m_pictures)
            p.valid = false;
    }

private:
    LevelScales const& scales_for(H264Pps const& pps)
    {
        auto found = m_scales.find(pps.id);
        if (found != m_scales.end())
            return found->second;
        LevelScales& s = m_scales[pps.id];
        s.build(pps.scaling);
        return s;
    }

    // 8.4.2.3.1: implicit weights for every pair of references.
    static void implicit_weights(Slice& slice)
    {
        slice.implicit.assign(32 * 32, { 32, 32 });
        for (std::size_t r0 = 0; r0 < slice.list[0].size() && r0 < 32; ++r0)
            for (std::size_t r1 = 0; r1 < slice.list[1].size() && r1 < 32; ++r1) {
                RefEntry const& p0 = slice.list[0][r0];
                RefEntry const& p1 = slice.list[1][r1];
                int const tb = std::clamp(slice.current_poc - p0.poc, -128, 127);
                int const td = std::clamp(p1.poc - p0.poc, -128, 127);
                std::array<int, 2> w { 32, 32 };
                if (td != 0 && !p0.long_term && !p1.long_term) {
                    int const tx = (16384 + std::abs(td / 2)) / td;
                    int const dsf = std::clamp((tb * tx + 32) >> 6, -1024, 1023);
                    if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128)
                        w = { 64 - (dsf >> 2), dsf >> 2 };
                }
                slice.implicit[r0 * 32 + r1] = w;
            }
    }

    std::string m_device = "software (Sashfold's own H.264 decoder)";
    std::vector<Picture> m_pictures;
    std::map<int, H264Sps> m_sps;
    std::map<int, H264Pps> m_pps;
    std::map<int, LevelScales> m_scales;
    std::int32_t m_serial = 0;
    int m_max_macroblocks;
    int m_thread_count;
    std::unique_ptr<WorkerPool> m_pool; // made at the first picture that uses it
};

}

std::unique_ptr<H264Accelerator> open_software_h264(int max_macroblocks, int threads)
{
    return std::make_unique<SoftwareH264>(max_macroblocks, threads);
}

}
