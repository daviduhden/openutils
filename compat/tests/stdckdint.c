/*
 * Self-test for the <stdckdint.h> fallback in compat/stdckdint.h.
 *
 * The fallback is forced (see run.sh) so that it is exercised even on
 * hosts whose compiler or libc already ships <stdckdint.h>.  The
 * cases cover the standard integer types the project actually feeds to
 * ckd_add()/ckd_mul() (size_t, uintmax_t, signed off_t) plus the
 * properties that distinguish the C23 contract from a naive a + b:
 * representability is judged against the result type, operands are not
 * converted to a common type first, and each argument is evaluated
 * exactly once.
 *
 * Exits non-zero if any check fails.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <stdckdint.h>

static int failures;

static void
check(int condition, const char *what)
{
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/*
 * Single-evaluation probes.  ckd_add() must expand each argument
 * exactly once, including the pointer that receives the result.
 */
static unsigned probe_calls;

static unsigned
probe(void)
{
	probe_calls++;
	return (1u);
}

static size_t	slots[4];
static unsigned	slot_index;

static size_t *
next_slot(void)
{
	return (&slots[slot_index++]);
}

int
main(void)
{
	size_t			z;
	uintmax_t		u;
	off_t			o;
	unsigned short		h;
	unsigned long long	q;
	long long		s;
	_Bool			ov;

	/* ---- size_t: the unsigned type used for allocation sizes ---- */
	z = 0;
	ov = ckd_add(&z, (size_t)SIZE_MAX, (size_t)0);
	check(!ov && z == SIZE_MAX, "size_t add max+0 does not overflow");

	ov = ckd_add(&z, (size_t)SIZE_MAX, (size_t)1);
	check(ov, "size_t add max+1 overflows");

	ov = ckd_mul(&z, (size_t)SIZE_MAX, (size_t)1);
	check(!ov && z == SIZE_MAX, "size_t mul max*1 does not overflow");

	ov = ckd_mul(&z, (size_t)SIZE_MAX, (size_t)2);
	check(ov, "size_t mul max*2 overflows");

	ov = ckd_sub(&z, (size_t)0, (size_t)1);
	check(ov, "size_t sub 0-1 overflows");

	ov = ckd_sub(&z, (size_t)SIZE_MAX, (size_t)SIZE_MAX);
	check(!ov && z == 0, "size_t sub max-max is zero");

	/* ---- uintmax_t: the widest unsigned type used by truncate(1) --- */
	u = 0;
	ov = ckd_add(&u, (uintmax_t)UINTMAX_MAX, (uintmax_t)0);
	check(!ov && u == UINTMAX_MAX, "uintmax_t add max+0 does not overflow");

	ov = ckd_add(&u, (uintmax_t)UINTMAX_MAX, (uintmax_t)1);
	check(ov, "uintmax_t add max+1 overflows");

	ov = ckd_mul(&u, (uintmax_t)UINTMAX_MAX, (uintmax_t)2);
	check(ov, "uintmax_t mul max*2 overflows");

	/* ---- off_t: signed sizes, overflow in both directions ---- */
	{
		const off_t off_max = (off_t)(((uintmax_t)1 <<
		    (sizeof(off_t) * CHAR_BIT - 1)) - 1);
		const off_t off_min = -off_max - 1;

		o = 0;
		ov = ckd_add(&o, off_max, (off_t)0);
		check(!ov && o == off_max, "off_t add max+0 does not overflow");

		ov = ckd_add(&o, off_max, (off_t)1);
		check(ov, "off_t add max+1 overflows");

		ov = ckd_add(&o, off_min, (off_t)-1);
		check(ov, "off_t add min-1 overflows");

		ov = ckd_sub(&o, off_min, (off_t)1);
		check(ov, "off_t sub min-1 overflows");

		ov = ckd_sub(&o, off_min, off_min);
		check(!ov && o == 0, "off_t sub min-min is zero");

		ov = ckd_mul(&o, off_min, (off_t)-1);
		check(ov, "off_t mul min*-1 overflows");

		ov = ckd_mul(&o, (off_t)-1, off_min);
		check(ov, "off_t mul -1*min overflows");

		ov = ckd_mul(&o, off_max, (off_t)1);
		check(!ov && o == off_max, "off_t mul max*1 does not overflow");
	}

	/* ---- representability is judged against the result type ---- */
	h = 0;
	ov = ckd_add(&h, 30000, 30000);
	check(!ov && h == 60000, "unsigned short add 30000+30000 fits");

	ov = ckd_add(&h, 40000, 40000);
	check(ov, "unsigned short add 40000+40000 overflows");

	/* ---- operands are promoted to infinite precision, not to a
	 *      common operand type (a naive typeof(a+b) add would wrap
	 *      here).  The mathematical sum exceeds UINT_MAX but fits in
	 *      the unsigned long long result. ---- */
	q = 0;
	ov = ckd_add(&q, (unsigned)UINT_MAX, 1u);
	check(!ov && q == (unsigned long long)UINT_MAX + 1ULL,
	    "unsigned long long add UINT_MAX+1 does not overflow");

	ov = ckd_mul(&q, (unsigned)UINT_MAX, 2u);
	check(!ov && q == (unsigned long long)UINT_MAX * 2ULL,
	    "unsigned long long mul UINT_MAX*2 does not overflow");

	s = 0;
	ov = ckd_add(&s, (int)INT_MAX, 1);
	check(!ov && s == (long long)INT_MAX + 1LL,
	    "long long add INT_MAX+1 does not overflow");

	/* ---- mixed operand widths, as used by ee and truncate ---- */
	z = 0;
	ov = ckd_mul(&z, (size_t)1000, 1000);
	check(!ov && z == 1000000, "size_t * int mixed operands");

	/* ---- each argument is evaluated exactly once ---- */
	probe_calls = 0;
	(void)ckd_add(&z, probe(), probe());
	check(probe_calls == 2, "operands are evaluated once each");

	slot_index = 0;
	(void)ckd_add(next_slot(), (size_t)1, (size_t)2);
	check(slot_index == 1, "result pointer is evaluated once");
	check(slots[0] == 3, "result was stored through the pointer");

	/* ---- return value and result type ---- */
	z = 0;
	ov = ckd_add(&z, (size_t)1, (size_t)2);
	check(ov == 0 && z == 3, "success returns false and stores result");

	if (failures != 0) {
		printf("%d stdckdint check(s) failed\n", failures);
		return (1);
	}
	printf("stdckdint fallback: all checks passed\n");
	return (0);
}
