#include <ctype.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// POSIX's fnmatch in the C locale: *, ? and bracket expressions with ranges, classes and '!', backslash escapes,
// FNM_PATHNAME and FNM_PERIOD. A bracket's "[=a=]" and "[.a.]" aren't read, and a backslash last matches itself.

enum { KNOWN_FLAGS = FNM_NOESCAPE | FNM_PATHNAME | FNM_PERIOD };

// A period at the start of the string, or of a name with FNM_PATHNAME, which only a period in the pattern matches.
static bool leading_period(const char *string, const char *at, int flags)
{
	return *at == '.' && (flags & FNM_PERIOD) && (at == string || ((flags & FNM_PATHNAME) && at[-1] == '/'));
}

// The character at *pattern, an escaped one for a backslash, moving *pattern past it.
static unsigned char take_char(const char **pattern, int flags)
{
	const char *p = *pattern;
	if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1])
		p++;
	*pattern = p + 1;
	return (unsigned char)*p;
}

// Whether c is in the character class of the name's length bytes, as "alpha" in "[:alpha:]"; an unknown class holds
// nothing.
static bool in_class(const char *name, size_t length, unsigned char c)
{
	static const struct {
		const char *name;
		int (*test)(int);
	} classes[] = {
		{ "alnum", isalnum },
		{ "alpha", isalpha },
		{ "blank", isblank },
		{ "cntrl", iscntrl },
		{ "digit", isdigit },
		{ "graph", isgraph },
		{ "lower", islower },
		{ "print", isprint },
		{ "punct", ispunct },
		{ "space", isspace },
		{ "upper", isupper },
		{ "xdigit", isxdigit },
	};
	for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		if (strlen(classes[i].name) == length && strncmp(classes[i].name, name, length) == 0)
			return classes[i].test(c) != 0;
	}
	return false;
}

// Matches c against the bracket expression whose '[' is just before pattern, and returns the pattern past its ']', or
// NULL where there's no ']', which leaves the '[' an ordinary character.
static const char *match_bracket(const char *pattern, unsigned char c, int flags, bool *matched)
{
	const bool negated = *pattern == '!';
	pattern += negated;
	bool found = false;
	// A ']' first is one of the characters.
	for (bool first = true; first || *pattern != ']'; first = false) {
		if (!*pattern)
			return NULL;
		const char *class_end = pattern[0] == '[' && pattern[1] == ':' ? strstr(pattern + 2, ":]") : NULL;
		if (class_end) {
			found |= in_class(pattern + 2, (size_t)(class_end - pattern - 2), c);
			pattern = class_end + 2;
			continue;
		}
		const unsigned char low = take_char(&pattern, flags);
		unsigned char high = low;
		if (pattern[0] == '-' && pattern[1] && pattern[1] != ']') {
			pattern++;
			high = take_char(&pattern, flags);
		}
		found |= low <= c && c <= high;
	}
	*matched = found != negated;
	return pattern + 1;
}

// Matches the character at at against the pattern element at *pattern, moving *pattern past the element.
static bool match_one(const char **pattern, const char *string, const char *at, int flags)
{
	const unsigned char c = (unsigned char)*at;
	// Only a slash or a period written in the pattern matches these.
	const bool literal_only = (c == '/' && (flags & FNM_PATHNAME)) || leading_period(string, at, flags);
	const char *p = *pattern;
	if (*p == '?') {
		*pattern = p + 1;
		return !literal_only;
	}
	if (*p == '[') {
		bool matched;
		const char *end = match_bracket(p + 1, c, flags, &matched);
		if (end) {
			*pattern = end;
			return matched && !literal_only;
		}
	}
	return take_char(pattern, flags) == c;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
	if (flags & ~KNOWN_FLAGS)
		return -1;
	const char *p = pattern;
	const char *s = string;
	// Where the last '*' resumes: the pattern after it, and the string after what it has taken so far.
	const char *star_pattern = NULL;
	const char *star_string = NULL;
	for (;;) {
		if (*p == '*') {
			// Not even empty: "*.c" doesn't match ".c", as in the shell. No other '*' could take this period instead.
			if (leading_period(string, s, flags))
				return FNM_NOMATCH;
			while (*p == '*')
				p++;
			star_pattern = p;
			star_string = s;
			continue;
		}
		if (!*p && !*s)
			return 0;
		if (*p && *s && match_one(&p, string, s, flags)) {
			s++;
			continue;
		}
		// The last '*' takes one more character, if it may; an earlier one could only take what that one can.
		if (!star_pattern || !*star_string || (*star_string == '/' && (flags & FNM_PATHNAME)))
			return FNM_NOMATCH;
		star_string++;
		p = star_pattern;
		s = star_string;
	}
}
