// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// NAX5: frames-lost arithmetic of the video receiver (16-bit frame index wrap)

#include <munit.h>

#include <chiaki/videoreceiver.h>

static MunitResult test_frames_lost_simple(const MunitParameter params[], void *user)
{
	bool absurd = true;
	// prev complete 99, current 100 failed -> 1 frame lost
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(100, 99, &absurd), ==, 1);
	munit_assert_false(absurd);
	// prev complete 99, current 105 failed -> 100..105 = 6 frames
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(105, 99, &absurd), ==, 6);
	munit_assert_false(absurd);
	// NULL absurd pointer is allowed
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(105, 99, NULL), ==, 6);
	return MUNIT_OK;
}

static MunitResult test_frames_lost_wrap(const MunitParameter params[], void *user)
{
	bool absurd = true;
	// prev complete 65535, current 0 -> 1 lost (the old int arithmetic gave 0 - 0 + 1 = 1 by luck)
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(0, 65535, &absurd), ==, 1);
	munit_assert_false(absurd);
	// prev complete 65534, current 2 -> 65535, 0, 1, 2 = 4 lost (old arithmetic: 2 - 65535 + 1 = -65532)
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(2, 65534, &absurd), ==, 4);
	munit_assert_false(absurd);
	// prev complete 65530, current 65535 -> 5 lost
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(65535, 65530, &absurd), ==, 5);
	munit_assert_false(absurd);
	// prev complete has itself wrapped in the int32 (stored as ChiakiSeqNum16 cast): 65535 + 1 -> 0
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(3, 65535, &absurd), ==, 4);
	munit_assert_false(absurd);
	return MUNIT_OK;
}

static MunitResult test_frames_lost_absurd(const MunitParameter params[], void *user)
{
	bool absurd = false;
	// current "behind" prev complete (should not happen) -> huge modulo value -> clamp to 1
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(99, 100, &absurd), ==, 1);
	munit_assert_true(absurd);
	absurd = false;
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(50, 100, &absurd), ==, 1);
	munit_assert_true(absurd);
	// exactly at the limit is accepted
	absurd = true;
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(30000, 0, &absurd), ==, 30000);
	munit_assert_false(absurd);
	// one past the limit is not
	absurd = false;
	munit_assert_int32(chiaki_video_receiver_calc_frames_lost(30001, 0, &absurd), ==, 1);
	munit_assert_true(absurd);
	return MUNIT_OK;
}

// Live log: consecutive FEC failures 1430, 1432, 1433 with prev_complete 1425 were counted
// as 5, 7, 8 lost frames. Each frame must be counted once: 5 (1426..1430), 2 (1431, 1432), 1.
static MunitResult test_account_lost_burst(const MunitParameter params[], void *user)
{
	int32_t prev_complete = 1425;
	int32_t accounted = prev_complete;
	bool absurd = true;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1430, &absurd), ==, 5);
	munit_assert_false(absurd);
	munit_assert_int32(accounted, ==, 1430);
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1432, &absurd), ==, 2);
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1433, &absurd), ==, 1);
	munit_assert_int32(accounted, ==, 1433);
	munit_assert_false(absurd);
	return MUNIT_OK;
}

// Frames already counted (e.g. missing-reference after the missing-frames path, or the same
// frame reported twice) add nothing and leave the accounted index alone.
static MunitResult test_account_lost_already_counted(const MunitParameter params[], void *user)
{
	int32_t prev_complete = 100;
	int32_t accounted = prev_complete;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 104, NULL), ==, 4);
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 104, NULL), ==, 0);
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 102, NULL), ==, 0);
	munit_assert_int32(accounted, ==, 104);
	// upto == prev_complete (nothing after the last complete frame)
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, 200, 200, NULL), ==, 0);
	// upto before prev_complete
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, 200, 150, NULL), ==, 0);
	return MUNIT_OK;
}

// 16-bit wrap: prev_complete 65533; frames 65534, 65535, 0 lost, then 1, 2 lost.
static MunitResult test_account_lost_wrap(const MunitParameter params[], void *user)
{
	int32_t prev_complete = 65533;
	int32_t accounted = prev_complete;
	bool absurd = true;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 0, &absurd), ==, 3);
	munit_assert_false(absurd);
	munit_assert_int32(accounted, ==, 0);
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 2, &absurd), ==, 2);
	munit_assert_int32(accounted, ==, 2);
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1, &absurd), ==, 0);
	// directly from 65533 to 1: 65534, 65535, 0, 1
	accounted = prev_complete;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1, &absurd), ==, 4);
	// accounted index across the wrap, prev_complete before it
	accounted = 65535;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, 65530, 3, &absurd), ==, 4);
	munit_assert_int32(accounted, ==, 3);
	return MUNIT_OK;
}

// After a successful frame prev_complete moves beyond the accounted index; counting restarts there.
static MunitResult test_account_lost_after_success(const MunitParameter params[], void *user)
{
	int32_t prev_complete = 1425;
	int32_t accounted = prev_complete;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1433, NULL), ==, 8);
	// frame 1440 (IDR) decoded successfully
	prev_complete = 1440;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, prev_complete, 1441, NULL), ==, 1);
	munit_assert_int32(accounted, ==, 1441);
	// the same with the accounted index lagging far behind (stale)
	accounted = 1000;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, 1440, 1443, NULL), ==, 3);
	return MUNIT_OK;
}

// 3-s outage at 60 fps: ~180 frames never arrive, then a few frames skipped while waiting for the
// IDR each re-trigger the missing-frames path. Total must be the real number, not the quadratic sum.
static MunitResult test_account_lost_outage(const MunitParameter params[], void *user)
{
	int32_t prev_complete = 5000;
	int32_t accounted = prev_complete;
	int32_t total = 0;
	// first frame after the outage is 5181: 5001..5180 never arrived
	total += chiaki_video_receiver_account_lost(&accounted, prev_complete, 5180, NULL);
	munit_assert_int32(total, ==, 180);
	for(int32_t f = 5181; f < 5190; f++)
	{
		total += chiaki_video_receiver_account_lost(&accounted, prev_complete, f - 1, NULL); // missing-frames path
		total += chiaki_video_receiver_account_lost(&accounted, prev_complete, f, NULL); // skipped / failed
	}
	munit_assert_int32(total, ==, 189);
	return MUNIT_OK;
}

// Spans beyond 30000 frames are treated as bogus and count 1.
static MunitResult test_account_lost_absurd(const MunitParameter params[], void *user)
{
	int32_t accounted = 0;
	bool absurd = false;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, 0, 30000, &absurd), ==, 30000);
	munit_assert_false(absurd);
	accounted = 0;
	munit_assert_int32(chiaki_video_receiver_account_lost(&accounted, 0, 30001, &absurd), ==, 1);
	munit_assert_true(absurd);
	munit_assert_int32(accounted, ==, 30001);
	return MUNIT_OK;
}

MunitTest tests_videoreceiver_nax5[] = {
	{ "/account_lost_burst", test_account_lost_burst, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/account_lost_already_counted", test_account_lost_already_counted, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/account_lost_wrap", test_account_lost_wrap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/account_lost_after_success", test_account_lost_after_success, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/account_lost_outage", test_account_lost_outage, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/account_lost_absurd", test_account_lost_absurd, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{
		"/frames_lost_simple",
		test_frames_lost_simple,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/frames_lost_wrap",
		test_frames_lost_wrap,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/frames_lost_absurd",
		test_frames_lost_absurd,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
