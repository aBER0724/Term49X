#ifndef WIDE_WIDTH_H
#define WIDE_WIDTH_H
#include <wchar.h>
int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);
/* QNX's iswprint()/iswgraph() say every non-ASCII wide char is
 * unprintable, which makes zle render CJK as "<7ec4>". The build maps the
 * names onto these. */
int t49_iswprint(wint_t c);
int t49_iswgraph(wint_t c);
#endif
