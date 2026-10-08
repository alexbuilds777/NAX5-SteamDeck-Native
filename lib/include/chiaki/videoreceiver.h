// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_VIDEORECEIVER_H
#define CHIAKI_VIDEORECEIVER_H

#include "common.h"
#include "log.h"
#include "video.h"
#include "takion.h"
#include "frameprocessor.h"
#include "bitstream.h"
#include "thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHIAKI_VIDEO_PROFILES_MAX 8

typedef struct chiaki_video_receiver_t
{
	struct chiaki_session_t *session;
	ChiakiLog *log;
	ChiakiVideoProfile profiles[CHIAKI_VIDEO_PROFILES_MAX];
	size_t profiles_count;
	int profile_cur; // < 1 if no profile selected yet, else index in profiles

	int32_t frame_index_cur; // frame that is currently being filled
	int32_t frame_index_prev; // last frame that has been at least partially decoded
	int32_t frame_index_prev_complete; // last frame that has been completely decoded
	int32_t frame_index_lost_accounted; // NAX5: last frame already counted in frames_lost (see chiaki_video_receiver_account_lost)
	ChiakiFrameProcessor frame_processor;
	ChiakiPacketStats *packet_stats;

	int32_t frames_lost;
	int32_t frames_lost_total;
	int32_t reference_frames[16];
	ChiakiBitstream bitstream;
	ChiakiMutex waiting_for_idr_mutex;
	bool waiting_for_idr;
	ChiakiMutex frames_lost_mutex;
	// NAX5: log rate limiting / diagnostics
	uint64_t nax5_ref_missing_last_log_us; // monotonic time of the last "Missing reference frame" warning
	uint32_t nax5_ref_missing_suppressed;  // events not logged since then
	bool nax5_lost_absurd_logged;          // absurd frames-lost value already logged once
	uint64_t nax5_missing_last_log_us;     // monotonic time of the last "Detected missing or corrupt frame(s)" warning
	uint32_t nax5_missing_suppressed;      // such warnings not logged since then
} ChiakiVideoReceiver;

/**
 * NAX5: number of frames lost when the frame frame_index_cur could not be completed, counting
 * from the frame after frame_index_prev_complete. Both indices are 16-bit sequence numbers
 * stored in int32_t, so the difference must be taken modulo 2^16 (the plain int subtraction
 * went negative on a wrap and made frames_lost negative).
 *
 * @param absurd set to true when the raw result was not in [1, 30000] and 1 is returned instead
 */
static inline int32_t chiaki_video_receiver_calc_frames_lost(int32_t frame_index_cur, int32_t frame_index_prev_complete, bool *absurd)
{
	ChiakiSeqNum16 next_frame_expected = (ChiakiSeqNum16)(frame_index_prev_complete + 1);
	int32_t lost = (int32_t)(ChiakiSeqNum16)((ChiakiSeqNum16)frame_index_cur - next_frame_expected) + 1;
	if(absurd)
		*absurd = false;
	if(lost <= 0 || lost > 30000)
	{
		if(absurd)
			*absurd = true;
		lost = 1;
	}
	return lost;
}

/**
 * NAX5: account frames as lost exactly once.
 *
 * frame_index_prev_complete only advances on a successful decode, so every loss event during a
 * burst (FEC failure, missing reference, frames that never arrived) used to count the whole span
 * from prev_complete + 1 again (quadratic frames_lost; it also advanced the decoder's synthetic PTS
 * by that inflated value). This returns only the frames in (base, upto] that were not counted yet,
 * where base is the later (16-bit wrap-aware) of prev_complete and *accounted, and moves *accounted
 * to upto.
 *
 * @param accounted last frame index already counted as lost (16-bit value in an int32_t), updated
 * @param prev_complete last frame index that has been completely decoded
 * @param upto last frame index to count as lost (inclusive)
 * @param absurd set to true when the span exceeds 30000 frames and 1 is returned instead
 * @return number of newly lost frames, 0 if upto is not after base
 */
static inline int32_t chiaki_video_receiver_account_lost(int32_t *accounted, int32_t prev_complete, int32_t upto, bool *absurd)
{
	if(absurd)
		*absurd = false;
	ChiakiSeqNum16 base = (ChiakiSeqNum16)prev_complete;
	ChiakiSeqNum16 acc = (ChiakiSeqNum16)*accounted;
	if(chiaki_seq_num_16_gt(acc, base))
		base = acc;
	ChiakiSeqNum16 up = (ChiakiSeqNum16)upto;
	if(!chiaki_seq_num_16_gt(up, base))
		return 0;
	int32_t lost = (int32_t)(ChiakiSeqNum16)(up - base);
	*accounted = (int32_t)up;
	if(lost > 30000)
	{
		if(absurd)
			*absurd = true;
		lost = 1;
	}
	return lost;
}

CHIAKI_EXPORT void chiaki_video_receiver_init(ChiakiVideoReceiver *video_receiver, struct chiaki_session_t *session, ChiakiPacketStats *packet_stats);
CHIAKI_EXPORT void chiaki_video_receiver_fini(ChiakiVideoReceiver *video_receiver);

/**
 * Called after receiving the Stream Info Packet.
 *
 * @param video_receiver
 * @param profiles Array of profiles. Ownership of the contained header buffers will be transferred to the ChiakiVideoReceiver!
 * @param profiles_count must be <= CHIAKI_VIDEO_PROFILES_MAX
 */
CHIAKI_EXPORT void chiaki_video_receiver_stream_info(ChiakiVideoReceiver *video_receiver, ChiakiVideoProfile *profiles, size_t profiles_count);

CHIAKI_EXPORT void chiaki_video_receiver_av_packet(ChiakiVideoReceiver *video_receiver, ChiakiTakionAVPacket *packet);
CHIAKI_EXPORT void chiaki_video_receiver_set_waiting_for_idr(ChiakiVideoReceiver *video_receiver, bool waiting_for_idr);
CHIAKI_EXPORT bool chiaki_video_receiver_get_waiting_for_idr(ChiakiVideoReceiver *video_receiver);
CHIAKI_EXPORT int32_t chiaki_video_receiver_get_frames_lost_total(ChiakiVideoReceiver *video_receiver);

static inline ChiakiVideoReceiver *chiaki_video_receiver_new(struct chiaki_session_t *session, ChiakiPacketStats *packet_stats)
{
	ChiakiVideoReceiver *video_receiver = CHIAKI_NEW(ChiakiVideoReceiver);
	if(!video_receiver)
		return NULL;
	chiaki_video_receiver_init(video_receiver, session, packet_stats);
	return video_receiver;
}

static inline void chiaki_video_receiver_free(ChiakiVideoReceiver *video_receiver)
{
	if(!video_receiver)
		return;
	chiaki_video_receiver_fini(video_receiver);
	free(video_receiver);
}

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_VIDEORECEIVER_H
