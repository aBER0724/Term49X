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

/*
 * Session chrome: tab bar, switcher, rename prompt, scrollback search.
 *
 * The bar is the primary session control on a touch device: one tab per
 * shell, a "+" to open another one, a hamburger to open the full-screen
 * switcher (rename / close / jump), and a horizontal swipe over the bar
 * to flick through tabs. The same actions are available from metamode
 * keys (see main.c) and from the switcher overlay.
 *
 * Drawing happens after the terminal cells are painted and before
 * SDL_Flip, so everything here is painted on top of the terminal and
 * simply reserves the top `sessionui_bar_height()` pixels of the grid.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/keycodes.h>
#include <unicode/utf.h>
#include <unicode/ustring.h>
#include <bps/navigator.h>
#include <bps/virtualkeyboard.h>

#include "SDL.h"
#include "SDL_ttf.h"

#include "terminal.h"
#include "types.h"
#include "preferences.h"
#include "buffer.h"
#include "ecma48.h"
#include "io.h"
#include "session.h"
#include "sessionui.h"

/* from main.c */
extern TTF_Font* app_font();
extern void indicate_event_input();

/* from buffer.c */
extern buf_t* buf;
extern int rows;
extern int cols;
extern int TEXT_BUFFER_SIZE;

#define UI_BAR_BG        { 28,  28,  30}
#define UI_BAR_BG_DIM    { 22,  22,  24}
#define UI_TAB_ACTIVE    { 70,  70,  78}
#define UI_TAB_INACTIVE  { 38,  38,  42}
#define UI_TAB_DEAD      { 60,  30,  30}
#define UI_TAB_TEXT      {255, 255, 255}
#define UI_TAB_TEXT_DIM  {150, 150, 155}
#define UI_ACCENT        { 90, 170, 255}
#define UI_PANEL_BG      { 18,  18,  20}
#define UI_PANEL_ROW     { 34,  34,  38}
#define UI_PANEL_ROW_SEL { 60,  60,  70}
#define UI_WARN          { 230, 120, 120}

#define UI_PAD 4
#define MAX_MATCHES 256

enum ui_mode {
  UI_MODE_NONE = 0,
  UI_MODE_SWITCHER,
  UI_MODE_RENAME,
  UI_MODE_SEARCH
};

typedef struct ui_match {
  int row;
  int col;
  int len;
} ui_match_t;

static pref_t *ui_prefs = NULL;
static SDL_Surface *ui_screen = NULL;

static int ui_text_height = 20;
static int ui_advance = 10;
static int ui_bar_visible = 1;
static int ui_layout_changed = 1;

static int ui_mode = UI_MODE_NONE;

/* prompt editing */
static char ui_edit_name[SESSION_NAME_LEN];
static int  ui_edit_len = 0;
static int  ui_edit_pos = 0;
static session_t *ui_edit_target = NULL;

/* search */
static UChar ui_query[48];
static int   ui_query_len = 0;
static ui_match_t ui_matches[MAX_MATCHES];
static int   ui_match_count = 0;
static int   ui_match_cur = -1;

/* touch tracking for bar swipes / tap detection */
static int ui_touch_x = 0, ui_touch_y = 0;
static int ui_touch_started = 0;
static int ui_touch_moved = 0;
static int ui_touch_in_bar = 0;

/* hit rectangles, computed by ui_layout() */
static SDL_Rect ui_tab_r[SESSION_MAX];
static SDL_Rect ui_new_r, ui_menu_r;
static SDL_Rect ui_sw_row_r[SESSION_MAX];
static SDL_Rect ui_sw_ren_r[SESSION_MAX];
static SDL_Rect ui_sw_cls_r[SESSION_MAX];
static SDL_Rect ui_sw_new_r;
static SDL_Rect ui_sw_close_r;
static SDL_Rect ui_prompt_ok_r, ui_prompt_cancel_r;
static SDL_Rect ui_search_prev_r, ui_search_next_r, ui_search_close_r;

static Uint32 ui_color(SDL_Color c){
  return SDL_MapRGB(ui_screen->format, c.r, c.g, c.b);
}

static void ui_fill(SDL_Rect r, SDL_Color c){
  SDL_FillRect(ui_screen, &r, ui_color(c));
}

static int ui_point_in(SDL_Rect r, int x, int y){
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* same, but with a forgiving margin around the target */
static int ui_point_in_pad(SDL_Rect r, int x, int y, int pad){
  return x >= r.x - pad && x < r.x + r.w + pad &&
         y >= r.y - pad && y < r.y + r.h + pad;
}

static int ui_utf8_to_uchar(const char *utf8, UChar *out, int maxout){
  int n;
  if(utf8 == NULL){ out[0] = 0; return 0; }
  n = (int)io_read_utf8_string(utf8, strlen(utf8), out);
  if(n < 0){ n = 0; }
  if(n > maxout - 1){ n = maxout - 1; }
  out[n] = 0;
  return n;
}

/* Draw a single line of text clipped to r, vertically centred. */
static void ui_text(SDL_Rect r, const char *utf8, SDL_Color fg, SDL_Color bg){
  UChar text[256];
  SDL_Surface *s;
  SDL_Rect src, dst;
  int n;

  if(r.w <= 0 || r.h <= 0){ return; }
  n = ui_utf8_to_uchar(utf8, text, 256);
  if(n <= 0){ return; }
  s = TTF_RenderUNICODE_Shaded(app_font(), text, fg, bg);
  if(s == NULL){ return; }

  src.x = 0;
  src.y = 0;
  src.w = s->w < r.w ? s->w : r.w;
  src.h = s->h < r.h ? s->h : r.h;

  dst.x = r.x;
  dst.y = r.y + (r.h - s->h) / 2;
  if(dst.y < r.y){ dst.y = r.y; }
  dst.w = src.w;
  dst.h = src.h;
  SDL_BlitSurface(s, &src, ui_screen, &dst);
  SDL_FreeSurface(s);
}

static void ui_text_uchar(SDL_Rect r, const UChar *text, int len, SDL_Color fg, SDL_Color bg){
  UChar tmp[256];
  SDL_Surface *s;
  SDL_Rect src, dst;
  int i, n = len < 255 ? len : 255;

  if(r.w <= 0 || r.h <= 0 || n <= 0){ return; }
  for(i = 0; i < n; ++i){ tmp[i] = text[i]; }
  tmp[n] = 0;
  s = TTF_RenderUNICODE_Shaded(app_font(), tmp, fg, bg);
  if(s == NULL){ return; }
  src.x = 0; src.y = 0;
  src.w = s->w < r.w ? s->w : r.w;
  src.h = s->h < r.h ? s->h : r.h;
  dst.x = r.x;
  dst.y = r.y + (r.h - s->h) / 2;
  if(dst.y < r.y){ dst.y = r.y; }
  dst.w = src.w; dst.h = src.h;
  SDL_BlitSurface(s, &src, ui_screen, &dst);
  SDL_FreeSurface(s);
}

static void ui_put_pixel(int x, int y, Uint32 pix, int bpp){
  Uint8 *row;
  if(x < 0 || y < 0 || x >= ui_screen->w || y >= ui_screen->h){ return; }
  row = (Uint8*)ui_screen->pixels + y * ui_screen->pitch + x * bpp;
  switch(bpp){
    case 4: *(Uint32*)row = pix; break;
    case 2: *(Uint16*)row = (Uint16)pix; break;
    default: *row = (Uint8)pix; break;
  }
}

/* A crisp X, drawn as two thick diagonals: a glyph-sized 'x' inside a
 * 44 px button is not readable and does not fill the target. */
static void ui_draw_cross(SDL_Rect r, SDL_Color c){
  int bpp = ui_screen->format->BytesPerPixel;
  Uint32 pix = ui_color(c);
  int n = r.w < r.h ? r.w : r.h;
  int in = n * 22 / 100;          /* inset */
  int len = n - in * 2;
  int thick = n / 8;
  int t, i;

  if(len <= 0 || thick < 1){ thick = 1; }
  if(SDL_LockSurface(ui_screen) != 0){ return; }
  for(i = 0; i < len; ++i){
    int y = r.y + in + i;
    int xa = r.x + in + i;
    int xb = r.x + r.w - in - 1 - i;
    for(t = 0; t < thick; ++t){
      ui_put_pixel(xa, y + t, pix, bpp);
      ui_put_pixel(xb, y + t, pix, bpp);
    }
  }
  SDL_UnlockSurface(ui_screen);
}

static void ui_draw_plus(SDL_Rect r, SDL_Color c){
  int bpp = ui_screen->format->BytesPerPixel;
  Uint32 pix = ui_color(c);
  int n = r.w < r.h ? r.w : r.h;
  int len = n * 60 / 100;
  int thick = n / 9;
  int x0 = r.x + (r.w - len) / 2;
  int y0 = r.y + (r.h - len) / 2;
  int i;

  if(thick < 2){ thick = 2; }
  if(SDL_LockSurface(ui_screen) != 0){ return; }
  for(i = 0; i < len; ++i){
    int t;
    for(t = 0; t < thick; ++t){
      ui_put_pixel(x0 + i, y0 + len / 2 - thick / 2 + t, pix, bpp);
      ui_put_pixel(x0 + len / 2 - thick / 2 + t, y0 + i, pix, bpp);
    }
  }
  SDL_UnlockSurface(ui_screen);
}

static void ui_draw_menu_button(SDL_Rect r, SDL_Color fg){
  int i;
  int h = r.h / 3;
  int w = r.w * 3 / 5;
  int x = r.x + (r.w - w) / 2;
  int y = r.y + (r.h - h * 3) / 2 + 1;
  for(i = 0; i < 3; ++i){
    SDL_Rect line = {x, y + i * h, w, (h > 2 ? 2 : 1)};
    ui_fill(line, fg);
  }
}

/* ---- layout ---------------------------------------------------------- */

/* Touch targets have to be thumb-sized: a 720x720 screen with ~26 px text
 * leaves very little room, and the first version's 32 px close button was
 * simply not hittable. The bar is one and a half text rows and every
 * button is at least UI_TOUCH px on its short side. */
#define UI_BAR_EXTRA   6
#define UI_TOUCH       44
#define UI_TOUCH_PAD   6

static int ui_bar_h(){
  int h = ui_text_height + ui_text_height / 2 + UI_BAR_EXTRA;
  if(h < UI_TOUCH){ h = UI_TOUCH; }
  if(ui_mode == UI_MODE_RENAME || ui_mode == UI_MODE_SEARCH){
    return h;
  }
  return ui_bar_visible ? h : 0;
}

/* side of a square button inside the bar */
static int ui_bar_btn(){
  int b = ui_bar_h() - 8;
  if(b < UI_TOUCH - 4){ b = UI_TOUCH - 4; }
  return b;
}

static void ui_layout_bar(){
  int i;
  int x = UI_PAD;
  int n = session_count;
  int slot, avail;
  int btn = ui_bar_btn();
  int bh = ui_bar_h() - 8;
  int by = (ui_bar_h() - bh) / 2;

  ui_menu_r.x = UI_PAD;
  ui_menu_r.y = by;
  ui_menu_r.w = btn;
  ui_menu_r.h = bh;

  avail = ui_screen->w - x - btn - UI_PAD * 2 - btn - UI_PAD;
  if(avail < 1){ avail = 1; }
  slot = n > 0 ? avail / n : avail;
  if(slot > 14 * ui_advance){ slot = 14 * ui_advance; }
  if(slot < 4 * ui_advance){ slot = 4 * ui_advance; }

  x += btn + UI_PAD;
  for(i = 0; i < session_count; ++i){
    ui_tab_r[i].x = x;
    ui_tab_r[i].y = by;
    ui_tab_r[i].w = slot - 2;
    ui_tab_r[i].h = bh;
    x += slot;
  }
  ui_new_r.x = ui_screen->w - btn - UI_PAD;
  ui_new_r.y = by;
  ui_new_r.w = btn;
  ui_new_r.h = bh;
}

static void ui_layout_prompt(){
  int h = ui_bar_h();
  int btn = ui_bar_btn();
  int bh = h - 8;
  int by = (h - bh) / 2;

  ui_prompt_cancel_r.x = ui_screen->w - btn - UI_PAD;
  ui_prompt_cancel_r.y = by;
  ui_prompt_cancel_r.w = btn;
  ui_prompt_cancel_r.h = bh;

  ui_prompt_ok_r.x = ui_prompt_cancel_r.x - btn - UI_PAD;
  ui_prompt_ok_r.y = by;
  ui_prompt_ok_r.w = btn;
  ui_prompt_ok_r.h = bh;

  ui_search_close_r = ui_prompt_cancel_r;
  ui_search_next_r = ui_prompt_ok_r;
  ui_search_prev_r.x = ui_search_next_r.x - btn - UI_PAD;
  ui_search_prev_r.y = by;
  ui_search_prev_r.w = btn;
  ui_search_prev_r.h = bh;
}

static void ui_layout_switcher(){
  int i;
  int w = ui_screen->w;
  int row_h = ui_text_height * 2;
  int panel_w = w > 640 ? w * 4 / 5 : w - 16;
  int x = (w - panel_w) / 2;
  int y = ui_text_height + 10;
  int btn = row_h - 10;
  int gap = 6;

  if(row_h < 56){ row_h = 56; }

  ui_sw_close_r.x = x + panel_w - btn - gap;
  ui_sw_close_r.y = y + (row_h - btn) / 2;
  ui_sw_close_r.w = btn;
  ui_sw_close_r.h = btn;

  y += row_h + 4;
  for(i = 0; i < session_count; ++i){
    ui_sw_row_r[i].x = x;
    ui_sw_row_r[i].y = y;
    ui_sw_row_r[i].w = panel_w;
    ui_sw_row_r[i].h = row_h;

    ui_sw_cls_r[i].x = x + panel_w - btn - gap;
    ui_sw_cls_r[i].y = y + (row_h - btn) / 2;
    ui_sw_cls_r[i].w = btn;
    ui_sw_cls_r[i].h = btn;

    ui_sw_ren_r[i].x = ui_sw_cls_r[i].x - btn - gap;
    ui_sw_ren_r[i].y = ui_sw_cls_r[i].y;
    ui_sw_ren_r[i].w = btn;
    ui_sw_ren_r[i].h = btn;

    y += row_h + 4;
  }
  ui_sw_new_r.x = x;
  ui_sw_new_r.y = y;
  ui_sw_new_r.w = panel_w;
  ui_sw_new_r.h = row_h;
}

static void ui_layout(){
  if(ui_screen == NULL){ return; }
  ui_layout_bar();
  ui_layout_prompt();
  if(ui_mode == UI_MODE_SWITCHER){ ui_layout_switcher(); }
}

/* ---- search ---------------------------------------------------------- */

static void ui_search_rescan(){
  int row, col, i;
  int qlen = ui_query_len;

  ui_match_count = 0;
  ui_match_cur = -1;
  if(qlen <= 0 || buf == NULL){ return; }

  for(row = 0; row < TEXT_BUFFER_SIZE && ui_match_count < MAX_MATCHES; ++row){
    for(col = 0; col + qlen <= cols; ++col){
      int hit = 1;
      for(i = 0; i < qlen; ++i){
        UChar a = buf->text[row][col + i].c;
        UChar b = ui_query[i];
        if(a >= 'A' && a <= 'Z'){ a = a - 'A' + 'a'; }
        if(b >= 'A' && b <= 'Z'){ b = b - 'A' + 'a'; }
        if(a == 0){ a = ' '; }
        if(a != b){ hit = 0; break; }
      }
      if(hit){
        ui_matches[ui_match_count].row = row;
        ui_matches[ui_match_count].col = col;
        ui_matches[ui_match_count].len = qlen;
        ++ui_match_count;
        if(ui_match_count >= MAX_MATCHES){ break; }
        col += qlen - 1;
      }
    }
  }
  if(ui_match_count > 0){
    ui_match_cur = 0;
  }
}

/* Put the current match in the middle of the screen (scrollback offset) */
static void ui_search_focus(){
  ui_match_t *m;
  int target;

  if(ui_match_cur < 0 || ui_match_cur >= ui_match_count || buf == NULL){ return; }
  m = &ui_matches[ui_match_cur];
  target = m->row - rows / 2;
  if(target < 0){ target = 0; }
  buf->view_offset = buf->top_line - target;
  if(buf->view_offset < 0){ buf->view_offset = 0; }
  if(buf->view_offset > buf->top_line){ buf->view_offset = buf->top_line; }
}

static void ui_search_step(int delta){
  if(ui_match_count <= 0){ return; }
  ui_match_cur += delta;
  while(ui_match_cur < 0){ ui_match_cur += ui_match_count; }
  while(ui_match_cur >= ui_match_count){ ui_match_cur -= ui_match_count; }
  ui_search_focus();
}

void sessionui_search_clear(){
  if(ui_mode == UI_MODE_SEARCH){
    ui_mode = UI_MODE_NONE;
    ui_layout_changed = 1;
  }
  ui_match_count = 0;
  ui_match_cur = -1;
}

void sessionui_draw_search_highlights(SDL_Surface *screen, int top_offset){
  int i, view_top, bpp;
  SDL_Rect cur_ul;
  int have_cur = 0;

  if(ui_match_count <= 0 || buf == NULL || screen == NULL){ return; }
  view_top = buf->top_line - buf->view_offset;

  if(SDL_LockSurface(screen) == 0){
    bpp = screen->format->BytesPerPixel;
    for(i = 0; i < ui_match_count; ++i){
      ui_match_t *m = &ui_matches[i];
      int x0, y0, w, h, yy, xx;

      if(m->row < view_top){ continue; }
      if(m->row >= view_top + rows){ break; }

      x0 = m->col * ui_advance;
      y0 = top_offset + (m->row - view_top) * ui_text_height;
      w = m->len * ui_advance;
      h = ui_text_height;
      if(x0 >= screen->w || y0 >= screen->h){ continue; }
      if(x0 + w > screen->w){ w = screen->w - x0; }
      if(y0 + h > screen->h){ h = screen->h - y0; }
      if(w <= 0 || h <= 0){ continue; }

      for(yy = y0; yy < y0 + h; ++yy){
        Uint8 *row = (Uint8*)screen->pixels + yy * screen->pitch;
        for(xx = x0; xx < x0 + w; ++xx){
          Uint8 *q = row + xx * bpp;
          Uint32 pix;
          Uint8 pr, pg, pb;
          switch(bpp){
            case 4: pix = *(Uint32*)q; break;
            case 2: pix = *(Uint16*)q; break;
            default: pix = *q; break;
          }
          SDL_GetRGB(pix, screen->format, &pr, &pg, &pb);
          pix = SDL_MapRGB(screen->format, 255 - pr, 255 - pg, 255 - pb);
          switch(bpp){
            case 4: *(Uint32*)q = pix; break;
            case 2: *(Uint16*)q = (Uint16)pix; break;
            default: *q = (Uint8)pix; break;
          }
        }
      }
      if(i == ui_match_cur){
        have_cur = 1;
        cur_ul.x = x0;
        cur_ul.y = y0 + h - (h / 8 + 1);
        cur_ul.w = w;
        cur_ul.h = h / 8 + 1;
        if(cur_ul.y < y0){ cur_ul.y = y0; }
      }
    }
    SDL_UnlockSurface(screen);
  }

  if(have_cur){
    SDL_Color accent = UI_ACCENT;
    SDL_FillRect(screen, &cur_ul, ui_color(accent));
  }
}

/* ---- URL handling ---------------------------------------------------- */

static int ui_is_url_char(UChar c){
  if(c <= ' '){ return 0; }
  switch(c){
    case '<': case '>': case '"': case '\'': case '`':
    case '(': case ')': case '[': case ']': case '{': case '}':
    case '|': case '\\': case '^':
      return 0;
    default:
      return 1;
  }
}

static void ui_row_text(int row, UChar *out, int maxout){
  int c;
  if(row < 0 || row >= TEXT_BUFFER_SIZE){ out[0] = 0; return; }
  for(c = 0; c < cols && c < maxout - 1; ++c){
    UChar ch = buf->text[row][c].c;
    out[c] = (ch == 0) ? ' ' : ch;
  }
  out[c] = 0;
  while(c > 0 && out[c - 1] == ' '){ out[--c] = 0; }
}

int sessionui_open_url_at(int bufrow, int col){
  UChar line[1024];
  UChar url[1024];
  char utf8[2048];
  int start, end, i, len;
  UErrorCode err = U_ZERO_ERROR;
  int32_t u8len = 0;

  if(buf == NULL || ui_screen == NULL){ return 0; }
  if(bufrow < 0 || bufrow >= TEXT_BUFFER_SIZE){ return 0; }
  ui_row_text(bufrow, line, 1024);
  len = u_strlen(line);
  if(col < 0 || col >= len){ return 0; }
  if(!ui_is_url_char(line[col])){ return 0; }

  start = col;
  while(start > 0 && ui_is_url_char(line[start - 1])){ --start; }
  end = col;
  while(end < len && ui_is_url_char(line[end])){ ++end; }

  /* strip sentence punctuation from the tail */
  while(end > start){
    UChar c = line[end - 1];
    if(c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?'){ --end; }
    else { break; }
  }
  if(end - start < 4){ return 0; }

  for(i = start; i < end; ++i){ url[i - start] = line[i]; }
  url[end - start] = 0;

  /* only hand over things that look like URLs */
  if(!((u_strncmp(url, (const UChar*)u"http://", 7) == 0) ||
       (u_strncmp(url, (const UChar*)u"https://", 8) == 0) ||
       (u_strncmp(url, (const UChar*)u"ftp://", 6) == 0) ||
       (u_strncmp(url, (const UChar*)u"www.", 4) == 0) ||
       (u_strncmp(url, (const UChar*)u"mailto:", 7) == 0))){
    return 0;
  }

  err = U_ZERO_ERROR;
  u_strToUTF8(utf8, sizeof(utf8), &u8len, url, -1, &err);
  if(U_FAILURE(err)){ return 0; }
  if(navigator_invoke(utf8, NULL) != BPS_SUCCESS){
    return 0;
  }
  return 1;
}

/* ---- drawing --------------------------------------------------------- */

static void ui_draw_bar(){
  SDL_Rect bar = {0, 0, ui_screen->w, ui_bar_h()};
  SDL_Color bg = UI_BAR_BG;
  SDL_Color dim = UI_TAB_TEXT_DIM;
  SDL_Color fg = UI_TAB_TEXT;
  SDL_Color active_bg = UI_TAB_ACTIVE;
  SDL_Color inactive_bg = UI_TAB_INACTIVE;
  SDL_Color dead_bg = UI_TAB_DEAD;
  SDL_Color accent = UI_ACCENT;
  SDL_Color panel = UI_PANEL_BG;
  SDL_Rect line;
  int i;

  ui_fill(bar, bg);
  line.x = 0; line.y = ui_bar_h() - 1; line.w = ui_screen->w; line.h = 1;
  ui_fill(line, panel);

  if(ui_mode == UI_MODE_RENAME){
    SDL_Rect lbl = {UI_PAD, 3, ui_prompt_ok_r.x - UI_PAD * 2, ui_bar_h() - 5};
    char text[SESSION_NAME_LEN + 16];
    snprintf(text, sizeof(text), "Rename: %s", ui_edit_name);
    ui_text(lbl, text, fg, bg);
    /* cursor */
    {
      int cx = lbl.x + (int)(strlen(text)) * ui_advance;
      SDL_Rect cur = {cx, lbl.y + 2, 2, lbl.h - 4};
      if(cur.x < ui_prompt_ok_r.x - 4){ ui_fill(cur, accent); }
    }
    ui_fill(ui_prompt_ok_r, (SDL_Color)UI_TAB_ACTIVE);
    ui_text(ui_prompt_ok_r, "OK", fg, (SDL_Color)UI_TAB_ACTIVE);
    ui_fill(ui_prompt_cancel_r, (SDL_Color)UI_TAB_DEAD);
    ui_draw_cross(ui_prompt_cancel_r, fg);
    return;
  }

  if(ui_mode == UI_MODE_SEARCH){
    SDL_Rect lbl = {UI_PAD, 3, ui_search_prev_r.x - UI_PAD * 2, ui_bar_h() - 5};
    SDL_Rect count;
    char num[32];
    UChar q[64];
    int n = ui_query_len < 63 ? ui_query_len : 63;
    for(i = 0; i < n; ++i){ q[i] = ui_query[i]; }
    q[n] = 0;

    count.x = ui_search_prev_r.x - 8 * ui_advance;
    count.y = 3;
    count.w = 8 * ui_advance;
    count.h = ui_bar_h() - 5;

    /* label: "Find: query" - render the query as Unicode so non-ASCII works */
    ui_text(lbl, "Find:", fg, bg);
    {
      SDL_Rect qr = lbl;
      qr.x = lbl.x + 6 * ui_advance;
      qr.w = count.x - qr.x - UI_PAD;
      if(qr.w > 0){ ui_text_uchar(qr, q, n, accent, bg); }
    }
    if(ui_match_count > 0){
      snprintf(num, sizeof(num), "%d/%d", ui_match_cur + 1, ui_match_count);
    } else {
      snprintf(num, sizeof(num), "0/0");
    }
    ui_text(count, num, dim, bg);
    ui_fill(ui_search_prev_r, (SDL_Color)UI_TAB_ACTIVE);
    ui_text(ui_search_prev_r, "<", fg, (SDL_Color)UI_TAB_ACTIVE);
    ui_fill(ui_search_next_r, (SDL_Color)UI_TAB_ACTIVE);
    ui_text(ui_search_next_r, ">", fg, (SDL_Color)UI_TAB_ACTIVE);
    ui_fill(ui_search_close_r, (SDL_Color)UI_TAB_DEAD);
    ui_draw_cross(ui_search_close_r, fg);
    return;
  }

  if(!ui_bar_visible){ return; }

  /* hamburger: opens the switcher */
  ui_draw_menu_button(ui_menu_r, fg);

  for(i = 0; i < session_count; ++i){
    session_t *s = sessions[i];
    SDL_Rect r = ui_tab_r[i];
    SDL_Color tab_bg = (s == cur_session) ? active_bg
                     : (s->alive ? inactive_bg : dead_bg);
    char label[SESSION_NAME_LEN + 8];
    SDL_Rect textr;

    if(r.x + r.w > ui_new_r.x - UI_PAD){ break; }
    ui_fill(r, tab_bg);
    if(s == cur_session){
      SDL_Rect top = {r.x, r.y, r.w, 2};
      ui_fill(top, accent);
    }
    snprintf(label, sizeof(label), "%s", s->name);
    textr = r;
    textr.x += 3;
    textr.w -= 6;
    ui_text(textr, label, s->alive ? fg : dim, tab_bg);

    if(s->has_output && s != cur_session){
      SDL_Rect dot = {r.x + r.w - 7, r.y + 3, 4, 4};
      ui_fill(dot, accent);
    }
  }

  {
    SDL_Color plus_bg = UI_TAB_ACTIVE;
    ui_fill(ui_new_r, plus_bg);
    ui_draw_plus(ui_new_r, fg);
  }
}

static void ui_draw_switcher(){
  SDL_Color panel = UI_PANEL_BG;
  SDL_Color row = UI_PANEL_ROW;
  SDL_Color rowsel = UI_PANEL_ROW_SEL;
  SDL_Color fg = UI_TAB_TEXT;
  SDL_Color dim = UI_TAB_TEXT_DIM;
  SDL_Color accent = UI_ACCENT;
  SDL_Color warn = UI_WARN;
  SDL_Rect hdr;
  int i;

  /* dim the terminal behind the panel */
  SDL_LockSurface(ui_screen);
  {
    int bpp = ui_screen->format->BytesPerPixel;
    int y;
    for(y = 0; y < ui_screen->h; ++y){
      Uint8 *p = (Uint8*)ui_screen->pixels + y * ui_screen->pitch;
      int x;
      for(x = 0; x < ui_screen->w; ++x){
        Uint8 *q = p + x * bpp;
        if(bpp == 4){ *(Uint32*)q >>= 2; }
        else if(bpp == 2){ *(Uint16*)q = (*(Uint16*)q >> 2) & 0x39e7; }
        else { *q >>= 2; }
      }
    }
  }
  SDL_UnlockSurface(ui_screen);

  hdr.x = ui_sw_row_r[0].x;
  hdr.y = ui_sw_close_r.y;
  hdr.w = ui_sw_close_r.x - hdr.x;
  hdr.h = ui_sw_close_r.h;
  ui_text(hdr, "Sessions", fg, panel);
  ui_fill(ui_sw_close_r, (SDL_Color)UI_TAB_DEAD);
  ui_draw_cross(ui_sw_close_r, fg);

  for(i = 0; i < session_count; ++i){
    session_t *s = sessions[i];
    SDL_Rect r = ui_sw_row_r[i];
    char label[SESSION_NAME_LEN + 16];
    SDL_Rect textr;

    ui_fill(r, (s == cur_session) ? rowsel : row);
    if(s == cur_session){
      SDL_Rect edge = {r.x, r.y, 3, r.h};
      ui_fill(edge, accent);
    }
    snprintf(label, sizeof(label), "%d  %s%s", i + 1, s->name,
             s->alive ? "" : "  [exited]");
    textr = r;
    textr.x += 8;
    textr.w = ui_sw_ren_r[i].x - textr.x - 4;
    ui_text(textr, label, s->alive ? fg : warn, (s == cur_session) ? rowsel : row);

    ui_fill(ui_sw_ren_r[i], (SDL_Color)UI_PANEL_ROW_SEL);
    ui_text(ui_sw_ren_r[i], "R", fg, (SDL_Color)UI_PANEL_ROW_SEL);
    ui_fill(ui_sw_cls_r[i], (SDL_Color)UI_TAB_DEAD);
    ui_draw_cross(ui_sw_cls_r[i], fg);
  }

  ui_fill(ui_sw_new_r, row);
  ui_text(ui_sw_new_r, "+  New session", fg, row);

  /* footer hint */
  {
    SDL_Rect foot = {ui_sw_row_r[0].x, ui_sw_new_r.y + ui_sw_new_r.h + 6,
                     ui_sw_row_r[0].w, ui_text_height};
    if(foot.y + foot.h < ui_screen->h){
      ui_text(foot, "Tap a row to switch. R = rename, x = close.", dim, panel);
    }
  }
}

static void ui_draw(){
  if(ui_screen == NULL){ return; }
  ui_layout();
  if(ui_mode == UI_MODE_SWITCHER){
    ui_draw_switcher();
    return;
  }
  if(ui_bar_h() > 0){
    ui_draw_bar();
  }
}

void sessionui_draw(SDL_Surface *screen){
  ui_screen = screen;
  ui_draw();
}

/* ---- commands -------------------------------------------------------- */

void sessionui_toggle_bar(){
  ui_bar_visible = ui_bar_visible ? 0 : 1;
  ui_layout_changed = 1;
}

void sessionui_open_switcher(){
  if(session_count == 0){ return; }
  ui_mode = UI_MODE_SWITCHER;
  ui_layout_changed = 1;
}

void sessionui_start_rename(){
  if(cur_session == NULL){ return; }
  ui_edit_target = cur_session;
  snprintf(ui_edit_name, sizeof(ui_edit_name), "%s", cur_session->name);
  ui_edit_len = (int)strlen(ui_edit_name);
  ui_edit_pos = ui_edit_len;
  ui_mode = UI_MODE_RENAME;
  ui_layout_changed = 1;
  if(ui_prefs != NULL && ui_prefs->auto_show_vkb){ virtualkeyboard_show(); }
}

void sessionui_start_search(){
  ui_mode = UI_MODE_SEARCH;
  ui_query_len = 0;
  ui_query[0] = 0;
  ui_match_count = 0;
  ui_match_cur = -1;
  ui_layout_changed = 1;
  if(ui_prefs != NULL && ui_prefs->auto_show_vkb){ virtualkeyboard_show(); }
}

void sessionui_new_session(){
  session_t *s = session_create(1);
  if(s == NULL){ return; }
  ui_mode = UI_MODE_NONE;
  ui_layout_changed = 1;
  indicate_event_input();
}

void sessionui_close_active(){
  session_t *s = cur_session;
  if(s == NULL){ return; }
  session_close(s);
  ui_mode = UI_MODE_NONE;
  ui_layout_changed = 1;
  indicate_event_input();
}

/* ---- touch ----------------------------------------------------------- */

static int ui_switch_to(int i){
  if(i < 0 || i >= session_count){ return 0; }
  if(session_switch_index(i)){
    indicate_event_input();
    return 1;
  }
  return 0;
}

int sessionui_touch_down(int x, int y){
  ui_touch_x = x;
  ui_touch_y = y;
  ui_touch_started = 1;
  ui_touch_moved = 0;
  ui_touch_in_bar = (y < ui_bar_h());
  ui_layout();

  if(ui_mode == UI_MODE_SWITCHER){
    return 1;
  }
  if(ui_mode == UI_MODE_RENAME || ui_mode == UI_MODE_SEARCH){
    int h = ui_bar_h();
    if(y >= h){ return 0; }   /* let the terminal handle the rest */
    return 1;
  }
  return ui_touch_in_bar ? 1 : 0;
}

int sessionui_touch_motion(int x, int y){
  if(!ui_touch_started){ return ui_mode == UI_MODE_SWITCHER ? 1 : 0; }
  if(abs(x - ui_touch_x) > 12 || abs(y - ui_touch_y) > 12){
    ui_touch_moved = 1;
  }
  if(ui_mode == UI_MODE_SWITCHER){ return 1; }
  return ui_touch_in_bar ? 1 : 0;
}

int sessionui_touch_up(int x, int y){
  int i;

  ui_layout();
  ui_touch_started = 0;

  if(ui_mode == UI_MODE_SWITCHER){
    if(!ui_touch_moved){
      if(ui_point_in_pad(ui_sw_close_r, x, y, 4)){
        ui_mode = UI_MODE_NONE;
        ui_layout_changed = 1;
        return 1;
      }
      for(i = 0; i < session_count; ++i){
        if(ui_point_in_pad(ui_sw_cls_r[i], x, y, 4)){
          session_close(sessions[i]);
          if(session_count == 0){ ui_mode = UI_MODE_NONE; }
          ui_layout_changed = 1;
          indicate_event_input();
          return 1;
        }
        if(ui_point_in_pad(ui_sw_ren_r[i], x, y, 4)){
          ui_switch_to(i);
          ui_mode = UI_MODE_NONE;
          sessionui_start_rename();
          return 1;
        }
        if(ui_point_in(ui_sw_row_r[i], x, y)){
          ui_switch_to(i);
          ui_mode = UI_MODE_NONE;
          ui_layout_changed = 1;
          return 1;
        }
      }
      if(ui_point_in(ui_sw_new_r, x, y)){
        sessionui_new_session();
        ui_mode = UI_MODE_NONE;
        return 1;
      }
    }
    return 1;
  }

  if(ui_mode == UI_MODE_RENAME){
    if(!ui_touch_in_bar){ return 0; }
    if(ui_point_in_pad(ui_prompt_ok_r, x, y, 4)){
      session_rename(ui_edit_target, ui_edit_name);
      ui_mode = UI_MODE_NONE;
      ui_layout_changed = 1;
      indicate_event_input();
      return 1;
    }
    if(ui_point_in_pad(ui_prompt_cancel_r, x, y, 4)){
      ui_mode = UI_MODE_NONE;
      ui_layout_changed = 1;
      return 1;
    }
    return 1;
  }

  if(ui_mode == UI_MODE_SEARCH){
    if(!ui_touch_in_bar){ return 0; }
    if(ui_point_in_pad(ui_search_close_r, x, y, 4)){ sessionui_search_clear(); indicate_event_input(); return 1; }
    if(ui_point_in_pad(ui_search_next_r, x, y, 4)){ ui_search_step(1); indicate_event_input(); return 1; }
    if(ui_point_in_pad(ui_search_prev_r, x, y, 4)){ ui_search_step(-1); indicate_event_input(); return 1; }
    return 1;
  }

  if(!ui_touch_in_bar){ return 0; }

  if(ui_touch_moved){
    /* horizontal swipe on the bar: previous / next session */
    if(abs(x - ui_touch_x) > 60){
      if(x < ui_touch_x){ session_prev(); } else { session_next(); }
      indicate_event_input();
    }
    return 1;
  }

  if(ui_point_in_pad(ui_new_r, x, y, 4)){ sessionui_new_session(); return 1; }
  if(ui_point_in_pad(ui_menu_r, x, y, 4)){ sessionui_open_switcher(); return 1; }
  for(i = 0; i < session_count; ++i){
    if(ui_point_in(ui_tab_r[i], x, y)){
      if(sessions[i] == cur_session){
        sessionui_open_switcher();
      } else {
        ui_switch_to(i);
      }
      return 1;
    }
  }
  return 1;
}

/* ---- keyboard (prompt / search editing) ------------------------------ */

/* encode one code point as UTF-8 into the rename buffer */
static void ui_edit_insert(int ch){
  char enc[4];
  int n = 0, i;

  if(ch < 0x20 || ch > 0x10FFFF){ return; }
  if(ch < 0x80){
    enc[n++] = (char)ch;
  } else if(ch < 0x800){
    enc[n++] = (char)(0xC0 | (ch >> 6));
    enc[n++] = (char)(0x80 | (ch & 0x3F));
  } else if(ch < 0x10000){
    enc[n++] = (char)(0xE0 | (ch >> 12));
    enc[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
    enc[n++] = (char)(0x80 | (ch & 0x3F));
  } else {
    enc[n++] = (char)(0xF0 | (ch >> 18));
    enc[n++] = (char)(0x80 | ((ch >> 12) & 0x3F));
    enc[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
    enc[n++] = (char)(0x80 | (ch & 0x3F));
  }
  if(ui_edit_len + n >= (int)sizeof(ui_edit_name) - 1){ return; }
  if(ui_edit_pos < 0){ ui_edit_pos = 0; }
  if(ui_edit_pos > ui_edit_len){ ui_edit_pos = ui_edit_len; }
  memmove(ui_edit_name + ui_edit_pos + n, ui_edit_name + ui_edit_pos,
          ui_edit_len - ui_edit_pos + 1);
  for(i = 0; i < n; ++i){ ui_edit_name[ui_edit_pos + i] = enc[i]; }
  ui_edit_pos += n;
  ui_edit_len += n;
}

static void ui_edit_backspace(){
  int n = 1;
  if(ui_edit_pos <= 0){ return; }
  /* step over a whole UTF-8 sequence */
  while(ui_edit_pos - n > 0 &&
        ((unsigned char)ui_edit_name[ui_edit_pos - n] & 0xC0) == 0x80){
    ++n;
  }
  memmove(ui_edit_name + ui_edit_pos - n, ui_edit_name + ui_edit_pos,
          ui_edit_len - ui_edit_pos + 1);
  ui_edit_pos -= n;
  ui_edit_len -= n;
}

int sessionui_key(int keycode, int modifiers, int key_flags){
  (void)key_flags;
  (void)modifiers;

  if(ui_mode == UI_MODE_NONE){ return 0; }

  if(ui_mode == UI_MODE_RENAME){
    switch(keycode){
      case KEYCODE_RETURN:
      case KEYCODE_KP_ENTER:
        session_rename(ui_edit_target, ui_edit_name);
        ui_mode = UI_MODE_NONE;
        ui_layout_changed = 1;
        indicate_event_input();
        return 1;
      case KEYCODE_ESCAPE:
        ui_mode = UI_MODE_NONE;
        ui_layout_changed = 1;
        return 1;
      case KEYCODE_BACKSPACE:
        ui_edit_backspace();
        return 1;
      case KEYCODE_LEFT:
        if(ui_edit_pos > 0){ --ui_edit_pos; }
        return 1;
      case KEYCODE_RIGHT:
        if(ui_edit_pos < ui_edit_len){ ++ui_edit_pos; }
        return 1;
      default:
        if(keycode >= 0x20 && keycode < 0xF000){ ui_edit_insert(keycode); }
        return 1;
    }
  }

  if(ui_mode == UI_MODE_SEARCH){
    switch(keycode){
      case KEYCODE_RETURN:
      case KEYCODE_KP_ENTER:
      case KEYCODE_TAB:
        ui_search_step(1);
        indicate_event_input();
        return 1;
      case KEYCODE_ESCAPE:
        sessionui_search_clear();
        indicate_event_input();
        return 1;
      case KEYCODE_BACKSPACE:
        if(ui_query_len > 0){ --ui_query_len; ui_query[ui_query_len] = 0; }
        ui_search_rescan();
        ui_search_focus();
        indicate_event_input();
        return 1;
      case KEYCODE_LEFT:
        ui_search_step(-1);
        indicate_event_input();
        return 1;
      case KEYCODE_RIGHT:
      case KEYCODE_DOWN:
        ui_search_step(1);
        indicate_event_input();
        return 1;
      case KEYCODE_UP:
        ui_search_step(-1);
        indicate_event_input();
        return 1;
      default:
        if(keycode >= 0x20 && keycode < 0xF000 && ui_query_len < 47){
          ui_query[ui_query_len++] = (UChar)keycode;
          ui_query[ui_query_len] = 0;
          ui_search_rescan();
          ui_search_focus();
          indicate_event_input();
        }
        return 1;
    }
  }

  if(ui_mode == UI_MODE_SWITCHER){
    if(keycode == KEYCODE_ESCAPE){ ui_mode = UI_MODE_NONE; ui_layout_changed = 1; return 1; }
    if(keycode >= '1' && keycode < '1' + session_count){
      ui_switch_to(keycode - '1');
      ui_mode = UI_MODE_NONE;
      ui_layout_changed = 1;
      return 1;
    }
    if(keycode == KEYCODE_DOWN){ session_next(); indicate_event_input(); return 1; }
    if(keycode == KEYCODE_UP){ session_prev(); indicate_event_input(); return 1; }
    return 1;
  }

  return 0;
}

/* ---- setup ----------------------------------------------------------- */

void sessionui_init(pref_t *prefs, SDL_Surface *screen){
  ui_prefs = prefs;
  ui_screen = screen;
  ui_mode = UI_MODE_NONE;
  ui_match_count = 0;
  ui_match_cur = -1;
  ui_bar_visible = 1;
  ui_layout_changed = 1;
}

void sessionui_set_screen(SDL_Surface *screen){
  ui_screen = screen;
}

void sessionui_set_metrics(int text_height, int advance_px){
  ui_text_height = text_height;
  ui_advance = advance_px > 0 ? advance_px : 1;
  ui_layout_changed = 1;
}

int sessionui_bar_height(){
  return ui_bar_h();
}

int sessionui_take_layout_change(){
  int v = ui_layout_changed;
  ui_layout_changed = 0;
  return v;
}

int sessionui_modal(){
  return ui_mode != UI_MODE_NONE;
}

int sessionui_cancel(){
  if(ui_mode == UI_MODE_NONE){ return 0; }
  ui_mode = UI_MODE_NONE;
  ui_match_count = 0;
  ui_match_cur = -1;
  ui_layout_changed = 1;
  indicate_event_input();
  return 1;
}
