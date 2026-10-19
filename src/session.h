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

#ifndef SESSION_H_
#define SESSION_H_

#include <sys/types.h>
#include <unistd.h>
#include <unicode/utf.h>

#include "SDL.h"

#include "buffer.h"
#include "ecma48.h"

/* Upper bound on concurrently open terminals. Each one costs a full
 * scrollback buffer (TEXT_BUFFER_SIZE * MAX_COLS cells, mostly empty
 * pointers) plus a pty, so keep this modest - the Classic has 2 GB and
 * the rest of the app (glyph cache, fonts) is not small either. */
#define SESSION_MAX 8
#define SESSION_NAME_LEN 40
#define SESSION_UNNAMED "term"

/*
 * One terminal: the child process, its pty, the visible grid/scrollback
 * and the VT parser state.
 *
 * Only one session is "live" at a time - the engine in buffer.c and
 * ecma48.c still works on file-scope globals, because rewriting those
 * two files into a reentrant, parameterised engine is a far bigger and
 * riskier change than this feature is worth. `session_save_live()` and
 * `session_load_live()` copy the live globals into and out of the block
 * below, which is what makes switching cheap (a few memcpys) and keeps
 * the parser/renderer untouched.
 *
 * The one consequence: a session that is not currently visible does not
 * drain its pty. Output piles up in the kernel's tty buffer, the child
 * blocks on its next write until you switch to that tab, and then the
 * backlog is read in one go. Nothing is lost - it just waits.
 */
typedef struct session_t {
	int index;                      /* position in the tab bar */
	char name[SESSION_NAME_LEN];    /* user-visible label */
	int alive;                      /* child process still running */
	int exit_status;
	pid_t child_pid;
	int master_fd;
	char slave_ptyname[L_ctermid];
	Uint32 last_output_ticks;       /* last time we parsed output */
	int has_output;                 /* pty readable while not visible */
	int ever_started;
	/* SHELL_LOGIN: mksh -l, so ~/.profile decides (usually exec zsh).
	 * SHELL_MKSH_UTF8: mksh -i, skipping the login files, which keeps
	 *                  mksh's own UTF-8 mode on.
	 * SHELL_ZSH: the bundled zsh 5.9, built with multibyte support because
	 *            BerryCore's 5.0.2 was compiled without it. */
	int shell_kind;

	/* --- live engine state, swapped on switch (see buffer.c/ecma48.c) */
	buf_t *screens;                 /* NUM_BUFFERS screen state structs */
	buf_t *buf;                     /* the active one of them */
	saved_buf_t *saved_buf;         /* per-screen saved cursor state */
	uint32_t saved_buf_p;
	char **tabs;
	struct scroll_region sr;
	int sel_active;
	int sel_anchor[2];
	int sel_point[2];
	struct ecma48_state vt;
} session_t;

extern session_t *sessions[SESSION_MAX];
extern int session_count;
extern session_t *cur_session;

void session_setup_environment();   /* TERM, terminfo, PATH - once */
void session_subsys_init();
/* Create a session (buffers + pty + shell). The first one is activated
 * explicitly by sdl_init; later ones with session_create(1). */
session_t *session_create(int activate);
/* same, but pick the shell: utf8 = 1 starts mksh's UTF-8 mode */
#define SHELL_LOGIN      0
#define SHELL_MKSH_UTF8  1
#define SHELL_ZSH        2
session_t *session_create_ex(int activate, int kind);
void session_close(session_t *s);
/* Switch the live engine to this session; returns 1 if it changed. */
int  session_switch(session_t *s);
int  session_switch_index(int i);
void session_next();
void session_prev();
void session_rename(session_t *s, const char *name);
int  session_index_of(session_t *s);
int  session_count_alive();
int  session_is_last_alive(session_t *s);
/* park/restore the live engine around a switch (called by session_switch) */
void session_save_live();
void session_load_live(session_t *s);
/* SIGCHLD reaping: the signal handler (main.c) only appends to these,
 * the UI thread folds them into the session table once per loop turn. */
#define SESSION_REAP_SLOTS 32
extern volatile pid_t session_reaped_pid[SESSION_REAP_SLOTS];
extern volatile int session_reaped_status[SESSION_REAP_SLOTS];
extern volatile int session_reaped_count;
int  session_handle_sigchld();   /* returns how many sessions died */
/* Kill every child and free every session (app shutdown). */
void session_shutdown();
/* Set when the user closed the only remaining session: main.c leaves the
 * event loop, and session_shutdown() frees everything. */
extern int session_exit_requested;
/* Drop every borrowed glyph-surface pointer in the parked sessions.
 * Called whenever the glyph cache is flushed (font change): those
 * surfaces are freed then, and a parked session must not keep a
 * dangling pointer to one. */
void session_clear_renders();
/* Make sure the child knows the current rows/cols */
void session_push_winsize(session_t *s);
void session_push_winsize_all();

#endif /* SESSION_H_ */
