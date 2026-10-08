// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// NAX5: helpers for the Takion reliable data channel. They live apart from takion.c
// so the unit tests can exercise them without a live socket (see test/takion.c).

#ifndef CHIAKI_TAKION_NAX5_H
#define CHIAKI_TAKION_NAX5_H

#include <chiaki/seqnum.h>
#include <chiaki/reorderqueue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAX5_TAKION_DATA_ACK_HEADER_SIZE 0xc

typedef struct nax5_takion_data_ack_t
{
	uint32_t cumulative_seq_num;
	uint32_t a_rwnd;
	uint16_t gap_ack_blocks_count;
	uint16_t dup_tsns_count;
} Nax5TakionDataAck;

static inline uint32_t nax5_takion_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t nax5_takion_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * Parse a DATA_ACK payload: an SCTP-style SACK, i.e. a 12-byte header followed by
 * gap_ack_blocks_count gap blocks and dup_tsns_count duplicate TSNs, 4 bytes each.
 *
 * Upstream accepted only acks of exactly 12 bytes. Under packet loss the console
 * reports gaps, so every ack was ignored, including its cumulative part: the client
 * send buffer never drained and overflowed ("Takion Send Buffer overflow").
 *
 * @return false if the payload is malformed (sizes don't add up)
 */
static inline bool nax5_takion_parse_data_ack(const uint8_t *buf, size_t buf_size, Nax5TakionDataAck *ack)
{
	if(buf_size < NAX5_TAKION_DATA_ACK_HEADER_SIZE)
		return false;
	ack->cumulative_seq_num = nax5_takion_be32(buf + 0);
	ack->a_rwnd = nax5_takion_be32(buf + 4);
	ack->gap_ack_blocks_count = nax5_takion_be16(buf + 8);
	ack->dup_tsns_count = nax5_takion_be16(buf + 0xa);
	size_t expected = NAX5_TAKION_DATA_ACK_HEADER_SIZE
		+ ((size_t)ack->gap_ack_blocks_count + (size_t)ack->dup_tsns_count) * 4;
	return buf_size == expected;
}

/**
 * True if a DATA chunk with seq_num was already delivered, i.e. it is older than the
 * next expected one. That is a retransmission because our ack got lost, and it must be
 * acked again: upstream dropped it silently, so the console kept retransmitting the
 * same chunk (hundreds of times) and stopped sending new data such as connection
 * quality reports.
 */
static inline bool nax5_takion_data_is_stale(ChiakiSeqNum32 seq_num, ChiakiSeqNum32 next_expected)
{
	return chiaki_seq_num_32_lt(seq_num, next_expected);
}

/**
 * Minimum spacing between two re-acks of the same cumulative seq num. A burst of
 * retransmitted chunks (the console resends everything outstanding at once) must not
 * turn into a burst of identical acks.
 */
#define NAX5_TAKION_REACK_MIN_INTERVAL_US 5000

/**
 * Decide whether a re-ack for cumulative seq num @p seq may be sent now and record it.
 *
 * @param last_us monotonic time of the last re-ack sent, 0 if none yet; updated when true is returned
 * @param last_seq cumulative seq num of the last re-ack sent; updated when true is returned
 * @return false if the same seq num was re-acked less than NAX5_TAKION_REACK_MIN_INTERVAL_US ago
 */
static inline bool nax5_takion_reack_allow(uint64_t *last_us, uint32_t *last_seq, uint32_t seq, uint64_t now_us)
{
	if(*last_us != 0 && *last_seq == seq && now_us - *last_us < NAX5_TAKION_REACK_MIN_INTERVAL_US)
		return false;
	*last_us = now_us;
	*last_seq = seq;
	return true;
}

/**
 * Simple rate limiter for log lines: true at most once per @p interval_us.
 *
 * @param last_us monotonic time of the last allowed event, 0 if none yet; updated when true is returned
 */
static inline bool nax5_takion_ratelimit(uint64_t *last_us, uint64_t now_us, uint64_t interval_us)
{
	if(*last_us != 0 && now_us - *last_us < interval_us)
		return false;
	*last_us = now_us;
	return true;
}

/**
 * After a reorder timeout, advance an AV reorder queue past its missing head slots to the
 * first buffered packet, so the whole burst pays a single timeout.
 *
 * If no slot in the span is set (must not happen: the span always ends on a set slot, but
 * the code is defensive), the span is reset to empty. Leaving count > 0 there made the
 * caller give up without clearing its head-wait timer: the next select() timeout was 0
 * and the Takion thread spun until the next packet arrived.
 *
 * @param skipped_out number of missing slots skipped, or the whole span if false is returned
 * @return true if the queue now starts at a buffered packet, false if it is now empty
 */
static inline bool nax5_takion_reorder_skip_missing(ChiakiReorderQueue *queue, uint64_t *skipped_out)
{
	uint64_t skipped = 0;
	while(skipped < queue->count)
	{
		uint64_t seq_num_peek;
		void *entry_user;
		if(chiaki_reorder_queue_peek(queue, skipped, &seq_num_peek, &entry_user))
			break;
		skipped++;
	}
	*skipped_out = skipped;
	if(skipped >= queue->count)
	{
		queue->count = 0;
		return false;
	}
	queue->begin = queue->seq_num_add(queue->begin, skipped);
	queue->count -= skipped;
	return true;
}

#endif // CHIAKI_TAKION_NAX5_H
