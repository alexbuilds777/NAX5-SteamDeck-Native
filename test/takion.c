// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>
#include <unistd.h>
#include <string.h>

#include <chiaki/takion.h>
#include <chiaki/seqnum.h>
#include <chiaki/base64.h>

#define CHIAKI_UNIT_TEST
#include "../lib/src/takionsendbuffer.c"
#include "../lib/src/takion_nax5.h"

#include "test_log.h"


static MunitResult test_av_packet_parse(const MunitParameter params[], void *user)
{
	uint8_t packet[] = {
			0x2, 0x0, 0x2d, 0x0, 0x5, 0x0, 0xc0, 0x1c, 0x1, 0x3, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
			0xe4, 0x10, 0x3, 0x67, 0x0, 0x29, 0xf3, 0x2f, 0x98, 0xf6, 0x99, 0x82, 0x83, 0x78, 0xdb, 0x29,
			0x43, 0xa9, 0xe5, 0x88, 0xf2, 0x11, 0x4, 0x20, 0xe6, 0x20, 0x96, 0xe9, 0x6, 0xee, 0xd, 0x27,
			0xa1, 0x83, 0x82, 0x88, 0xe6, 0x21, 0x49, 0x2, 0x75, 0x74, 0x32, 0x5b, 0xf6, 0xe9, 0xdc, 0x93,
			0xea, 0x31, 0x88, 0xd, 0x2b, 0x4b, 0x34, 0xf9, 0xec, 0x1b, 0x26, 0xcc, 0xbb, 0xbb, 0x81, 0xf2,
			0xd9, 0x2d, 0x8e, 0xa1, 0xb9, 0xe2, 0xb3, 0xca, 0xb2, 0x7d, 0xa3, 0x31, 0xf0, 0x42, 0xb7, 0xb6,
			0x1e, 0x8f, 0x6d, 0xa2, 0x70, 0x46, 0xfd, 0x7e, 0x9b, 0x60, 0x85, 0xb0, 0xed, 0x4f, 0x20, 0xb5,
			0x1, 0x71, 0xa9, 0xaa, 0x18, 0x6b, 0x2a, 0x90, 0xf3, 0xa7, 0x84, 0x36, 0xfd, 0x6d, 0x14, 0x83,
			0x68, 0xa3, 0x9b, 0x3a, 0xc8, 0xd4, 0x3a, 0x31, 0xa0, 0x9b, 0x61, 0xde, 0xa7, 0xed, 0x46, 0xb4,
			0xa3, 0xdf, 0x3f, 0x44, 0x8f, 0xad, 0x64, 0x9, 0xfc, 0x7a, 0xe7, 0x24, 0xf0, 0xd2, 0x42, 0xd3,
			0x57, 0x5a, 0x76, 0x0, 0xc5, 0xe0, 0x93, 0xa9, 0xf5, 0x32, 0x5d, 0xee, 0xf7, 0x9d
	};

	ChiakiKeyState key_state;
	chiaki_key_state_init(&key_state);

	ChiakiTakionAVPacket av_packet;

	ChiakiErrorCode err = chiaki_takion_v9_av_packet_parse(&av_packet, &key_state, packet, sizeof(packet));
	munit_assert_int(err, ==, CHIAKI_ERR_SUCCESS);

	munit_assert(av_packet.is_video);
	munit_assert_uint16(av_packet.packet_index, ==, 45);
	munit_assert_uint16(av_packet.frame_index, ==, 5);
	// TODO: uses_nalu_info_structs
	munit_assert_uint16(av_packet.unit_index, ==, 6);
	munit_assert_uint16(av_packet.units_in_frame_total, ==, 8);
	munit_assert_uint16(av_packet.units_in_frame_fec, ==, 1);
	munit_assert_uint32(av_packet.codec, ==, 3);
//	munit_assert_uint16(av_packet.word_at_0x18, ==, 871);
	munit_assert_uint8(av_packet.adaptive_stream_index, ==, 0);
//	munit_assert_uint8(av_packet.byte_at_0x2c, ==, 0);

	munit_assert_ptr_equal(av_packet.data, packet + 0x15);
	munit_assert_size(av_packet.data_size, ==, 0x99);

	return MUNIT_OK;
}


static MunitResult test_av_packet_parse_real_video(const MunitParameter params[], void *user)
{
#include "takion_av_packet_parse_real_video.inl"
	return MUNIT_OK;
}

static void random_seqnums(ChiakiSeqNum32 *nums, size_t count)
{
	for(size_t i=0; i<count; i++)
	{
		ChiakiSeqNum32 seqnum;
		retry:
		seqnum = munit_rand_uint32();
		for(size_t j=0; j<i; j++)
		{
			if(nums[j] == seqnum)
				goto retry;
		}
		nums[i] = seqnum;
	}
}

static bool check_send_buffer_contents(ChiakiTakionSendBuffer *send_buffer, const ChiakiSeqNum32 *nums_expected, size_t nums_expected_count)
{
	// nums_expected must be unique

	if(chiaki_mutex_lock(&send_buffer->mutex) != CHIAKI_ERR_SUCCESS)
		return false;

	if(send_buffer->packets_count != nums_expected_count)
		goto fail;

	for(size_t i=0; i<nums_expected_count; i++)
	{
		bool found = false;
		for(size_t j=0; j<send_buffer->packets_count; j++)
		{
			if(send_buffer->packets[j].seq_num == nums_expected[i])
			{
				found = true;
				break;
			}
		}
		if(!found)
			goto fail;
	}

	chiaki_mutex_unlock(&send_buffer->mutex);
	return true;
fail:
	chiaki_mutex_unlock(&send_buffer->mutex);
	return false;
}

static void seqnums_ack(ChiakiSeqNum32 *nums, size_t *nums_count, ChiakiSeqNum32 ack_num)
{
	// simulate ack of ack_num
	for(size_t i=0; i<*nums_count; i++)
	{
		if(nums[i] == ack_num || chiaki_seq_num_32_lt(nums[i], ack_num))
		{
			for(size_t j=i+1; j<*nums_count; j++)
				nums[j-1] = nums[j];
			(*nums_count)--;
			i--;
		}
	}
}

static MunitResult test_takion_send_buffer(const MunitParameter params[], void *user)
{
#define nums_count 0x30
	ChiakiTakionSendBuffer send_buffer;
	ChiakiErrorCode err = chiaki_takion_send_buffer_init(&send_buffer, NULL, nums_count);
	munit_assert_int(err, ==, CHIAKI_ERR_SUCCESS);
	send_buffer.log = get_test_log();

	ChiakiSeqNum32 nums_expected[nums_count + 1];
	random_seqnums(nums_expected, nums_count + 1);

	for(size_t i=0; i<nums_count; i++)
	{
		err = chiaki_takion_send_buffer_push(&send_buffer, nums_expected[i], malloc(8), 8);
		munit_assert_int(err, ==, CHIAKI_ERR_SUCCESS);
	}

	err = chiaki_takion_send_buffer_push(&send_buffer, nums_expected[nums_count], malloc(8), 8);
	munit_assert_int(err, ==, CHIAKI_ERR_OVERFLOW);

	size_t nums_count_cur = nums_count;
	while(nums_count_cur > 0)
	{
		ChiakiSeqNum32 ack_num = nums_expected[nums_count_cur - 1]
				+ munit_rand_int_range(-1, 1) * munit_rand_int_range(1, 32);
		chiaki_takion_send_buffer_ack(&send_buffer, ack_num, NULL, NULL); // TODO: test acked seqnums params
		seqnums_ack(nums_expected, &nums_count_cur, ack_num);
		bool correct = check_send_buffer_contents(&send_buffer, nums_expected, nums_count_cur);
		munit_assert(correct);
	}

	chiaki_takion_send_buffer_fini(&send_buffer);
	return MUNIT_OK;
#undef nums_count
}

// NAX5: live RTT is sampled only for the packet an ACK names exactly and only if never retransmitted.
static MunitResult test_takion_send_buffer_rtt(const MunitParameter params[], void *user)
{
	ChiakiTakionSendBuffer send_buffer;
	ChiakiErrorCode err = chiaki_takion_send_buffer_init(&send_buffer, NULL, 8);
	munit_assert_int(err, ==, CHIAKI_ERR_SUCCESS);
	send_buffer.log = get_test_log();
	ChiakiNax5Counters ctr;
	memset(&ctr, 0, sizeof(ctr));
	send_buffer.nax5_counters = &ctr;

	munit_assert_int(chiaki_takion_send_buffer_push(&send_buffer, 10, malloc(8), 8), ==, CHIAKI_ERR_SUCCESS);
	munit_assert_int(chiaki_takion_send_buffer_push(&send_buffer, 11, malloc(8), 8), ==, CHIAKI_ERR_SUCCESS);
	munit_assert_int(chiaki_takion_send_buffer_push(&send_buffer, 12, malloc(8), 8), ==, CHIAKI_ERR_SUCCESS);
	send_buffer.packets[1].tries = 1; // seq 11 was retransmitted
	usleep(3000);

	// Cumulative ACK of 11: 10 is only covered (not named), 11 is ambiguous (retransmitted) -> no sample
	chiaki_takion_send_buffer_ack(&send_buffer, 11, NULL, NULL);
	munit_assert_uint32(ctr.data_rtt_samples, ==, 0);

	chiaki_takion_send_buffer_ack(&send_buffer, 12, NULL, NULL);
	munit_assert_uint32(ctr.data_rtt_samples, ==, 1);
	munit_assert_uint32(ctr.data_rtt_us_last, >=, 3000);
	munit_assert_uint32(ctr.data_rtt_us_smoothed, ==, ctr.data_rtt_us_last);
	uint32_t first = ctr.data_rtt_us_last;

	munit_assert_int(chiaki_takion_send_buffer_push(&send_buffer, 13, malloc(8), 8), ==, CHIAKI_ERR_SUCCESS);
	chiaki_takion_send_buffer_ack(&send_buffer, 13, NULL, NULL);
	munit_assert_uint32(ctr.data_rtt_samples, ==, 2);
	uint32_t second = ctr.data_rtt_us_last;
	munit_assert_uint32(ctr.data_rtt_us_smoothed, ==, (uint32_t)((7ull * first + second) / 8));

	chiaki_takion_send_buffer_fini(&send_buffer);
	return MUNIT_OK;
}

static MunitResult test_takion_format_congestion(const MunitParameter params[], void *user)
{
	static const uint8_t handshake_key[] = { 0x54, 0x65, 0x4c, 0x34, 0x5c, 0xac, 0x56, 0xb8, 0xea, 0xe6, 0x15, 0x2a, 0xde, 0x1c, 0xe2, 0xe8 };
	static const uint8_t ecdh_secret[] = { 0x00, 0x34, 0xf8, 0x21, 0xc7, 0xd9, 0xde, 0xa9, 0xe9, 0x11, 0xca, 0x5a, 0xd6, 0x7d, 0x11, 0xce, 0x4f, 0x02, 0xb1, 0xce, 0x1e, 0xe7, 0xc3, 0x8d, 0x54, 0x39, 0xfa, 0x64, 0xe3, 0xdb, 0xd8, 0x0d };

	ChiakiGKCrypt gkcrypt;
	ChiakiErrorCode err = chiaki_gkcrypt_init(&gkcrypt, NULL, 0, 2, handshake_key, ecdh_secret);
	if(err != CHIAKI_ERR_SUCCESS)
		return MUNIT_ERROR;

	ChiakiTakionCongestionPacket packet;
	packet.word_0 = 0x42;
	packet.received = 26;
	packet.lost = 10;

	const uint64_t key_pos = 0x1e5;

	uint8_t buf[CHIAKI_TAKION_CONGESTION_PACKET_SIZE];
	chiaki_takion_format_congestion(buf, &packet, key_pos);

	static const uint8_t buf_expected[] = { 0x05, 0x00, 0x42, 0x00, 0x1a, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xe5 };
	munit_assert_memory_equal(sizeof(buf), buf, buf_expected);

	err = chiaki_takion_packet_mac(&gkcrypt, buf, sizeof(buf), key_pos, NULL, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
		return MUNIT_ERROR;

	static const uint8_t buf_expected_mac[] = { 0x05, 0x00, 0x42, 0x00, 0x1a, 0x00, 0x0a, 0x64, 0x8a, 0x7c, 0x74, 0x00, 0x00, 0x01, 0xe5 };
	munit_assert_memory_equal(sizeof(buf), buf, buf_expected_mac);

	chiaki_gkcrypt_fini(&gkcrypt);

	return MUNIT_OK;
}

static MunitResult test_takion_parse_data_ack(const MunitParameter params[], void *user)
{
	Nax5TakionDataAck ack;

	// Plain ack: cumulative 0x11223344, a_rwnd 0x10000, no gaps.
	static const uint8_t plain[] = { 0x11, 0x22, 0x33, 0x44, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	munit_assert_true(nax5_takion_parse_data_ack(plain, sizeof(plain), &ack));
	munit_assert_uint32(ack.cumulative_seq_num, ==, 0x11223344);
	munit_assert_uint32(ack.a_rwnd, ==, 0x10000);
	munit_assert_uint16(ack.gap_ack_blocks_count, ==, 0);

	// Ack with one gap block (size 0x10): what the console sends under packet loss,
	// rejected upstream. The cumulative part must still be readable.
	static const uint8_t one_gap[] = { 0x8b, 0xba, 0xfd, 0x58, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x02, 0x00, 0x03 };
	munit_assert_true(nax5_takion_parse_data_ack(one_gap, sizeof(one_gap), &ack));
	munit_assert_uint32(ack.cumulative_seq_num, ==, 0x8bbafd58);
	munit_assert_uint16(ack.gap_ack_blocks_count, ==, 1);

	// One gap block and one duplicate TSN (size 0x14).
	static const uint8_t gap_dup[] = { 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x01,
		0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04 };
	munit_assert_true(nax5_takion_parse_data_ack(gap_dup, sizeof(gap_dup), &ack));
	munit_assert_uint16(ack.dup_tsns_count, ==, 1);

	// Malformed: shorter than the header, or counts that don't match the size.
	munit_assert_false(nax5_takion_parse_data_ack(plain, 10, &ack));
	munit_assert_false(nax5_takion_parse_data_ack(plain, 0, &ack));
	static const uint8_t gap_missing[] = { 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x02, 0x00, 0x03 };
	munit_assert_false(nax5_takion_parse_data_ack(gap_missing, sizeof(gap_missing), &ack));
	munit_assert_false(nax5_takion_parse_data_ack(one_gap, 14, &ack));

	return MUNIT_OK;
}

static MunitResult test_takion_data_is_stale(const MunitParameter params[], void *user)
{
	// Older than the next expected chunk: a retransmission of delivered data.
	munit_assert_true(nax5_takion_data_is_stale(5, 6));
	munit_assert_true(nax5_takion_data_is_stale(0x3bcbcd69, 0x3bcbcd72));
	// The next expected chunk and anything after it are new.
	munit_assert_false(nax5_takion_data_is_stale(6, 6));
	munit_assert_false(nax5_takion_data_is_stale(7, 6));
	// Across the 32-bit wrap.
	munit_assert_true(nax5_takion_data_is_stale(0xffffffff, 0x1));
	munit_assert_false(nax5_takion_data_is_stale(0x1, 0xffffffff));
	return MUNIT_OK;
}

static MunitResult test_takion_reack_allow(const MunitParameter params[], void *user)
{
	uint64_t last_us = 0;
	uint32_t last_seq = 0;
	// First re-ack always goes out.
	munit_assert_true(nax5_takion_reack_allow(&last_us, &last_seq, 0x100, 1000000));
	munit_assert_uint64(last_us, ==, 1000000);
	munit_assert_uint32(last_seq, ==, 0x100);
	// Same seq within 5 ms: a retransmission burst, suppressed. State untouched.
	munit_assert_false(nax5_takion_reack_allow(&last_us, &last_seq, 0x100, 1000000 + 4999));
	munit_assert_uint64(last_us, ==, 1000000);
	// Same seq at exactly 5 ms: allowed again.
	munit_assert_true(nax5_takion_reack_allow(&last_us, &last_seq, 0x100, 1000000 + 5000));
	munit_assert_uint64(last_us, ==, 1005000);
	// A different seq is never suppressed, even right away.
	munit_assert_true(nax5_takion_reack_allow(&last_us, &last_seq, 0x101, 1005001));
	munit_assert_uint32(last_seq, ==, 0x101);
	// ...and going back to the old seq immediately is a new seq relative to the last one.
	munit_assert_true(nax5_takion_reack_allow(&last_us, &last_seq, 0x100, 1005002));
	munit_assert_false(nax5_takion_reack_allow(&last_us, &last_seq, 0x100, 1005003));

	// A monotonic clock that happens to start at 0 must still be treated as "no re-ack yet".
	last_us = 0;
	last_seq = 7;
	munit_assert_true(nax5_takion_reack_allow(&last_us, &last_seq, 7, 10));
	return MUNIT_OK;
}

static MunitResult test_takion_ratelimit(const MunitParameter params[], void *user)
{
	uint64_t last_us = 0;
	munit_assert_true(nax5_takion_ratelimit(&last_us, 5, 1000000));
	munit_assert_uint64(last_us, ==, 5);
	munit_assert_false(nax5_takion_ratelimit(&last_us, 999999, 1000000));
	munit_assert_false(nax5_takion_ratelimit(&last_us, 1000004, 1000000));
	munit_assert_true(nax5_takion_ratelimit(&last_us, 1000005, 1000000));
	munit_assert_uint64(last_us, ==, 1000005);
	munit_assert_false(nax5_takion_ratelimit(&last_us, 1000006, 1000000));
	return MUNIT_OK;
}

static unsigned int reorder_skip_drops;
static void reorder_skip_drop_cb(uint64_t seq_num, void *elem_user, void *cb_user)
{
	(void)seq_num;
	(void)elem_user;
	(void)cb_user;
	reorder_skip_drops++;
}

static MunitResult test_takion_reorder_skip_missing(const MunitParameter params[], void *user)
{
	ChiakiReorderQueue queue;
	munit_assert_int(chiaki_reorder_queue_init_16(&queue, 3, 40), ==, CHIAKI_ERR_SUCCESS); // 8 slots, expecting 40
	chiaki_reorder_queue_set_drop_strategy(&queue, CHIAKI_REORDER_QUEUE_DROP_STRATEGY_BEGIN);
	reorder_skip_drops = 0;
	chiaki_reorder_queue_set_drop_cb(&queue, reorder_skip_drop_cb, NULL);

	uint64_t seq_num = 0;
	void *elem = NULL;
	uint64_t skipped = 0;

	// 40 and 41 missing, 42 and 45 buffered: span is 40..45 (count 6)
	chiaki_reorder_queue_push(&queue, 42, (void *)42);
	chiaki_reorder_queue_push(&queue, 45, (void *)45);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 6);
	munit_assert_false(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));

	munit_assert_true(nax5_takion_reorder_skip_missing(&queue, &skipped));
	munit_assert_uint64(skipped, ==, 2);
	munit_assert_uint64(queue.begin, ==, 42);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 4);
	munit_assert_true(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));
	munit_assert_uint64(seq_num, ==, 42);
	munit_assert_false(chiaki_reorder_queue_pull(&queue, &seq_num, &elem)); // 43 missing

	// 43, 44 missing before 45
	munit_assert_true(nax5_takion_reorder_skip_missing(&queue, &skipped));
	munit_assert_uint64(skipped, ==, 2);
	munit_assert_uint64(queue.begin, ==, 45);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 1);
	munit_assert_true(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));
	munit_assert_uint64(seq_num, ==, 45);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 0);

	// Head present: nothing to skip, queue untouched.
	chiaki_reorder_queue_push(&queue, 46, (void *)46);
	munit_assert_true(nax5_takion_reorder_skip_missing(&queue, &skipped));
	munit_assert_uint64(skipped, ==, 0);
	munit_assert_uint64(queue.begin, ==, 46);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 1);
	munit_assert_true(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));

	// Invariant under an inconsistent span (count > 0 but no slot set, seq 47..49): the
	// helper must empty the span instead of leaving count > 0, and the queue must keep
	// working afterwards: 47 is still accepted in order, 46 is dropped as too old.
	queue.count = 3;
	for(uint64_t i=0; i<3; i++)
		queue.queue[(queue.begin + i) & 7].set = false;
	munit_assert_false(nax5_takion_reorder_skip_missing(&queue, &skipped));
	munit_assert_uint64(skipped, ==, 3);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 0);
	munit_assert_uint64(queue.begin, ==, 47);
	munit_assert_false(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));
	chiaki_reorder_queue_push(&queue, 48, (void *)48);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 2);
	chiaki_reorder_queue_push(&queue, 47, (void *)47);
	munit_assert_uint(reorder_skip_drops, ==, 0);
	chiaki_reorder_queue_push(&queue, 46, (void *)46);
	munit_assert_uint(reorder_skip_drops, ==, 1);
	munit_assert_true(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));
	munit_assert_uint64(seq_num, ==, 47);
	munit_assert_true(chiaki_reorder_queue_pull(&queue, &seq_num, &elem));
	munit_assert_uint64(seq_num, ==, 48);
	munit_assert_uint64(chiaki_reorder_queue_count(&queue), ==, 0);

	chiaki_reorder_queue_fini(&queue);
	return MUNIT_OK;
}

MunitTest tests_takion[] = {
	{
		"/av_packet_parse",
		test_av_packet_parse,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/av_packet_parse_real_video",
		test_av_packet_parse_real_video,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/send_buffer",
		test_takion_send_buffer,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/send_buffer_rtt",
		test_takion_send_buffer_rtt,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/format_congestion",
		test_takion_format_congestion,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/parse_data_ack",
		test_takion_parse_data_ack,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/data_is_stale",
		test_takion_data_is_stale,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/reack_allow",
		test_takion_reack_allow,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/ratelimit",
		test_takion_ratelimit,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/reorder_skip_missing",
		test_takion_reorder_skip_missing,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
