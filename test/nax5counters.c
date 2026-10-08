// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// NAX5: chiaki_nax5_counters_snapshot must copy every field of ChiakiNax5Counters.

#include <munit.h>

#include <chiaki/nax5counters.h>

#include <stddef.h>
#include <string.h>

// Fill the source with a byte pattern (so every field, including ones added later, holds a
// distinct non-zero value), snapshot it into two destinations pre-filled with 0x00 and 0xff.
// A byte that differs between the two destinations was not written by the snapshot. The only
// such bytes allowed are the alignment padding before the 64-bit bytes_rx; everything else must
// equal the source. A field missing from the snapshot shows up as unwritten bytes.
static MunitResult test_snapshot_copies_every_field(const MunitParameter params[], void *user)
{
	ChiakiNax5Counters src, dst_zero, dst_ones;
	uint8_t *s = (uint8_t *)&src;
	for(size_t i = 0; i < sizeof(src); i++)
		s[i] = (uint8_t)(1 + (i * 37) % 253); // never 0x00 or 0xff
	memset(&dst_zero, 0x00, sizeof(dst_zero));
	memset(&dst_ones, 0xff, sizeof(dst_ones));

	chiaki_nax5_counters_snapshot(&src, &dst_zero);
	chiaki_nax5_counters_snapshot(&src, &dst_ones);

	const uint8_t *a = (const uint8_t *)&dst_zero;
	const uint8_t *b = (const uint8_t *)&dst_ones;
	size_t pad_begin = offsetof(ChiakiNax5Counters, ref_missing) + sizeof(src.ref_missing);
	size_t pad_end = offsetof(ChiakiNax5Counters, bytes_rx);
	for(size_t i = 0; i < sizeof(src); i++)
	{
		bool written = a[i] == b[i];
		if(i >= pad_begin && i < pad_end)
			continue; // padding (4 bytes on x86-64, none on i386)
		if(!written)
			munit_errorf("byte %zu of ChiakiNax5Counters not copied by chiaki_nax5_counters_snapshot "
					"(new field missing there, or new padding: update the test)", i);
		munit_assert_uint8(a[i], ==, s[i]);
	}

	// Same check as a plain memcmp once the padding is equalized.
	memset((uint8_t *)&src + pad_begin, 0, pad_end - pad_begin);
	munit_assert_memory_equal(sizeof(src), &src, &dst_zero);
	return MUNIT_OK;
}

// bytes_rx is 64-bit: the full value must survive (the old word-by-word copy could tear it).
static MunitResult test_snapshot_bytes_rx_64bit(const MunitParameter params[], void *user)
{
	ChiakiNax5Counters src, dst;
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	CHIAKI_NAX5_ADD(&src, bytes_rx, (uint64_t)0x123456789abcdef0ULL);
	CHIAKI_NAX5_SET(&src, audio_jitter_last, 5);
	CHIAKI_NAX5_ADD(&src, audio_skipped, 321);
	chiaki_nax5_counters_snapshot(&src, &dst);
	munit_assert_uint64(dst.bytes_rx, ==, 0x123456789abcdef0ULL);
	munit_assert_uint32(dst.audio_jitter_last, ==, 5);
	munit_assert_uint32(dst.audio_skipped, ==, 321);
	// NULL arguments are ignored
	chiaki_nax5_counters_snapshot(NULL, &dst);
	chiaki_nax5_counters_snapshot(&src, NULL);
	return MUNIT_OK;
}

MunitTest tests_nax5counters[] = {
	{ "/snapshot_copies_every_field", test_snapshot_copies_every_field, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/snapshot_bytes_rx_64bit", test_snapshot_bytes_rx_64bit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
