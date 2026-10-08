// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_AUDIORECEIVER_H
#define CHIAKI_AUDIORECEIVER_H

#include "common.h"
#include "log.h"
#include "audio.h"
#include "takion.h"
#include "thread.h"
#include "packetstats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ChiakiAudioSinkHeader)(ChiakiAudioHeader *header, void *user);
typedef void (*ChiakiAudioSinkFrame)(uint8_t *buf, size_t buf_size, void *user);

/**
 * Sink that receives Audio encoded as Opus
 */
typedef struct chiaki_audio_sink_t
{
	void *user;
	ChiakiAudioSinkHeader header_cb;
	ChiakiAudioSinkFrame frame_cb;
} ChiakiAudioSink;

typedef struct chiaki_audio_receiver_t
{
	struct chiaki_session_t *session;
	ChiakiLog *log;
	ChiakiMutex mutex;
	ChiakiSeqNum16 frame_index_prev;
	ChiakiSeqNum16 next_frame_index;
	bool next_frame_index_valid;
	bool playback_started;
	bool frame_index_startup; // whether frame_index_prev has definitely not wrapped yet
	ChiakiPacketStats *packet_stats;
	struct {
		bool occupied;
		ChiakiSeqNum16 frame_index;
		uint8_t *buf;
		size_t buf_size;
	} jitter_buffer[8];
	size_t jitter_buffer_count;
} ChiakiAudioReceiver;

/**
 * NAX5: order in which the units of an audio AV packet are processed.
 * FEC units carry copies of OLDER frames (frame_index - fec_units_count + fec_index), the source
 * units carry frame_index + i. The FEC units must be processed first: processing the source unit
 * first delivers frame F (concealing a missing F-1 with PLC), after which the FEC copy of F-1
 * is discarded as too old and the FEC data is never used.
 *
 * @param step 0 .. source_units_count + fec_units_count - 1
 * @return unit index within the packet's data (source units 0..source-1, FEC units after them)
 */
static inline size_t chiaki_audio_receiver_unit_order(size_t step, uint8_t source_units_count, uint8_t fec_units_count)
{
	if(step < fec_units_count)
		return (size_t)source_units_count + step;
	return step - fec_units_count;
}

/**
 * NAX5: frame index carried by the unit at index unit of an audio AV packet.
 */
static inline ChiakiSeqNum16 chiaki_audio_receiver_unit_frame_index(size_t unit, ChiakiSeqNum16 frame_index, uint8_t source_units_count, uint8_t fec_units_count)
{
	if(unit < source_units_count)
		return (ChiakiSeqNum16)(frame_index + unit);
	return (ChiakiSeqNum16)(frame_index - fec_units_count + (unit - source_units_count));
}

/**
 * NAX5: maximum number of consecutive missing frames concealed with PLC (3 frames = 30 ms).
 * After a longer gap (e.g. a 3-s outage = ~300 frames) the receiver jumps ahead instead of
 * delivering one concealed frame per missing index, which would add the whole gap as latency.
 */
#define CHIAKI_AUDIO_MAX_CONCEAL 3

/**
 * NAX5: cap the concealment burst. If more than CHIAKI_AUDIO_MAX_CONCEAL frames are missing between
 * next_frame_index and the oldest buffered frame, jump to oldest_buffered - CHIAKI_AUDIO_MAX_CONCEAL
 * so that only the last CHIAKI_AUDIO_MAX_CONCEAL missing frames are concealed.
 *
 * @param skipped set to the number of frames jumped over (0 if no jump)
 * @return the new next_frame_index
 */
static inline ChiakiSeqNum16 chiaki_audio_receiver_cap_conceal(ChiakiSeqNum16 next_frame_index, ChiakiSeqNum16 oldest_buffered, uint32_t *skipped)
{
	*skipped = 0;
	if(!chiaki_seq_num_16_gt(oldest_buffered, next_frame_index))
		return next_frame_index;
	ChiakiSeqNum16 gap = (ChiakiSeqNum16)(oldest_buffered - next_frame_index);
	if(gap <= CHIAKI_AUDIO_MAX_CONCEAL)
		return next_frame_index;
	*skipped = (uint32_t)gap - CHIAKI_AUDIO_MAX_CONCEAL;
	return (ChiakiSeqNum16)(oldest_buffered - CHIAKI_AUDIO_MAX_CONCEAL);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_audio_receiver_init(ChiakiAudioReceiver *audio_receiver, struct chiaki_session_t *session, ChiakiPacketStats *packet_stats);
CHIAKI_EXPORT void chiaki_audio_receiver_fini(ChiakiAudioReceiver *audio_receiver);
CHIAKI_EXPORT void chiaki_audio_receiver_stream_info(ChiakiAudioReceiver *audio_receiver, ChiakiAudioHeader *audio_header);
CHIAKI_EXPORT void chiaki_audio_receiver_av_packet(ChiakiAudioReceiver *audio_receiver, ChiakiTakionAVPacket *packet);

static inline ChiakiAudioReceiver *chiaki_audio_receiver_new(struct chiaki_session_t *session, ChiakiPacketStats *packet_stats)
{
	ChiakiAudioReceiver *audio_receiver = CHIAKI_NEW(ChiakiAudioReceiver);
	if(!audio_receiver)
		return NULL;
	ChiakiErrorCode err = chiaki_audio_receiver_init(audio_receiver, session, packet_stats);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		free(audio_receiver);
		return NULL;
	}
	return audio_receiver;
}

static inline void chiaki_audio_receiver_free(ChiakiAudioReceiver *audio_receiver)
{
	if(!audio_receiver)
		return;
	chiaki_audio_receiver_fini(audio_receiver);
	free(audio_receiver);
}

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_AUDIORECEIVER_H
