// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/congestioncontrol.h>
#include <chiaki/time.h>

#include "takion_nax5.h"

#define CONGESTION_CONTROL_INTERVAL_MS 200
// NAX5: the clamp line is per report (5/s); summarize it at most once per 10 s.
#define CONGESTION_CLAMP_LOG_INTERVAL_US (10 * 1000 * 1000)

static void *congestion_control_thread_func(void *user)
{
	ChiakiCongestionControl *control = user;
	chiaki_thread_set_affinity(CHIAKI_THREAD_NAME_CONGESTION);

	ChiakiErrorCode err = chiaki_bool_pred_cond_lock(&control->stop_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		return NULL;

	while(true)
	{
		err = chiaki_bool_pred_cond_timedwait(&control->stop_cond, CONGESTION_CONTROL_INTERVAL_MS);
		if(err != CHIAKI_ERR_TIMEOUT)
			break;

		uint64_t received;
		uint64_t lost;
		chiaki_packet_stats_get(control->stats, true, &received, &lost);
		ChiakiTakionCongestionPacket packet = { 0 };
		uint64_t total = received + lost;
		control->packet_loss = total > 0 ? (double)lost / total : 0;
		// NAX5: takion->nax5_counters is set from the stream thread, hence the acquire load
		ChiakiNax5Counters *ctr = __atomic_load_n(&control->takion->nax5_counters, __ATOMIC_ACQUIRE);
		CHIAKI_NAX5_INC(ctr, congestion_reports);
		if(control->packet_loss > control->packet_loss_max)
		{
			CHIAKI_NAX5_INC(ctr, congestion_loss_clamped);
			control->clamp_since_log++;
			if(nax5_takion_ratelimit(&control->clamp_log_last_us, chiaki_time_now_monotonic_us(), CONGESTION_CLAMP_LOG_INTERVAL_US))
			{
				CHIAKI_LOGI(control->takion->log, "Clamping reported packet loss: measured=%.1f%% reported_max=%.1f%% (%u report(s) clamped since last line)",
					control->packet_loss * 100.0, control->packet_loss_max * 100.0, (unsigned int)control->clamp_since_log);
				control->clamp_since_log = 0;
			}
			lost = total * control->packet_loss_max;
			received = total - lost;
		}
		packet.received = (uint16_t)received;
		packet.lost = (uint16_t)lost;
		CHIAKI_LOGV(control->takion->log, "Sending Congestion Control Packet, received: %u, lost: %u",
			(unsigned int)packet.received, (unsigned int)packet.lost);
		chiaki_takion_send_congestion(control->takion, &packet);
	}

	chiaki_bool_pred_cond_unlock(&control->stop_cond);
	return NULL;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_congestion_control_start(ChiakiCongestionControl *control, ChiakiTakion *takion, ChiakiPacketStats *stats, double packet_loss_max)
{
	control->takion = takion;
	control->stats = stats;
	control->packet_loss_max = packet_loss_max;
	control->packet_loss = 0;
	control->clamp_log_last_us = 0;
	control->clamp_since_log = 0;

	ChiakiErrorCode err = chiaki_bool_pred_cond_init(&control->stop_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_thread_create(&control->thread, congestion_control_thread_func, control);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		chiaki_bool_pred_cond_fini(&control->stop_cond);
		return err;
	}

	chiaki_thread_set_name(&control->thread, "Chiaki Congestion Control");

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_congestion_control_stop(ChiakiCongestionControl *control)
{
	ChiakiErrorCode err = chiaki_bool_pred_cond_signal(&control->stop_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_thread_join(&control->thread, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	control->thread.thread = 0;

	return chiaki_bool_pred_cond_fini(&control->stop_cond);
}
