/*
 * Unit test for ee's own spell engine (spell/spell.c).
 *
 * Usage: test-spell [affix] [dictionary]
 *
 * The dictionary used here (tests/spell-test.aff/.dic) is tiny and was
 * written for this test; it carries the project licence and no
 * third-party data.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spell/spell.h"

static int failures;

static void
expect_check(struct ee_spell *sp, const char *w, int want)
{
	int got = ee_spell_check(sp, w, strlen(w));

	printf("%-12s check=%d want=%d %s\n", w, got, want,
	    (got == want) ? "ok" : "FAIL");
	if (got != want)
		failures++;
}

static void
expect_suggest(struct ee_spell *sp, const char *w, const char *want)
{
	char out[8][64];
	int  n, i, found = 0;

	n = ee_spell_suggest(sp, w, strlen(w), out, 8);
	for (i = 0; i < n; i++) {
		int k;

		if (strcmp(out[i], want) == 0)
			found = 1;
		for (k = i + 1; k < n; k++) {
			if (strcmp(out[i], out[k]) == 0) {
				printf("duplicate suggestion: %s\n", out[i]);
				failures++;
			}
		}
	}
	printf("suggest(%-8s) -> %-2d, contains %-10s %s\n", w, n, want,
	    found ? "ok" : "FAIL");
	if (!found) {
		for (i = 0; i < n; i++)
			printf("    candidate: %s\n", out[i]);
		failures++;
	}
}

int
main(int argc, char **argv)
{
	const char	*aff = (argc > 1) ? argv[1] : "tests/spell-test.aff";
	const char	*dic = (argc > 2) ? argv[2] : "tests/spell-test.dic";
	struct ee_spell *sp = ee_spell_open(aff, dic);

	if (sp == nullptr) {
		printf("not ok - ee_spell_open(%s, %s)\n", aff, dic);
		return (1);
	}
	printf("ok - dictionary opened\n");

	expect_check(sp, "cat", 1);
	expect_check(sp, "cats", 1);
	expect_check(sp, "box", 1);
	expect_check(sp, "boxes", 1);
	expect_check(sp, "boxs", 0);
	expect_check(sp, "carry", 1);
	expect_check(sp, "carries", 1);
	expect_check(sp, "carrys", 0);
	expect_check(sp, "unhappy", 1);
	expect_check(sp, "unwork", 1);
	expect_check(sp, "unquick", 0);
	expect_check(sp, "runing", 0);

	expect_suggest(sp, "catt", "cat");
	expect_suggest(sp, "boxs", "box");

	ee_spell_close(sp);
	if (failures > 0)
		printf("FAILED: %d\n", failures);
	return (failures != 0);
}
