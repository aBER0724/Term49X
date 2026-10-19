/*
 * Built-in pinyin input for Term49: Xiaohe shuangpin (小鹤双拼) and, if
 * switched on, full pinyin (全拼).
 *
 * BB10's system input method does not deliver text to SDL apps, so the
 * terminal carries its own: a sorted dictionary keyed by Xiaohe codes (one
 * "code<TAB>word word ..." line per key, words ordered by frequency) plus a
 * small composition state. Every syllable is exactly two keys, so word
 * boundaries always fall on even offsets.
 *
 *  - letters build the code buffer
 *  - space / 1..9 pick a candidate (space = first)
 *  - , and . page the candidate list
 *  - backspace deletes from the buffer
 *  - enter commits the raw letters
 *  - esc (back key) cancels
 *
 * Candidate generation, in order:
 *  1. the whole buffer as one dictionary key (e.g. "zdjm" -> 再见 再建 在建)
 *  2. otherwise syllable-wise segmentation (DP, best total score) so a long
 *     run of letters still comes out as a phrase, e.g.
 *     "vswfuurufa" -> 中文输入法, with the unconsumed tail left in the buffer
 *  3. otherwise the longest whole-syllable prefix that is in the dictionary
 * In full-pinyin mode the typed letters are first split into syllables and
 * each syllable is turned into its two-key Xiaohe code (the same dictionary
 * therefore serves both modes: "nihao" and "nihc" both become "nihc").
 *
 * Words the user picks are counted in $HOME/term49_user.dict and move to the
 * front of their candidate list; the count survives restarts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pinyin.h"

#define MAX_BUF        48
#define MAX_CANDS      64
#define PAGE           9
#define MAX_PHRASE     10          /* letters per dictionary key (5 syllables) */
#define MAX_WORDS      8           /* words in one segmentation */
#define MAX_USER       512
#define CHUNK_COST     260     /* penalty per word in the segmentation */
#define USER_FILE      "term49_user.dict"


/* ---- dictionary ------------------------------------------------------ */

static char  *dict;          /* whole file, '\n' replaced by '\0' */
static char **lines;         /* start of each line */
static int    nlines;
static int    loaded = 0;

/* ---- composition state ----------------------------------------------- */

static int   enabled = 0;
static int   full = 0;               /* 0 = Xiaohe shuangpin, 1 = full pinyin */
static char  buf[MAX_BUF + 1];       /* what the user typed */
static int   blen = 0;

/* The dictionary is keyed by Xiaohe codes, so in both modes we look things up
 * through `code`. For shuangpin that is the buffer itself; for full pinyin it
 * is the syllable-by-syllable conversion. code_in[c] = input letters the
 * first c code characters were built from. */
static char  code[MAX_BUF * 2 + 1];
static int   code_len = 0;
static int   code_in[MAX_BUF * 2 + 2];

static char  cand_store[8192];
static const char *cands[MAX_CANDS];
static int   cand_used[MAX_CANDS];   /* letters consumed by this candidate */
static int   cand_score[MAX_CANDS];
static int   ncands = 0;
static int   matched_len = 0;        /* letters the candidate list covers */
static int   page = 0;

/* ---- user word counts ------------------------------------------------ */

typedef struct {
	char word[32];
	int  count;
} userword_t;

static userword_t user[MAX_USER];
static int nuser = 0;
static int user_loaded = 0;

static const char *dict_paths[] = {
	"app/native/xiaohe.dict",
	NULL, /* $HOME/term49_xiaohe.dict, filled at load time */
	"/accounts/1000/shared/misc/term49_xiaohe.dict",
	NULL
};

static int user_lookup(const char *word) {
	int i;
	for (i = 0; i < nuser; ++i) {
		if (strcmp(user[i].word, word) == 0) {
			return user[i].count;
		}
	}
	return 0;
}

static void user_load(void) {
	char path[512];
	const char *h;
	FILE *fp;
	char line[128];

	if (user_loaded) {
		return;
	}
	user_loaded = 1;
	h = getenv("HOME");
	if (h == NULL) {
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", h, USER_FILE);
	fp = fopen(path, "r");
	if (fp == NULL) {
		return;
	}
	while (nuser < MAX_USER && fgets(line, sizeof(line), fp) != NULL) {
		char *tab = strchr(line, '\t');
		size_t wl;
		if (tab == NULL) {
			continue;
		}
		wl = (size_t)(tab - line);
		if (wl == 0 || wl >= sizeof(user[0].word)) {
			continue;
		}
		memcpy(user[nuser].word, line, wl);
		user[nuser].word[wl] = '\0';
		user[nuser].count = atoi(tab + 1);
		if (user[nuser].count > 0) {
			++nuser;
		}
	}
	fclose(fp);
}

/* rewrite the whole file: small, and only on a pick */
static void user_save(void) {
	char path[512];
	const char *h;
	FILE *fp;
	int i;
	int total = 0;

	h = getenv("HOME");
	if (h == NULL) {
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", h, USER_FILE);
	fp = fopen(path, "w");
	if (fp == NULL) {
		return;
	}
	for (i = 0; i < nuser; ++i) {
		if (user[i].count > 0) {
			fprintf(fp, "%s\t%d\n", user[i].word, user[i].count);
			++total;
		}
		if (total >= MAX_USER) {
			break;
		}
	}
	fclose(fp);
}

static void user_bump(const char *word) {
	int i, weakest = 0;
	if (word == NULL || word[0] == '\0' || strlen(word) >= sizeof(user[0].word)) {
		return;
	}
	user_load();
	for (i = 0; i < nuser; ++i) {
		if (strcmp(user[i].word, word) == 0) {
			if (user[i].count < 9999) {
				++user[i].count;
			}
			user_save();
			return;
		}
		if (user[i].count < user[weakest].count) {
			weakest = i;
		}
	}
	if (nuser < MAX_USER) {
		snprintf(user[nuser].word, sizeof(user[0].word), "%s", word);
		user[nuser].count = 1;
		++nuser;
	} else {
		/* replace the least used word so the list cannot grow forever */
		snprintf(user[weakest].word, sizeof(user[0].word), "%s", word);
		user[weakest].count = 1;
	}
	user_save();
}

/* ---- dictionary loading ---------------------------------------------- */

int pinyin_load(void) {
	char home_path[512];
	const char *h;
	FILE *fp = NULL;
	struct stat st;
	long size;
	int i;

	/* Only a successful load is final: if the dictionary was missing when
	 * the bar was switched on, try again next time instead of staying
	 * empty until the app restarts. */
	if (loaded && nlines > 0) {
		return 1;
	}
	loaded = 1;
	h = getenv("HOME");
	if (h != NULL) {
		snprintf(home_path, sizeof(home_path), "%s/term49_xiaohe.dict", h);
		dict_paths[1] = home_path;
	}
	for (i = 0; i < 3 && fp == NULL; ++i) {
		if (dict_paths[i] != NULL && stat(dict_paths[i], &st) == 0) {
			fp = fopen(dict_paths[i], "r");
		}
	}
	if (fp == NULL) {
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	dict = malloc((size_t)size + 1);
	if (dict == NULL || fread(dict, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(dict);
		dict = NULL;
		return 0;
	}
	fclose(fp);
	dict[size] = '\0';

	nlines = 0;
	for (i = 0; i < size; ++i) {
		if (dict[i] == '\n') {
			++nlines;
		}
	}
	lines = malloc(sizeof(char *) * (size_t)(nlines + 1));
	if (lines == NULL) {
		nlines = 0;
		return 0;
	}
	nlines = 0;
	lines[nlines++] = dict;
	for (i = 0; i < size; ++i) {
		if (dict[i] == '\n') {
			dict[i] = '\0';
			if (i + 1 < size) {
				lines[nlines++] = dict + i + 1;
			}
		}
	}
	user_load();
	return nlines > 0;
}

/* compare key (len bytes) against the key part of a line */
static int keycmp(const char *key, int len, const char *line) {
	int i;
	for (i = 0; i < len; ++i) {
		unsigned char a = (unsigned char)key[i];
		unsigned char b = (unsigned char)line[i];
		if (b == '\t' || b == '\0') {
			return 1;
		}
		if (a != b) {
			return a < b ? -1 : 1;
		}
	}
	return (line[len] == '\t') ? 0 : -1;
}

/* word list of one key, or NULL */
static const char *lookup(const char *key, int len) {
	int lo = 0, hi = nlines - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = keycmp(key, len, lines[mid]);
		if (c == 0) {
			return strchr(lines[mid], '\t') + 1;
		}
		if (c < 0) {
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}
	return NULL;
}

/* nth word (0-based) of a key's word list; *rank gets its position */
static const char *nth_word(const char *words, int n, int *rank) {
	int i = 0;
	while (words != NULL && *words) {
		const char *sp = strchr(words, ' ');
		size_t wl = sp ? (size_t)(sp - words) : strlen(words);
		if (i == n) {
			if (rank != NULL) {
				*rank = i;
			}
			return words;
		}
		(void)wl;
		if (sp == NULL) {
			break;
		}
		words = sp + 1;
		++i;
	}
	return NULL;
}

/* score of the n-th word of a list: earlier = better, user picks help */
static int word_score(const char *words, int n, int letters) {
	const char *w = nth_word(words, n, NULL);
	char tmp[32];
	size_t wl;
	if (w == NULL) {
		return -1000000;
	}
	{
		const char *sp = strchr(w, ' ');
		wl = sp ? (size_t)(sp - w) : strlen(w);
	}
	if (wl >= sizeof(tmp)) {
		wl = sizeof(tmp) - 1;
	}
	memcpy(tmp, w, wl);
	tmp[wl] = '\0';
	/* rank inside the dictionary line, plus how often the user chose it */
	(void)letters;
	return (200 - n * 10) + user_lookup(tmp) * 150;
}

/* append a candidate (dedup by text); returns 0 when the list is full */
static int add_cand(const char *text, int consumed, int score) {
	int i;
	if (text == NULL || text[0] == '\0') {
		return 1;
	}
	for (i = 0; i < ncands; ++i) {
		if (strcmp(cands[i], text) == 0) {
			if (score > cand_score[i]) {
				cand_score[i] = score;
				cand_used[i] = consumed;
			}
			return 1;
		}
	}
	if (ncands >= MAX_CANDS) {
		return 0;
	}
	if (strlen(text) >= sizeof(cand_store) - 1) {
		return 1;
	}
	/* cand_store is consumed front to back; strings are never freed */
	{
		size_t used = 0;
		for (i = 0; i < ncands; ++i) {
			used += strlen(cands[i]) + 1;
		}
		if (used + strlen(text) + 1 >= sizeof(cand_store)) {
			return 1;
		}
		memcpy(cand_store + used, text, strlen(text) + 1);
		cands[ncands] = cand_store + used;
	}
	cand_used[ncands] = consumed;
	cand_score[ncands] = score;
	++ncands;
	return 1;
}

/* append the nth word of a key to dst (UTF-8, NUL terminated) */
static void append_word(char *dst, size_t cap, const char *words, int n) {
	const char *w = nth_word(words, n, NULL);
	size_t wl, len;
	if (w == NULL) {
		return;
	}
	{
		const char *sp = strchr(w, ' ');
		wl = sp ? (size_t)(sp - w) : strlen(w);
	}
	len = strlen(dst);
	if (len + wl >= cap) {
		return;
	}
	memcpy(dst + len, w, wl);
	dst[len + wl] = '\0';
}

/* xiaohe code for every toneless pinyin syllable (2 keys each) */
typedef struct { const char *syl; char code[2]; } syl_code_t;
static const syl_code_t syl_codes[] = {
	{"chuang", {'i', 'l'}}, {"shuang", {'u', 'l'}}, {"zhuang", {'v', 'l'}}, {"chang", {'i', 'h'}},
	{"cheng", {'i', 'g'}}, {"chong", {'i', 's'}}, {"chuai", {'i', 'k'}}, {"chuan", {'i', 'r'}},
	{"guang", {'g', 'l'}}, {"huang", {'h', 'l'}}, {"jiang", {'j', 'l'}}, {"jiong", {'j', 's'}},
	{"kuang", {'k', 'l'}}, {"liang", {'l', 'l'}}, {"niang", {'n', 'l'}}, {"qiang", {'q', 'l'}},
	{"qiong", {'q', 's'}}, {"shang", {'u', 'h'}}, {"sheng", {'u', 'g'}}, {"shuai", {'u', 'k'}},
	{"shuan", {'u', 'r'}}, {"xiang", {'x', 'l'}}, {"xiong", {'x', 's'}}, {"zhang", {'v', 'h'}},
	{"zheng", {'v', 'g'}}, {"zhong", {'v', 's'}}, {"zhuai", {'v', 'k'}}, {"zhuan", {'v', 'r'}},
	{"bang", {'b', 'h'}}, {"beng", {'b', 'g'}}, {"bian", {'b', 'm'}}, {"biao", {'b', 'n'}},
	{"bing", {'b', 'k'}}, {"cang", {'c', 'h'}}, {"ceng", {'c', 'g'}}, {"chai", {'i', 'd'}},
	{"chan", {'i', 'j'}}, {"chao", {'i', 'c'}}, {"chen", {'i', 'f'}}, {"chou", {'i', 'z'}},
	{"chua", {'i', 'x'}}, {"chui", {'i', 'v'}}, {"chun", {'i', 'y'}}, {"chuo", {'i', 'o'}},
	{"cong", {'c', 's'}}, {"cuan", {'c', 'r'}}, {"dang", {'d', 'h'}}, {"deng", {'d', 'g'}},
	{"dian", {'d', 'm'}}, {"diao", {'d', 'n'}}, {"ding", {'d', 'k'}}, {"dong", {'d', 's'}},
	{"duan", {'d', 'r'}}, {"fang", {'f', 'h'}}, {"feng", {'f', 'g'}}, {"fiao", {'f', 'n'}},
	{"gang", {'g', 'h'}}, {"geng", {'g', 'g'}}, {"gong", {'g', 's'}}, {"guai", {'g', 'k'}},
	{"guan", {'g', 'r'}}, {"hang", {'h', 'h'}}, {"heng", {'h', 'g'}}, {"hong", {'h', 's'}},
	{"huai", {'h', 'k'}}, {"huan", {'h', 'r'}}, {"jian", {'j', 'm'}}, {"jiao", {'j', 'n'}},
	{"jing", {'j', 'k'}}, {"juan", {'j', 'r'}}, {"kang", {'k', 'h'}}, {"keng", {'k', 'g'}},
	{"kong", {'k', 's'}}, {"kuai", {'k', 'k'}}, {"kuan", {'k', 'r'}}, {"lang", {'l', 'h'}},
	{"leng", {'l', 'g'}}, {"lian", {'l', 'm'}}, {"liao", {'l', 'n'}}, {"ling", {'l', 'k'}},
	{"long", {'l', 's'}}, {"luan", {'l', 'r'}}, {"mang", {'m', 'h'}}, {"meng", {'m', 'g'}},
	{"mian", {'m', 'm'}}, {"miao", {'m', 'n'}}, {"ming", {'m', 'k'}}, {"nang", {'n', 'h'}},
	{"neng", {'n', 'g'}}, {"nian", {'n', 'm'}}, {"niao", {'n', 'n'}}, {"ning", {'n', 'k'}},
	{"nong", {'n', 's'}}, {"nuan", {'n', 'r'}}, {"pang", {'p', 'h'}}, {"peng", {'p', 'g'}},
	{"pian", {'p', 'm'}}, {"piao", {'p', 'n'}}, {"ping", {'p', 'k'}}, {"qian", {'q', 'm'}},
	{"qiao", {'q', 'n'}}, {"qing", {'q', 'k'}}, {"quan", {'q', 'r'}}, {"rang", {'r', 'h'}},
	{"reng", {'r', 'g'}}, {"rong", {'r', 's'}}, {"ruan", {'r', 'r'}}, {"sang", {'s', 'h'}},
	{"seng", {'s', 'g'}}, {"shai", {'u', 'd'}}, {"shan", {'u', 'j'}}, {"shao", {'u', 'c'}},
	{"shei", {'u', 'w'}}, {"shen", {'u', 'f'}}, {"shou", {'u', 'z'}}, {"shua", {'u', 'x'}},
	{"shui", {'u', 'v'}}, {"shun", {'u', 'y'}}, {"shuo", {'u', 'o'}}, {"song", {'s', 's'}},
	{"suan", {'s', 'r'}}, {"tang", {'t', 'h'}}, {"teng", {'t', 'g'}}, {"tian", {'t', 'm'}},
	{"tiao", {'t', 'n'}}, {"ting", {'t', 'k'}}, {"tong", {'t', 's'}}, {"tuan", {'t', 'r'}},
	{"wang", {'w', 'h'}}, {"weng", {'w', 'g'}}, {"xian", {'x', 'm'}}, {"xiao", {'x', 'n'}},
	{"xing", {'x', 'k'}}, {"xuan", {'x', 'r'}}, {"yang", {'y', 'h'}}, {"ying", {'y', 'k'}},
	{"yong", {'y', 's'}}, {"yuan", {'y', 'r'}}, {"zang", {'z', 'h'}}, {"zeng", {'z', 'g'}},
	{"zhai", {'v', 'd'}}, {"zhan", {'v', 'j'}}, {"zhao", {'v', 'c'}}, {"zhei", {'v', 'w'}},
	{"zhen", {'v', 'f'}}, {"zhou", {'v', 'z'}}, {"zhua", {'v', 'x'}}, {"zhui", {'v', 'v'}},
	{"zhun", {'v', 'y'}}, {"zhuo", {'v', 'o'}}, {"zong", {'z', 's'}}, {"zuan", {'z', 'r'}},
	{"ang", {'a', 'h'}}, {"bai", {'b', 'd'}}, {"ban", {'b', 'j'}}, {"bao", {'b', 'c'}},
	{"bei", {'b', 'w'}}, {"ben", {'b', 'f'}}, {"bie", {'b', 'p'}}, {"bin", {'b', 'b'}},
	{"cai", {'c', 'd'}}, {"can", {'c', 'j'}}, {"cao", {'c', 'c'}}, {"cen", {'c', 'f'}},
	{"cha", {'i', 'a'}}, {"che", {'i', 'e'}}, {"chi", {'i', 'i'}}, {"chu", {'i', 'u'}},
	{"cou", {'c', 'z'}}, {"cui", {'c', 'v'}}, {"cun", {'c', 'y'}}, {"cuo", {'c', 'o'}},
	{"dai", {'d', 'd'}}, {"dan", {'d', 'j'}}, {"dao", {'d', 'c'}}, {"dei", {'d', 'w'}},
	{"den", {'d', 'f'}}, {"dia", {'d', 'x'}}, {"die", {'d', 'p'}}, {"diu", {'d', 'q'}},
	{"dou", {'d', 'z'}}, {"dui", {'d', 'v'}}, {"dun", {'d', 'y'}}, {"duo", {'d', 'o'}},
	{"eng", {'e', 'g'}}, {"fan", {'f', 'j'}}, {"fei", {'f', 'w'}}, {"fen", {'f', 'f'}},
	{"fou", {'f', 'z'}}, {"gai", {'g', 'd'}}, {"gan", {'g', 'j'}}, {"gao", {'g', 'c'}},
	{"gei", {'g', 'w'}}, {"gen", {'g', 'f'}}, {"gou", {'g', 'z'}}, {"gua", {'g', 'x'}},
	{"gui", {'g', 'v'}}, {"gun", {'g', 'y'}}, {"guo", {'g', 'o'}}, {"hai", {'h', 'd'}},
	{"han", {'h', 'j'}}, {"hao", {'h', 'c'}}, {"hei", {'h', 'w'}}, {"hen", {'h', 'f'}},
	{"hou", {'h', 'z'}}, {"hua", {'h', 'x'}}, {"hui", {'h', 'v'}}, {"hun", {'h', 'y'}},
	{"huo", {'h', 'o'}}, {"jia", {'j', 'x'}}, {"jie", {'j', 'p'}}, {"jin", {'j', 'b'}},
	{"jiu", {'j', 'q'}}, {"jue", {'j', 't'}}, {"jun", {'j', 'y'}}, {"kai", {'k', 'd'}},
	{"kan", {'k', 'j'}}, {"kao", {'k', 'c'}}, {"kei", {'k', 'w'}}, {"ken", {'k', 'f'}},
	{"kou", {'k', 'z'}}, {"kua", {'k', 'x'}}, {"kui", {'k', 'v'}}, {"kun", {'k', 'y'}},
	{"kuo", {'k', 'o'}}, {"lai", {'l', 'd'}}, {"lan", {'l', 'j'}}, {"lao", {'l', 'c'}},
	{"lei", {'l', 'w'}}, {"lia", {'l', 'x'}}, {"lie", {'l', 'p'}}, {"lin", {'l', 'b'}},
	{"liu", {'l', 'q'}}, {"lou", {'l', 'z'}}, {"lue", {'l', 't'}}, {"lun", {'l', 'y'}},
	{"luo", {'l', 'o'}}, {"mai", {'m', 'd'}}, {"man", {'m', 'j'}}, {"mao", {'m', 'c'}},
	{"mei", {'m', 'w'}}, {"men", {'m', 'f'}}, {"mie", {'m', 'p'}}, {"min", {'m', 'b'}},
	{"miu", {'m', 'q'}}, {"mou", {'m', 'z'}}, {"nai", {'n', 'd'}}, {"nan", {'n', 'j'}},
	{"nao", {'n', 'c'}}, {"nei", {'n', 'w'}}, {"nen", {'n', 'f'}}, {"nie", {'n', 'p'}},
	{"nin", {'n', 'b'}}, {"niu", {'n', 'q'}}, {"nou", {'n', 'z'}}, {"nue", {'n', 't'}},
	{"nuo", {'n', 'o'}}, {"pai", {'p', 'd'}}, {"pan", {'p', 'j'}}, {"pao", {'p', 'c'}},
	{"pei", {'p', 'w'}}, {"pen", {'p', 'f'}}, {"pie", {'p', 'p'}}, {"pin", {'p', 'b'}},
	{"pou", {'p', 'z'}}, {"qia", {'q', 'x'}}, {"qie", {'q', 'p'}}, {"qin", {'q', 'b'}},
	{"qiu", {'q', 'q'}}, {"que", {'q', 't'}}, {"qun", {'q', 'y'}}, {"ran", {'r', 'j'}},
	{"rao", {'r', 'c'}}, {"ren", {'r', 'f'}}, {"rou", {'r', 'z'}}, {"rui", {'r', 'v'}},
	{"run", {'r', 'y'}}, {"ruo", {'r', 'o'}}, {"sai", {'s', 'd'}}, {"san", {'s', 'j'}},
	{"sao", {'s', 'c'}}, {"sen", {'s', 'f'}}, {"sha", {'u', 'a'}}, {"she", {'u', 'e'}},
	{"shi", {'u', 'i'}}, {"shu", {'u', 'u'}}, {"sou", {'s', 'z'}}, {"sui", {'s', 'v'}},
	{"sun", {'s', 'y'}}, {"suo", {'s', 'o'}}, {"tai", {'t', 'd'}}, {"tan", {'t', 'j'}},
	{"tao", {'t', 'c'}}, {"tei", {'t', 'w'}}, {"tie", {'t', 'p'}}, {"tou", {'t', 'z'}},
	{"tui", {'t', 'v'}}, {"tun", {'t', 'y'}}, {"tuo", {'t', 'o'}}, {"wai", {'w', 'd'}},
	{"wan", {'w', 'j'}}, {"wei", {'w', 'w'}}, {"wen", {'w', 'f'}}, {"xia", {'x', 'x'}},
	{"xie", {'x', 'p'}}, {"xin", {'x', 'b'}}, {"xiu", {'x', 'q'}}, {"xue", {'x', 't'}},
	{"xun", {'x', 'y'}}, {"yan", {'y', 'j'}}, {"yao", {'y', 'c'}}, {"yin", {'y', 'b'}},
	{"you", {'y', 'z'}}, {"yue", {'y', 't'}}, {"yun", {'y', 'y'}}, {"zai", {'z', 'd'}},
	{"zan", {'z', 'j'}}, {"zao", {'z', 'c'}}, {"zei", {'z', 'w'}}, {"zen", {'z', 'f'}},
	{"zha", {'v', 'a'}}, {"zhe", {'v', 'e'}}, {"zhi", {'v', 'i'}}, {"zhu", {'v', 'u'}},
	{"zou", {'z', 'z'}}, {"zui", {'z', 'v'}}, {"zun", {'z', 'y'}}, {"zuo", {'z', 'o'}},
	{"ai", {'a', 'i'}}, {"an", {'a', 'n'}}, {"ao", {'a', 'o'}}, {"ba", {'b', 'a'}},
	{"bi", {'b', 'i'}}, {"bo", {'b', 'o'}}, {"bu", {'b', 'u'}}, {"ca", {'c', 'a'}},
	{"ce", {'c', 'e'}}, {"ci", {'c', 'i'}}, {"cu", {'c', 'u'}}, {"da", {'d', 'a'}},
	{"de", {'d', 'e'}}, {"di", {'d', 'i'}}, {"du", {'d', 'u'}}, {"ei", {'e', 'i'}},
	{"en", {'e', 'n'}}, {"er", {'e', 'r'}}, {"fa", {'f', 'a'}}, {"fo", {'f', 'o'}},
	{"fu", {'f', 'u'}}, {"ga", {'g', 'a'}}, {"ge", {'g', 'e'}}, {"gu", {'g', 'u'}},
	{"ha", {'h', 'a'}}, {"he", {'h', 'e'}}, {"hu", {'h', 'u'}}, {"ji", {'j', 'i'}},
	{"ju", {'j', 'u'}}, {"ka", {'k', 'a'}}, {"ke", {'k', 'e'}}, {"ku", {'k', 'u'}},
	{"la", {'l', 'a'}}, {"le", {'l', 'e'}}, {"li", {'l', 'i'}}, {"lu", {'l', 'u'}},
	{"lv", {'l', 'v'}}, {"ma", {'m', 'a'}}, {"me", {'m', 'e'}}, {"mi", {'m', 'i'}},
	{"mo", {'m', 'o'}}, {"mu", {'m', 'u'}}, {"na", {'n', 'a'}}, {"ne", {'n', 'e'}},
	{"ni", {'n', 'i'}}, {"nu", {'n', 'u'}}, {"nv", {'n', 'v'}}, {"ou", {'o', 'u'}},
	{"pa", {'p', 'a'}}, {"pi", {'p', 'i'}}, {"po", {'p', 'o'}}, {"pu", {'p', 'u'}},
	{"qi", {'q', 'i'}}, {"qu", {'q', 'u'}}, {"re", {'r', 'e'}}, {"ri", {'r', 'i'}},
	{"ru", {'r', 'u'}}, {"sa", {'s', 'a'}}, {"se", {'s', 'e'}}, {"si", {'s', 'i'}},
	{"su", {'s', 'u'}}, {"ta", {'t', 'a'}}, {"te", {'t', 'e'}}, {"ti", {'t', 'i'}},
	{"tu", {'t', 'u'}}, {"wa", {'w', 'a'}}, {"wo", {'w', 'o'}}, {"wu", {'w', 'u'}},
	{"xi", {'x', 'i'}}, {"xu", {'x', 'u'}}, {"ya", {'y', 'a'}}, {"ye", {'y', 'e'}},
	{"yi", {'y', 'i'}}, {"yo", {'y', 'o'}}, {"yu", {'y', 'u'}}, {"za", {'z', 'a'}},
	{"ze", {'z', 'e'}}, {"zi", {'z', 'i'}}, {"zu", {'z', 'u'}}, {"a", {'a', 'a'}},
	{"e", {'e', 'e'}}, {"o", {'o', 'o'}},
};
#define NUM_SYL_CODES ((int)(sizeof(syl_codes) / sizeof(syl_codes[0])))

/* ---- full pinyin: syllable -> Xiaohe code ---------------------------- */

static void refresh(void);

static void build_code(void) {
	int i = 0, c = 0;

	code[0] = '\0';
	code_len = 0;
	code_in[0] = 0;
	if (!full) {
		for (i = 0; i < blen; ++i) {
			code[i] = buf[i];
			code_in[i + 1] = i + 1;
		}
		code[blen] = '\0';
		code_len = blen;
		return;
	}
	while (i < blen && c + 2 < (int)sizeof(code)) {
		int k, chosen = -1, fallback = -1;
		for (k = 0; k < NUM_SYL_CODES; ++k) {
			int L = (int)strlen(syl_codes[k].syl);
			if (i + L > blen) {
				continue;
			}
			if (memcmp(buf + i, syl_codes[k].syl, (size_t)L) != 0) {
				continue;
			}
			if (fallback < 0) {
				fallback = k;   /* longest match so far (table is longest first) */
			}
			if (nlines > 0) {
				char key[3];
				key[0] = syl_codes[k].code[0];
				key[1] = syl_codes[k].code[1];
				key[2] = '\0';
				if (lookup(key, 2) != NULL) {
					chosen = k;
					break;
				}
			}
		}
		if (chosen < 0) {
			chosen = fallback;
		}
		if (chosen < 0) {
			break;              /* not a syllable we know: keep it as tail */
		}
		code[c++] = syl_codes[chosen].code[0];
		code[c++] = syl_codes[chosen].code[1];
		i += (int)strlen(syl_codes[chosen].syl);
		code_in[c] = i;
	}
	code[c] = '\0';
	code_len = c;
}

void pinyin_set_full(int on) {
	full = on ? 1 : 0;
	blen = 0;
	buf[0] = '\0';
	refresh();
}

int pinyin_full(void) {
	return full;
}

/* ---- segmentation ---------------------------------------------------- */

/* best total score to cover exactly i letters; -1 = unreachable */
static int  dp_score[MAX_BUF + 2];
static int  dp_len[MAX_BUF + 2];      /* letters of the last chunk */
static int  dp_rank[MAX_BUF + 2];     /* word index inside that chunk */

static void segment(void) {
	int i, L;
	for (i = 0; i <= code_len; ++i) {
		dp_score[i] = -1;
		dp_len[i] = 0;
		dp_rank[i] = -1;
	}
	dp_score[0] = 0;
	for (i = 2; i <= code_len; i += 2) {
		for (L = 2; L <= MAX_PHRASE && L <= i; L += 2) {
			const char *words;
			int s, len;
			if ((len = i - L) < 0 || dp_score[len] < 0) {
				continue;
			}
			words = lookup(code + len, L);
			if (words == NULL) {
				continue;
			}
			/* CHUNK_COST > the best possible word score, so a split
			 * into fewer words always beats one into more words */
			s = dp_score[len] + word_score(words, 0, L) - CHUNK_COST;
			if (s > dp_score[i]) {
				dp_score[i] = s;
				dp_len[i] = L;
				dp_rank[i] = 0;
			}
		}
	}
}

/* text of the best segmentation up to position end (exclusive) */
static int segment_text(int end, char *dst, size_t cap, int *last_len, int *last_rank) {
	int pos = end;
	char parts[MAX_WORDS][48];
	int lens[MAX_WORDS];
	int n = 0;

	dst[0] = '\0';
	*last_len = 0;
	*last_rank = -1;
	if (end <= 0 || dp_score[end] < 0) {
		return 0;
	}
	while (pos > 0 && n < MAX_WORDS && dp_len[pos] > 0) {
		int L = dp_len[pos];
		const char *words = lookup(code + (pos - L), L);
		lens[n] = L;
		parts[n][0] = '\0';
		append_word(parts[n], sizeof(parts[n]), words, 0);
		++n;
		pos -= L;
	}
	/* collected backwards: emit in reading order */
	{
		int i;
		for (i = n - 1; i >= 0; --i) {
			size_t len = strlen(dst);
			if (len + strlen(parts[i]) >= cap) {
				break;
			}
			strcat(dst, parts[i]);
			if (i == 0) {
				*last_len = lens[i];
			}
		}
	}
	return n > 0;
}

/* ---- candidate list --------------------------------------------------- */

static void refresh(void) {
	const char *words;
	int len;
	char text[MAX_BUF * 4];
	int end;

	ncands = 0;
	matched_len = 0;
	page = 0;
	build_code();
	if (code_len == 0 || nlines == 0) {
		return;
	}

	/* 1. the whole buffer as one key: 再/再见/中文输入法 style entries */
	if ((code_len & 1) == 0 && code_len <= MAX_PHRASE) {
		words = lookup(code, code_len);
		if (words != NULL) {
			int n = 0;
			matched_len = code_in[code_len];
			while (n < MAX_CANDS) {
				text[0] = '\0';
				append_word(text, sizeof(text), words, n);
				if (text[0] == '\0') {
					break;
				}
				add_cand(text, code_in[code_len], word_score(words, n, code_len));
				++n;
			}
			return;
		}
	}

	/* 2. syllable-wise segmentation: phrase from the dictionary */
	segment();
	end = code_len & ~1;
	while (end > 0 && dp_score[end] < 0) {
		end -= 2;
	}
	if (end > 0) {
		char prefix[MAX_BUF * 4];
		int last_len = 0, last_rank = 0;
		matched_len = code_in[end];
		if (segment_text(end, prefix, sizeof(prefix), &last_len, &last_rank) &&
		    last_len > 0) {
			int n;
			/* the best segmentation first, regardless of sorting */
			add_cand(prefix, code_in[end], 100000);
			/* other words for its last chunk, same prefix */
			{
				char base[MAX_BUF * 4];
				int base_end = end - last_len;
				if (base_end < 0) {
					base_end = 0;
				}
				base[0] = '\0';
				segment_text(base_end, base, sizeof(base), &last_len, &last_rank);
				words = lookup(code + base_end, dp_len[end]);
				for (n = 0; n < MAX_CANDS && words != NULL; ++n) {
					char alt[MAX_BUF * 4];
					snprintf(alt, sizeof(alt), "%s", base);
					append_word(alt, sizeof(alt), words, n);
					if (strlen(alt) == strlen(base)) {
						break;
					}
					add_cand(alt, code_in[end],
					         word_score(words, n, dp_len[end]) + dp_score[base_end]);
				}
			}
			return;
		}
	}

	/* 3. longest whole-syllable prefix, as the previous engine did */
	for (len = code_len & ~1; len > 0 && ncands == 0; len -= 2) {
		words = lookup(code, len);
		if (words == NULL) {
			continue;
		}
		matched_len = code_in[len];
		{
			int n;
			for (n = 0; n < MAX_CANDS; ++n) {
				text[0] = '\0';
				append_word(text, sizeof(text), words, n);
				if (text[0] == '\0') {
					break;
				}
				add_cand(text, code_in[len], word_score(words, n, len));
			}
		}
	}
}

static void sort_cands(void) {
	int i, j;
	for (i = 1; i < ncands; ++i) {
		const char *t = cands[i];
		int u = cand_used[i], s = cand_score[i];
		for (j = i - 1; j >= 0 && cand_score[j] < s; --j) {
			cands[j + 1] = cands[j];
			cand_used[j + 1] = cand_used[j];
			cand_score[j + 1] = cand_score[j];
		}
		cands[j + 1] = t;
		cand_used[j + 1] = u;
		cand_score[j + 1] = s;
	}
}

/* ---- public API ------------------------------------------------------- */

/* drop any half-typed composition (used when the visible session changes,
 * so letters cannot leak into another shell) */
void pinyin_reset(void) {
	blen = 0;
	buf[0] = '\0';
	refresh();
}

void pinyin_toggle(void) {
	enabled = !enabled;
	if (enabled) {
		const char *f = getenv("TERM49_PINYIN_FULL");
		if (f != NULL && strcmp(f, "0") != 0) {
			full = 1;
		}
		pinyin_load();
	}
	blen = 0;
	buf[0] = '\0';
	refresh();
}

int pinyin_enabled(void) {
	return enabled;
}

int pinyin_composing(void) {
	return enabled && blen > 0;
}

static void pick(int idx, pinyin_commit_fn commit) {
	int rest;
	if (idx < 0 || idx >= ncands) {
		return;
	}
	commit(cands[idx], strlen(cands[idx]));
	user_bump(cands[idx]);
	/* keep the letters the chosen word did not cover */
	rest = blen - cand_used[idx];
	if (rest > 0) {
		memmove(buf, buf + cand_used[idx], (size_t)rest);
	}
	blen = rest > 0 ? rest : 0;
	buf[blen] = '\0';
	refresh();
}

/* comma/period/etc. -> full-width punctuation, only when it is the first
 * thing typed (nothing composing) and TERM49_PINYIN_PUNCT=1 is set */
static int punctuation(int ch, pinyin_commit_fn commit) {
	const char *e = getenv("TERM49_PINYIN_PUNCT");
	const char *p = NULL;
	if (e == NULL || strcmp(e, "0") == 0) {
		return 0;
	}
	switch (ch) {
	case ',': p = "，"; break;
	case '.': p = "。"; break;
	case '?': p = "？"; break;
	case '!': p = "！"; break;
	case ':': p = "："; break;
	case ';': p = "；"; break;
	case '(': p = "（"; break;
	case ')': p = "）"; break;
	default: break;
	}
	if (p == NULL) {
		return 0;
	}
	commit(p, strlen(p));
	return 1;
}

int pinyin_key(int ch, pinyin_commit_fn commit) {
	if (!enabled) {
		return 0;
	}
	if (ch >= 'A' && ch <= 'Z') {
		ch += 'a' - 'A';
	}
	if (ch >= 'a' && ch <= 'z') {
		if (blen < MAX_BUF) {
			buf[blen++] = (char)ch;
			buf[blen] = '\0';
			refresh();
		}
		return 1;
	}
	if (blen == 0) {
		/* nothing composing: punctuation mapping (opt-in), else let the
		 * terminal have the key */
		if (punctuation(ch, commit)) {
			return 1;
		}
		return 0;
	}
	switch (ch) {
	case ' ':
		if (ncands > 0) {
			pick(page * PAGE, commit);
		} else {
			commit(buf, (size_t)blen);
			blen = 0;
			buf[0] = '\0';
			refresh();
		}
		return 1;
	case '\r':
	case '\n':
		commit(buf, (size_t)blen);
		blen = 0;
		buf[0] = '\0';
		refresh();
		return 1;
	case 0x08:
	case 0x7f:
		--blen;
		buf[blen] = '\0';
		refresh();
		return 1;
	case 0x1b:
		blen = 0;
		buf[0] = '\0';
		refresh();
		return 1;
	case ',':
	case '-':
		if (page > 0) {
			--page;
		}
		return 1;
	case '.':
	case '=':
		if ((page + 1) * PAGE < ncands) {
			++page;
		}
		return 1;
	default:
		if (ch >= '1' && ch <= '9') {
			pick(page * PAGE + (ch - '1'), commit);
			return 1;
		}
		/* any other key: commit the first candidate, then let the key through */
		if (ncands > 0) {
			pick(page * PAGE, commit);
		}
		return 0;
	}
}

/* "[鹤] ni hc | 1.你好 2.拟好 >" for the bottom bar */
void pinyin_status(char *out, size_t cap) {
	size_t o = 0;
	int i;

	if (cap == 0) {
		return;
	}
	out[0] = '\0';
	if (!enabled) {
		return;
	}
	if (blen == 0) {
		snprintf(out, cap, full ? "[全]" : "[鹤]");
		return;
	}
	o += (size_t)snprintf(out + o, cap - o, full ? "[全]" : "[鹤]");
	if (full) {
		/* raw letters; a gap marks where the candidates stop matching */
		o += (size_t)snprintf(out + o, cap - o, " %.*s", matched_len, buf);
		if (matched_len < blen && o + 4 < cap) {
			o += (size_t)snprintf(out + o, cap - o, " | %s", buf + matched_len);
		}
	} else {
		for (i = 0; i < blen && o + 4 < cap; i += 2) {
			/* syllables in pairs; a gap marks where the candidates stop matching */
			o += (size_t)snprintf(out + o, cap - o, "%s%.*s",
			                      (i == matched_len && i > 0) ? " | " : " ",
			                      (i + 1 < blen) ? 2 : 1, buf + i);
		}
	}
	sort_cands();
	for (i = 0; i < PAGE && page * PAGE + i < ncands && o + 8 < cap; ++i) {
		o += (size_t)snprintf(out + o, cap - o, " %d.%s", i + 1, cands[page * PAGE + i]);
	}
	if ((page + 1) * PAGE < ncands && o + 4 < cap) {
		snprintf(out + o, cap - o, " >");
	}
}
