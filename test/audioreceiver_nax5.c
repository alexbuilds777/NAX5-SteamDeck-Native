// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// NAX5: audio AV packet unit ordering (FEC units before source units)

#include <munit.h>

#include <chiaki/audioreceiver.h>

// 1 source + 1 FEC unit: FEC unit (index 1) first, then source unit (index 0)
static MunitResult test_unit_order_1_1(const MunitParameter params[], void *user)
{
	munit_assert_size(chiaki_audio_receiver_unit_order(0, 1, 1), ==, 1);
	munit_assert_size(chiaki_audio_receiver_unit_order(1, 1, 1), ==, 0);
	return MUNIT_OK;
}

// 2 source + 2 FEC units: FEC units 2, 3 first, then source units 0, 1
static MunitResult test_unit_order_2_2(const MunitParameter params[], void *user)
{
	munit_assert_size(chiaki_audio_receiver_unit_order(0, 2, 2), ==, 2);
	munit_assert_size(chiaki_audio_receiver_unit_order(1, 2, 2), ==, 3);
	munit_assert_size(chiaki_audio_receiver_unit_order(2, 2, 2), ==, 0);
	munit_assert_size(chiaki_audio_receiver_unit_order(3, 2, 2), ==, 1);
	return MUNIT_OK;
}

// no FEC units: identity
static MunitResult test_unit_order_no_fec(const MunitParameter params[], void *user)
{
	for(size_t step = 0; step < 3; step++)
		munit_assert_size(chiaki_audio_receiver_unit_order(step, 3, 0), ==, step);
	return MUNIT_OK;
}

// every unit index appears exactly once
static MunitResult test_unit_order_permutation(const MunitParameter params[], void *user)
{
	for(uint8_t src = 1; src <= 4; src++)
	{
		for(uint8_t fec = 0; fec <= 4; fec++)
		{
			size_t total = (size_t)src + fec;
			bool seen[8] = { false };
			for(size_t step = 0; step < total; step++)
			{
				size_t unit = chiaki_audio_receiver_unit_order(step, src, fec);
				munit_assert_size(unit, <, total);
				munit_assert_false(seen[unit]);
				seen[unit] = true;
			}
		}
	}
	return MUNIT_OK;
}

// frame index per unit: source units F + i, FEC unit j carries F - fec + j
static MunitResult test_unit_frame_index(const MunitParameter params[], void *user)
{
	ChiakiSeqNum16 f = 1000;
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(0, f, 1, 1), ==, 1000);
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(1, f, 1, 1), ==, 999);
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(0, f, 2, 2), ==, 1000);
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(1, f, 2, 2), ==, 1001);
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(2, f, 2, 2), ==, 998);
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(3, f, 2, 2), ==, 999);
	// 16-bit wrap
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(1, 0, 1, 1), ==, 65535);
	munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(0, 65535, 1, 1), ==, 65535);
	return MUNIT_OK;
}

// The sequence of frame indices processed for a packet (frame_index = F, 1 source, 1 FEC)
// must be [F-1, F]: the FEC copy of F-1 is stored before F can trigger PLC for it.
static MunitResult test_processing_sequence_1_1(const MunitParameter params[], void *user)
{
	ChiakiSeqNum16 f = 1234;
	uint8_t src = 1, fec = 1;
	ChiakiSeqNum16 seq[2];
	for(size_t step = 0; step < 2; step++)
	{
		size_t unit = chiaki_audio_receiver_unit_order(step, src, fec);
		seq[step] = chiaki_audio_receiver_unit_frame_index(unit, f, src, fec);
	}
	munit_assert_uint16(seq[0], ==, f - 1);
	munit_assert_uint16(seq[1], ==, f);
	return MUNIT_OK;
}

// 2 source + 2 FEC: [F-2, F-1, F, F+1]
static MunitResult test_processing_sequence_2_2(const MunitParameter params[], void *user)
{
	ChiakiSeqNum16 f = 500;
	uint8_t src = 2, fec = 2;
	ChiakiSeqNum16 expected[4] = { 498, 499, 500, 501 };
	for(size_t step = 0; step < 4; step++)
	{
		size_t unit = chiaki_audio_receiver_unit_order(step, src, fec);
		munit_assert_uint16(chiaki_audio_receiver_unit_frame_index(unit, f, src, fec), ==, expected[step]);
	}
	return MUNIT_OK;
}

// PLC cap: gaps up to CHIAKI_AUDIO_MAX_CONCEAL are concealed frame by frame, longer gaps jump
// so that exactly CHIAKI_AUDIO_MAX_CONCEAL frames are concealed before the oldest buffered frame.
static MunitResult test_cap_conceal_gaps(const MunitParameter params[], void *user)
{
	uint32_t skipped = 1234;
	munit_assert_int(CHIAKI_AUDIO_MAX_CONCEAL, ==, 3);
	// gap 1: next 100 missing, 101 buffered
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(100, 101, &skipped), ==, 100);
	munit_assert_uint32(skipped, ==, 0);
	// gap 3
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(100, 103, &skipped), ==, 100);
	munit_assert_uint32(skipped, ==, 0);
	// gap 4: skip 1, conceal 101, 102, 103
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(100, 104, &skipped), ==, 101);
	munit_assert_uint32(skipped, ==, 1);
	// gap 324 (3-s outage): skip 321, conceal 3
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(1000, 1324, &skipped), ==, 1321);
	munit_assert_uint32(skipped, ==, 321);
	// no gap / oldest not newer: unchanged
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(100, 100, &skipped), ==, 100);
	munit_assert_uint32(skipped, ==, 0);
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(100, 90, &skipped), ==, 100);
	munit_assert_uint32(skipped, ==, 0);
	return MUNIT_OK;
}

static MunitResult test_cap_conceal_wrap(const MunitParameter params[], void *user)
{
	uint32_t skipped = 1234;
	// 65534 .. 1 missing (gap 4), oldest buffered is 2
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(65534, 2, &skipped), ==, 65535);
	munit_assert_uint32(skipped, ==, 1);
	// gap 3 across the wrap: 65535, 0, 1 concealed
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(65535, 2, &skipped), ==, 65535);
	munit_assert_uint32(skipped, ==, 0);
	// long gap across the wrap: 65500 .. 199 missing (gap 236), resume at 197
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(65500, 200, &skipped), ==, 197);
	munit_assert_uint32(skipped, ==, 233);
	// jump target itself wraps: oldest 1 → next 65534
	munit_assert_uint16(chiaki_audio_receiver_cap_conceal(65000, 1, &skipped), ==, 65534);
	munit_assert_uint32(skipped, ==, 534);
	return MUNIT_OK;
}

MunitTest tests_audioreceiver_nax5[] = {
	{ "/cap_conceal_gaps", test_cap_conceal_gaps, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cap_conceal_wrap", test_cap_conceal_wrap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unit_order_1_1", test_unit_order_1_1, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unit_order_2_2", test_unit_order_2_2, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unit_order_no_fec", test_unit_order_no_fec, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unit_order_permutation", test_unit_order_permutation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unit_frame_index", test_unit_frame_index, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/processing_sequence_1_1", test_processing_sequence_1_1, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/processing_sequence_2_2", test_processing_sequence_2_2, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
