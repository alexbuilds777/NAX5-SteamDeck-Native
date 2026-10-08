// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// NAX5: lock-free session counters for the per-second stats CSV.
// All fields are monotonically increasing since session start unless the
// name says otherwise ("_last" = last reported value, "_max" = maximum seen).
// Producers use the CHIAKI_NAX5_* macros (relaxed atomics, pointer may be NULL).
// The GUI reads a snapshot with chiaki_session_get_nax5_counters() once a second
// and computes per-second deltas itself.

#ifndef CHIAKI_NAX5COUNTERS_H
#define CHIAKI_NAX5COUNTERS_H

#include "common.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chiaki_nax5_counters_t
{
	// --- Takion / reorder (takion.c, reorderqueue users) ---
	uint32_t pkts_rx;              // AV packets received (video + audio) after decryption
	uint32_t pkts_late;            // AV packets that arrived after the reorder queue had already skipped them
	uint32_t pkts_dup;             // duplicate units (frameprocessor "Received duplicate unit")
	uint32_t reorder_skips;        // number of reorder-timeout events
	uint32_t reorder_skipped_pkts; // total packets given up by those events
	uint32_t av_drops;             // takion_av_drop calls (any reason)
	uint32_t retransmits;          // own send-buffer re-sends (takionsendbuffer.c)
	uint32_t reacks;               // acks re-sent for retransmitted console data
	uint32_t sack_rx;              // SACKs with gap blocks accepted

	// --- Video (videoreceiver.c / frameprocessor.c) ---
	uint32_t units_rx;             // video units received (non-resetting mirror of packet stats)
	uint32_t units_lost;           // video units lost (non-resetting)
	uint32_t frames_rx;            // frames flushed to the decoder (complete or FEC-recovered)
	uint32_t frames_fec_attempt;   // frames that needed FEC
	uint32_t frames_fec_ok;        // FEC succeeded
	uint32_t frames_fec_failed;    // FEC failed (frame lost)
	uint32_t frames_corrupt_reported; // corrupt-frame reports sent to the console
	uint32_t idr_frames;           // IDR frames received
	uint32_t idr_requests;         // IDR requests sent
	uint32_t ref_missing;          // "Missing reference frame" events
	uint64_t bytes_rx;             // bytes of video frames flushed

	// --- Console CONNECTIONQUALITY (streamconnection.c) ---
	uint32_t quality_msgs;                 // messages received
	uint32_t console_rtt_us_last;          // raw console q.rtt x1000, unit unknown, NOT an RTT (sawtooth); see data_rtt_* for ping
	uint32_t console_loss_last;            // loss reported by the console (raw value)
	uint32_t console_upstream_loss_x10000_last; // upstream_loss * 10000
	uint32_t console_upstream_bitrate_last;     // bits per second as reported
	uint32_t console_target_bitrate_last;       // bits per second as reported
	uint32_t congestion_reports;           // congestion reports sent by us
	uint32_t congestion_loss_clamped;      // reports where measured loss exceeded the clamp

	// --- Audio (audioreceiver.c / opusdecoder.c) ---
	uint32_t audio_frames;         // frames delivered to the sink
	uint32_t audio_plc;            // frames concealed (NULL delivered → Opus PLC)
	uint32_t audio_late;           // frames discarded as older than next_frame_index
	uint32_t audio_fec_used;       // frames recovered from an audio FEC unit
	uint32_t audio_jitter_max;     // max jitter_buffer_count observed

	// --- Live RTT from the reliable data channel (takionsendbuffer.c) ---
	// Time from the first send of a data packet (e.g. the 1 s heartbeat) to its ACK.
	// Only never-retransmitted packets are sampled (Karn's rule).
	uint32_t data_rtt_us_last;
	uint32_t data_rtt_us_smoothed; // RFC 6298 SRTT, alpha = 1/8
	uint32_t data_rtt_samples;

	// --- Review fixes 24.09 ---
	uint32_t audio_skipped;        // audio frames jumped over after a long gap instead of concealed (PLC cap)
	uint32_t audio_jitter_last;    // jitter_buffer_count after the last store (replaces reading the receiver from the GUI)
} ChiakiNax5Counters;

#define CHIAKI_NAX5_ADD(ctr, field, n) \
	do { if(ctr) __atomic_fetch_add(&(ctr)->field, (n), __ATOMIC_RELAXED); } while(0)
#define CHIAKI_NAX5_INC(ctr, field) CHIAKI_NAX5_ADD(ctr, field, 1)
#define CHIAKI_NAX5_SET(ctr, field, v) \
	do { if(ctr) __atomic_store_n(&(ctr)->field, (v), __ATOMIC_RELAXED); } while(0)
#define CHIAKI_NAX5_MAX(ctr, field, v) \
	do { if(ctr) { __typeof__((ctr)->field) nax5_cur_ = __atomic_load_n(&(ctr)->field, __ATOMIC_RELAXED); \
		while(nax5_cur_ < (v) && !__atomic_compare_exchange_n(&(ctr)->field, &nax5_cur_, (v), true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {} } } while(0)

/** Copy all fields with relaxed atomic loads. */
CHIAKI_EXPORT void chiaki_nax5_counters_snapshot(const ChiakiNax5Counters *src, ChiakiNax5Counters *dst);

#ifdef __cplusplus
}
#endif

#endif
