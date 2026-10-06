/*
 * Integration test for the bundled en_US dictionary (spell/en_US.aff and
 * spell/en_US.dic), run by `make test`.
 *
 * It checks that ee's spell engine accepts the forms the affix rules are
 * meant to derive, the irregular forms stored explicitly, and a few
 * contractions, and that it rejects non-words and forms that would need
 * two affixes at once (which the engine deliberately does not support).
 *
 * Usage: test-dictionary [en_US.aff] [en_US.dic]
 */

#include <stdio.h>
#include <string.h>

#include "spell/spell.h"

static int failures;

static void
expect(struct ee_spell *sp, const char *word, int want)
{
	int got = ee_spell_check(sp, word, strlen(word));

	if (got != want) {
		printf("FAIL %-12s check=%d want=%d\n", word, got, want);
		failures++;
	}
}

static void
expect_suggest(struct ee_spell *sp, const char *word, const char *want)
{
	char out[8][64];
	int  n, i, found = 0;

	n = ee_spell_suggest(sp, word, strlen(word), out, 8);
	for (i = 0; i < n; i++) {
		if (strcmp(out[i], want) == 0)
			found = 1;
	}
	if (!found) {
		printf("FAIL suggest(%s) does not contain %s\n", word, want);
		failures++;
	}
}

int
main(int argc, char **argv)
{
	const char	*aff = (argc > 1) ? argv[1] : "spell/en_US.aff";
	const char	*dic = (argc > 2) ? argv[2] : "spell/en_US.dic";
	struct ee_spell *sp = ee_spell_open(aff, dic);

	if (sp == nullptr) {
		printf("FAIL cannot open %s / %s\n", aff, dic);
		return (1);
	}

	/* Base words. */
	expect(sp, "hello", 1);
	expect(sp, "computer", 1);
	expect(sp, "editor", 1);
	expect(sp, "text", 1);

	/* Plural / third person -s, -es, -ies. */
	expect(sp, "cats", 1);
	expect(sp, "boxes", 1);
	expect(sp, "buses", 1);
	expect(sp, "churches", 1);
	expect(sp, "bushes", 1);
	expect(sp, "cities", 1);
	expect(sp, "carries", 1);
	expect(sp, "monkeys", 1);
	expect(sp, "potatoes", 1);

	/* Past tense -ed. */
	expect(sp, "walked", 1);
	expect(sp, "loved", 1);
	expect(sp, "carried", 1);
	expect(sp, "stopped", 1);
	expect(sp, "planned", 1);

	/* Present participle -ing. */
	expect(sp, "walking", 1);
	expect(sp, "loving", 1);
	expect(sp, "carrying", 1);
	expect(sp, "stopping", 1);
	expect(sp, "planning", 1);
	expect(sp, "writing", 1);

	/* Comparative and superlative. */
	expect(sp, "quicker", 1);
	expect(sp, "quickest", 1);
	expect(sp, "happier", 1);
	expect(sp, "happiest", 1);
	expect(sp, "bigger", 1);
	expect(sp, "biggest", 1);

	/* Adverbs -ly. */
	expect(sp, "quickly", 1);
	expect(sp, "happily", 1);
	expect(sp, "fully", 1);
	expect(sp, "simply", 1);
	expect(sp, "basically", 1);

	/* Prefixes. */
	expect(sp, "unhappy", 1);
	expect(sp, "rewrite", 1);
	expect(sp, "dislike", 1);
	expect(sp, "react", 1);
	expect(sp, "undo", 1);

	/* Irregular forms stored explicitly. */
	expect(sp, "children", 1);
	expect(sp, "mice", 1);
	expect(sp, "ran", 1);
	expect(sp, "went", 1);
	expect(sp, "gone", 1);
	expect(sp, "written", 1);
	expect(sp, "better", 1);
	expect(sp, "best", 1);
	expect(sp, "feet", 1);

	/* Contractions. */
	expect(sp, "don't", 1);
	expect(sp, "can't", 1);
	expect(sp, "it's", 1);
	expect(sp, "o'clock", 1);

	/* Rejected: non-words and forms needing two affixes. */
	expect(sp, "zzzqx", 0);
	expect(sp, "asdfgh", 0);
	expect(sp, "boxs", 0);
	expect(sp, "carrys", 0);
	expect(sp, "runed", 0);
	expect(sp, "unwalked", 0);
	expect(sp, "unhappied", 0);

	/* Suggestions. */
	expect_suggest(sp, "recieve", "receive");
	expect_suggest(sp, "happyness", "happiness");
	expect_suggest(sp, "teh", "the");
	expect_suggest(sp, "adress", "address");
	expect_suggest(sp, "occured", "occurred");
	expect_suggest(sp, "definately", "definitely");

	ee_spell_close(sp);
	if (failures > 0) {
		printf("FAILED: %d\n", failures);
		return (1);
	}
	printf("ok - en_US dictionary integration\n");
	return (0);
}
