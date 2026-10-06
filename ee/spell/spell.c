// clang-format off
#include "bsdcompat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spell.h"
// clang-format on

/*
 * An original, small affix-based spell checker written for ee in C23.
 *
 * The design is ee's own and shares no code with any third-party spell
 * engine; the ".aff"/".dic" file format is the only shared concept:
 *
 *   - words live in a growable array, indexed by a chained hash table
 *     built with FNV-1a (ee's own choice of hash);
 *   - affix conditions are compiled to a small token sequence and
 *     matched directly against the stem;
 *   - prefix and suffix rules are stored in flat arrays.
 *
 * Only what ee needs is present: exact lookup, one prefix or one suffix
 * at a time, and one-edit suggestions.  Compound words, cross-product
 * affixes, n-gram suggestions and multiple languages are out of scope.
 *
 * This file is covered by the project licence (see LICENSES/ee); it
 * contains no third-party code.
 */

#define SPELL_MAX_WORD 128
#define SPELL_MAX_RULE 48
#define SPELL_MAX_TOK 16

struct spell_entry {
	char	       *word;
	unsigned short *flags;
	int		nflags;
};

struct spell_tok {
	unsigned char set[128];
	int	      negate;
	int	      any;
	int	      rep;
};

struct spell_rule {
	unsigned short	 flag;
	char		 strip[SPELL_MAX_RULE];
	char		 add[SPELL_MAX_RULE];
	int		 striplen;
	int		 addlen;
	struct spell_tok tok[SPELL_MAX_TOK];
	int		 ntok;
};

struct spell_repl {
	char *from;
	char *to;
};

struct ee_spell {
	int		    flag_long;
	char		    tryset[128];
	int		    ntry;
	struct spell_rule  *prefix;
	int		    nprefix;
	struct spell_rule  *suffix;
	int		    nsuffix;
	struct spell_repl  *rep;
	int		    nrep;
	struct spell_entry *ents;
	int		    nents;
	int		   *head; /* hash buckets: entry index + 1, 0 empty */
	int		   *next; /* next entry index + 1, 0 end */
	int		    nbuckets;
};

/* ---------------------------------------------------------------- */
/* helpers							    */
/* ---------------------------------------------------------------- */

static char *
spell_strdup(const char *s)
{
	size_t len;
	char  *p;

	len = strlen(s);
	p = malloc(len + 1);
	if (p != nullptr)
		memcpy(p, s, len + 1);
	return (p);
}

static void
spell_rstrip(char *s)
{
	auto len = strlen(s);

	while ((len > 0) && ((s[len - 1] == '\n') || (s[len - 1] == '\r')))
		s[--len] = '\0';
}

/*
 * FNV-1a over the bytes of the word.  A private choice for this module;
 * it has no relationship to any other spell checker's hash.
 */
static unsigned long
spell_hash(const char *word, size_t len)
{
	unsigned long h = 2166136261UL;
	size_t	      i;

	for (i = 0; i < len; i++) {
		h ^= (unsigned char)word[i];
		h *= 16777619UL;
	}
	return (h);
}

/* ---------------------------------------------------------------- */
/* word tables							    */
/* ---------------------------------------------------------------- */

static int
add_flag(struct spell_entry *e, unsigned short flag)
{
	unsigned short *nf;

	nf = reallocarray(
	    e->flags, (size_t)(e->nflags + 1), sizeof(unsigned short));
	if (nf == nullptr)
		return (-1);
	e->flags = nf;
	e->flags[e->nflags++] = flag;
	return (0);
}

static int
entry_has_flag(const struct spell_entry *e, unsigned short flag)
{
	int i;

	for (i = 0; i < e->nflags; i++) {
		if (e->flags[i] == flag)
			return (1);
	}
	return (0);
}

static int
find_word(const struct ee_spell *sp, const char *word, size_t len)
{
	unsigned long h;
	int	      idx;

	if ((sp->head == nullptr) || (sp->nbuckets <= 0) ||
	    (len >= SPELL_MAX_WORD))
		return (-1);
	h = spell_hash(word, len) % (unsigned long)sp->nbuckets;
	for (idx = sp->head[h]; idx != 0; idx = sp->next[idx - 1]) {
		const struct spell_entry *e = &sp->ents[idx - 1];

		if ((strlen(e->word) == len) &&
		    (memcmp(e->word, word, len) == 0))
			return (idx - 1);
	}
	return (-1);
}

static int
build_index(struct ee_spell *sp)
{
	unsigned long h;
	int	      i, n;

	n = sp->nents * 2 + 1;
	sp->head = calloc((size_t)n, sizeof(int));
	sp->next = calloc((size_t)(sp->nents > 0 ? sp->nents : 1), sizeof(int));
	if ((sp->head == nullptr) || (sp->next == nullptr)) {
		free(sp->head);
		free(sp->next);
		sp->head = nullptr;
		sp->next = nullptr;
		return (-1);
	}
	sp->nbuckets = n;
	for (i = 0; i < sp->nents; i++) {
		const char *w = sp->ents[i].word;

		h = spell_hash(w, strlen(w)) % (unsigned long)n;
		sp->next[i] = sp->head[h];
		sp->head[h] = i + 1;
	}
	return (0);
}

/* ---------------------------------------------------------------- */
/* conditions							    */
/* ---------------------------------------------------------------- */

static int
tok_match(const struct spell_tok *t, unsigned char c)
{
	int in;

	if (t->any)
		return (1);
	in = (c < 128) ? t->set[c] : 0;
	return (t->negate ? !in : in);
}

/* Match tokens left to right, anchored at the start of s. */
static int
match_prefix(const struct spell_tok *tok, int ti, int ntok,
    const unsigned char *s, int len, int pos)
{
	const struct spell_tok *t;
	int			k;

	if (ti == ntok)
		return (1);
	t = &tok[ti];
	if (t->rep) {
		for (k = pos;; k++) {
			if (match_prefix(tok, ti + 1, ntok, s, len, k))
				return (1);
			if ((k >= len) || !tok_match(t, s[k]))
				break;
		}
		return (0);
	}
	if ((pos < len) && tok_match(t, s[pos]))
		return (match_prefix(tok, ti + 1, ntok, s, len, pos + 1));
	return (0);
}

/* Match tokens right to left, anchored at the end of s. */
static int
match_suffix(
    const struct spell_tok *tok, int ti, const unsigned char *s, int pos)
{
	const struct spell_tok *t;
	int			k;

	if (ti < 0)
		return (1);
	t = &tok[ti];
	if (t->rep) {
		for (k = pos;; k--) {
			if (match_suffix(tok, ti - 1, s, k))
				return (1);
			if ((k <= 0) || !tok_match(t, s[k - 1]))
				break;
		}
		return (0);
	}
	if ((pos > 0) && tok_match(t, s[pos - 1]))
		return (match_suffix(tok, ti - 1, s, pos - 1));
	return (0);
}

static int
rule_matches(const struct spell_rule *r, const char *stem, int len, int suffix)
{
	if (r->ntok == 0)
		return (1);
	if (len < 0)
		return (0);
	if (suffix)
		return (match_suffix(
		    r->tok, r->ntok - 1, (const unsigned char *)stem, len));
	return (match_prefix(
	    r->tok, 0, r->ntok, (const unsigned char *)stem, len, 0));
}

static int
parse_condition(const char *s, struct spell_rule *r)
{
	const unsigned char *p = (const unsigned char *)s;
	int		     n = 0;

	r->ntok = 0;
	if ((s == nullptr) || (s[0] == '\0') || (strcmp(s, ".") == 0))
		return (0);
	while (*p != '\0') {
		struct spell_tok *t;

		if (n >= SPELL_MAX_TOK)
			return (-1);
		t = &r->tok[n];
		memset(t, 0, sizeof(*t));
		if (*p == '[') {
			p++;
			if (*p == '^') {
				t->negate = 1;
				p++;
			}
			while ((*p != '\0') && (*p != ']')) {
				if (*p < 128)
					t->set[*p] = 1;
				p++;
			}
			if (*p == ']')
				p++;
		} else if (*p == '.') {
			t->any = 1;
			p++;
		} else {
			if (*p < 128)
				t->set[*p] = 1;
			p++;
		}
		if (*p == '*') {
			t->rep = 1;
			p++;
		}
		n++;
	}
	r->ntok = n;
	return (0);
}

/* ---------------------------------------------------------------- */
/* rules and checking						    */
/* ---------------------------------------------------------------- */

static void
copy_part(char *dst, int dstsz, const char *src, int *lenp)
{
	if ((src == nullptr) || (strcmp(src, "0") == 0)) {
		dst[0] = '\0';
		*lenp = 0;
		return;
	}
	strlcpy(dst, src, (size_t)dstsz);
	*lenp = (int)strlen(dst);
}

static unsigned short
parse_flag(const char *s, int flag_long, size_t *used)
{
	if (flag_long) {
		if ((s[0] == '\0') || (s[1] == '\0')) {
			*used = (s[0] == '\0') ? 0 : 1;
			return ((unsigned short)(unsigned char)s[0] << 8);
		}
		*used = 2;
		return ((unsigned short)(((unsigned char)s[0] << 8) |
		    (unsigned char)s[1]));
	}
	*used = 1;
	return ((unsigned char)s[0]);
}

static int
append_rule(struct spell_rule **list, int *n, const struct spell_rule *r)
{
	struct spell_rule *nr;

	nr = reallocarray(*list, (size_t)(*n + 1), sizeof(struct spell_rule));
	if (nr == nullptr)
		return (-1);
	nr[*n] = *r;
	(*n)++;
	*list = nr;
	return (0);
}

static int
check_affixes(struct ee_spell *sp, const char *word, int len)
{
	int i;

	for (i = 0; i < sp->nsuffix; i++) {
		const struct spell_rule *r = &sp->suffix[i];
		char			 stem[SPELL_MAX_WORD];
		int			 stemlen, j;

		if ((r->addlen == 0) || (len < r->addlen))
			continue;
		if (memcmp(word + len - r->addlen, r->add, (size_t)r->addlen) !=
		    0)
			continue;
		stemlen = len - r->addlen + r->striplen;
		if ((stemlen <= 0) || (stemlen >= SPELL_MAX_WORD))
			continue;
		memcpy(stem, word, (size_t)(len - r->addlen));
		memcpy(stem + (len - r->addlen), r->strip, (size_t)r->striplen);
		stem[stemlen] = '\0';
		if (!rule_matches(r, stem, stemlen, 1))
			continue;
		j = find_word(sp, stem, (size_t)stemlen);
		if ((j >= 0) && entry_has_flag(&sp->ents[j], r->flag))
			return (1);
	}
	for (i = 0; i < sp->nprefix; i++) {
		const struct spell_rule *r = &sp->prefix[i];
		char			 stem[SPELL_MAX_WORD];
		int			 stemlen, j;

		if ((r->addlen == 0) || (len < r->addlen))
			continue;
		if (memcmp(word, r->add, (size_t)r->addlen) != 0)
			continue;
		stemlen = r->striplen + (len - r->addlen);
		if ((stemlen <= 0) || (stemlen >= SPELL_MAX_WORD))
			continue;
		memcpy(stem, r->strip, (size_t)r->striplen);
		memcpy(stem + r->striplen, word + r->addlen,
		    (size_t)(len - r->addlen));
		stem[stemlen] = '\0';
		if (!rule_matches(r, stem, stemlen, 0))
			continue;
		j = find_word(sp, stem, (size_t)stemlen);
		if ((j >= 0) && entry_has_flag(&sp->ents[j], r->flag))
			return (1);
	}
	return (0);
}

static int
check_word(struct ee_spell *sp, const char *word, size_t len)
{
	if ((len == 0) || (len >= SPELL_MAX_WORD))
		return (0);
	if (find_word(sp, word, len) >= 0)
		return (1);
	return (check_affixes(sp, word, (int)len));
}

/* ---------------------------------------------------------------- */
/* dictionary and affix files					    */
/* ---------------------------------------------------------------- */

static int
load_dic(struct ee_spell *sp, const char *path)
{
	FILE   *f;
	char   *line = nullptr;
	size_t	cap = 0;
	ssize_t n;
	int	first = 1;

	f = fopen(path, "r");
	if (f == nullptr)
		return (-1);
	while ((n = getline(&line, &cap, f)) != -1) {
		char		   *slash;
		struct spell_entry *e;

		spell_rstrip(line);
		if (first) {
			first = 0;
			continue; /* the count on the first line is advisory */
		}
		if (line[0] == '\0')
			continue;
		if ((strchr(line, ' ') != nullptr) ||
		    (strchr(line, '\t') != nullptr))
			continue; /* compound entries are out of scope */
		slash = strchr(line, '/');
		if (slash != nullptr)
			*slash = '\0';
		if ((line[0] == '\0') || (strlen(line) >= SPELL_MAX_WORD))
			continue;

		e = reallocarray(sp->ents, (size_t)(sp->nents + 1),
		    sizeof(struct spell_entry));
		if (e == nullptr)
			break;
		sp->ents = e;
		e = &sp->ents[sp->nents];
		e->word = spell_strdup(line);
		e->flags = nullptr;
		e->nflags = 0;
		if (e->word == nullptr)
			break;
		sp->nents++;

		if (slash != nullptr) {
			const char *p = slash + 1;

			while (*p != '\0') {
				size_t	       used = 0;
				unsigned short fl =
				    parse_flag(p, sp->flag_long, &used);

				if (used == 0)
					break;
				if (add_flag(e, fl) != 0)
					break;
				p += used;
			}
		}
	}
	free(line);
	fclose(f);
	return (0);
}

static int
load_aff(struct ee_spell *sp, const char *path)
{
	FILE   *f;
	char   *line = nullptr;
	size_t	cap = 0;
	ssize_t n;

	f = fopen(path, "r");
	if (f == nullptr)
		return (-1);
	while ((n = getline(&line, &cap, f)) != -1) {
		struct spell_rule r;
		char		  type[8];
		char		  flagf[8];
		char		  stripf[SPELL_MAX_RULE];
		char		  addf[SPELL_MAX_RULE];
		char		  condf[SPELL_MAX_RULE];
		int		  got;

		spell_rstrip(line);
		if ((line[0] == '\0') || (line[0] == '#'))
			continue;
		if (strncmp(line, "FLAG ", 5) == 0) {
			if (strncmp(line + 5, "long", 4) == 0)
				sp->flag_long = 1;
			continue;
		}
		if (strncmp(line, "TRY ", 4) == 0) {
			const char *p = line + 4;
			int	    k = 0;

			while ((*p != '\0') && (k < 127)) {
				if ((*p != ' ') && (*p != '\t'))
					sp->tryset[k++] = *p;
				p++;
			}
			sp->ntry = k;
			continue;
		}
		if (strncmp(line, "REP ", 4) == 0) {
			struct spell_repl *nr;
			char		  *from;
			char		  *to;

			from = line + 4;
			while ((*from == ' ') || (*from == '\t'))
				from++;
			to = from;
			while ((*to != '\0') && (*to != ' ') && (*to != '\t'))
				to++;
			if (*to == '\0')
				continue;
			*to++ = '\0';
			while ((*to == ' ') || (*to == '\t'))
				to++;
			if (*to == '\0')
				continue;
			/*
			 * Grow with a temporary so that a failed
			 * allocation cannot lose the existing array.
			 */
			nr = reallocarray(sp->rep, (size_t)(sp->nrep + 1),
			    sizeof(struct spell_repl));
			if (nr == nullptr)
				break;
			sp->rep = nr;
			sp->rep[sp->nrep].from = spell_strdup(from);
			sp->rep[sp->nrep].to = spell_strdup(to);
			if ((sp->rep[sp->nrep].from == nullptr) ||
			    (sp->rep[sp->nrep].to == nullptr)) {
				free(sp->rep[sp->nrep].from);
				free(sp->rep[sp->nrep].to);
				break;
			}
			sp->nrep++;
			continue;
		}
		if ((strncmp(line, "PFX", 3) != 0) &&
		    (strncmp(line, "SFX", 3) != 0))
			continue;
		/*
		 * A rule line has five fields; a header ("PFX A Y 1") has
		 * four and is skipped.
		 */
		got = sscanf(line, "%7s %7s %47s %47s %47s", type, flagf,
		    stripf, addf, condf);
		if (got != 5)
			continue;
		memset(&r, 0, sizeof(r));
		r.flag = parse_flag(flagf, sp->flag_long, &(size_t){0});
		copy_part(r.strip, SPELL_MAX_RULE, stripf, &r.striplen);
		copy_part(r.add, SPELL_MAX_RULE, addf, &r.addlen);
		if (parse_condition(condf, &r) != 0) {
			/* Unsupported condition: make the rule unusable. */
			r.add[0] = '\0';
			r.addlen = 0;
		}
		if (strcmp(type, "SFX") == 0)
			(void)append_rule(&sp->suffix, &sp->nsuffix, &r);
		else
			(void)append_rule(&sp->prefix, &sp->nprefix, &r);
	}
	free(line);
	fclose(f);
	return (0);
}

/* ---------------------------------------------------------------- */
/* suggestions							    */
/* ---------------------------------------------------------------- */

static int
sug_has(char out[][64], int ns, const char *cand, size_t len)
{
	int i;

	for (i = 0; i < ns; i++) {
		if ((strlen(out[i]) == len) && (memcmp(out[i], cand, len) == 0))
			return (1);
	}
	return (0);
}

static int
sug_add(struct ee_spell *sp, char out[][64], int ns, int max, const char *cand,
    size_t len)
{
	if ((ns >= max) || (len == 0) || (len >= 64))
		return (ns);
	if (sug_has(out, ns, cand, len))
		return (ns);
	if (!check_word(sp, cand, len))
		return (ns);
	memcpy(out[ns], cand, len);
	out[ns][len] = '\0';
	return (ns + 1);
}

static int
sug_deletions(struct ee_spell *sp, char out[][64], int ns, int max,
    const char *word, size_t len)
{
	char   cand[SPELL_MAX_WORD];
	size_t i, j;

	for (i = 0; (i < len) && (ns < max); i++) {
		size_t k = 0;

		for (j = 0; j < len; j++) {
			if (j != i)
				cand[k++] = word[j];
		}
		ns = sug_add(sp, out, ns, max, cand, k);
	}
	return (ns);
}

static int
sug_transpositions(struct ee_spell *sp, char out[][64], int ns, int max,
    const char *word, size_t len)
{
	char   cand[SPELL_MAX_WORD];
	size_t i;

	if (len >= SPELL_MAX_WORD)
		return (ns);
	memcpy(cand, word, len);
	for (i = 0; (i + 1 < len) && (ns < max); i++) {
		cand[i] = word[i + 1];
		cand[i + 1] = word[i];
		ns = sug_add(sp, out, ns, max, cand, len);
		cand[i] = word[i];
		cand[i + 1] = word[i + 1];
	}
	return (ns);
}

static int
sug_substitutions(struct ee_spell *sp, char out[][64], int ns, int max,
    const char *word, size_t len)
{
	char   cand[SPELL_MAX_WORD];
	size_t i;
	int    j;

	if (len >= SPELL_MAX_WORD)
		return (ns);
	memcpy(cand, word, len);
	for (i = 0; (i < len) && (ns < max); i++) {
		for (j = 0; (j < sp->ntry) && (ns < max); j++) {
			cand[i] = sp->tryset[j];
			ns = sug_add(sp, out, ns, max, cand, len);
		}
		cand[i] = word[i];
	}
	return (ns);
}

static int
sug_insertions(struct ee_spell *sp, char out[][64], int ns, int max,
    const char *word, size_t len)
{
	char   cand[SPELL_MAX_WORD];
	size_t i;
	int    j;

	if (len + 1 >= SPELL_MAX_WORD)
		return (ns);
	for (i = 0; (i <= len) && (ns < max); i++) {
		for (j = 0; (j < sp->ntry) && (ns < max); j++) {
			memcpy(cand, word, i);
			cand[i] = sp->tryset[j];
			memcpy(cand + i + 1, word + i, len - i);
			ns = sug_add(sp, out, ns, max, cand, len + 1);
		}
	}
	return (ns);
}

static int
sug_replacements(struct ee_spell *sp, char out[][64], int ns, int max,
    const char *word, size_t len)
{
	char cand[SPELL_MAX_WORD];
	int  i;

	if (len >= SPELL_MAX_WORD)
		return (ns);
	for (i = 0; (i < sp->nrep) && (ns < max); i++) {
		const char *from = sp->rep[i].from;
		const char *to = sp->rep[i].to;
		auto	    fl = strlen(from);
		auto	    tl = strlen(to);
		const char *p = word;

		if (fl == 0)
			continue;
		while ((p = strstr(p, from)) != nullptr) {
			size_t off = (size_t)(p - word);

			if ((off + tl + strlen(p + fl)) >= SPELL_MAX_WORD)
				break;
			memcpy(cand, word, (size_t)len);
			memcpy(cand + off, to, tl);
			memcpy(cand + off + tl, p + fl, strlen(p + fl) + 1);
			ns = sug_add(
			    sp, out, ns, max, cand, off + tl + strlen(p + fl));
			p++;
		}
	}
	return (ns);
}

/* ---------------------------------------------------------------- */
/* public interface						    */
/* ---------------------------------------------------------------- */

struct ee_spell *
ee_spell_open(const char *aff_path, const char *dic_path)
{
	struct ee_spell *sp;

	if (dic_path == nullptr)
		return (nullptr);
	sp = calloc(1, sizeof(*sp));
	if (sp == nullptr)
		return (nullptr);
	if ((aff_path != nullptr) && (load_aff(sp, aff_path) != 0)) {
		ee_spell_close(sp);
		return (nullptr);
	}
	if (load_dic(sp, dic_path) != 0 || sp->nents == 0) {
		ee_spell_close(sp);
		return (nullptr);
	}
	if (build_index(sp) != 0) {
		ee_spell_close(sp);
		return (nullptr);
	}
	return (sp);
}

void
ee_spell_close(struct ee_spell *sp)
{
	int i;

	if (sp == nullptr)
		return;
	for (i = 0; i < sp->nents; i++) {
		free(sp->ents[i].word);
		free(sp->ents[i].flags);
	}
	free(sp->ents);
	free(sp->head);
	free(sp->next);
	free(sp->prefix);
	free(sp->suffix);
	for (i = 0; i < sp->nrep; i++) {
		free(sp->rep[i].from);
		free(sp->rep[i].to);
	}
	free(sp->rep);
	free(sp);
}

int
ee_spell_check(struct ee_spell *sp, const char *word, size_t len)
{
	if (sp == nullptr)
		return (0);
	return (check_word(sp, word, len));
}

int
ee_spell_suggest(
    struct ee_spell *sp, const char *word, size_t len, char out[][64], int max)
{
	int ns = 0;

	if ((sp == nullptr) || (len == 0) || (len >= SPELL_MAX_WORD) ||
	    (max <= 0))
		return (0);
	if (check_word(sp, word, len))
		return (0);
	ns = sug_replacements(sp, out, ns, max, word, len);
	if (ns < max)
		ns = sug_deletions(sp, out, ns, max, word, len);
	if (ns < max)
		ns = sug_transpositions(sp, out, ns, max, word, len);
	if (ns < max)
		ns = sug_substitutions(sp, out, ns, max, word, len);
	if (ns < max)
		ns = sug_insertions(sp, out, ns, max, word, len);
	return (ns);
}
