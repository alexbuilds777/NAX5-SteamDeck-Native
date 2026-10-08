// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/nax5counters.h>

CHIAKI_EXPORT void chiaki_nax5_counters_snapshot(const ChiakiNax5Counters *src, ChiakiNax5Counters *dst)
{
	if(!src || !dst)
		return;
	// Every field with its own relaxed atomic load (a 64-bit field needs a 64-bit atomic load or it
	// can tear on 32-bit targets). Per-second deltas tolerate one field being an increment ahead of
	// another. When adding a field to ChiakiNax5Counters, add it here too: the unit test
	// test/nax5counters.c fails if a field is not copied.
#define NAX5_COPY(field) dst->field = __atomic_load_n(&src->field, __ATOMIC_RELAXED)
	// Takion / reorder
	NAX5_COPY(pkts_rx);
	NAX5_COPY(pkts_late);
	NAX5_COPY(pkts_dup);
	NAX5_COPY(reorder_skips);
	NAX5_COPY(reorder_skipped_pkts);
	NAX5_COPY(av_drops);
	NAX5_COPY(retransmits);
	NAX5_COPY(reacks);
	NAX5_COPY(sack_rx);
	// Video
	NAX5_COPY(units_rx);
	NAX5_COPY(units_lost);
	NAX5_COPY(frames_rx);
	NAX5_COPY(frames_fec_attempt);
	NAX5_COPY(frames_fec_ok);
	NAX5_COPY(frames_fec_failed);
	NAX5_COPY(frames_corrupt_reported);
	NAX5_COPY(idr_frames);
	NAX5_COPY(idr_requests);
	NAX5_COPY(ref_missing);
	NAX5_COPY(bytes_rx);
	// Console CONNECTIONQUALITY
	NAX5_COPY(quality_msgs);
	NAX5_COPY(console_rtt_us_last);
	NAX5_COPY(console_loss_last);
	NAX5_COPY(console_upstream_loss_x10000_last);
	NAX5_COPY(console_upstream_bitrate_last);
	NAX5_COPY(console_target_bitrate_last);
	NAX5_COPY(congestion_reports);
	NAX5_COPY(congestion_loss_clamped);
	// Audio
	NAX5_COPY(audio_frames);
	NAX5_COPY(audio_plc);
	NAX5_COPY(audio_late);
	NAX5_COPY(audio_fec_used);
	NAX5_COPY(audio_jitter_max);
	// Data channel RTT
	NAX5_COPY(data_rtt_us_last);
	NAX5_COPY(data_rtt_us_smoothed);
	NAX5_COPY(data_rtt_samples);
	// Review fixes 24.09
	NAX5_COPY(audio_skipped);
	NAX5_COPY(audio_jitter_last);
#undef NAX5_COPY
}
