/*
 * Copyright (c) 2013 Todd Mortimer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ioctl.h>
#include <unix.h>
#include <termios.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <sys/select.h>

#include <bps/screen.h>
#include <bps/navigator.h>
#include <bps/virtualkeyboard.h>
#include <bps/deviceinfo.h>
#include <unicode/utf.h>
#include <unicode/ustring.h>

#include "SDL.h"
#include "SDL_ttf.h"
#include "SDL_syswm.h"
#include "SDL_thread.h"

#include "types.h"
#include "terminal.h"
#include "ecma48.h"
#include "preferences.h"
#include "pinyin.h"
#include "buffer.h"
#include "io.h"
#include "colors.h"

#include "session.h"
#include "sessionui.h"

static int exit_application = 0;

void indicate_event_input();

/* top edge of the terminal grid in pixels: the session tab bar owns the
 * strip above it (0 when the bar is hidden) */
int content_top = 0;

static int cursor_x = 0;
static int cursor_y = 0;
char draw_cursor = 1;
char flash = 0;

static pref_t *prefs = NULL;
static symmenu_t *current_symmenu = NULL;

static char symmenu_lock = 0;
static char altsym_lock = 0;   /* one-shot: consumed by the next key */
static char altsym_hold = 0;   /* persistent alt mode (menu key) */

static char metamode = 0;
/* terminal rows reserved at the bottom for the pinyin input bar */
static int pinyin_bar_rows = 0;
static int metamode_doubletap_key = 0;
static struct timespec metamode_last;
static SDL_Color metamode_cursor_fg = SDL_BLACK;
static SDL_Color metamode_cursor_bg = SDL_GREEN;
static SDL_Surface* metamode_cursor;
static int vmodifiers = 0;

static TTF_Font* font;
static TTF_Font* fallback_font;
/* Symbols Nerd Font Mono: private-use glyphs (powerline, devicons, ...) that
 * neither the terminal font nor the Cascadia fallback carries. */
static TTF_Font* nerd_font;
/* Chain of CJK faces tried in order for glyphs the fonts above lack. No single
 * BB10 face covers all of CJK: the Simplified Hei (GB18030) has Han but not
 * Japanese kana or Hangul, so extra faces are chained (MSung/cp950 for kana,
 * malgun for Hangul) - matching bb10-remote's resolved fallback order. */
#define MAX_CJK_FONTS 4
static TTF_Font* cjk_fonts[MAX_CJK_FONTS];
static int num_cjk_fonts;
static int text_width;
static int text_height;
static int text_height_padding;
static int advance;
static int default_text_color_arr[PREFS_COLOR_NUM_ELEMENTS];
static int default_bg_color_arr[PREFS_COLOR_NUM_ELEMENTS];
static SDL_Color default_text_color = SDL_WHITE;
static SDL_Color default_bg_color = SDL_BLACK;
struct font_style default_text_style;

struct screenchar blank_sc;
static SDL_Surface* flash_surface;
static SDL_Surface* cursor;
static SDL_Surface* inv_cursor;
static SDL_Surface* screen;
static SDL_Surface* ctrl_key_indicator;
static SDL_Surface* alt_key_indicator;
static SDL_Surface* shift_key_indicator;
static SDL_Surface* altsym_indicator;
static SDL_Surface* scroll_indicator;
SDL_Surface* blank_surface;

static char virtualkeyboard_visible = 0;
static char key_repeat_done = 0;

static SDL_mutex *input_mutex = NULL;

static int event_pipe[2];

/* leftover touch-drag pixels not yet turned into scrolled lines */
static int touch_scroll_acc = 0;
/* touch bookkeeping for tap-to-open-URL and bar swipes */
static int touch_down_ticks = 0;
static int touch_down_x = 0, touch_down_y = 0;

/* a touch drag is currently extending a text selection */
static char touch_selecting = 0;

/* mouse pointer for xterm mouse-tracking apps, driven by the trackpad */
static SDL_Surface* mouse_pointer;
static int pointer_x = 0, pointer_y = 0; /* pixels */
static char pointer_visible = 0;
/* last cell reported for a touch contact, col/row; -1 = finger up */
static int touch_mouse_cell[2] = {-1, -1};

/* from buffer.c */
extern int rows;
extern int cols;
extern buf_t* buf;
extern int MAX_COLS;
extern int MAX_ROWS;
extern int TEXT_BUFFER_SIZE;
extern struct scroll_region sr;

#define PB_D_PIXELS 32

int is_terminfo_keystrokes(const char* keystrokes){
	if(keystrokes[0] == 'k'){
		if(0 == strncmp(keystrokes, "kcub1", 5)){ return KEYCODE_LEFT; }
		if(0 == strncmp(keystrokes, "kcud1", 5)){ return KEYCODE_DOWN; }
		if(0 == strncmp(keystrokes, "kcuf1", 5)){ return KEYCODE_RIGHT; }
		if(0 == strncmp(keystrokes, "kcuu1", 5)){ return KEYCODE_UP; }
		if(0 == strncmp(keystrokes, "khome", 5)){ return KEYCODE_HOME; }
		if(0 == strncmp(keystrokes, "kend", 4)){ return KEYCODE_END; }
		if(0 == strncmp(keystrokes, "kent", 4)){ return KEYCODE_RETURN; }
		if(0 == strncmp(keystrokes, "kf1", 3)){ return KEYCODE_F1; }
		if(0 == strncmp(keystrokes, "kf2", 3)){ return KEYCODE_F2; }
		if(0 == strncmp(keystrokes, "kf3", 3)){ return KEYCODE_F3; }
		if(0 == strncmp(keystrokes, "kf4", 3)){ return KEYCODE_F4; }
		if(0 == strncmp(keystrokes, "kf5", 3)){ return KEYCODE_F5; }
		if(0 == strncmp(keystrokes, "kf6", 3)){ return KEYCODE_F6; }
		if(0 == strncmp(keystrokes, "kf7", 3)){ return KEYCODE_F7; }
		if(0 == strncmp(keystrokes, "kf8", 3)){ return KEYCODE_F8; }
		if(0 == strncmp(keystrokes, "kf9", 3)){ return KEYCODE_F9; }
		if(0 == strncmp(keystrokes, "kf10", 4)){ return KEYCODE_F10; }
		if(0 == strncmp(keystrokes, "kf11", 4)){ return KEYCODE_F11; }
		if(0 == strncmp(keystrokes, "kf12", 4)){ return KEYCODE_F12; }
	}
	return 0;
}

int send_metamode_keystrokes(const char* keystrokes){

	UChar* ukeystrokes;
	size_t ukeystrokes_len;
	size_t keystrokes_len;
	int terminfo_key = 0;
	UChar terminfo_keystrokes[CHARACTER_BUFFER];

	if(keystrokes){
		/* sending input snaps the view back to the live screen */
		buf_reset_view();
		terminfo_key = is_terminfo_keystrokes(keystrokes);
		/* if the keystrokes for this key match a terminfo pattern,
		 * send the appropriate sequence instead of the literal string */
		if(terminfo_key){
			ukeystrokes_len = ecma48_parse_control_codes(terminfo_key, 0, terminfo_keystrokes);
			/* and write out to the tty whatever the keys were */
			io_write_master(terminfo_keystrokes, ukeystrokes_len);
			return 1;
		}
		// else
		keystrokes_len = strlen(keystrokes);
		/* libconfig will return ascii strings, but we can put utf8 in there too */
		ukeystrokes = (UChar*)calloc(keystrokes_len, sizeof(UChar));
		ukeystrokes_len = io_read_utf8_string(keystrokes, keystrokes_len, ukeystrokes);
		/* and write out to the tty whatever the keys were */
		io_write_master(ukeystrokes, ukeystrokes_len);
		free(ukeystrokes);
		return 1;
	}
	/* no keystrokes saved for this key */
	return 0;
}

/* honor an active shift on keys bound to Tab: send back-tab (CSI Z)
 * instead, consuming the sticky shift if it was armed */
static const char* shifted_keystrokes(const char* keys, int shifted){
	if(shifted && keys != NULL && keys[0] == '\t' && keys[1] == '\0'){
		vmodifiers &= ~KEYMOD_SHIFT;
		return "\x1b[Z";
	}
	return keys;
}

int get_virtualkeyboard_height(){
	int rc, vkb_h;
	rc = virtualkeyboard_get_height(&vkb_h);
	if(rc != BPS_SUCCESS){
		fprintf(stderr, "Could not get virtual keyboard height\n");
		vkb_h = 0; // assume zero?
	}
	return vkb_h;
}

int is_passport() {
	deviceinfo_details_t *di_t = NULL;
	int rc = deviceinfo_get_details(&di_t);
	if(rc != BPS_SUCCESS){
		fprintf(stderr, "Could not get device info");
		return 0;
	}
	
	int passport = 0;
	if(strncmp("Passport", deviceinfo_details_get_model_name(di_t), 8) == 0){
		passport = 1;
	}
	deviceinfo_free_details(&di_t);

	return passport;
}

int get_wm_info(SDL_SysWMinfo* info){
	SDL_version version;
	SDL_VERSION(&version);
	info->version = version;
	return SDL_GetWMInfo(info);
}

void metamode_toggle(){
	metamode = metamode ? 0 : 1;
}

void altsym_toggle() {
	altsym_lock = altsym_lock ? 0 : 1;
}

void altsym_hold_toggle() {
	altsym_hold = altsym_hold ? 0 : 1;
}

/* ---- Menu-key action popup ------------------------------------------
 * The Menu (BlackBerry) key opens a small action grid: paste, toggle the
 * pinyin bar, toggle the virtual keyboard, clear the screen, rescreen.
 * Actions that change the screen layout have to run *after* the popup is
 * dismissed (closing it resizes the screen), hence menu_pending_*.
 * TERM49_MENU_ALT=1 restores the old behaviour (Menu = sticky Alt).
 */
static int menu_pending_vkb = 0;
static int menu_pending_clear = 0;
static int menu_pending_bar = 0;
static void pinyin_set_bar(int on);

static void menu_log(const char *what) {
	const char *home = getenv("HOME");
	char path[512];
	FILE *fp;

	if (home == NULL) {
		return;
	}
	snprintf(path, sizeof(path), "%s/.term49_keys.log", home);
	fp = fopen(path, "a");
	if (fp == NULL) {
		return;
	}
	fprintf(fp, "action=%s vkb_visible=%d\n", what, (int)virtualkeyboard_visible);
	fclose(fp);
}

/* Dump the visible grid: one line per row, each cell as its code point plus
 * the wide/combining flags. This is the ground truth for rendering bugs -
 * it says whether the program sent garbage or the renderer drew it wrong. */
static void dump_grid(void) {
	char path[512];
	const char *home = getenv("HOME");
	FILE *fp;
	int r, c;

	if (home == NULL) { return; }
	snprintf(path, sizeof(path), "%s/grid.txt", home);
	fp = fopen(path, "w");
	if (fp == NULL) { return; }
	fprintf(fp, "rows=%d cols=%d line=%d top=%d view=%d\n",
	        rows, cols, buf->line, buf->top_line, buf->view_offset);
	for (r = 0; r < rows; ++r) {
		int line = buf->top_line + r;
		fprintf(fp, "%03d|", r);
		for (c = 0; c < cols; ++c) {
			struct screenchar *sc;
			if (line < 0 || line >= TEXT_BUFFER_SIZE) { fputs(" ....", fp); continue; }
			sc = &buf->text[line][c];
			fprintf(fp, " %04X%s", (unsigned)sc->c,
			        sc->wide == 2 ? "w" : (sc->wide ? "W" : (sc->c ? " " : "_")));
			if (sc->c2) { fprintf(fp, "+%04X", (unsigned)sc->c2); }
			else { fputs("    ", fp); }
		}
		fputc('\n', fp);
	}
	fclose(fp);
}

/* Save the frame for off-device inspection (Menu -> shot) */
int screenshot_save(SDL_Surface *s, const char *path);

static void take_screenshot(void) {
	char path[512];
	const char *home = getenv("HOME");
	if (home == NULL) { return; }
	snprintf(path, sizeof(path), "%s/shot.bmp", home);
	if (screenshot_save(screen, path) == 0) {
		menu_log("screenshot saved");
	} else {
		menu_log("screenshot failed");
	}
}


/* Toggle the system input language (pinyin <-> english) by rewriting the
 * input service's options object.  The App cannot write /pps/services/input
 * itself, but the rooted OS ships /base/bin/__input_service (setuid
 * input_service) which runs a shell script fed on stdin.
 */
static void switch_input_language(void) {
	const char *home = getenv("HOME");
	char dir[512], sh_path[512], state_path[512];
	char line[64], cmd[1200];
	FILE *fp;
	int currently_pinyin = 0;

	if (home == NULL) {
		return;
	}
	snprintf(dir, sizeof(dir), "%s/tmp", home);
	mkdir(dir, 0755);
	snprintf(state_path, sizeof(state_path), "%s/term49_lang.state", dir);
	snprintf(sh_path, sizeof(sh_path), "%s/term49_lang.sh", dir);

	/* our own record of the last language we applied */
	fp = fopen(state_path, "r");
	if (fp != NULL) {
		if (fgets(line, sizeof(line), fp) != NULL && strstr(line, "pinyin") != NULL) {
			currently_pinyin = 1;
		}
		fclose(fp);
	}

	/* the payload is embedded in the script itself: the setuid wrapper runs
	 * as input_service, which cannot read files under our app data dir */
	fp = fopen(sh_path, "w");
	if (fp == NULL) {
		return;
	}
	if (currently_pinyin) {
		fprintf(fp, "cat > /pps/services/input/options <<'PPS'\n");
		fprintf(fp, "@options\n");
		fprintf(fp, "languages.default_lang_layout:json:{\"layout_id\":\"QWERTY\",\"layout_text\":\"QWERTY\",\"locale\":\"en_US\"}\n");
		fprintf(fp, "languages.active_lang_layouts:json:{\"layout_id\":\"qwerty\",\"locales\":[\"en_US\"]}\n");
		fprintf(fp, "layout_configurations:json:[{\"layout_id\":\"qwerty\",\"locale\":\"en_US\"},{\"layout_id\":\"pinyin_qwerty\",\"locale\":\"zh_CN_Pinyin\"}]\n");
		fprintf(fp, "PPS\n");
	} else {
		fprintf(fp, "cat > /pps/services/input/options <<'PPS'\n");
		fprintf(fp, "@options\n");
		fprintf(fp, "languages.additional_lang_layouts:json:[{\"layout_id\":\"拼音\",\"layout_text\":\"拼音\",\"locale\":\"zh_CN_Pinyin\"}]\n");
		fprintf(fp, "languages.default_lang_layout:json:{\"layout_id\":\"拼音\",\"layout_text\":\"拼音\",\"locale\":\"zh_CN_Pinyin\"}\n");
		fprintf(fp, "languages.active_lang_layouts:json:{\"layout_id\":\"pinyin_qwerty\",\"locales\":[\"zh_CN_Pinyin\"]}\n");
		fprintf(fp, "layout_configurations:json:[{\"layout_id\":\"pinyin_qwerty\",\"locale\":\"zh_CN_Pinyin\"},{\"layout_id\":\"qwerty\",\"locale\":\"en_US\"}]\n");
		fprintf(fp, "PPS\n");
	}
	fclose(fp);

	snprintf(cmd, sizeof(cmd), "/bin/sh -c '/base/bin/__input_service < %s'", sh_path);
	system(cmd);

	fp = fopen(state_path, "w");
	if (fp != NULL) {
		fprintf(fp, currently_pinyin ? "en\n" : "pinyin\n");
		fclose(fp);
	}
	menu_log(currently_pinyin ? "lang->en" : "lang->pinyin");
}

/* Menu-key popup actions. Returns 1 when the entry was an action (and was
 * executed or queued), 0 when it is an ordinary keystroke sequence. */
static int handle_menu_action(const char *keys) {
	if (keys == NULL) {
		return 0;
	}
	if (0 == strncmp(keys, "paste_clipboard", 15)) {
		menu_log("paste_clipboard");
		io_paste_from_clipboard();
		return 1;
	}
	if (0 == strncmp(keys, "rescreen", 8)) {
		rescreen(-1, -1);
		return 1;
	}
	if (0 == strncmp(keys, "menu_close", 10)) {
		return 1; /* closing happens in the caller */
	}
	if (0 == strncmp(keys, "screenshot", 10)) {
		take_screenshot();
		return 1;
	}
	if (0 == strncmp(keys, "dump_grid", 9)) {
		dump_grid();
		menu_log("grid dumped");
		return 1;
	}
	if (0 == strncmp(keys, "pinyin_full", 11)) {
		pinyin_set_full(!pinyin_full());
		menu_log(pinyin_full() ? "pinyin full" : "pinyin xiaohe");
		return 1;
	}
	if (0 == strncmp(keys, "pinyin_toggle", 13)) {
		pinyin_toggle();
		menu_log(pinyin_enabled() ? "pinyin on" : "pinyin off");
		menu_pending_bar = 1;
		return 1;
	}
	if (0 == strncmp(keys, "lang_toggle", 11)) {
		switch_input_language();
		return 1;
	}
	if (0 == strncmp(keys, "vkb_toggle", 10)) {
		menu_log("vkb_toggle(queued)");
		menu_pending_vkb = 1;
		return 1;
	}
	if (0 == strncmp(keys, "clear_screen", 12)) {
		menu_log("clear_screen(queued)");
		menu_pending_clear = 1;
		return 1;
	}
	/* session actions, so they can sit in the same menu */
	if (0 == strncmp(keys, "session_new_zsh", 15))  { session_create_ex(1, SHELL_ZSH); return 1; }
	if (0 == strncmp(keys, "session_new_utf8", 16)) { session_create_ex(1, SHELL_MKSH_UTF8); return 1; }
	if (0 == strncmp(keys, "session_new", 11))       { sessionui_new_session(); return 1; }
	if (0 == strncmp(keys, "session_next", 12))      { session_next(); return 1; }
	if (0 == strncmp(keys, "session_prev", 12))      { session_prev(); return 1; }
	if (0 == strncmp(keys, "session_close", 13))     { sessionui_close_active(); return 1; }
	if (0 == strncmp(keys, "session_rename", 14))    { sessionui_start_rename(); return 1; }
	if (0 == strncmp(keys, "session_search", 14))    { sessionui_start_search(); return 1; }
	if (0 == strncmp(keys, "session_menu", 12))      { sessionui_open_switcher(); return 1; }
	if (0 == strncmp(keys, "session_bar", 11))       { sessionui_toggle_bar(); return 1; }
	return 0;
}

void symmenu_stick(){
	PRINT(stderr, "Sticking Sym key\n");
	symmenu_lock = 1;
}

void symmenu_toggle(symmenu_t *target){
	if (current_symmenu == NULL){
		current_symmenu = target;
		// resize to show menu
		if (prefs->rescreen_for_symmenu) {
			setup_screen_size(screen->w, screen->h - current_symmenu->surface->h);
		}
		if (prefs->sticky_sym_key) {
			symmenu_stick();
		}
	} else {
		current_symmenu = NULL;
		if (prefs->rescreen_for_symmenu) {
			// resize to take full screen
			setup_screen_size(screen->w, screen->h);
		}
		symmenu_lock = 0;
		/* deferred menu actions: run now that the screen is back to full size */
		if (menu_pending_vkb) {
			menu_pending_vkb = 0;
			if (virtualkeyboard_visible) {
				virtualkeyboard_hide();
			} else {
				virtualkeyboard_show();
			}
		}
		if (menu_pending_bar) {
			menu_pending_bar = 0;
			pinyin_set_bar(pinyin_enabled());
		}
		if (menu_pending_clear) {
			menu_pending_clear = 0;
			io_write_master_char("\033[2J\033[H", 7);
			buf_reset_view();
		}
	}
}

static const char* symkey_for_mousedown(symmenu_t *menu, Uint16 x, Uint16 y) {
	for (int row = 0; menu->keys[row] != NULL; ++row) {
		for (int col = 0; menu->keys[row][col].map != NULL; ++col) {
			symkey_t *key = &menu->keys[row][col];

			if((x >= key->hitbox.x) &&
			   (x <= key->hitbox.x + key->hitbox.w) &&
			   (y >= key->hitbox.y) &&
			   (y <= key->hitbox.y + key->hitbox.h)) {
				if (!symmenu_lock) {
					symmenu_toggle(NULL);
				} else {
					key->flash = 1;
				}
				return key->map->to;
			}
		}
	}
	
	return NULL;
}

int font_init(int font_size){
	if(font_size < MIN_FONT_SIZE){
		fprintf(stderr, "Refusing to set font size to %d - too small\n",font_size);
		int default_font_columns = (atoi(getenv("WIDTH")) <= 720) ? 45 : 60;
		font_size = preferences_guess_best_font_size(prefs, default_font_columns);
	}

	/* Load the font */
	font = TTF_OpenFont(prefs->font_path, font_size);
	if ( font == NULL ) {
		/* try opening the default stuff */
		fprintf(stderr, "Couldn't load %d pt font from %s: %s\n", font_size, prefs->font_path, SDL_GetError());
		font = TTF_OpenFont(DEFAULT_FONT_PATH, DEFAULT_FONT_SIZE);
		if(font == NULL){
			fprintf(stderr, "Could not open default font %s: %s\n", DEFAULT_FONT_PATH, SDL_GetError());
			return TERM_FAILURE;
		}
	}
	PRINT(stderr, "Font is Fixed Width: %d\n", TTF_FontFaceIsFixedWidth(font));

	/* Set default options */
	TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
	TTF_SetFontOutline(font, 0);
	TTF_SetFontKerning(font, 0);
	TTF_SetFontHinting(font, TTF_HINTING_NORMAL);

	/* get default colour settings from prefs struct*/
	default_text_color.r = (Uint8)prefs->text_color[0];
	default_text_color.g = (Uint8)prefs->text_color[1];
	default_text_color.b = (Uint8)prefs->text_color[2];
	default_text_color.unused = 0;
	
	default_bg_color.r = (Uint8)prefs->background_color[0];
	default_bg_color.g = (Uint8)prefs->background_color[1];
	default_bg_color.b = (Uint8)prefs->background_color[2];
	default_bg_color.unused = 0;

	default_text_style.fg_color = default_text_color;
	default_text_style.bg_color = default_bg_color;
	default_text_style.style = TTF_STYLE_NORMAL;
	default_text_style.reverse = 0;

	/* initialize special characters */
	UChar str[2] = {' ', NULL};
	blank_surface = TTF_RenderUNICODE_Shaded(font, str, default_text_color, default_bg_color);
	if (blank_surface == NULL){
		PRINT(stderr, "Couldn't render blank surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	blank_sc.c = ' ';
	blank_sc.style = default_text_style;
	blank_sc.surface = blank_surface;
	flash_surface = TTF_RenderUNICODE_Shaded(font, str, default_bg_color, default_text_color);
	if (flash_surface == NULL){
		PRINT(stderr, "Couldn't render flash surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = 'A';
	alt_key_indicator = TTF_RenderUNICODE_Shaded(font, str, metamode_cursor_fg, metamode_cursor_bg);
	if (alt_key_indicator == NULL){
		PRINT(stderr, "Couldn't render alt_key_indicator surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = 'C';
	ctrl_key_indicator = TTF_RenderUNICODE_Shaded(font, str, metamode_cursor_fg, metamode_cursor_bg);
	if (ctrl_key_indicator == NULL){
		PRINT(stderr, "Couldn't render ctrl_key_indicator surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = 0x2191;
	shift_key_indicator = TTF_RenderUNICODE_Shaded(font, str, metamode_cursor_fg, metamode_cursor_bg);
	if (shift_key_indicator == NULL){
		PRINT(stderr, "Couldn't render shift_key_indicator surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = 'a';
	altsym_indicator = TTF_RenderUNICODE_Shaded(font, str, metamode_cursor_fg, metamode_cursor_bg);
	if (shift_key_indicator == NULL){
		PRINT(stderr, "Couldn't render altsym_indicator surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = 'M';
	metamode_cursor = TTF_RenderUNICODE_Shaded(font, str, metamode_cursor_fg, metamode_cursor_bg);
	if (metamode_cursor == NULL){
		PRINT(stderr, "Couldn't render metamode_cursor surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = 0x25B2; /* black up triangle - scrolled back into history
	                  * (distinct from the 0x2191 shift indicator) */
	scroll_indicator = TTF_RenderUNICODE_Shaded(font, str, metamode_cursor_fg, metamode_cursor_bg);
	if (scroll_indicator == NULL){
		PRINT(stderr, "Couldn't render scroll_indicator surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	str[0] = '+'; /* inverse crosshair - mouse pointer for tracking apps */
	mouse_pointer = TTF_RenderUNICODE_Shaded(font, str, default_bg_color, default_text_color);
	if (mouse_pointer == NULL){
		PRINT(stderr, "Couldn't render mouse_pointer surface: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	/* Initialize the cursor */
	UChar cursorstr[2] = {' ', NULL};
	cursor = TTF_RenderUNICODE_Shaded(font, cursorstr, default_bg_color, default_text_color);
	if (cursor == NULL){
		PRINT(stderr, "Couldn't render cursor char: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	/* Get the size of the font */
	int minx, maxx, miny, maxy;
	if(TTF_GlyphMetrics(font, (Uint16)'X', &minx, &maxx, &miny, &maxy, &advance) != 0){
		PRINT(stderr, "Could not get Glyph Metrics: %s\n", TTF_GetError());
		return TERM_FAILURE;
	}

	text_width = advance;
	text_height = maxy - miny;
	text_height_padding = TTF_FontLineSkip(font) - text_height;
	text_height += text_height_padding;
	PRINT(stderr, "Character h: %d w:%d (h padding: %d) advance: %d\n", text_height, text_width, text_height_padding, advance);

	/* the tab bar is drawn with the terminal font, so it has to know the
	 * cell metrics before the grid is sized against it */
	sessionui_set_metrics(text_height, advance);

	/* Fallback font for glyphs the main font does not provide (box
	 * drawing, braille, powerline, ...). Sized down until it fits the
	 * cell width; height is left alone so box glyphs stay connected. */
	nerd_font = NULL;
	{
		const char *nerd_path = "../app/native/fonts/SymbolsNerdFontMono-Regular.ttf";
		nerd_font = TTF_OpenFont(nerd_path, font_size);
		if(nerd_font == NULL){
			fprintf(stderr, "No nerd font at %s: %s\n", nerd_path, SDL_GetError());
		} else {
			TTF_SetFontStyle(nerd_font, TTF_STYLE_NORMAL);
			TTF_SetFontOutline(nerd_font, 0);
			TTF_SetFontKerning(nerd_font, 0);
			TTF_SetFontHinting(nerd_font, TTF_HINTING_NORMAL);
		}
	}

	fallback_font = NULL;
	if(prefs->fallback_font_path && prefs->fallback_font_path[0]){
		int fb_size = font_size;
		while(fb_size >= MIN_FONT_SIZE){
			int fb_minx, fb_maxx, fb_miny, fb_maxy, fb_advance;
			fallback_font = TTF_OpenFont(prefs->fallback_font_path, fb_size);
			if(fallback_font == NULL){
				fprintf(stderr, "No fallback font at %s: %s\n", prefs->fallback_font_path, SDL_GetError());
				break;
			}
			if(TTF_GlyphMetrics(fallback_font, (Uint16)'X', &fb_minx, &fb_maxx, &fb_miny, &fb_maxy, &fb_advance) == 0
			   && fb_advance <= advance){
				TTF_SetFontStyle(fallback_font, TTF_STYLE_NORMAL);
				TTF_SetFontOutline(fallback_font, 0);
				TTF_SetFontKerning(fallback_font, 0);
				TTF_SetFontHinting(fallback_font, TTF_HINTING_NORMAL);
				PRINT(stderr, "Fallback font %s at size %d\n", prefs->fallback_font_path, fb_size);
				break;
			}
			TTF_CloseFont(fallback_font);
			fallback_font = NULL;
			--fb_size;
		}
	}

	/* CJK fonts for Han/kana/hangul glyphs the fonts above lack. These are
	 * full-width by nature, so they are loaded at the normal size (no
	 * cell-width shrinking); double-width layout is handled by the terminal
	 * core. The primary comes from prefs (a Simplified Chinese Hei face that
	 * covers Han); the fallbacks below add the scripts it lacks (MSung/cp950
	 * carries Japanese kana, malgun carries Hangul). */
	num_cjk_fonts = 0;
	{
		int i;
		const char* paths[MAX_CJK_FONTS];
		int n = 0;
		if(prefs->cjk_font_path && prefs->cjk_font_path[0]){
			paths[n++] = prefs->cjk_font_path;
		}
		paths[n++] = "/usr/fonts/font_repository/monotype/MSungM.cp950.v311.1.ttf";
		paths[n++] = "/usr/fonts/font_repository/monotype/malgun.ttf";
		for(i = 0; i < n && num_cjk_fonts < MAX_CJK_FONTS; ++i){
			TTF_Font* f = TTF_OpenFont(paths[i], font_size);
			if(f == NULL){
				fprintf(stderr, "No CJK font at %s: %s\n", paths[i], SDL_GetError());
				continue;
			}
			TTF_SetFontStyle(f, TTF_STYLE_NORMAL);
			TTF_SetFontOutline(f, 0);
			TTF_SetFontKerning(f, 0);
			TTF_SetFontHinting(f, TTF_HINTING_NORMAL);
			PRINT(stderr, "CJK font %s at size %d\n", paths[i], font_size);
			cjk_fonts[num_cjk_fonts++] = f;
		}
	}

	return TERM_SUCCESS;
}

/* the terminal's primary font, for the session UI chrome */
TTF_Font* app_font(){
	return font;
}

/* pick the font that can actually draw this character */
static TTF_Font* font_for_char(UChar c){
	if(TTF_GlyphIsProvided(font, c)){
		return font;
	}
	if(fallback_font != NULL && TTF_GlyphIsProvided(fallback_font, c)){
		return fallback_font;
	}
	if(nerd_font != NULL && TTF_GlyphIsProvided(nerd_font, c)){
		return nerd_font;
	}
	int i;
	for(i = 0; i < num_cjk_fonts; ++i){
		if(TTF_GlyphIsProvided(cjk_fonts[i], c)){
			return cjk_fonts[i];
		}
	}
	return font;
}

/* Glyph cache.
 *
 * Rendering a glyph means a FreeType rasterisation plus a surface
 * allocation, and it happens with the input lock held - the SDL event pump
 * needs that same lock before it can deliver a keystroke. Rendering every
 * cell of a full-screen repaint separately (the same 'e' dozens of times)
 * therefore shows up directly as input latency, seconds of it when a tmux
 * pane repaints continuously.
 *
 * So glyphs are rasterised once per (character, style, colours) and shared
 * by every cell that draws them. Cells hold *borrowed* pointers: entries
 * are never freed individually, only wholesale by glyph_cache_flush(),
 * which font_uninit() calls before any font or size change. Every caller
 * of font_uninit() follows it with buf_clear_all_renders(), which nulls the
 * cell pointers, and neither releases the input lock in between. */
#define GLYPH_CACHE_SLOTS 4096  /* power of two, far more than one screen */
#define GLYPH_CACHE_PROBES 8

struct glyph_entry {
	SDL_Surface* surface;
	UChar c;
	UChar c2;          /* low surrogate or combining mark, 0 = none */
	int style;
	Uint32 fg, bg;
	char used;
};
static struct glyph_entry glyph_cache[GLYPH_CACHE_SLOTS];

static Uint32 pack_color(SDL_Color c){
	return ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | (Uint32)c.b;
}

void glyph_cache_flush(){
	int i;
	for(i = 0; i < GLYPH_CACHE_SLOTS; ++i){
		if(glyph_cache[i].used){
			SDL_FreeSurface(glyph_cache[i].surface);
			glyph_cache[i].surface = NULL;
			glyph_cache[i].used = 0;
		}
	}
}

/* Shaded glyph for this character in this style and these colours.
 * *shared is 1 when the surface belongs to the cache - borrow it, never
 * free it. It is 0 only when the bucket is full, in which case the caller
 * owns the returned surface and must free it after drawing. */
static SDL_Surface* glyph_render(UChar c, UChar c2, int style, SDL_Color fg, SDL_Color bg, int* shared){
	UChar str[3];
	str[0] = c;
	str[1] = c2;
	str[2] = 0;
	Uint32 pfg = pack_color(fg);
	Uint32 pbg = pack_color(bg);
	Uint32 h = ((Uint32)c * 2654435761u) ^ ((Uint32)c2 * 12289u) ^ ((Uint32)style * 40503u)
	           ^ (pfg * 2246822519u) ^ (pbg * 3266489917u);
	int slot = (int)(h & (GLYPH_CACHE_SLOTS - 1));
	int probe;
	TTF_Font* rfont;

	for(probe = 0; probe < GLYPH_CACHE_PROBES; ++probe){
		struct glyph_entry* e = &glyph_cache[(slot + probe) & (GLYPH_CACHE_SLOTS - 1)];
		if(e->used){
			if(e->c == c && e->c2 == c2 && e->style == style && e->fg == pfg && e->bg == pbg){
				*shared = 1;
				return e->surface;
			}
			continue;
		}
		/* empty slot: rasterise into it */
		rfont = font_for_char(c);
		TTF_SetFontStyle(rfont, style);
		e->surface = TTF_RenderUNICODE_Shaded(rfont, str, fg, bg);
		if(e->surface == NULL){
			*shared = 1;
			return NULL;
		}
		e->c = c;
		e->c2 = c2;
		e->style = style;
		e->fg = pfg;
		e->bg = pbg;
		e->used = 1;
		*shared = 1;
		return e->surface;
	}

	/* bucket full: fall back to a one-off the caller frees */
	rfont = font_for_char(c);
	TTF_SetFontStyle(rfont, style);
	*shared = 0;
	return TTF_RenderUNICODE_Shaded(rfont, str, fg, bg);
}

void font_uninit(){

	int i;

	/* cells borrow from the cache, so it must go before the fonts do */
	glyph_cache_flush();
	for(i = 0; i < num_cjk_fonts; ++i){
		TTF_CloseFont(cjk_fonts[i]);
		cjk_fonts[i] = NULL;
	}
	num_cjk_fonts = 0;
	if(fallback_font != NULL){
		TTF_CloseFont(fallback_font);
		fallback_font = NULL;
	}
	if(nerd_font != NULL){
		TTF_CloseFont(nerd_font);
		nerd_font = NULL;
	}
	SDL_FreeSurface(blank_surface);
	SDL_FreeSurface(flash_surface);
	SDL_FreeSurface(metamode_cursor);
	SDL_FreeSurface(ctrl_key_indicator);
	SDL_FreeSurface(alt_key_indicator);
	SDL_FreeSurface(shift_key_indicator);
	SDL_FreeSurface(scroll_indicator);
	SDL_FreeSurface(mouse_pointer);
	SDL_FreeSurface(cursor);
	if(font != NULL){
		TTF_CloseFont(font);
	}
}

void handle_activeevent(int gain, int state){
	if (gain && prefs->auto_show_vkb){
		PRINT(stderr, "Got ActiveEvent - initializing keyboard\n");
		virtualkeyboard_show();
	}
}

void handle_mousedown(Uint16 x, Uint16 y){
	/* The metamode hitbox is measured from the top of the *grid*: the tab
	 * bar owns the strip above it, so the corner gesture keeps working
	 * without fighting the tabs. Symmenu hitboxes below are in screen
	 * coordinates, since the menu is drawn on the window bottom. */
	int gy = y - content_top;
	if((x >= prefs->metamode_hitbox->x) &&
	   (x <= prefs->metamode_hitbox->x + prefs->metamode_hitbox->w) &&
	   (gy >= prefs->metamode_hitbox->y) &&
	   (gy <= prefs->metamode_hitbox->y + prefs->metamode_hitbox->h)) {
		/* hit in the box */
		metamode_toggle();
	}
	/* touching the screen will reveal the keyboard on a Passport,
	 * since the system wide gesture doesn't work to reveal. */
	if (prefs->auto_show_vkb){
		virtualkeyboard_show();
	}

	/* check for symmenu touches */
	if(current_symmenu != NULL){
		const char *menu_key = symkey_for_mousedown(current_symmenu, x, y);
		if (!handle_menu_action(menu_key)) {
			send_metamode_keystrokes(menu_key);
		}
	}
}

void handle_virtualkeyboard_event(bps_event_t *event){
	PRINT(stderr, "Virtual Keyboard event\n");
	int event_code = bps_event_get_code(event);
	int vkb_h;
	int resolution[2] = {screen->w, screen->h};

	vkb_h = get_virtualkeyboard_height();

	switch (event_code){
	case VIRTUALKEYBOARD_EVENT_VISIBLE:
		setup_screen_size(resolution[0], resolution[1] - vkb_h);
		virtualkeyboard_visible = 1;
		break;
	case VIRTUALKEYBOARD_EVENT_HIDDEN:
		setup_screen_size(resolution[0], resolution[1]);
		virtualkeyboard_visible = 0;
		break;
	case VIRTUALKEYBOARD_EVENT_INFO:
		vkb_h = virtualkeyboard_visible ? virtualkeyboard_event_get_height(event) : 0;
		setup_screen_size(resolution[0], resolution[1] - vkb_h);
		break;
	default:
		fprintf(stderr, "Unknown keyboard event code %d\n", event_code);
		break;
	}
}

void rescreen(int w, int h){

	int width  = w == -1 ? screen->w : w;
	int height = h == -1 ? screen->h : h;
	int vkb_h = 0;
	screen = SDL_SetVideoMode(width, height, PB_D_PIXELS, SDL_HWSURFACE | SDL_DOUBLEBUF);
	sessionui_set_screen(screen);
	/* reset the font size as well */
	font_uninit();
	buf_clear_all_renders();
	session_clear_renders();
	if(font_init(prefs->font_size) == TERM_FAILURE){
		fprintf(stderr, "Couldn't initialize font\n");
		exit_application = 1;
	}

	setup_screen_size(width, height);
	if(virtualkeyboard_visible){
		vkb_h = get_virtualkeyboard_height();
		setup_screen_size(width, height - vkb_h);
	}
}

void toggle_vkeymod(int mod){
	PRINT(stderr, "Toggle modifier %d\n", mod);
	if(vmodifiers & mod){
		vmodifiers &= ~mod;
	}
	else {
		vmodifiers |= mod;
	}
}

static symmenu_t *get_keyhold_actions(int keycode) {
	if (!prefs->keyhold_actions) {
		return NULL;
	}

	int uppercase = 0;
	if (vmodifiers & KEYMOD_SHIFT) {
		uppercase = 1;
	}

	switch (keycode) {
	case KEYCODE_A:
		return prefs->accent_menus[0][uppercase];
	case KEYCODE_B:
		return prefs->accent_menus[1][uppercase];
	case KEYCODE_C:
		return prefs->accent_menus[2][uppercase];
	case KEYCODE_D:
		return prefs->accent_menus[3][uppercase];
	case KEYCODE_E:
		return prefs->accent_menus[4][uppercase];
	case KEYCODE_F:
		return prefs->accent_menus[5][uppercase];
	case KEYCODE_G:
		return prefs->accent_menus[6][uppercase];
	case KEYCODE_H:
		return prefs->accent_menus[7][uppercase];
	case KEYCODE_I:
		return prefs->accent_menus[8][uppercase];
	case KEYCODE_J:
		return prefs->accent_menus[9][uppercase];
	case KEYCODE_K:
		return prefs->accent_menus[10][uppercase];
	case KEYCODE_L:
		return prefs->accent_menus[11][uppercase];
	case KEYCODE_M:
		return prefs->accent_menus[12][uppercase];
	case KEYCODE_N:
		return prefs->accent_menus[13][uppercase];
	case KEYCODE_O:
		return prefs->accent_menus[14][uppercase];
	case KEYCODE_P:
		return prefs->accent_menus[15][uppercase];
	case KEYCODE_Q:
		return prefs->accent_menus[16][uppercase];
	case KEYCODE_R:
		return prefs->accent_menus[17][uppercase];
	case KEYCODE_S:
		return prefs->accent_menus[18][uppercase];
	case KEYCODE_T:
		return prefs->accent_menus[19][uppercase];
	case KEYCODE_U:
		return prefs->accent_menus[20][uppercase];
	case KEYCODE_V:
		return prefs->accent_menus[21][uppercase];
	case KEYCODE_W:
		return prefs->accent_menus[22][uppercase];
	case KEYCODE_X:
		return prefs->accent_menus[23][uppercase];
	case KEYCODE_Y:
		return prefs->accent_menus[24][uppercase];
	case KEYCODE_Z:
		return prefs->accent_menus[25][uppercase];
	}

	return NULL;
}

/* Hardware belt keys (BlackBerry Classic). Called from the patched SDL
 * navigator event handler; return nonzero to claim the key from the OS.
 * The back button sends configurable keystrokes (Esc by default). */
/* ---- built-in pinyin input bar ------------------------------------- */
static FILE *term49_keylog_fp(void);

static void pinyin_commit(const char *utf8, size_t len) {
	ssize_t w = io_write_master_char(utf8, len);
	FILE *fp = term49_keylog_fp();
	if (fp != NULL) {
		fprintf(fp, "commit len=%u wrote=%d master=%d text=[%.*s]\n",
		        (unsigned)len, (int)w, io_get_master(), (int)len, utf8);
		fflush(fp);
	}
	buf_reset_view();
}

/* reserve the bottom row for the input bar while it is on */
static void pinyin_set_bar(int on) {
	int want = on ? 1 : 0;
	if (want != pinyin_bar_rows) {
		pinyin_bar_rows = want;
		rescreen(-1, -1);
	}
}

int handleSyskeyEvent(int syskey)
{
	switch(syskey){
	case NAVIGATOR_SYSKEY_BACK:
		/* while a prompt / search / switcher is up, Back means "cancel" */
		if(sessionui_cancel()){
			return 1;
		}
		/* then an unfinished pinyin composition */
		if (pinyin_composing()) {
			pinyin_key(0x1b, pinyin_commit);
			indicate_event_input();
			return 1;
		}
		send_metamode_keystrokes(prefs->back_button_keys);
		return 1;
	default:
		/* send/end keys keep their system behaviour */
		return 0;
	}
}

/* The trackpad on devices like the BlackBerry Classic (Q20) is presented
 * as a joystick input device that reports relative displacement and a
 * button. We accumulate displacement and translate it into arrow
 * keystrokes (DECCKM aware via ecma48_parse_control_codes), and send the
 * configured keystrokes when the trackpad is clicked. Called from the
 * patched SDL event pump (like handleKeyboardEvent), under lock_input. */
#define TRACKPAD_MAX_STEPS_PER_EVENT 5
void handleTrackpadEvent(screen_event_t screen_event)
{
	static int trackpad_acc[2] = {0, 0};
	static int trackpad_buttons = 0;

	int displacement[2] = {0, 0};
	int buttons = 0;
	int threshold, axis, steps, i;
	int keycode[2][2] = {{KEYCODE_LEFT, KEYCODE_RIGHT}, {KEYCODE_UP, KEYCODE_DOWN}};
	UChar c[CHARACTER_BUFFER];
	int num_chars;

	if(!prefs->trackpad_enabled){
		return;
	}

	screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_DISPLACEMENT, displacement);
	screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_BUTTONS, &buttons);

	if(sessionui_modal()){
		/* the prompt / search / switcher owns input: swipe moves through
		 * it, click confirms - nothing goes to the shell */
		int click = buttons && !trackpad_buttons;
		trackpad_buttons = buttons;
		trackpad_acc[0] += displacement[0];
		trackpad_acc[1] += displacement[1];
		threshold = prefs->trackpad_sensitivity > 0 ? prefs->trackpad_sensitivity : DEFAULT_TRACKPAD_SENSITIVITY;
		if(click){
			trackpad_acc[0] = trackpad_acc[1] = 0;
			sessionui_key(KEYCODE_RETURN, 0, 0);
			return;
		}
		if(displacement[0] == 0 && displacement[1] == 0){
			return;
		}
		if(abs(trackpad_acc[1]) >= threshold){
			int key = trackpad_acc[1] < 0 ? KEYCODE_UP : KEYCODE_DOWN;
			trackpad_acc[1] = 0;
			sessionui_key(key, 0, 0);
		}
		if(abs(trackpad_acc[0]) >= threshold){
			int key = trackpad_acc[0] < 0 ? KEYCODE_LEFT : KEYCODE_RIGHT;
			trackpad_acc[0] = 0;
			sessionui_key(key, 0, 0);
		}
		return;
	}

	threshold = prefs->trackpad_sensitivity > 0 ? prefs->trackpad_sensitivity : DEFAULT_TRACKPAD_SENSITIVITY;

	/* with the alt modifier active, the trackpad scrolls the local
	 * scrollback instead of sending arrow keys */
	int scroll_mode = altsym_lock || altsym_hold || (vmodifiers & KEYMOD_ALT);

	/* when the application has enabled xterm mouse tracking, the trackpad
	 * drives a visible mouse pointer instead of sending arrow keys */
	int tracking = ecma48_mouse_tracking();

	if(tracking && !scroll_mode && buf->view_offset == 0){
		int prev_col = pointer_x / advance;
		int prev_row = pointer_y / text_height;
		int col, row;

		pointer_x += displacement[0];
		pointer_y += displacement[1];
		if(pointer_x < 0){ pointer_x = 0; }
		if(pointer_y < 0){ pointer_y = 0; }
		if(pointer_x > cols * advance - 1){ pointer_x = cols * advance - 1; }
		if(pointer_y > rows * text_height - 1){ pointer_y = rows * text_height - 1; }
		if(displacement[0] || displacement[1]){
			pointer_visible = 1;
		}
		col = pointer_x / advance;
		row = pointer_y / text_height;

		/* motion reports: any-motion mode always, button-motion mode
		 * only while the trackpad is pressed */
		if((col != prev_col || row != prev_row) &&
		   ((tracking == 1003) || (tracking == 1002 && trackpad_buttons))){
			ecma48_send_mouse_event(32 + (trackpad_buttons ? 0 : 3), 1, col + 1, row + 1);
		}

		/* trackpad press / release = left mouse button */
		if(buttons && !trackpad_buttons){
			ecma48_send_mouse_event(0, 1, col + 1, row + 1);
			pointer_visible = 1;
		} else if(!buttons && trackpad_buttons){
			ecma48_send_mouse_event(0, 0, col + 1, row + 1);
		}
		trackpad_buttons = buttons;
		trackpad_acc[0] = 0;
		trackpad_acc[1] = 0;
		return;
	}

	/* a click (button press edge) sends the configured keystrokes */
	if(buttons && !trackpad_buttons){
		send_metamode_keystrokes(prefs->trackpad_click_keys);
		trackpad_acc[0] = 0;
		trackpad_acc[1] = 0;
	}
	trackpad_buttons = buttons;

	for(axis = 0; axis < 2; ++axis){
		/* drop leftover movement when the finger changes direction */
		if((displacement[axis] < 0 && trackpad_acc[axis] > 0) ||
		   (displacement[axis] > 0 && trackpad_acc[axis] < 0)){
			trackpad_acc[axis] = 0;
		}
		trackpad_acc[axis] += displacement[axis];
		steps = abs(trackpad_acc[axis]) / threshold;
		if(steps > TRACKPAD_MAX_STEPS_PER_EVENT){
			steps = TRACKPAD_MAX_STEPS_PER_EVENT;
		}
		if(steps > 0){
			if(scroll_mode){
				/* vertical swipes scroll: swipe up = view older lines.
				 * In mouse-tracking apps send wheel events instead (there
				 * is no local scrollback on the alternate screen). */
				if(axis == 1){
					if(tracking){
						int wheel = trackpad_acc[axis] < 0 ? 64 : 65;
						for(i = 0; i < steps; ++i){
							ecma48_send_mouse_event(wheel, 1,
							        pointer_x / advance + 1, pointer_y / text_height + 1);
						}
					} else {
						buf_scroll_view(trackpad_acc[axis] < 0 ? steps : -steps);
					}
				}
			} else {
				int key = keycode[axis][trackpad_acc[axis] < 0 ? 0 : 1];
				for(i = 0; i < steps; ++i){
					num_chars = ecma48_parse_control_codes(key, 0, c);
					io_write_master(c, num_chars);
				}
				buf_reset_view();
			}
			trackpad_acc[axis] %= threshold;
		}
	}
}

/* --- IME diagnostics ---------------------------------------------------
 * Log non-ASCII and control key events to $HOME/.term49_keys.log so the
 * virtual keyboard / input method path can be inspected on device.
 */
static FILE *term49_keylog_fp(void)
{
	const char *home = getenv("HOME");
	static FILE *fp = NULL;
	static int checked = 0;
	char path[512];

	if (!checked) {
		checked = 1;
		if (home != NULL) {
			snprintf(path, sizeof(path), "%s/.term49_keys.log", home);
			fp = fopen(path, "a");
		}
	}
	return fp;
}

static void term49_ime_log(int sym, int flags, int mods)
{
	FILE *fp = term49_keylog_fp();
	if (fp == NULL) {
		return;
	}
	fprintf(fp, "sym=0x%04x (%d) flags=0x%x mods=%d\n", sym, sym, (unsigned)flags, mods);
	fflush(fp);
}

/* Every write to the pty, as hex plus a printable rendering. Together with
 * the gate/pkey/commit lines this shows exactly what the shell received. */
void io_trace_write(const char *buf, size_t len, ssize_t wrote)
{
	FILE *fp;
	size_t i;
	const char *e = getenv("TERM49_KEYLOG");
	if (e != NULL && strcmp(e, "0") == 0) {
		return;
	}
	fp = term49_keylog_fp();
	if (fp == NULL) {
		return;
	}
	fprintf(fp, "write len=%u wrote=%d master=%d hex=", (unsigned)len, (int)wrote, io_get_master());
	for (i = 0; i < len && i < 48; ++i) {
		fprintf(fp, "%02x", (unsigned char)buf[i]);
	}
	fprintf(fp, " txt=[");
	for (i = 0; i < len && i < 48; ++i) {
		unsigned char c = (unsigned char)buf[i];
		if (c == 0x1b) fputs("\\e", fp);
		else if (c == 0x7f) fputs("<DEL>", fp);
		else if (c == '\r') fputs("\\r", fp);
		else if (c < 0x20) fprintf(fp, "^%c", c + '@');
		else fputc(c, fp);
	}
	fputs("]\n", fp);
	fflush(fp);
}

/* Trace for the input methods: which key arrived, what the routing gate
 * decided, and what pinyin did with it. */
static void term49_pinyin_log(int sym, int flags, int mods, int pk,
                              int composing, int gate, int rc)
{
	static int enabled = -1;
	FILE *fp;
	if (enabled < 0) {
		const char *e = getenv("TERM49_KEYLOG");
		enabled = !(e != NULL && strcmp(e, "0") == 0);
	}
	if (!enabled) {
		return;
	}
	fp = term49_keylog_fp();
	if (fp == NULL) {
		return;
	}
	/* keep the trace small: start over past 256 KB */
	if (ftell(fp) > 256 * 1024) {
		ftruncate(fileno(fp), 0);
		rewind(fp);
	}
	{
		char st[256];
		pinyin_status(st, sizeof(st));
		fprintf(fp, "pkey sym=0x%04x flags=0x%x mods=%d pk=%d composing=%d gate=%d rc=%d bar=[%s]\n",
		        (unsigned)sym, (unsigned)flags, mods, pk, composing, gate, rc, st);
	}
	fflush(fp);
}

void handleKeyboardEvent(screen_event_t screen_event)
{
	int screen_val, screen_flags, screen_alt_val;
	int modifiers;
	int num_chars;
	int vkbd_h;
	int metamode_just_set = 0;
	UChar c[CHARACTER_BUFFER];
	UChar *target = c;
	struct timespec now;
	uint64_t now_t, diff_t, metamode_last_t;
	const char* keys = NULL;
	int32_t last_len = 0;
	int32_t bs_i = 0;
	size_t upcase_len = 0;
	UChar backspace = (UChar)ecma48_backspace();

	screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_KEY_FLAGS, &screen_flags);
	screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_KEY_SYM, &screen_val);
	screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_KEY_ALTERNATE_SYM, &screen_alt_val);
	screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_KEY_MODIFIERS, &modifiers);
	//screen_get_event_property_iv(screen_event, SCREEN_PROPERTY_KEY_CAP, &cap);

	/* Built-in pinyin bar: plain letters, digits, space, enter, backspace
	 * and , . go to the bar while it is on. Modifier/menu/metamode keys and
	 * anything with ctrl/alt keep their usual meaning, and a rename prompt
	 * or the search bar always sees the keyboard first. */
	{
		int composing = pinyin_composing();
		/* On the Classic (Q20) there is no number row: digits, ',' '.' '-'
		 * '=' live on the Alt layer, so those keys arrive with KEYMOD_ALT
		 * and the sym already resolved to the digit. While a composition
		 * is up they must still pick a candidate / turn the page instead
		 * of being typed into the shell. Every other Alt combination (and
		 * all of Ctrl) keeps its usual meaning. */
		int alt = ((modifiers | vmodifiers) & KEYMOD_ALT) != 0;
		int sticky_alt = altsym_lock || altsym_hold;
		/* Digits and the paging keys are control characters for the bar, so
		 * they win over Alt entirely while a composition is up. Off the
		 * composition they keep their normal meaning (on the Classic they
		 * *are* the Alt layer of the number keys). */
		int ime_char = composing &&
		               (((screen_val >= '1' && screen_val <= '9')) ||
		                screen_val == ',' || screen_val == '.' ||
		                screen_val == '-' || screen_val == '=');
		int pinyin_gate = (screen_flags & KEY_DOWN) && pinyin_enabled() &&
		                  !sessionui_modal() && !metamode && current_symmenu == NULL &&
		                  !((modifiers | vmodifiers) & KEYMOD_CTRL) &&
		                  (!alt || ime_char) &&
		                  (!sticky_alt || ime_char);
		if (pinyin_enabled() && (screen_flags & KEY_DOWN)) {
			FILE *lf = term49_keylog_fp();
			if (lf != NULL) {
				fprintf(lf, "gate sym=0x%04x flags=0x%x mods=%d vmods=%d alt=%d sticky=%d lock=%d hold=%d "
				        "meta=%d symmenu=%d modal=%d composing=%d ime_char=%d -> gate=%d\n",
				        (unsigned)screen_val, (unsigned)screen_flags, modifiers, vmodifiers,
				        alt, sticky_alt, (int)altsym_lock, (int)altsym_hold, (int)metamode,
				        current_symmenu != NULL, sessionui_modal(), composing, ime_char, pinyin_gate);
				fflush(lf);
			}
		}
		if (pinyin_gate) {
			int pk = -1;
			int rc = 0;
			if (screen_val == KEYCODE_BACKSPACE) {
				pk = 0x7f;
			} else if (screen_val == KEYCODE_RETURN) {
				pk = '\r';
			} else if (screen_val >= 0x20 && screen_val < 0x7f) {
				pk = screen_val;
			}
			if (pk >= 0) {
				/* Candidate digits are always taken, even on a repeat: a
				 * lost digit here means the digit is typed into the shell
				 * instead of picking a word. Other keys may repeat only
				 * backspace; holding space is the metamode gesture. */
				int is_digit = (pk >= '1' && pk <= '9');
				if (!is_digit && (screen_flags & KEY_REPEAT) && pk != 0x7f) {
					if (composing || (pk >= 'a' && pk <= 'z') || (pk >= 'A' && pk <= 'Z')) {
						term49_pinyin_log(screen_val, screen_flags, modifiers, pk, composing, 1, -1);
						return;
					}
				} else {
					rc = pinyin_key(pk, pinyin_commit);
					term49_pinyin_log(screen_val, screen_flags, modifiers, pk, composing, 1, rc);
					if (rc) {
						/* an Alt-layer key that the bar took also uses up
						 * the one-shot sticky Alt */
						if (altsym_lock) {
							altsym_toggle();
						}
						buf_reset_view();
						indicate_event_input();
						return;
					}
				}
			}
		}
	}

	if (screen_flags & KEY_DOWN) {
		PRINT(stderr, "The '%d' key was pressed (modifiers: %d) (char %c) (alt %d)\n", (int)screen_val, modifiers, (char)screen_val, (int)screen_alt_val);

		/* a rename prompt or the scrollback search bar owns the keyboard
		 * while it is up - nothing typed there may reach the shell */
		if(sessionui_key(screen_val, modifiers, screen_flags)){
			return;
		}

		/* BB10 hands composed characters (CJK, accented, symbols) from the
		 * virtual keyboard to the app as Unicode codepoints in KEY_SYM.
		 * Real keycodes live in the Unicode private use area, so
		 * 0x80..0xDFFF is literal text: pass it straight to the pty. */
		if (screen_val >= 0x80 && screen_val < 0xE000) {
			UChar ime_text;
			term49_ime_log(screen_val, screen_flags, modifiers);
			ime_text = (UChar)screen_val;
			io_write_master(&ime_text, 1);
			buf_reset_view();
			pointer_visible = 0;
			return;
		}
		fflush(stdout);

		/* typing snaps the view back to the live screen; the alt and menu
		 * keys are excluded so they can arm trackpad scrolling while
		 * scrolled back */
		if(screen_val != KEYCODE_BB_ALT_KEY && screen_val != KEYCODE_MENU){
			buf_reset_view();
		}
		/* and hides the mouse pointer */
		pointer_visible = 0;

		/* if we're toggling metamode on or off with doubletap */
		if((screen_val == metamode_doubletap_key) && !(screen_flags & KEY_REPEAT)){
			clock_gettime(CLOCK_MONOTONIC, &now);
			now_t = timespec2nsec(&now);
			metamode_last_t = timespec2nsec(&metamode_last);
			diff_t = now_t > metamode_last_t ? now_t - metamode_last_t : now_t;
			if(diff_t <= prefs->metamode_doubletap_delay){
				metamode_toggle();
				metamode_just_set = 1;
			}
			metamode_last = now;
		}

		/* handle sticky keys */
		if(screen_val == KEYCODE_BB_SYM_KEY){
			if(!(screen_flags & KEY_REPEAT)){
				symmenu_toggle(prefs->main_symmenu);
			} else{
				/* they are holding it down */
				symmenu_stick();
			}
			return;
		}

		if(screen_val == KEYCODE_BB_ALT_KEY){
			if (prefs->sticky_alt_key) {
				if(screen_flags & KEY_REPEAT){
					return;
				} else {
					altsym_toggle();
					return;
				}
			}
		}

		/* the menu / BlackBerry key toggles persistent alt mode; handle it
		 * before the altsym lookup below, or its keycode (0xf067) would be
		 * truncated to 'g' and matched against the alt table */
		if(screen_val == KEYCODE_MENU){
			if(!(screen_flags & KEY_REPEAT)){
				if (prefs->action_menu != NULL && getenv("TERM49_MENU_ALT") == NULL) {
					/* Menu key pops the action menu (paste / pinyin / vkb / ...) */
					if (current_symmenu != NULL) {
						symmenu_toggle(NULL);
					} else {
						symmenu_toggle(prefs->action_menu);
					}
				} else {
					altsym_hold_toggle();
				}
			}
			return;
		}
		
		if(!virtualkeyboard_visible
		   && ((screen_val == KEYCODE_LEFT_SHIFT) || (screen_val == KEYCODE_RIGHT_SHIFT))){
			if (prefs->sticky_shift_key) {
				if(screen_flags & KEY_REPEAT){
					return;
				} else {
					toggle_vkeymod(KEYMOD_SHIFT);
					return;
				}
			}
		}

		/* metamode sticky keys don't trigger repreat */
		if (metamode && !metamode_just_set) {
			keys = keystroke_lookup((char)screen_val, prefs->metamode_sticky_keys);
			if (keys != NULL){
				send_metamode_keystrokes(keys);
				return;
			}
		}

		/* handle key repeat to upcase / metamode */
		if ((screen_flags & KEY_REPEAT) &&
		    prefs->keyhold_actions &&
		    !is_int_member(prefs->keyhold_actions_exempt, screen_val)) {
			if (!key_repeat_done) {
				/* Check for a metamode toggle key first */
				if (screen_val == prefs->metamode_hold_key) {
					io_write_master(&backspace, 1);
					metamode_toggle();
					key_repeat_done = 1;
					return;
				}
				
				symmenu_t *menu = get_keyhold_actions(screen_val);
				if (menu == NULL) {
					return;
				}
				
				last_len = io_upcase_last_write(&target, CHARACTER_BUFFER);
				/* write backspace */
				for(bs_i = 1; bs_i <= last_len; ++bs_i) {
					io_write_master(&backspace, 1);
				}

				/* select the mapping */

				// uppercase automatically if there's no accents or accents disabled
				if ((menu->entries[1].to == NULL) || (!prefs->keyhold_accents)) {
					/* We can upcase, send last_len backspaces and then the upcase char.
					 * Note that this really only works if the program on the other
					 * end of the line understands unicode, and can marry up backspaces
					 * with codepoints, instead of just blindly deleting one byte at a time. */
					send_metamode_keystrokes(menu->entries[0].to);
				} else {
					symmenu_toggle(menu);
				}
				key_repeat_done = 1;
				return;
			} else {
				// We have already handled this key repeat
				return;
			}
		} else {
			key_repeat_done = 0;
		}

		if(metamode && !metamode_just_set){
			/* metamode is shift-aware: with Shift held (or armed via the
			 * sticky shift key), an uppercase binding wins if one exists,
			 * and a key bound to Tab sends back-tab (CSI Z) instead */
			int mm_shifted = (modifiers & KEYMOD_SHIFT) || (vmodifiers & KEYMOD_SHIFT)
			                 || (screen_val >= 'A' && screen_val <= 'Z');
			int mm_sym = (screen_val >= 'A' && screen_val <= 'Z') ? screen_val + 040 : screen_val;
			keys = NULL;
			if(mm_shifted && mm_sym >= 'a' && mm_sym <= 'z'){
				keys = keystroke_lookup((char)(mm_sym - 040), prefs->metamode_keys);
				if(keys != NULL){
					vmodifiers &= ~KEYMOD_SHIFT;
				}
			}
			if(keys == NULL){
				keys = shifted_keystrokes(keystroke_lookup((char)mm_sym, prefs->metamode_keys), mm_shifted);
			}
			if(keys != NULL){
				send_metamode_keystrokes(keys);
				metamode_toggle();
				return;
			}
			// else
			keys = keystroke_lookup((char)screen_val, prefs->metamode_func_keys);
			if(keys != NULL){
				int f = 0; /* check custom func commands */
				if(!f && (0 == strncmp(keys, "alt_down", 8)))          { toggle_vkeymod(KEYMOD_ALT);f=1;}
				if(!f && (0 == strncmp(keys, "ctrl_down", 9)))         { toggle_vkeymod(KEYMOD_CTRL);f=1;}
				if(!f && (0 == strncmp(keys, "rescreen", 8)))          { rescreen(-1, -1);f=1;}
				if(!f && (0 == strncmp(keys, "paste_clipboard", 15)))  { io_paste_from_clipboard();f=1;}
				if(!f && (0 == strncmp(keys, "session_new", 11)))      { sessionui_new_session();f=1;}
				if(!f && (0 == strncmp(keys, "session_menu", 12)))     { sessionui_open_switcher();f=1;}
				if(!f && (0 == strncmp(keys, "session_prev", 12)))     { session_prev();f=1;}
				if(!f && (0 == strncmp(keys, "session_next", 12)))     { session_next();f=1;}
				if(!f && (0 == strncmp(keys, "session_close", 13)))    { sessionui_close_active();f=1;}
				if(!f && (0 == strncmp(keys, "session_rename", 14)))   { sessionui_start_rename();f=1;}
				if(!f && (0 == strncmp(keys, "session_search", 14)))   { sessionui_start_search();f=1;}
				if(!f && (0 == strncmp(keys, "session_bar", 11)))      { sessionui_toggle_bar();f=1;}
			}
			/* Session shortcuts: built in, so they also work with an old
			 * ~/.term49rc that knows nothing about sessions. Checked last,
			 * so the user's own metamode_keys/metamode_func_keys win. */
			if(keys == NULL && screen_val >= 0x20){
				int session_action = 0;
				switch(screen_val){
					case 'n': sessionui_new_session();   session_action = 1; break;
					case 'b': sessionui_open_switcher(); session_action = 1; break;
					case 'r': sessionui_start_rename();  session_action = 1; break;
					case 'f': sessionui_start_search();  session_action = 1; break;
					case ']': session_next();            session_action = 1; break;
					case '[': session_prev();            session_action = 1; break;
					case 'x': sessionui_close_active();  session_action = 1; break;
					case '0': sessionui_toggle_bar();    session_action = 1; break;
					default:
						if(screen_val >= '1' && screen_val <= '9'){
							session_switch_index(screen_val - '1');
							session_action = 1;
						}
						break;
				}
				if(session_action){
					metamode_toggle();
					indicate_event_input();
					return;
				}
			}
			metamode_toggle();
			return;
		}

		/* handle alt keys: altsym_lock is one-shot, altsym_hold stays on
		 * until the menu key is pressed again */
		if (altsym_lock || altsym_hold) {
			keys = shifted_keystrokes(keystroke_lookup((char)screen_val, prefs->altsym_entries),
			                          (modifiers & KEYMOD_SHIFT) || (vmodifiers & KEYMOD_SHIFT));
			if (altsym_lock) {
				altsym_toggle();
			}
			if (keys != NULL){
				send_metamode_keystrokes(keys);
				return;
			}
		}

		/* handle sym keys */
		if (current_symmenu != NULL) {
			keys = shifted_keystrokes(keystroke_lookup((char)screen_val, current_symmenu->entries),
			                          (modifiers & KEYMOD_SHIFT) || (vmodifiers & KEYMOD_SHIFT));
			if (keys != NULL){
				int is_action = handle_menu_action(keys);
				if (!is_action) {
					send_metamode_keystrokes(keys);
				}
				symmenu_toggle(NULL);
				return;
			}
		}

		/* if we have virtual keymods, then put them in, then turn them off */
		modifiers |= vmodifiers;
		vmodifiers = 0;

		/* now process the keypress */
		switch (screen_val) {
		case KEYCODE_PAUSE      :
		case KEYCODE_SCROLL_LOCK:
		case KEYCODE_PRINT      :
		case KEYCODE_SYSREQ     :
		case KEYCODE_BREAK      :
			//case KEYCODE_ESCAPE     :
			//case KEYCODE_BACKSPACE  :
			//case KEYCODE_TAB        :
			//case KEYCODE_BACK_TAB   :
		case KEYCODE_LEFT_ALT   :
		case KEYCODE_RIGHT_ALT  :
		case KEYCODE_LEFT_SHIFT :
		case KEYCODE_RIGHT_SHIFT:
			//case KEYCODE_INSERT     :
			//case KEYCODE_HOME       :
			//case KEYCODE_PG_UP      :
			//case KEYCODE_DELETE     :
			//case KEYCODE_END        :
			//case KEYCODE_PG_DOWN    :
		case KEYCODE_NUM_LOCK   :
			//case KEYCODE_F1         :
			//case KEYCODE_F2         :
			//case KEYCODE_F3         :
			//case KEYCODE_F4         :
			//case KEYCODE_F5         :
			//case KEYCODE_F6         :
			//case KEYCODE_F7         :
			//case KEYCODE_F8         :
			//case KEYCODE_F9         :
			//case KEYCODE_F10        :
			//case KEYCODE_F11        :
			//case KEYCODE_F12        :
			PRINT(stderr, "Modifier %d\n", screen_val);
			break;
		case KEYCODE_LEFT_CTRL  :
		case KEYCODE_RIGHT_CTRL :
			toggle_vkeymod(KEYMOD_CTRL);
			break;
		case KEYCODE_LEFT_HYPER :
		case KEYCODE_RIGHT_HYPER:
			toggle_vkeymod(KEYMOD_CTRL);
			break;
		case KEYCODE_CAPS_LOCK  :
			toggle_vkeymod(KEYMOD_CTRL);
			break;
		default:
			num_chars = ecma48_parse_control_codes(screen_val, modifiers, c);
			int nc;
			for(nc = 0; nc < num_chars; ++nc){
				PRINT(stderr, "Writing 0x%x\n", (int)c[nc]);
			}
			io_write_master((const UChar*)&c, num_chars);
			break;
		}
	}
}

/* Push the current grid size to one session's pty. Background sessions
 * need this too: they keep their own window size, and a shell that is
 * switched back to after a rotation must already agree with the grid. */
void session_push_winsize(session_t *s){
	if(s == NULL || s->master_fd < 0){ return; }
	if(tcsetsize(s->master_fd, rows, cols) < 0){
		PRINT(stderr, "ERROR: tcsetsize() returned <0 (%s). Did not set child pty window size. \n", strerror(errno));
	}
}

void session_push_winsize_all(){
	int i;
	for(i = 0; i < session_count; ++i){
		session_push_winsize(sessions[i]);
	}
}

void set_tty_window_size(){
	int i;
	session_push_winsize_all();
	/* and send SIGWINCH to every child */
	for(i = 0; i < session_count; ++i){
		session_t *s = sessions[i];
		int pgrp;
		if(s->master_fd < 0){ continue; }
		if(ioctl(s->master_fd, TIOCGPGRP, &pgrp) != -1){
			killpg(pgrp, SIGWINCH);
		} else {
			PRINT(stderr, "Could not get pgrp of tty: %s\n", strerror(errno));
			killpg(getpid(), SIGWINCH);
		}
	}
}

/* Call _after_ we have calculated the text size */
void setup_screen_size(int s_w, int s_h){

	if(s_w <= 1 || s_h <= 1){
		/* refusing to do that */
		return;
	}

	/* resizing works on the live screen */
	buf_reset_view();

	/* the session bar owns the top strip of the screen; the grid lives
	 * below it so no terminal line is ever hidden by the chrome */
	int bar = sessionui_bar_height();
	int avail_h = s_h - bar;
	if(avail_h < text_height){ avail_h = text_height; }
	content_top = bar;

	int old_rows = rows;
	int old_bottom_line = buf_bottom_line();
	rows = avail_h / text_height - pinyin_bar_rows;
	if (rows < 1) {
		rows = 1;
	}
	cols = s_w / text_width;
	int diff_rows = rows - old_rows;
	PRINT(stderr, "Rows: %d Cols: %d\n", rows, cols);

	/* calculate where we should start drawing */
	if(buf->line && old_rows > rows){ // new size smaller
		buf->top_line = buf->line - buf->top_line + 1 > rows ? buf->line - rows + 1 : buf->top_line;
		// clear covered up lines that are below the cursor
		int toclear = old_bottom_line - buf_bottom_line();
		PRINT(stderr, "new rows: %d, new bottom: %d, old rows: %d, old_bottom: %d, clearing %d lines (SIZE: %d)\n",
		      rows, buf_bottom_line(), old_rows, old_bottom_line, toclear, TEXT_BUFFER_SIZE);
		buf_erase_lines(buf_bottom_line() + 1, toclear);
	} else if (buf->line && old_rows < rows){ // new size bigger
		//buf->top_line = buf->line - rows + 1 < 0 ? 0 : buf->line - rows + 1;
		buf->top_line = buf->top_line - diff_rows < 0 ? 0 : buf->top_line - diff_rows;
		// clear newly revealed lines of artifacts
		int toclear = buf_bottom_line() < TEXT_BUFFER_SIZE ?
			buf_bottom_line() - old_bottom_line :
			TEXT_BUFFER_SIZE - 1 - old_bottom_line;
		PRINT(stderr, "new rows: %d, new bottom: %d, old rows: %d, old_bottom: %d, clearing %d lines\n",
		      rows, buf_bottom_line(), old_rows, old_bottom_line, toclear);
		buf_erase_lines(old_bottom_line + 1, toclear);
	}

	/* and reset the scroll region */
	sr.top = 1;
	sr.bottom = rows;

	// set the tty size
	set_tty_window_size();
}

/* Number of UI-side threads blocked on the input lock. The SDL event pump
 * takes this lock before it can deliver a keystroke, and the render thread
 * asks for it far more often (once to parse, once to draw, per frame), so
 * on an unfair mutex the pump can lose the race for many frames in a row -
 * seconds of key latency. The render thread uses lock_input_lowpri() below
 * to stand aside while anyone is waiting. */
static volatile int ui_lock_waiters = 0;

void lock_input(){
	++ui_lock_waiters;
	if(SDL_LockMutex(input_mutex) == -1){
		fprintf(stderr, "Couldn't lock input mutex - exiting\n");
		exit_application = 1;
	}
	--ui_lock_waiters;
}

/* Take the input lock, but let any waiting UI thread in first. Bounded so a
 * storm of input cannot starve drawing completely. */
#define LOWPRI_MAX_YIELDS 64
static void lock_input_lowpri(){
	int yields = 0;
	while(ui_lock_waiters > 0 && yields < LOWPRI_MAX_YIELDS){
		sched_yield();
		++yields;
	}
	if(SDL_LockMutex(input_mutex) == -1){
		fprintf(stderr, "Couldn't lock input mutex - exiting\n");
		exit_application = 1;
	}
}
void unlock_input(){
	if(SDL_UnlockMutex(input_mutex) == -1){
		fprintf(stderr, "Couldn't unlock input mutex - exiting\n");
		exit_application = 1;
	}
}

void indicate_event_input(){
	char *indicate_buf = "w";
	/* indicate that the render thread should run. Note that
	 * we are logging errors here, but aren't doing anything with them. */
	if(write(event_pipe[1], (void*)indicate_buf, 1) < 0 && errno != EAGAIN){
		fprintf(stderr, "Error writing to event pipe: %d\n", errno);
	}
}


/* This function is intended for resizing the number of
 * colums after app init */
void set_screen_cols(int ncols){
	/* Reloading every face and dropping every cached glyph is expensive
	 * enough to look like a hang, and DECCOLM arrives from the wire - an
	 * app that re-asserts the width it already has must not cost anything. */
	if (ncols <= 0 || ncols == cols) {
		return;
	}
	/* the user wants this number of columns */
	if (prefs->allow_resize_columns) {
		int new_fontsize = preferences_guess_best_font_size(prefs, ncols);
		font_uninit();
		buf_clear_all_renders();
		session_clear_renders();
		if(font_init(new_fontsize) == TERM_FAILURE){
			fprintf(stderr, "Error setting new font size\n");
			exit_application = 1;
		} else {
			setup_screen_size(screen->w, screen->h);
			/* and force the number of columns */
			cols = ncols;
			set_tty_window_size();
		}
	}
}

static int sdl_init() {
	/* init the input mutex */
	input_mutex = SDL_CreateMutex();
	
	/* init the event input pipe */
	if(pipe(event_pipe) == -1){
		fprintf(stderr, "Couldn't create event pipe\n");
		return TERM_FAILURE;
	}
	/* Neither end may block. The write end is written from the main thread
	 * with the input lock held, so a full pipe there would deadlock the app
	 * against the render thread; a dropped byte costs nothing because it is
	 * only a hint that there is something new to draw. The read end is
	 * drained in a loop, which needs the -1/EAGAIN to terminate. */
	fcntl(event_pipe[0], F_SETFL, fcntl(event_pipe[0], F_GETFL) | O_NONBLOCK);
	fcntl(event_pipe[1], F_SETFL, fcntl(event_pipe[1], F_GETFL) | O_NONBLOCK);

	/* Initialize SDL */
	if (SDL_Init(SDL_INIT_VIDEO) < 0 ) {
		PRINT(stderr, "Couldn't initialize SDL: %s\n",SDL_GetError());
		return TERM_FAILURE;
	}
	PRINT(stderr, "Post SDL_Init()\n");

	SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
	
	// We get keyboard events from the SysWMEvents
	SDL_EventState(SDL_KEYDOWN, SDL_IGNORE);
	SDL_EventState(SDL_KEYUP, SDL_IGNORE);

	screen_window_t window;
	screen_context_t context;

	SDL_SysWMinfo info;
	if(get_wm_info(&info) != 1){
		fprintf(stderr, "Couldn't get WM Info: %s\n",SDL_GetError());
		return TERM_FAILURE;
	}

	/* grab the orientation and resolution so we can start up that way */
	window = info.mainWindow;
	context = info.context;
	int wm_size[2] = {0,0};

	if (screen_get_window_property_iv(window, SCREEN_PROPERTY_SIZE, wm_size)) {
		fprintf(stderr, "Cannot get resolution: %s", strerror(errno));
		return TERM_FAILURE;
	}
	PRINT(stderr, "wm size returned: w:%d, h:%d\n", wm_size[0], wm_size[1]);

	/* Initialize the TTF library */
	if ( TTF_Init() < 0 ) {
		PRINT(stderr, "Couldn't initialize TTF: %s\n",SDL_GetError());
		SDL_Quit();
		return TERM_FAILURE;
	}

	/* set screen idle mode */
	if(!prefs->screen_idle_awake){
		setenv("SCREEN_IDLE_NORMAL", "1", 0);
	}

	/* check to verify if the wm returned the native resolution */
	if (getenv("WIDTH") != NULL && getenv("HEIGHT") != NULL) {
		if(wm_size[0] != atoi(getenv("WIDTH")) || wm_size[1] != atoi(getenv("HEIGHT"))){
			fprintf(stderr, "SDL_WMInfo returned non-native screen resolution - forcing\n");
			wm_size[0] = atoi(getenv("WIDTH"));
			wm_size[1] = atoi(getenv("HEIGHT"));
		}
	}

	screen = SDL_SetVideoMode(wm_size[0], wm_size[1], PB_D_PIXELS, SDL_HWSURFACE | SDL_DOUBLEBUF);
	sessionui_set_screen(screen);
	if ( screen == NULL ) {
		PRINT(stderr, "Couldn't set %d x %d x %d video mode: %s\n", wm_size[0], wm_size[1], PB_D_PIXELS, SDL_GetError());
		TTF_Quit();
		SDL_Quit();
		return TERM_FAILURE;
	}

	if(font_init(prefs->font_size) == TERM_FAILURE){
		PRINT(stderr, "Couldn't initialize font\n");
		TTF_Quit();
		SDL_Quit();
		return TERM_FAILURE;
	}

	/* Don't show the mouse icon */
	SDL_ShowCursor(SDL_DISABLE);

	/* we allocate as much buffer as we will ever need */
	int largest_dimension = screen->w > screen->h ? screen->w : screen->h;
	MAX_ROWS = largest_dimension / MIN_FONT_SIZE;
	MAX_COLS = largest_dimension / MIN_FONT_SIZE;
	TEXT_BUFFER_SIZE = MAX_ROWS * 2 + (prefs->scrollback_lines > 0 ? prefs->scrollback_lines : 0);
	fprintf(stderr, "Allocating %d rows and %d cols\n",TEXT_BUFFER_SIZE, MAX_COLS);

	/* initialize the number of rows and columns (the grid sits below the
	 * session bar) */
	content_top = sessionui_bar_height();
	rows = (screen->h - content_top) / text_height;
	cols = screen->w / text_width;

	/* Bring up the first terminal. Its buffers become the live engine
	 * state, and its shell is forked with the geometry just computed. */
	session_subsys_init();
	{
		session_t *first = session_create(0);
		if(first == NULL){
			PRINT(stderr, "Couldn't create the first session\n");
			TTF_Quit();
			SDL_Quit();
			return TERM_FAILURE;
		}
		cur_session = first;
		session_load_live(first);
		io_set_master(first->master_fd);
	}

	setup_screen_size(screen->w, screen->h);

	/* and set the last 'press' */
	clock_gettime(CLOCK_MONOTONIC, &metamode_last);

	return TERM_SUCCESS;
}

void uninit(){

	session_shutdown();

	SDL_DestroyMutex(input_mutex);

	font_uninit();
	SDL_FreeSurface(screen);

	TTF_Quit();
	SDL_Quit();

	destroy_preferences(prefs);

	io_uninit();
}

/* SGR 2: half intensity, approximated by blending towards the background */
static SDL_Color dim_color(SDL_Color fg, SDL_Color bg){
	SDL_Color out;
	out.r = (Uint8)(((int)fg.r + bg.r) / 2);
	out.g = (Uint8)(((int)fg.g + bg.g) / 2);
	out.b = (Uint8)(((int)fg.b + bg.b) / 2);
	out.unused = 0;
	return out;
}

SDL_Color adjust_color(SDL_Color in, struct font_style sty){
	int i;
	if(sty.style & TTF_STYLE_BOLD){
		for(i = 0; i < 8; ++i){
			if((in.b == term_colors[i].b) &&
			   (in.g == term_colors[i].g) &&
			   (in.r == term_colors[i].r)){
				in = term_colors[i+8];
				break;
			}
		}
	}
	return in;
}

/* invert the pixels in a screen rectangle (used to highlight a selection) */
static void invert_rect(SDL_Rect* r){
	if(SDL_LockSurface(screen) != 0){
		return;
	}
	int bpp = screen->format->BytesPerPixel;
	for(int yy = r->y; yy < r->y + r->h && yy < screen->h; ++yy){
		Uint8* row = (Uint8*)screen->pixels + yy * screen->pitch;
		for(int xx = r->x; xx < r->x + r->w && xx < screen->w; ++xx){
			Uint8* p = row + xx * bpp;
			Uint32 pix;
			switch(bpp){
			case 4: pix = *(Uint32*)p; break;
			case 2: pix = *(Uint16*)p; break;
			default: pix = *p; break;
			}
			Uint8 pr, pg, pb;
			SDL_GetRGB(pix, screen->format, &pr, &pg, &pb);
			pix = SDL_MapRGB(screen->format, 255 - pr, 255 - pg, 255 - pb);
			switch(bpp){
			case 4: *(Uint32*)p = pix; break;
			case 2: *(Uint16*)p = (Uint16)pix; break;
			default: *p = (Uint8)pix; break;
			}
		}
	}
	SDL_UnlockSurface(screen);
}

/* build the UTF-16 text of the current selection (caller frees). Trailing
 * spaces on each row are trimmed and rows joined with newlines. */
static UChar* selection_to_text(int* out_len){
	int r1, c1, r2, c2;
	if(!buf_sel_active()){
		*out_len = 0;
		return NULL;
	}
	buf_sel_get(&r1, &c1, &r2, &c2);
	int cap = (r2 - r1 + 1) * (cols + 1) + 1;
	UChar* text = calloc(cap, sizeof(UChar));
	if(text == NULL){ *out_len = 0; return NULL; }
	int n = 0;
	for(int row = r1; row <= r2; ++row){
		if(row < 0 || row >= TEXT_BUFFER_SIZE){ continue; }
		int start = (row == r1) ? c1 : 0;
		int end   = (row == r2) ? c2 : cols - 1;
		int last = start - 1;
		for(int col = start; col <= end && col < cols; ++col){
			if(buf->text[row][col].c != ' ' && buf->text[row][col].c != 0){
				last = col;
			}
		}
		for(int col = start; col <= last && col < cols; ++col){
			/* skip the trailing half of a double-width char; its glyph was
			 * already emitted from the lead cell */
			if(buf->text[row][col].wide == 2){
				continue;
			}
			UChar c = buf->text[row][col].c;
			text[n++] = (c == 0) ? ' ' : c;
			if(buf->text[row][col].c2 != 0){
				text[n++] = buf->text[row][col].c2;
			}
		}
		if(row != r2){
			text[n++] = '\n';
		}
	}
	*out_len = n;
	return text;
}

void render() {

	int offset;
	struct screenchar* sc;
	SDL_Surface* torender;
	UChar str[3];
	str[1] = 0;
	str[2] = 0;

	/* Set the background */
	SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, default_bg_color.r, default_bg_color.g, default_bg_color.b));

	/* the first buffer line to draw; view_offset > 0 means the user has
	 * scrolled back into history */
	int view_top = buf->top_line - buf->view_offset;

	for(int i = 0; i < rows; ++i){
		float x = 0.0;
		float y = content_top + text_height * (i);
		int bufline = i + view_top;

		for(int j = 0; j < cols; ++j){
			/* guard against screen rotations that push the bottom of the screen past the
			 * bottom of the buffer. */
			sc = (bufline >= 0 && bufline < TEXT_BUFFER_SIZE) ? &buf->text[bufline][j] : &blank_sc;
			/* trailing cell of a double-width char: the lead glyph already
			 * painted this column, so leave it and keep the columns aligned */
			if(sc->wide == 2){
				if(buf_sel_contains(bufline, j)){
					SDL_Rect selrect = {x, y, advance, text_height};
					invert_rect(&selrect);
				}
				x += advance;
				continue;
			}
			/* set only when the cache was full and handed us a surface
			 * of our own, which is freed again after the blit below */
			SDL_Surface* oneoff = NULL;
			if((sc->surface == NULL) && (sc->c != 0)){
				// we have added a new char, but not rendered it yet
				int shared = 1;
				SDL_Surface* glyph;
				SDL_Color fg = sc->style.fg_color;
				SDL_Color bg = sc->style.bg_color;
				if(sc->style.dim){
					fg = dim_color(fg, bg);
				}
				if(buf->inverse_video){
					glyph = glyph_render(sc->c, sc->c2, sc->style.style,
					        adjust_color(bg, sc->style), fg, &shared);
				} else {
					glyph = glyph_render(sc->c, sc->c2, sc->style.style,
					        adjust_color(fg, sc->style), bg, &shared);
				}
				if(glyph == NULL){
					PRINT(stderr, "Rendering failed for char %d\n", (int)sc->c);
				} else if(shared){
					sc->surface = glyph;   /* borrowed from the cache */
				} else {
					oneoff = glyph;
				}
			}
			if((sc->surface == NULL && oneoff == NULL) || flash){
				// no glyph here - render blank
				if(buf->inverse_video){
					torender = flash ? blank_surface : flash_surface;
				} else {
					torender = flash ? flash_surface : blank_surface;
				}
			} else {
				torender = sc->surface != NULL ? sc->surface : oneoff;
			}

			/* construct the destination rectangle */
			SDL_Rect destrect;
			destrect.x = x;
			destrect.y = y;
			destrect.w = torender->w;
			destrect.h = torender->h;
			if(SDL_BlitSurface(torender, NULL, screen, &destrect) != 0){
				PRINT(stderr, "Blit Failed: %s\n", SDL_GetError());
			}
			/* highlight selected cells by inverting them */
			if(buf_sel_contains(bufline, j)){
				SDL_Rect selrect = {x, y, advance, text_height};
				invert_rect(&selrect);
			}

			if(oneoff != NULL){
				SDL_FreeSurface(oneoff);
			}
			x += advance;
		}
	}

	/* scrollback search hits are painted as one batch over the finished
	 * rows (see sessionui.c) */
	sessionui_draw_search_highlights(screen, content_top);

	if (draw_cursor && buf->view_offset == 0){
		// draw the cursor
		/* Free the old cursor if we have one */
		SDL_FreeSurface(inv_cursor);
		inv_cursor = NULL;
		/* Get the character under the cursor */

		int drawcols = buf->col;
		if(buf->col == cols){
			// Don't draw off the edge - also make backspace from the right margin work 'right'
			drawcols -= 1;
		}

		sc = &buf->text[buf->line][drawcols];
		if(sc->c){
			str[0] = sc->c;
			str[1] = sc->c2;
			TTF_Font *rfont = font_for_char(sc->c);
			TTF_SetFontStyle(rfont, sc->style.style);
			if(buf->inverse_video){
				inv_cursor = TTF_RenderUNICODE_Shaded(rfont, str, adjust_color(sc->style.fg_color, sc->style), sc->style.bg_color);
			} else {
				inv_cursor = TTF_RenderUNICODE_Shaded(rfont, str, adjust_color(sc->style.bg_color, sc->style), sc->style.fg_color);
			}
			if(inv_cursor == NULL){
				PRINT(stderr, "Rendering failed for char %d\n", (int)sc->c);
			}
		}
		cursor_x = drawcols;
		cursor_y = buf->line - buf->top_line;

		SDL_Rect destrect;
		destrect.x = cursor_x * advance;
		destrect.y = content_top + cursor_y * text_height;
		destrect.w = cursor->w;
		destrect.h = cursor->h;
		int cursor_shape = ecma48_cursor_shape();
		if(cursor_shape >= 3){
			/* underline (3/4) or bar (5/6): draw the glyph normally, then
			 * a solid strip in the text colour */
			sc = &buf->text[buf->line][drawcols];
			if(sc->c && sc->surface != NULL){
				SDL_BlitSurface(sc->surface, NULL, screen, &destrect);
			}
			SDL_Rect bar;
			if(cursor_shape <= 4){
				bar.x = destrect.x;         bar.w = advance;
				bar.h = text_height / 8 + 1; bar.y = destrect.y + text_height - bar.h;
			} else {
				bar.x = destrect.x; bar.y = destrect.y;
				bar.w = advance / 6 + 1; bar.h = text_height;
			}
			SDL_FillRect(screen, &bar,
			    SDL_MapRGB(screen->format, default_text_color.r, default_text_color.g, default_text_color.b));
		} else if(inv_cursor != NULL){
			SDL_BlitSurface(inv_cursor, NULL, screen, &destrect);
		} else {
			SDL_BlitSurface(buf->inverse_video ? blank_surface: cursor, NULL, screen, &destrect);
		}
	}

	if(metamode && metamode_cursor != NULL){
		/* draw the metamode cursor */
		SDL_Rect destrect;
		destrect.x = (cols-1) * advance;
		destrect.y = content_top;
		destrect.w = metamode_cursor->w;
		destrect.h = metamode_cursor->h;
		SDL_BlitSurface(metamode_cursor, NULL, screen, &destrect);
	}

	if(buf->view_offset > 0 && scroll_indicator != NULL){
		/* show that we are scrolled back into history */
		SDL_Rect destrect;
		destrect.x = (cols-1) * advance;
		destrect.y = content_top;
		destrect.w = scroll_indicator->w;
		destrect.h = scroll_indicator->h;
		SDL_BlitSurface(scroll_indicator, NULL, screen, &destrect);
	}

	if(pointer_visible && buf->view_offset == 0 &&
	   ecma48_mouse_tracking() && mouse_pointer != NULL){
		/* mouse pointer for tracking apps, snapped to its cell */
		SDL_Rect destrect;
		destrect.x = (pointer_x / advance) * advance;
		destrect.y = content_top + (pointer_y / text_height) * text_height;
		destrect.w = mouse_pointer->w;
		destrect.h = mouse_pointer->h;
		SDL_BlitSurface(mouse_pointer, NULL, screen, &destrect);
	}

	if(vmodifiers & KEYMOD_CTRL){
		SDL_Rect destrect;
		destrect.x = (cols-1) * advance;
		destrect.y = content_top + 1 * text_height;
		destrect.w = ctrl_key_indicator->w;
		destrect.h = ctrl_key_indicator->h;
		SDL_BlitSurface(ctrl_key_indicator, NULL, screen, &destrect);
	}

	if(vmodifiers & KEYMOD_ALT){
		SDL_Rect destrect;
		destrect.x = (cols-1) * advance;
		destrect.y = content_top + 2 * text_height;
		destrect.w = alt_key_indicator->w;
		destrect.h = alt_key_indicator->h;
		SDL_BlitSurface(alt_key_indicator, NULL, screen, &destrect);
	}

	if(vmodifiers & KEYMOD_SHIFT){
		SDL_Rect destrect;
		destrect.x = (cols-1) * advance;
		destrect.y = content_top + 3 * text_height;
		destrect.w = shift_key_indicator->w;
		destrect.h = shift_key_indicator->h;
		SDL_BlitSurface(shift_key_indicator, NULL, screen, &destrect);
	}

	if (altsym_lock || altsym_hold) {
		SDL_Rect destrect;
		destrect.x = (cols-1) * advance;
		destrect.y = content_top + 3 * text_height;
		destrect.w = shift_key_indicator->w;
		destrect.h = shift_key_indicator->h;
		SDL_BlitSurface(altsym_indicator, NULL, screen, &destrect);
	}

	if (pinyin_enabled()) {
		/* input bar on the reserved bottom row: "[鹤] ni hc 1.你 2.尼 ..." */
		char st8[1024];
		UChar st16[512];
		int32_t slen = 0;
		UErrorCode serr = U_ZERO_ERROR;
		SDL_Rect bar;
		bar.x = 0;
		bar.w = screen->w;
		bar.h = text_height;
		bar.y = screen->h - text_height;
		if (current_symmenu != NULL && current_symmenu->surface != NULL) {
			bar.y -= current_symmenu->surface->h;
		}
		SDL_FillRect(screen, &bar, SDL_MapRGB(screen->format, 0x18, 0x30, 0x60));
		pinyin_status(st8, sizeof(st8));
		u_strFromUTF8(st16, 511, &slen, st8, -1, &serr);
		if (U_SUCCESS(serr) && slen > 0) {
			SDL_Color fg = {0xff, 0xff, 0xff, 0};
			SDL_Color bg = {0x18, 0x30, 0x60, 0};
			int x = 0, i;
			/* render glyph by glyph so CJK uses the CJK/fallback font */
			for (i = 0; i < slen && x < screen->w; ++i) {
				UChar g[2];
				SDL_Surface *gs;
				SDL_Rect gr;
				g[0] = st16[i];
				g[1] = 0;
				TTF_SetFontStyle(font_for_char(g[0]), TTF_STYLE_NORMAL);
				gs = TTF_RenderUNICODE_Shaded(font_for_char(g[0]), g, fg, bg);
				if (gs == NULL) {
					continue;
				}
				gr.x = x;
				gr.y = bar.y;
				gr.w = gs->w;
				gr.h = gs->h;
				SDL_BlitSurface(gs, NULL, screen, &gr);
				x += gs->w;
				SDL_FreeSurface(gs);
			}
		}
	}

	if ((current_symmenu != NULL) && (current_symmenu->surface != NULL)) {
		/* blit symmenu surface */
		SDL_Rect destrect;
		destrect.w = current_symmenu->surface->w;
		destrect.h = current_symmenu->surface->h;
		destrect.x = 0;
		destrect.y = screen->h - current_symmenu->surface->h;;
	
		if (SDL_BlitSurface(current_symmenu->surface, NULL, screen, &destrect) != 0) {
			PRINT(stderr, "Symmenu blit failed: %s\n", SDL_GetError());
			return;
		}
	}

	/* session chrome (tab bar, switcher overlay, prompts) sits on top of
	 * the terminal and is part of the same frame */
	sessionui_draw(screen);

	SDL_Flip(screen);

	if(flash){
		/* turn it off */
		flash = 0;
		/* write to the input pipe so we run again */
		indicate_event_input();
	}

}


extern int SDL_PrivateQuit(void);

/* SIGCHLD only records which children went away: reaping inside the
 * handler is fine (waitpid is async-signal-safe), but touching the
 * session list or the UI from here is not. The UI thread calls
 * session_handle_sigchld() once per event-loop iteration to fold the
 * results into the session table. */
void sig_child(int signo){
	int old_errno = errno;
	(void)signo;

	for(;;){
		int status;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		int n;
		if(pid <= 0){ break; }
		n = session_reaped_count;
		if(n < 32){
			session_reaped_pid[n] = pid;
			session_reaped_status[n] = status;
			session_reaped_count = n + 1;
		}
	}
	/* make the UI thread come back and look */
	indicate_event_input();

	errno = old_errno;
}

/* pty fds handed over by session_close(); drained by the render thread */
#define CLOSE_QUEUE_MAX 16
static int close_queue[CLOSE_QUEUE_MAX];
static int close_queue_n = 0;

void queue_close_fd(int fd){
	if(fd < 0){ return; }
	if(close_queue_n < CLOSE_QUEUE_MAX){
		close_queue[close_queue_n++] = fd;
	} else {
		/* queue full: leaking an fd is better than yanking it out from
		 * under a select() in the other thread */
	}
}

/* How many UChars of child output we parse before letting go of the input
 * lock and drawing a frame. Without a cap, a child that keeps producing
 * (a full-screen TUI repainting under tmux, `yes`, a large cat) keeps this
 * thread inside the lock indefinitely: the main thread needs the same lock
 * to service keys and the sym menu, and render() below never gets to run
 * either, so the app looks completely wedged even though it is making
 * progress. Parsing stays ahead of any real terminal output at this size. */
#define DRAIN_UCHARS_PER_FRAME (READ_BUFFER_SIZE * 2)
/* Minimum gap between frames while output keeps arriving. Rendering is by
 * far the most expensive thing here (a glyph render per changed cell plus a
 * full-screen blit), so coalescing bursts into ~60 fps is both faster
 * overall and what keeps the UI thread's lock waits short. */
#define FRAME_INTERVAL_MS 16

/* This function is run in an SDL_Thread, and will check
 * for either input event indication or data from the
 * shell, then run the render loop
 */
int run_render(void* data){

	fd_set fds;
	char ev_buf[256];
	int n = 0;
	UChar lbuf[READ_BUFFER_SIZE];
	ssize_t num_chars = 0;
	int pending = 0;          /* parsed output not yet drawn */
	Uint32 last_render = 0;
	Uint32 now, since, wait_ms;
	struct timeval tv;

	while(!exit_application){
		int master;
		int maxfd = event_pipe[0];
		int i;

		/* Everything that touches the session table happens under the
		 * input lock; only select() itself runs unlocked. The fd set is
		 * built here and re-checked after select(), so a session that is
		 * closed while we wait cannot be dereferenced (its fd stays open
		 * until this thread closes it from close_queue). */
		lock_input_lowpri();
		while(close_queue_n > 0){
			close(close_queue[--close_queue_n]);
		}
		/* Fold reaped children in here as well as in the UI thread: the
		 * signal only writes to event_pipe, and SDL_WaitEvent does not
		 * wake on that, so the main loop may not run for a long time. */
		if(session_handle_sigchld() > 0){
			pending = 1;
			if(session_count_alive() == 0 && session_count == 1){
				/* the only shell is gone: leave, like the single-session
				 * app always did */
				SDL_Event quit_event;
				memset(&quit_event, 0, sizeof(quit_event));
				quit_event.type = SDL_QUIT;
				SDL_PushEvent(&quit_event);
			}
		}
		master = io_get_master();
		FD_ZERO(&fds);
		FD_SET(event_pipe[0], &fds);
		if(master >= 0){
			FD_SET(master, &fds);
			if(master > maxfd){ maxfd = master; }
		}
		/* Background sessions are only watched until they have something
		 * to read - then they are marked "has output" and dropped from
		 * the set. Their pty is not drained (the child blocks instead,
		 * see session.h); the backlog is read when the user switches to
		 * that tab. Leaving a readable fd in the set would spin this
		 * loop at 100% CPU. */
		for(i = 0; i < session_count; ++i){
			session_t *s = sessions[i];
			if(s == cur_session || s->master_fd < 0 || s->has_output){ continue; }
			FD_SET(s->master_fd, &fds);
			if(s->master_fd > maxfd){ maxfd = s->master_fd; }
		}
		unlock_input();

		/* With nothing to draw, block until something happens. With a frame
		 * owed, wait no longer than the frame deadline. */
		wait_ms = 0;
		if(pending){
			since = SDL_GetTicks() - last_render;
			wait_ms = since >= FRAME_INTERVAL_MS ? 0 : FRAME_INTERVAL_MS - since;
		}
		tv.tv_sec = 0;
		tv.tv_usec = wait_ms * 1000;
		n = select(maxfd + 1, &fds, NULL, NULL, pending ? &tv : NULL);
		if(n < 0){
			if(errno != EINTR){
				printf("Error calling select on inputs: %d\n", errno);
			}
		} else if(n > 0) {
			lock_input_lowpri();
			for(i = 0; i < session_count; ++i){
				session_t *s = sessions[i];
				if(s == cur_session || s->master_fd < 0 || s->has_output){ continue; }
				if(FD_ISSET(s->master_fd, &fds)){
					s->has_output = 1;
					pending = 1;
				}
			}
			if(master >= 0 && FD_ISSET(master, &fds) && io_get_master() == master){
				ssize_t budget = DRAIN_UCHARS_PER_FRAME;
				int eof = 0;
				session_t *s = cur_session;
				// Read anything from the child, up to this frame's budget
				while(budget > 0){
					errno = 0;
					num_chars = io_read_master(lbuf, READ_BUFFER_SIZE);
					if(num_chars == 0){ eof = 1; break; }
					if(num_chars < 0){
						/* EAGAIN/EINTR: nothing (complete) to read right now.
						 * Anything else (EIO after the child exits) is the
						 * real end of the session. */
						if(errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK){
							eof = 1;
						}
						break;
					}
					ecma48_filter_text(lbuf, num_chars);
					budget -= num_chars;
				}
				if(eof && s != NULL){
					FILE *lf = term49_keylog_fp();
					if(lf != NULL){
						fprintf(lf, "session eof fd=%d errno=%d alive=%d\n", master, errno, s->alive);
						fflush(lf);
					}
				}
				if(s != NULL){
					if(s->last_output_ticks == 0){
						/* Re-assert the window size once the child first speaks.
						 * The initial SIGWINCH from sdl_init can be delivered to
						 * the login shell before it exec()s into a slower shell
						 * (e.g. `exec zsh`), so that shell never gets a size event
						 * and its prompt can fail to paint until one arrives. */
						set_tty_window_size();
					}
					s->last_output_ticks = SDL_GetTicks();
					if(eof){
						s->alive = 0;
						if(s->master_fd >= 0){
							/* child is gone: stop selecting this fd, or select()
							 * would report EOF forever */
							queue_close_fd(s->master_fd);
							s->master_fd = -1;
							io_set_master(-1);
						}
					}
				}
				pending = 1;
			}
			unlock_input();
			if(FD_ISSET(event_pipe[0], &fds)){
				// Just read the stuff and throw it away (the fd is
				// non-blocking, so this drains and then returns -1)
				while(read(event_pipe[0], (void*)ev_buf, sizeof(ev_buf)) > 0){}
				pending = 1;
			}
		}
		/* Hand the main thread the lock it may have been waiting on for the
		 * whole drain above: the mutex is not fair, and this thread is about
		 * to ask for it again. */
		sched_yield();
		now = SDL_GetTicks();
		if(pending && (Uint32)(now - last_render) >= FRAME_INTERVAL_MS){
			PRINT(stderr, "Render Loop\n");
			lock_input_lowpri();
			render();
			unlock_input();
			last_render = now;
			pending = 0;
		}
	}
	/* never reached */
	return 0;
}

int main(int argc, char **argv) {
	int rc;

	/* Switch to our home directory */
	char* home = getenv("HOME");
	if(home != NULL){ chdir(home); }
	
	prefs = read_preferences(PREFS_FILE_PATH);
	/* Backspace byte: DEL by default, ^H on request. The same byte is
	 * programmed as the pty's erase character (see session.c). */
	ecma48_set_backspace((prefs->backspace != NULL &&
	                      strcmp(prefs->backspace, "bs") == 0) ? 0x08 : 0x7f);
	/* input method from the config: "full" = 全拼, anything else = 小鹤双拼 */
	pinyin_set_full(prefs->pinyin_mode != NULL &&
	                strcmp(prefs->pinyin_mode, "full") == 0);
	if (is_passport()) {
		prefs->auto_show_vkb = 1;
	}

	/* set auto orientation */
	setenv("AUTO_ORIENTATION", "1", 0);

	/* TERM / terminfo / LC_CTYPE / private PATH for every shell we fork */
	session_setup_environment();

	/* the session chrome has to know the font metrics before sdl_init
	 * sizes the grid against it */
	sessionui_init(prefs, NULL);

	/* Initialize IO */
	if (TERM_SUCCESS != io_init(prefs)) {
		PRINT(stderr, "Unable to initialize IO\n");
		uninit();
		return TERM_FAILURE;
	}

	/* Install signal handler for SIGCHILD */
	struct sigaction act;
	act.sa_handler = &sig_child;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &act, NULL) < 0) {
		PRINT(stderr, "sigaction failed\n");
		uninit();
		return TERM_FAILURE;
	}

	/* initialize SDL video etc */
	if (TERM_SUCCESS != sdl_init()) {
		PRINT(stderr, "Unable to initialize SDL\n");
		uninit();
		return TERM_FAILURE;
	}

	/* render the symmenus */
	prefs->main_symmenu->surface = render_symmenu(screen, prefs, prefs->main_symmenu);
	if (prefs->action_menu != NULL) {
		prefs->action_menu->surface = render_symmenu(screen, prefs, prefs->action_menu);
	}

	/* Pinyin (xiaohe shuangpin) is on by default; TERM49_PINYIN=0 gives a
	 * plain terminal back. The bar is toggled from the Menu popup. */
	if (getenv("TERM49_PINYIN") == NULL || atoi(getenv("TERM49_PINYIN")) != 0) {
		if (!pinyin_enabled()) {
			pinyin_toggle();
		}
		pinyin_set_bar(pinyin_enabled());
	}
	for (char c = 'a'; c <= 'z'; ++c) {
		size_t idx = (size_t)(c - 'a');

		// lowercase
		symmenu_t *m = prefs->accent_menus[idx][0];
		if (m->entries[1].to != NULL) {
			m->surface = render_symmenu(screen, prefs, m);
		}

		// uppercase
		m = prefs->accent_menus[idx][1];
		if (m->entries[1].to != NULL) {
			m->surface = render_symmenu(screen, prefs, m);
		}
	}

	if (prefs->auto_show_vkb) {
		virtualkeyboard_show();
	}

	/* start up main event loop */
	SDL_Thread *render_thread = SDL_CreateThread(run_render, NULL);
	while (!exit_application) {

		//Request and process all available events
		SDL_Event event;

		SDL_WaitEvent(&event);
		lock_input();
		switch (event.type) {
		case SDL_QUIT:
			exit_application = 1;
			break;
		case SDL_VIDEORESIZE:
			rescreen(event.resize.w, event.resize.h);
			break;
		case SDL_KEYDOWN:
			{
				fprintf(stderr, "SDL_KEYDOWN\n");
				UChar uc;
				char sdlkey = event.key.keysym.sym;
				uc = (UChar)sdlkey;
				io_write_master(&uc, 1);
			}
			break;
		case SDL_SYSWMEVENT:
			{
				bps_event_t* bps_event = event.syswm.msg->event;
				int screene_type;
				int domain = bps_event_get_domain(bps_event);
				PRINT(stderr, "Unhandled SYSWMEVENT: %d\n", domain);
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		{
			int ty = event.button.y - content_top;
			touch_down_ticks = SDL_GetTicks();
			touch_down_x = event.button.x;
			touch_down_y = event.button.y;
			/* the tab bar / switcher / prompt gets first refusal */
			if(sessionui_touch_down(event.button.x, event.button.y)){
				break;
			}
			handle_mousedown(event.button.x, event.button.y);
			if(event.button.which == 0 && (altsym_lock || altsym_hold)){
				/* alt + touch begins a text selection */
				int bufrow = ty / text_height + buf->top_line - buf->view_offset;
				int bufcol = event.button.x / advance;
				buf_sel_begin(bufrow, bufcol);
				touch_selecting = 1;
			} else if(event.button.which == 0 && ecma48_mouse_tracking() && buf->view_offset == 0){
				/* in mouse-tracking apps a touch is a left mouse press */
				touch_mouse_cell[0] = event.button.x / advance + 1;
				touch_mouse_cell[1] = ty / text_height + 1;
				ecma48_send_mouse_event(0, 1, touch_mouse_cell[0], touch_mouse_cell[1]);
			} else if(event.button.which == 0){
				/* a plain tap clears any existing selection */
				buf_sel_cancel();
			}
			break;
		}
		case SDL_MOUSEMOTION:
		{
			int ty = event.motion.y - content_top;
			if(sessionui_touch_motion(event.motion.x, event.motion.y)){
				break;
			}
			if(event.motion.which == 0 && event.motion.state){
				if(touch_selecting){
					/* extend the selection to the finger */
					int bufrow = ty / text_height + buf->top_line - buf->view_offset;
					int bufcol = event.motion.x / advance;
					buf_sel_update(bufrow, bufcol);
				} else if(touch_mouse_cell[0] > 0){
					/* mouse-tracking app: report the drag */
					int mcol = event.motion.x / advance + 1;
					int mrow = ty / text_height + 1;
					int mmode = ecma48_mouse_tracking();
					if((mcol != touch_mouse_cell[0] || mrow != touch_mouse_cell[1]) &&
					   (mmode == 1002 || mmode == 1003)){
						ecma48_send_mouse_event(32, 1, mcol, mrow);
					}
					touch_mouse_cell[0] = mcol;
					touch_mouse_cell[1] = mrow;
				} else {
					/* dragging a finger scrolls the view; content
					 * follows the finger (first contact only) */
					touch_scroll_acc += event.motion.yrel;
					int scroll_lines = touch_scroll_acc / text_height;
					if(scroll_lines != 0){
						buf_scroll_view(scroll_lines);
						touch_scroll_acc -= scroll_lines * text_height;
					}
				}
			}
			break;
		}
		case SDL_MOUSEBUTTONUP:
		{
			int ty = event.button.y - content_top;
			if(sessionui_touch_up(event.button.x, event.button.y)){
				touch_down_ticks = 0;
				touch_scroll_acc = 0;
				break;
			}
			if(event.button.which == 0 && !touch_selecting && touch_mouse_cell[0] <= 0 &&
			   !ecma48_mouse_tracking() && touch_down_ticks > 0 &&
			   (SDL_GetTicks() - touch_down_ticks) < 400 &&
			   abs(event.button.x - touch_down_x) < 12 &&
			   abs(event.button.y - touch_down_y) < 12){
				/* a short tap that did not scroll or select: open a URL
				 * under the finger, if there is one */
				int bufrow = ty / text_height + buf->top_line - buf->view_offset;
				int bufcol = event.button.x / advance;
				if(sessionui_open_url_at(bufrow, bufcol)){
					touch_down_ticks = 0;
					break;
				}
			}
			touch_down_ticks = 0;
			if(event.button.which == 0 && touch_selecting){
				/* finishing an alt+touch drag copies the selection */
				int sel_len = 0;
				UChar* sel = selection_to_text(&sel_len);
				if(sel != NULL && sel_len > 0){
					io_copy_to_clipboard(sel, sel_len);
				}
				free(sel);
				touch_selecting = 0;
			} else if(event.button.which == 0 && touch_mouse_cell[0] > 0){
				ecma48_send_mouse_event(0, 0,
				        event.button.x / advance + 1, ty / text_height + 1);
				touch_mouse_cell[0] = -1;
				touch_mouse_cell[1] = -1;
			}
			touch_scroll_acc = 0;
			break;
		}
		case SDL_USEREVENT:
			/* posted by SDL for the app-menu request (swipe down from the
			 * top bezel; the Classic menu key may arrive this way too):
			 * toggle persistent alt mode */
			altsym_hold_toggle();
			break;
		case SDL_ACTIVEEVENT:
			handle_activeevent(event.active.gain, event.active.state);
			break;
		default:
			PRINT(stderr, "Unknown Event: %d\n", event.type);
			break;
		}

		/* fold reaped children into the session table, then let the UI
		 * catch up with anything that changed (a tab height change from a
		 * prompt, a session that just exited, ...) */
		session_handle_sigchld();
		if(session_exit_requested){
			exit_application = 1;
		}
		if(cur_session != NULL && !cur_session->alive &&
		   session_count_alive() == 0 && session_count == 1){
			/* the only shell exited: keep the historic behaviour and leave */
			exit_application = 1;
		}
		if(sessionui_take_layout_change()){
			/* Only re-lay-out when the bar actually took or gave back
			 * screen space: opening the switcher overlay must not send
			 * SIGWINCH to every shell (vim/tmux would redraw for nothing). */
			if(sessionui_bar_height() != content_top || rows == 0){
				int sh = screen->h;
				if(virtualkeyboard_visible){ sh -= get_virtualkeyboard_height(); }
				setup_screen_size(screen->w, sh);
				if(cur_session != NULL){ buf_reset_view(); }
			}
			indicate_event_input();
		}
		indicate_event_input();
		unlock_input();
	}

	PRINT(stderr, "Exiting run loop\n");
	/* Ask the render thread to leave instead of killing it: SDL_KillThread
	 * cancels it wherever it happens to be, which is exactly the sort of
	 * thing that turns a clean exit into an abort() inside malloc when the
	 * heap is torn down a moment later. The pipe write wakes it out of
	 * select(). */
	exit_application = 1;
	indicate_event_input();
	SDL_WaitThread(render_thread, NULL);
	virtualkeyboard_hide();
	uninit();

	return 0;
}
