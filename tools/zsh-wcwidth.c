/* wcwidth/wcswidth for QNX: its libc has the mbrtowc family but no width
 * functions, which zle needs to place the cursor after CJK/emoji. */
#include <wchar.h>
#include <wctype.h>

/* QNX declares these as macros; we are replacing them with real functions */
#undef iswprint
#undef iswgraph

static int in(wchar_t c, unsigned lo, unsigned hi) {
	return (unsigned)c >= lo && (unsigned)c <= hi;
}

int wcwidth(wchar_t c) {
	unsigned u = (unsigned)c;
	if (u == 0) return 0;
	if (u < 32 || (u >= 0x7f && u < 0xa0)) return -1;
	/* combining marks take no cell */
	if (in(c, 0x0300, 0x036f) || in(c, 0x0483, 0x0489) ||
	    in(c, 0x0591, 0x05bd) || in(c, 0x0610, 0x061a) ||
	    in(c, 0x064b, 0x065f) || in(c, 0x0670, 0x0670) ||
	    in(c, 0x06d6, 0x06dc) || in(c, 0x0e31, 0x0e31) ||
	    in(c, 0x0e34, 0x0e3a) || in(c, 0x0e47, 0x0e4e) ||
	    in(c, 0x1ab0, 0x1aff) || in(c, 0x1dc0, 0x1dff) ||
	    in(c, 0x20d0, 0x20f0) || in(c, 0xfe20, 0xfe2f)) return 0;
	/* East-Asian wide / fullwidth */
	if (in(c, 0x1100, 0x115f) || in(c, 0x2329, 0x232a) ||
	    in(c, 0x2e80, 0x303e) || in(c, 0x3041, 0x33ff) ||
	    in(c, 0x3400, 0x4dbf) || in(c, 0x4e00, 0x9fff) ||
	    in(c, 0xa000, 0xa4cf) || in(c, 0xa960, 0xa97f) ||
	    in(c, 0xac00, 0xd7a3) || in(c, 0xf900, 0xfaff) ||
	    in(c, 0xfe10, 0xfe19) || in(c, 0xfe30, 0xfe6f) ||
	    in(c, 0xff00, 0xff60) || in(c, 0xffe0, 0xffe6) ||
	    in(c, 0x1b000, 0x1b001) || in(c, 0x1f004, 0x1f004) ||
	    in(c, 0x1f0cf, 0x1f0cf) || in(c, 0x1f18e, 0x1f18e) ||
	    in(c, 0x1f191, 0x1f19a) || in(c, 0x1f200, 0x1f320) ||
	    in(c, 0x1f32d, 0x1f335) || in(c, 0x1f337, 0x1f37c) ||
	    in(c, 0x1f37e, 0x1f393) || in(c, 0x1f3a0, 0x1f3ca) ||
	    in(c, 0x1f3cf, 0x1f3d3) || in(c, 0x1f3e0, 0x1f3f0) ||
	    in(c, 0x1f3f4, 0x1f3f4) || in(c, 0x1f3f8, 0x1f43e) ||
	    in(c, 0x1f440, 0x1f440) || in(c, 0x1f442, 0x1f4fc) ||
	    in(c, 0x1f4ff, 0x1f53d) || in(c, 0x1f54b, 0x1f54e) ||
	    in(c, 0x1f550, 0x1f567) || in(c, 0x1f57a, 0x1f57a) ||
	    in(c, 0x1f595, 0x1f596) || in(c, 0x1f5a4, 0x1f5a4) ||
	    in(c, 0x1f5fb, 0x1f64f) || in(c, 0x1f680, 0x1f6c5) ||
	    in(c, 0x1f6cc, 0x1f6cc) || in(c, 0x1f6d0, 0x1f6d2) ||
	    in(c, 0x1f6eb, 0x1f6ec) || in(c, 0x1f6f4, 0x1f6f9) ||
	    in(c, 0x1f910, 0x1f93e) || in(c, 0x1f940, 0x1f970) ||
	    in(c, 0x1f973, 0x1f976) || in(c, 0x1f97a, 0x1f97a) ||
	    in(c, 0x1f97c, 0x1f9a2) || in(c, 0x1f9b0, 0x1f9b9) ||
	    in(c, 0x1f9c0, 0x1f9c2) || in(c, 0x1f9d0, 0x1f9ff) ||
	    in(c, 0x20000, 0x3fffd)) return 2;
	return 1;
}

/* QNX's C locale reports every non-ASCII wide char as non-printable, which
 * makes zle draw CJK as "<7ec4>" instead of the glyph. Everything above the
 * C1 controls is printable as far as a terminal is concerned. */
int t49_iswprint(wint_t c) {
	unsigned u = (unsigned)c;
	if (u < 0x20 || (u >= 0x7f && u < 0xa0)) return 0;
	return 1;
}

int t49_iswgraph(wint_t c) {
	unsigned u = (unsigned)c;
	if (u <= 0x20 || (u >= 0x7f && u < 0xa0)) return 0;
	return 1;
}

int wcswidth(const wchar_t *s, size_t n) {
	int total = 0;
	size_t i;
	for (i = 0; i < n && s[i]; ++i) {
		int w = wcwidth(s[i]);
		if (w < 0) return -1;
		total += w;
	}
	return total;
}
