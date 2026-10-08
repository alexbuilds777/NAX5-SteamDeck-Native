// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/videoreceiver.h>
#include <chiaki/time.h>
#include "../include/chiaki/session.h"

#include <string.h>

static ChiakiErrorCode chiaki_video_receiver_flush_frame(ChiakiVideoReceiver *video_receiver);

/**
 * NAX5: rate limit for "Missing reference frame" warnings: at most one per second.
 * Always counts the event; returns true if this event should be logged and then
 * stores in *suppressed how many events were swallowed since the last logged one.
 */
static bool nax5_ref_missing_should_log(ChiakiVideoReceiver *video_receiver, uint32_t *suppressed)
{
	CHIAKI_NAX5_INC(&video_receiver->session->nax5_counters, ref_missing);
	uint64_t now_us = chiaki_time_now_monotonic_us();
	if(video_receiver->nax5_ref_missing_last_log_us
		&& now_us - video_receiver->nax5_ref_missing_last_log_us < 1000000)
	{
		video_receiver->nax5_ref_missing_suppressed++;
		return false;
	}
	*suppressed = video_receiver->nax5_ref_missing_suppressed;
	video_receiver->nax5_ref_missing_suppressed = 0;
	video_receiver->nax5_ref_missing_last_log_us = now_us;
	return true;
}

/**
 * NAX5: count the frames up to and including upto as lost, each frame only once
 * (see chiaki_video_receiver_account_lost). Returns the number of newly lost frames.
 */
static int32_t nax5_account_lost(ChiakiVideoReceiver *video_receiver, int32_t upto)
{
	bool absurd = false;
	int32_t lost = chiaki_video_receiver_account_lost(&video_receiver->frame_index_lost_accounted,
			video_receiver->frame_index_prev_complete, upto, &absurd);
	if(absurd && !video_receiver->nax5_lost_absurd_logged)
	{
		video_receiver->nax5_lost_absurd_logged = true;
		CHIAKI_LOGW(video_receiver->log, "Absurd frames-lost value up to frame %d (prev complete %d), counting 1",
				(int)upto, (int)video_receiver->frame_index_prev_complete);
	}
	if(lost > 0)
	{
		chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
		video_receiver->frames_lost += lost;
		video_receiver->frames_lost_total += lost;
		chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
	}
	return lost;
}

static void add_ref_frame(ChiakiVideoReceiver *video_receiver, int32_t frame)
{
	if(video_receiver->reference_frames[0] != -1)
	{
		memmove(&video_receiver->reference_frames[1], &video_receiver->reference_frames[0], sizeof(int32_t) * 15);
		video_receiver->reference_frames[0] = frame;
		return;
	}
	for(int i=15; i>=0; i--)
	{
		if(video_receiver->reference_frames[i] == -1)
		{
			video_receiver->reference_frames[i] = frame;
			return;
		}
	}
}

static bool have_ref_frame(ChiakiVideoReceiver *video_receiver, int32_t frame)
{
	for(int i=0; i<16; i++)
		if(video_receiver->reference_frames[i] == frame)
			return true;
	return false;
}

CHIAKI_EXPORT void chiaki_video_receiver_init(ChiakiVideoReceiver *video_receiver, struct chiaki_session_t *session, ChiakiPacketStats *packet_stats)
{
	video_receiver->session = session;
	video_receiver->log = session->log;
	memset(video_receiver->profiles, 0, sizeof(video_receiver->profiles));
	video_receiver->profiles_count = 0;
	video_receiver->profile_cur = -1;

	video_receiver->frame_index_cur = -1;
	video_receiver->frame_index_prev = -1;
	video_receiver->frame_index_prev_complete = 0;
	video_receiver->frame_index_lost_accounted = video_receiver->frame_index_prev_complete;

	chiaki_frame_processor_init(&video_receiver->frame_processor, video_receiver->log);
	video_receiver->frame_processor.nax5_counters = &session->nax5_counters;
	video_receiver->packet_stats = packet_stats;
	video_receiver->nax5_ref_missing_last_log_us = 0;
	video_receiver->nax5_ref_missing_suppressed = 0;
	video_receiver->nax5_lost_absurd_logged = false;
	video_receiver->nax5_missing_last_log_us = 0;
	video_receiver->nax5_missing_suppressed = 0;

	video_receiver->frames_lost = 0;
	video_receiver->frames_lost_total = 0;
	memset(video_receiver->reference_frames, -1, sizeof(video_receiver->reference_frames));
	chiaki_bitstream_init(&video_receiver->bitstream, video_receiver->log, video_receiver->session->connect_info.video_profile.codec);
	chiaki_mutex_init(&video_receiver->waiting_for_idr_mutex, false);
	video_receiver->waiting_for_idr = false;
	chiaki_mutex_init(&video_receiver->frames_lost_mutex, false);
}

CHIAKI_EXPORT void chiaki_video_receiver_fini(ChiakiVideoReceiver *video_receiver)
{
	for(size_t i=0; i<video_receiver->profiles_count; i++)
		free(video_receiver->profiles[i].header);
	chiaki_mutex_fini(&video_receiver->waiting_for_idr_mutex);
	chiaki_mutex_fini(&video_receiver->frames_lost_mutex);
	chiaki_frame_processor_fini(&video_receiver->frame_processor);
}

CHIAKI_EXPORT void chiaki_video_receiver_set_waiting_for_idr(ChiakiVideoReceiver *video_receiver, bool waiting_for_idr)
{
	chiaki_mutex_lock(&video_receiver->waiting_for_idr_mutex);
	video_receiver->waiting_for_idr = waiting_for_idr;
	chiaki_mutex_unlock(&video_receiver->waiting_for_idr_mutex);
}

CHIAKI_EXPORT bool chiaki_video_receiver_get_waiting_for_idr(ChiakiVideoReceiver *video_receiver)
{
	bool waiting_for_idr;
	chiaki_mutex_lock(&video_receiver->waiting_for_idr_mutex);
	waiting_for_idr = video_receiver->waiting_for_idr;
	chiaki_mutex_unlock(&video_receiver->waiting_for_idr_mutex);
	return waiting_for_idr;
}

CHIAKI_EXPORT int32_t chiaki_video_receiver_get_frames_lost_total(ChiakiVideoReceiver *video_receiver)
{
	int32_t total;
	chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
	total = video_receiver->frames_lost_total;
	chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
	return total;
}

CHIAKI_EXPORT void chiaki_video_receiver_stream_info(ChiakiVideoReceiver *video_receiver, ChiakiVideoProfile *profiles, size_t profiles_count)
{
	if(video_receiver->profiles_count > 0)
	{
		CHIAKI_LOGE(video_receiver->log, "Video Receiver profiles already set");
		return;
	}

	memcpy(video_receiver->profiles, profiles, profiles_count * sizeof(ChiakiVideoProfile));
	video_receiver->profiles_count = profiles_count;

	CHIAKI_LOGI(video_receiver->log, "Video Profiles:");
	for(size_t i=0; i<video_receiver->profiles_count; i++)
	{
		ChiakiVideoProfile *profile = &video_receiver->profiles[i];
		CHIAKI_LOGI(video_receiver->log, "  %zu: %ux%u", i, profile->width, profile->height);
		//chiaki_log_hexdump(video_receiver->log, CHIAKI_LOG_DEBUG, profile->header, profile->header_sz);
	}
}

CHIAKI_EXPORT void chiaki_video_receiver_av_packet(ChiakiVideoReceiver *video_receiver, ChiakiTakionAVPacket *packet)
{
	// old frame?
	ChiakiSeqNum16 frame_index = packet->frame_index;
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	if(video_receiver->frame_index_cur >= 0
		&& chiaki_seq_num_16_lt(frame_index, (ChiakiSeqNum16)video_receiver->frame_index_cur))
	{
		CHIAKI_LOGW(video_receiver->log, "Video Receiver received old frame packet");
		return;
	}

	// check adaptive stream index
	if(video_receiver->profile_cur < 0 || video_receiver->profile_cur != packet->adaptive_stream_index)
	{
		if(packet->adaptive_stream_index >= video_receiver->profiles_count)
		{
			CHIAKI_LOGE(video_receiver->log, "Packet has invalid adaptive stream index %u >= %u",
					(unsigned int)packet->adaptive_stream_index,
					(unsigned int)video_receiver->profiles_count);
			return;
		}
		video_receiver->profile_cur = packet->adaptive_stream_index;

		ChiakiVideoProfile *profile = video_receiver->profiles + video_receiver->profile_cur;
		CHIAKI_LOGI(video_receiver->log, "Switched to profile %d, resolution: %ux%u", video_receiver->profile_cur, profile->width, profile->height);
		if(video_receiver->session->video_sample_cb)
			video_receiver->session->video_sample_cb(profile->header, profile->header_sz, 0, false, video_receiver->session->video_sample_cb_user);
		if(!chiaki_bitstream_header(&video_receiver->bitstream, profile->header, profile->header_sz))
			CHIAKI_LOGW(video_receiver->log, "Failed to parse video header");
	}

	// next frame?
	if(video_receiver->frame_index_cur < 0 ||
		chiaki_seq_num_16_gt(frame_index, (ChiakiSeqNum16)video_receiver->frame_index_cur))
	{
		if(video_receiver->packet_stats)
			chiaki_frame_processor_report_packet_stats(&video_receiver->frame_processor, video_receiver->packet_stats);

		// last frame not flushed yet? (flush_frame logs its own failures)
		if(video_receiver->frame_index_cur >= 0 && video_receiver->frame_index_prev != video_receiver->frame_index_cur)
			chiaki_video_receiver_flush_frame(video_receiver);

		ChiakiSeqNum16 next_frame_expected = (ChiakiSeqNum16)(video_receiver->frame_index_prev_complete + 1);
		if(chiaki_seq_num_16_gt(frame_index, next_frame_expected)
			&& !(frame_index == 1 && video_receiver->frame_index_cur < 0)) // ok for frame 1
		{
			// NAX5: the frames before this one never arrived; count them once
			int32_t lost = nax5_account_lost(video_receiver, (int32_t)(ChiakiSeqNum16)(frame_index - 1));
			// NAX5: fires for every frame while waiting for an IDR, so at most one warning per second
			uint64_t now_us = chiaki_time_now_monotonic_us();
			if(video_receiver->nax5_missing_last_log_us
				&& now_us - video_receiver->nax5_missing_last_log_us < 1000000)
				video_receiver->nax5_missing_suppressed++;
			else
			{
				CHIAKI_LOGW(video_receiver->log, "Detected missing or corrupt frame(s) from %d to %d, %d newly lost (%u similar suppressed)",
						next_frame_expected, (int)frame_index, (int)lost, video_receiver->nax5_missing_suppressed);
				video_receiver->nax5_missing_suppressed = 0;
				video_receiver->nax5_missing_last_log_us = now_us;
			}
			err = stream_connection_send_corrupt_frame(&video_receiver->session->stream_connection, next_frame_expected, frame_index - 1);
			if(err != CHIAKI_ERR_SUCCESS)
				CHIAKI_LOGW(video_receiver->log, "Error sending corrupt frame.");
		}

		video_receiver->frame_index_cur = frame_index;
		err = chiaki_frame_processor_alloc_frame(&video_receiver->frame_processor, packet);
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGW(video_receiver->log, "Video receiver could not allocate frame for packet.");
	}

	err = chiaki_frame_processor_put_unit(&video_receiver->frame_processor, packet);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGW(video_receiver->log, "Video receiver could not put unit.");

	// if we are currently building up a frame
	if(video_receiver->frame_index_cur != video_receiver->frame_index_prev)
	{
		// if we already have enough for the whole frame, flush it already
		if(chiaki_frame_processor_flush_possible(&video_receiver->frame_processor) || packet->unit_index == packet->units_in_frame_total - 1)
			chiaki_video_receiver_flush_frame(video_receiver); // logs its own failures
	}
}

static ChiakiErrorCode chiaki_video_receiver_flush_frame(ChiakiVideoReceiver *video_receiver)
{
	uint8_t *frame;
	size_t frame_size;
	ChiakiFrameProcessorFlushResult flush_result = chiaki_frame_processor_flush(&video_receiver->frame_processor, &frame, &frame_size);

	if(flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FAILED
		|| flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_FAILED)
	{
		if(flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_FAILED)
		{
			ChiakiSeqNum16 next_frame_expected = (ChiakiSeqNum16)(video_receiver->frame_index_prev_complete + 1);
			const char *idr_state = "no IDR request (disabled)";
			stream_connection_send_corrupt_frame(&video_receiver->session->stream_connection, next_frame_expected, video_receiver->frame_index_cur);
			if(video_receiver->session->connect_info.enable_idr_on_fec_failure)
			{
				bool waiting_for_idr = chiaki_video_receiver_get_waiting_for_idr(video_receiver);
				bool idr_request_sent = waiting_for_idr;
				if(!waiting_for_idr)
				{
					ChiakiErrorCode err = stream_connection_send_idr_request(&video_receiver->session->stream_connection);
					idr_request_sent = err == CHIAKI_ERR_SUCCESS;
					if(err == CHIAKI_ERR_SUCCESS)
					{
						chiaki_video_receiver_set_waiting_for_idr(video_receiver, true);
						CHIAKI_NAX5_INC(&video_receiver->session->nax5_counters, idr_requests);
						idr_state = "IDR requested";
					}
					else
					{
						idr_state = "IDR request could not be sent";
						CHIAKI_LOGW(video_receiver->log, "IDR request could not be sent: %s", chiaki_error_string(err));
					}
				}
				else
					idr_state = "already waiting for IDR";
				ChiakiEvent event = { 0 };
				event.type = CHIAKI_EVENT_VIDEO_FEC_FAILURE;
				event.video_fec_failure.frame_index = video_receiver->frame_index_cur;
				event.video_fec_failure.idr_request_sent = idr_request_sent;
				chiaki_session_send_event(video_receiver->session, &event);
			}
			// NAX5: wrap-aware, and only frames not counted by an earlier loss event of the same burst
			int32_t lost = nax5_account_lost(video_receiver, video_receiver->frame_index_cur);
			video_receiver->frame_index_prev = video_receiver->frame_index_cur;
			// NAX5: one line for FEC failure + lost frame (was: "FEC failed", "Failed to complete frame", "could not flush frame")
			CHIAKI_LOGW(video_receiver->log, "Frame %d: FEC failed (%u+%u / %u+%u units), %d frame(s) lost, %s",
					(int)video_receiver->frame_index_cur,
					video_receiver->frame_processor.units_source_received, video_receiver->frame_processor.units_fec_received,
					video_receiver->frame_processor.units_source_expected, video_receiver->frame_processor.units_fec_expected,
					(int)lost, idr_state);
		}
		else
			CHIAKI_LOGW(video_receiver->log, "Frame %d: failed to complete (nothing to flush)", (int)video_receiver->frame_index_cur);
		return CHIAKI_ERR_UNKNOWN;
	}

	bool succ = flush_result != CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_FAILED;
	bool recovered = false;

	ChiakiBitstreamSlice slice;
	if(chiaki_bitstream_slice(&video_receiver->bitstream, frame, frame_size, &slice))
	{
		if(slice.slice_type == CHIAKI_BITSTREAM_SLICE_I)
			CHIAKI_NAX5_INC(&video_receiver->session->nax5_counters, idr_frames);
		if(chiaki_video_receiver_get_waiting_for_idr(video_receiver))
		{
			if(slice.slice_type == CHIAKI_BITSTREAM_SLICE_I)
			{
				chiaki_video_receiver_set_waiting_for_idr(video_receiver, false);
				CHIAKI_LOGI(video_receiver->log, "Received IDR frame, resuming decode");
			}
			else
			{
				CHIAKI_LOGV(video_receiver->log, "Skipping P-frame %d while waiting for IDR", (int)video_receiver->frame_index_cur);
				// NAX5: not shown either; count it now so it is neither lost from the count nor counted twice
				nax5_account_lost(video_receiver, video_receiver->frame_index_cur);
				video_receiver->frame_index_prev = video_receiver->frame_index_cur;
				return CHIAKI_ERR_SUCCESS;
			}
		}

		if(slice.slice_type == CHIAKI_BITSTREAM_SLICE_P)
		{
			ChiakiSeqNum16 ref_frame_index = video_receiver->frame_index_cur - slice.reference_frame - 1;
			if(slice.reference_frame != 0xff && !have_ref_frame(video_receiver, ref_frame_index))
			{
				for(unsigned i=slice.reference_frame+1; i<16; i++)
				{
					ChiakiSeqNum16 ref_frame_index_new = video_receiver->frame_index_cur - i - 1;
					if(have_ref_frame(video_receiver, ref_frame_index_new))
					{
						if(chiaki_bitstream_slice_set_reference_frame(&video_receiver->bitstream, frame, frame_size, i))
						{
							recovered = true;
							uint32_t suppressed = 0;
							if(nax5_ref_missing_should_log(video_receiver, &suppressed))
								CHIAKI_LOGW(video_receiver->log, "Missing reference frame %d for decoding frame %d -> changed to %d (%u similar suppressed)",
										(int)ref_frame_index, (int)video_receiver->frame_index_cur, (int)ref_frame_index_new, suppressed);
						}
						break;
					}
				}
				if(!recovered)
				{
					succ = false;
					nax5_account_lost(video_receiver, video_receiver->frame_index_cur);
					uint32_t suppressed = 0;
					if(nax5_ref_missing_should_log(video_receiver, &suppressed))
						CHIAKI_LOGW(video_receiver->log, "Missing reference frame %d for decoding frame %d (%u similar suppressed)",
								(int)ref_frame_index, (int)video_receiver->frame_index_cur, suppressed);
				}
			}
		}
	}

	if(succ && video_receiver->session->video_sample_cb)
	{
		CHIAKI_NAX5_INC(&video_receiver->session->nax5_counters, frames_rx);
		CHIAKI_NAX5_ADD(&video_receiver->session->nax5_counters, bytes_rx, (uint64_t)frame_size);
		bool cb_succ = video_receiver->session->video_sample_cb(frame, frame_size, video_receiver->frames_lost, recovered, video_receiver->session->video_sample_cb_user);
		chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
		video_receiver->frames_lost = 0;
		chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
		if(!cb_succ)
		{
			succ = false;
			CHIAKI_LOGW(video_receiver->log, "Video callback did not process frame successfully.");
		}
		else
		{
			add_ref_frame(video_receiver, video_receiver->frame_index_cur);
			CHIAKI_LOGV(video_receiver->log, "Added reference %c frame %d", slice.slice_type == CHIAKI_BITSTREAM_SLICE_I ? 'I' : 'P', (int)video_receiver->frame_index_cur);
		}
	}

	video_receiver->frame_index_prev = video_receiver->frame_index_cur;

	if(succ)
	{
		video_receiver->frame_index_prev_complete = video_receiver->frame_index_cur;
		// NAX5: keep the accounted index from going stale (16-bit comparisons break after half the space)
		video_receiver->frame_index_lost_accounted = video_receiver->frame_index_prev_complete;
	}

	return CHIAKI_ERR_SUCCESS;
}
