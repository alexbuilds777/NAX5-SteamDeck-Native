#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// NAX5: slow trim of standing audio latency.
//
// Audio waiting for playback (SDL queue + ring buffer) saw-tooths with network jitter
// and with every device period (50 ms), so the hard cap's "stayed above the cap for
// 500 ms" check keeps resetting and almost never fires: in a 6 min WAN session the
// latency crept from 20-90 ms up to 140-210 ms and stayed there.
//
// Instead, track the minimum over a window. Audio that stayed queued for the whole
// window was never used as jitter headroom, so dropping it cannot starve playback
// (unless the jitter grows, which the queue then absorbs again). Only small steps are
// dropped, so each skip is short.

struct Nax5AudioTrimParams
{
	uint64_t window_ms;        // observation window
	uint64_t headroom_bytes;   // standing minimum to keep
	uint64_t hysteresis_bytes; // trim only when the minimum exceeds headroom by this much
	uint64_t max_step_bytes;   // at most this much per window
};

struct Nax5AudioTrim
{
	uint64_t window_start_ms = 0;
	uint64_t window_min_bytes = 0;
};

// Feed one observation of the queued audio. Returns how many bytes of the oldest
// audio to drop now, 0 for none. The caller may drop less (e.g. ring fill, frame
// alignment); the next window then simply sees the remainder.
inline uint64_t nax5_audio_trim_update(Nax5AudioTrim &trim, const Nax5AudioTrimParams &params,
		uint64_t now_ms, uint64_t queued_bytes)
{
	if(!trim.window_start_ms)
	{
		trim.window_start_ms = now_ms ? now_ms : 1;
		trim.window_min_bytes = queued_bytes;
		return 0;
	}
	trim.window_min_bytes = std::min(trim.window_min_bytes, queued_bytes);
	if(now_ms - trim.window_start_ms < params.window_ms)
		return 0;

	const uint64_t standing = trim.window_min_bytes;
	uint64_t drop = 0;
	if(standing > params.headroom_bytes + params.hysteresis_bytes)
		drop = std::min(standing - params.headroom_bytes, params.max_step_bytes);

	trim.window_start_ms = now_ms ? now_ms : 1;
	trim.window_min_bytes = queued_bytes > drop ? queued_bytes - drop : 0;
	return drop;
}

// Cutting audio out of the stream leaves a step in the waveform, heard as a click.
// Instead, blend the audio that would have played next (old, at the read position)
// into the audio after the cut (new) over fade_frames frames. The blended frames
// replace the first ones after the cut, so playback stays continuous on both ends:
// it starts where the already queued audio ended and ends in the new audio.
//
// Samples are interleaved S16 in a ring; ring_at(offset) returns the byte pointer for
// the byte offset bytes after the read position (handles wrap; samples are accessed
// byte by byte, so one may straddle the ring's end). fade_frames is
// clamped so that the blend never reads beyond the ring's fill.
template<typename RingAt>
inline int16_t nax5_audio_ring_get_s16(RingAt &ring_at, size_t offset)
{
	char b[2] = { *ring_at(offset), *ring_at(offset + 1) };
	int16_t v;
	std::memcpy(&v, b, sizeof v);
	return v;
}

template<typename RingAt>
inline void nax5_audio_ring_set_s16(RingAt &ring_at, size_t offset, int16_t v)
{
	char b[2];
	std::memcpy(b, &v, sizeof v);
	*ring_at(offset) = b[0];
	*ring_at(offset + 1) = b[1];
}

template<typename RingAt>
inline void nax5_audio_splice_s16(RingAt ring_at, size_t channels, size_t fill_bytes,
		size_t drop_bytes, size_t fade_frames)
{
	const size_t frame_bytes = channels * sizeof(int16_t);
	if(!frame_bytes || drop_bytes >= fill_bytes)
		return;
	fade_frames = std::min(fade_frames, std::min(drop_bytes, fill_bytes - drop_bytes) / frame_bytes);
	for(size_t f = 0; f < fade_frames; f++)
	{
		const float w = (float)(f + 1) / (float)(fade_frames + 1);
		for(size_t c = 0; c < channels; c++)
		{
			const size_t off = f * frame_bytes + c * sizeof(int16_t);
			const int16_t old_s = nax5_audio_ring_get_s16(ring_at, off);
			const int16_t new_s = nax5_audio_ring_get_s16(ring_at, drop_bytes + off);
			const float v = (float)old_s * (1.0f - w) + (float)new_s * w;
			const int16_t out = (int16_t)std::max(-32768.0f, std::min(32767.0f, v + (v < 0 ? -0.5f : 0.5f)));
			nax5_audio_ring_set_s16(ring_at, drop_bytes + off, out);
		}
	}
}
