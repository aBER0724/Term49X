#ifndef TERM49_PINYIN_H
#define TERM49_PINYIN_H

#include <stddef.h>

typedef void (*pinyin_commit_fn)(const char *utf8, size_t len);

int  pinyin_load(void);
void pinyin_toggle(void);
/* 0 = Xiaohe shuangpin (default), 1 = full pinyin */
void pinyin_set_full(int on);
int  pinyin_full(void);
void pinyin_reset(void);
int  pinyin_enabled(void);
int  pinyin_composing(void);
/* returns 1 when the key was consumed by the input bar */
int  pinyin_key(int ch, pinyin_commit_fn commit);
void pinyin_status(char *out, size_t cap);

#endif
