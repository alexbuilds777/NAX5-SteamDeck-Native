#include "nax5/nax5audiotrim.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_failed = 0;

static void expect(bool condition, const char *name)
{
    if (condition)
        std::printf("ok %s\n", name);
    else
    {
        std::fprintf(stderr, "FAIL %s\n", name);
        g_failed++;
    }
}

// 48 kHz stereo s16: 192 bytes per ms
static const uint64_t MS = 192;

static Nax5AudioTrimParams params()
{
    return { 3000, 40 * MS, 20 * MS, 40 * MS };
}

// Feed a saw-tooth between lo and hi ms (as the SDL queue + ring do) every 10 ms for
// duration_ms, starting at start_ms. Returns the total bytes the trimmer asked to drop.
static uint64_t feed(Nax5AudioTrim &trim, uint64_t start_ms, uint64_t duration_ms, uint64_t lo, uint64_t hi)
{
    uint64_t dropped = 0;
    for (uint64_t t = 0; t <= duration_ms; t += 10)
    {
        const uint64_t phase = (t / 10) % 5;
        const uint64_t queued_ms = lo + (hi - lo) * phase / 4;
        dropped += nax5_audio_trim_update(trim, params(), start_ms + t, queued_ms * MS);
    }
    return dropped;
}

static void test_no_trim_within_headroom()
{
    Nax5AudioTrim trim;
    // Standing minimum 50 ms <= headroom 40 + hysteresis 20: leave it alone.
    expect(feed(trim, 1000, 10000, 50, 120) == 0, "no trim while the minimum is within headroom");
}

static void test_trims_standing_latency_seen_in_the_field()
{
    Nax5AudioTrim trim;
    // The live session: 140-210 ms queued. Minimum 140 > 60, so each 3 s window drops
    // min(140 - 40, 40) = 40 ms.
    const uint64_t first = feed(trim, 1000, 3000, 140, 210);
    expect(first == 40 * MS, "first window drops one 40 ms step");
    const uint64_t more = feed(trim, 4010, 5990, 140, 210);
    expect(more == 2 * 40 * MS, "later windows keep stepping while the backlog persists");
}

static void test_does_not_trim_before_the_window_ends()
{
    Nax5AudioTrim trim;
    expect(feed(trim, 1000, 2900, 300, 400) == 0, "nothing dropped inside the first window");
}

static void test_short_dip_prevents_trim()
{
    Nax5AudioTrim trim;
    uint64_t dropped = 0;
    // High backlog, but one observation near empty: that headroom was needed.
    for (uint64_t t = 0; t <= 3000; t += 10)
        dropped += nax5_audio_trim_update(trim, params(), 1000 + t, (t == 1500 ? 10 : 200) * MS);
    expect(dropped == 0, "a single dip in the window keeps the buffer");
}

static void test_step_is_capped_and_bounded_by_excess()
{
    Nax5AudioTrim trim;
    // Minimum 70 ms: excess over headroom is 30 ms, below the 40 ms step.
    expect(feed(trim, 1000, 3000, 70, 90) == 30 * MS, "drop is limited to the excess over headroom");
}

static void test_now_zero_is_safe()
{
    Nax5AudioTrim trim;
    // A monotonic clock of 0 must not be mistaken for "no window yet" forever.
    nax5_audio_trim_update(trim, params(), 0, 500 * MS);
    expect(trim.window_start_ms != 0, "window starts even at time 0");
    expect(nax5_audio_trim_update(trim, params(), 3001, 500 * MS) == 40 * MS, "trims after a window that began at 0");
}

// Ring of interleaved stereo S16 whose read position sits near the end, so the splice
// wraps around. Left channel ramps 0,1,2..., right is its negation.
struct TestRing
{
    std::vector<char> buf;
    size_t read_pos;
    char *at(size_t offset) { return buf.data() + (read_pos + offset) % buf.size(); }
    int16_t sample(size_t frame, size_t ch)
    {
        auto f = [this](size_t o) { return at(o); };
        return nax5_audio_ring_get_s16(f, frame * 4 + ch * 2);
    }
};

static TestRing make_ring(size_t frames, size_t read_pos)
{
    TestRing r{ std::vector<char>(frames * 4), read_pos };
    for (size_t i = 0; i < frames; i++)
    {
        auto f = [&r](size_t o) { return r.at(o); };
        nax5_audio_ring_set_s16(f, i * 4, (int16_t)(i * 100));
        nax5_audio_ring_set_s16(f, i * 4 + 2, (int16_t)-(int)(i * 100));
    }
    return r;
}

static void test_splice_blends_old_into_new()
{
    // 100 frames, read position 3 bytes before the end (odd: samples straddle the wrap)
    TestRing r = make_ring(100, 100 * 4 - 3);
    auto at = [&r](size_t o) { return r.at(o); };
    nax5_audio_splice_s16(at, 2, 100 * 4, 40 * 4, 9);
    // First blended frame is mostly old (frame 0 = 0), last mostly new (frame 48 = 4800).
    const int first = r.sample(40, 0), last = r.sample(48, 0);
    expect(first > 0 && first < 4000 / 5, "first blended frame is close to the old audio");
    expect(last > 4000 && last <= 4800, "last blended frame is close to the new audio");
    expect(r.sample(49, 0) == 4900, "frames after the fade are untouched");
    expect(r.sample(44, 1) == -r.sample(44, 0), "channels are blended independently");
    bool monotonic = true;
    for (size_t f = 40; f < 49; f++)
        monotonic = monotonic && r.sample(f + 1, 0) >= r.sample(f, 0);
    expect(monotonic, "blend has no step");
}

static void test_splice_stays_within_fill()
{
    TestRing r = make_ring(100, 0);
    auto at = [&r](size_t o) { return r.at(o); };
    // Only 3 frames left after the cut: the fade must not touch anything beyond them.
    nax5_audio_splice_s16(at, 2, 43 * 4, 40 * 4, 480);
    expect(r.sample(43, 0) == 4300, "no write beyond the ring fill");
    expect(r.sample(42, 0) != 4200, "the frames within the fill are blended");
    TestRing r2 = make_ring(10, 0);
    auto at2 = [&r2](size_t o) { return r2.at(o); };
    nax5_audio_splice_s16(at2, 2, 10 * 4, 10 * 4, 480);
    expect(r2.sample(0, 0) == 0 && r2.sample(9, 0) == 900, "dropping everything leaves the ring alone");
}

int main()
{
    test_splice_blends_old_into_new();
    test_splice_stays_within_fill();
    test_no_trim_within_headroom();
    test_trims_standing_latency_seen_in_the_field();
    test_does_not_trim_before_the_window_ends();
    test_short_dip_prevents_trim();
    test_step_is_capped_and_bounded_by_excess();
    test_now_zero_is_safe();
    if (g_failed)
    {
        std::fprintf(stderr, "%d NAX5 audio trim tests failed\n", g_failed);
        return 1;
    }
    std::printf("All NAX5 audio trim tests passed\n");
    return 0;
}
