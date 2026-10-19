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

#ifndef SESSIONUI_H_
#define SESSIONUI_H_

#include "SDL.h"
#include "types.h"
#include "session.h"

/* Chrome around the terminal: the tab bar at the top, a full-screen
 * session switcher, the rename prompt and the scrollback search bar.
 *
 * Everything here is drawn on top of the terminal surface after the
 * cells have been rendered, and everything here consumes touch/key
 * events before the terminal does (returning 1 from the handlers). */

void sessionui_init(pref_t *prefs, SDL_Surface *screen);
void sessionui_set_screen(SDL_Surface *screen);
/* The font changed; keep the bar metrics in sync with the terminal. */
void sessionui_set_metrics(int text_height, int advance_px);

/* Pixels reserved at the top of the screen for the bar (0 = none). The
 * terminal grid is sized against this, so it must be asked before the
 * screen size is computed. */
int  sessionui_bar_height(void);
/* Set when bar_height() changed and the grid has to be re-laid-out. */
int  sessionui_take_layout_change(void);
/* A modal (switcher / prompt / search) owns the keyboard and touches. */
int  sessionui_modal(void);
/* Close whatever modal is open (belt Back key, trackpad). Returns 1 if
 * there was one. */
int  sessionui_cancel(void);

void sessionui_draw(SDL_Surface *screen);

/* Touch handlers, coordinates in screen pixels. Return 1 if consumed. */
int  sessionui_touch_down(int x, int y);
int  sessionui_touch_motion(int x, int y);
int  sessionui_touch_up(int x, int y);

/* Raw keyboard hook, called for every key-down before the terminal sees
 * it. Returns 1 if the key was consumed (prompt editing, search). */
int  sessionui_key(int keycode, int modifiers, int key_flags);

/* Commands, also bound to metamode keys in main.c */
void sessionui_toggle_bar(void);
void sessionui_open_switcher(void);
void sessionui_start_search(void);
void sessionui_start_rename(void);
void sessionui_new_session(void);
void sessionui_close_active(void);

/* Paint the scrollback-search hits onto the finished frame. Done as one
 * batch (a single surface lock for all hits): a per-cell hook would lock
 * and unlock the surface thousands of times per frame. */
void sessionui_draw_search_highlights(SDL_Surface *screen, int top_offset);
/* Stop-highlighting hook so a tap can clear the search. */
void sessionui_search_clear(void);

/* If the cell (bufrow, col) holds a URL, hand it to the browser.
 * Returns 1 if something was opened. */
int  sessionui_open_url_at(int bufrow, int col);

#endif /* SESSIONUI_H_ */
