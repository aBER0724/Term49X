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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unix.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <unicode/utf.h>

#include "terminal.h"
#include "ecma48.h"
#include "buffer.h"
#include "io.h"
#include "pinyin.h"
#include "session.h"

session_t *sessions[SESSION_MAX];
int session_count = 0;
session_t *cur_session = NULL;
int session_exit_requested = 0;

static int session_name_counter = 0;

/* children reaped by the SIGCHLD handler (filled there, consumed by the
 * UI thread; see session_handle_sigchld) */
volatile pid_t session_reaped_pid[SESSION_REAP_SLOTS];
volatile int session_reaped_status[SESSION_REAP_SLOTS];
volatile int session_reaped_count = 0;

/* from main.c */
extern int rows;
extern int cols;

/* from buffer.c */
extern int MAX_ROWS;
extern int MAX_COLS;
extern int TEXT_BUFFER_SIZE;

int session_index_of(session_t *s){
  int i;
  for(i = 0; i < session_count; ++i){
    if(sessions[i] == s){ return i; }
  }
  return -1;
}

int session_count_alive(){
  int i, n = 0;
  for(i = 0; i < session_count; ++i){
    if(sessions[i]->alive){ ++n; }
  }
  return n;
}

int session_is_last_alive(session_t *s){
  int i;
  for(i = 0; i < session_count; ++i){
    if(sessions[i] != s && sessions[i]->alive){ return 0; }
  }
  return 1;
}

/* TERM, the .terminfo link, LC_CTYPE and the private bin directory are
 * process-wide, so they are set up once at startup - before any thread
 * exists - and inherited by every shell we fork later. */
void session_setup_environment(){
  ecma48_setenv();
  setenv("LC_CTYPE", "en_US.UTF-8", 0);
  {
    char* home = getenv("SANDBOX");
    char* path = getenv("PATH");
    if(home != NULL && path != NULL){
      char* root = "app/native/root/bin";
      int len = strlen(home) + strlen(path) + strlen(root) + 10;
      char* newpath = malloc(len);
      if(newpath != NULL){
        snprintf(newpath, len, "PATH=%s/%s:%s", home, root, path);
        /* putenv keeps the pointer; the block is intentionally not freed */
        putenv(newpath);
      }
    }
  }
}

void session_subsys_init(){
  memset(sessions, 0, sizeof(sessions));
  session_count = 0;
  cur_session = NULL;
  session_name_counter = 0;
}

/* Fork a shell on a fresh pty.
 *
 * The child only ever runs async-signal-safe calls after fork(): this
 * process has a second (render) thread by the time a tab is opened, and
 * getenv/malloc/putenv/stat/execvp can deadlock on a lock that thread
 * happened to hold. All the environment setup therefore happens in the
 * parent, where it is also shared by every session. */
static int session_spawn_shell(session_t *s){
  struct winsize winp;
  int pty_ret, slave_fd = -1, master_fd = -1;
  int uid = getuid();
  int gid = getgid();
  pid_t pid;
  char private_shell[] = "../app/native/root/bin/mksh";
  int use_private = 0;
  char *argv[3];

  winp.ws_row = rows > 0 ? rows : 24;
  winp.ws_col = cols > 0 ? cols : 80;
  winp.ws_xpixel = 1024;
  winp.ws_ypixel = 600;

  use_private = (access(private_shell, X_OK) == 0);

  /* both non-login shells need the BerryCore tools on PATH themselves */
  if (s->shell_kind != SHELL_LOGIN) {
    char *path = getenv("PATH");
    char *extra = "/accounts/1000/shared/misc/berrycore/bin:"
                  "/accounts/1000/shared/misc/clitools/bin";
    if (path != NULL) {
      int len = strlen(extra) + strlen(path) + 2;
      char *newpath = malloc(len);
      if (newpath != NULL) {
        snprintf(newpath, len, "%s:%s", extra, path);
        setenv("PATH", newpath, 1);
        free(newpath);
      }
    }
  }

  pty_ret = openpty(&master_fd, &slave_fd, s->slave_ptyname, NULL, &winp);
  if(pty_ret != 0){
    fprintf(stderr, "openpty returned: %s\n", strerror(errno));
    if(master_fd >= 0){ close(master_fd); }
    if(slave_fd >= 0){ close(slave_fd); }
    s->master_fd = -1;
    s->alive = 0;
    return TERM_FAILURE;
  }

  fcntl(master_fd, F_SETFL, fcntl(master_fd, F_GETFL) | O_NONBLOCK);

  /* Make the tty agree with what the Backspace key sends. QNX defaults to
   * ^H, while full-screen programs (claude, vim, less) expect DEL. */
  {
    struct termios tio;
    if(tcgetattr(slave_fd, &tio) == 0){
      tio.c_cc[VERASE] = (cc_t)ecma48_backspace();
      if(tcsetattr(slave_fd, TCSANOW, &tio) != 0){
        fprintf(stderr, "tcsetattr(VERASE) failed: %s\n", strerror(errno));
      }
    }
  }

  pid = fork();
  if(pid == 0){
    /* child: async-signal-safe calls only, then exec */
    setuid(uid);
    setgid(gid);
    setsid();
    ioctl(slave_fd, TIOCSCTTY, NULL);
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    argv[0] = "zsh";
    argv[1] = "-l";
    argv[2] = NULL;
    if (s->shell_kind == SHELL_ZSH) {
      /* the bundled zsh 5.9: built with multibyte support */
      execv("../app/native/root/bin/zsh", argv);
    }
    argv[0] = use_private ? "mksh" : "sh";
    argv[1] = (s->shell_kind == SHELL_MKSH_UTF8) ? "-i" : "-l";
    execv(use_private ? private_shell : "/bin/sh", argv);
    _exit(127);
  }

  if(pid == -1){
    fprintf(stderr, "fork returned: %s\n", strerror(errno));
    close(master_fd);
    close(slave_fd);
    s->master_fd = -1;
    s->alive = 0;
    return TERM_FAILURE;
  }

  close(slave_fd);
  s->master_fd = master_fd;
  s->child_pid = pid;
  s->alive = 1;
  s->ever_started = 1;
  return TERM_SUCCESS;
}

session_t *session_create(int activate){
  return session_create_ex(activate, 0);
}

void session_save_live(){
  if(cur_session == NULL){ return; }
  buffer_state_save(cur_session);
  ecma48_state_save(&cur_session->vt);
}

void session_load_live(session_t *s){
  if(s == NULL){ return; }
  buffer_state_load(s);
  ecma48_state_load(&s->vt);
}

session_t *session_create_ex(int activate, int kind){
  session_t *s;
  char namebuf[SESSION_NAME_LEN];

  if(session_count >= SESSION_MAX){
    fprintf(stderr, "Refusing to open more than %d sessions\n", SESSION_MAX);
    return NULL;
  }

  s = (session_t*)calloc(1, sizeof(session_t));
  if(s == NULL){ return NULL; }
  s->shell_kind = kind;
  s->master_fd = -1;
  s->child_pid = -1;

  ecma48_state_init(&s->vt);
  if(buffer_state_init(s) != TERM_SUCCESS){
    fprintf(stderr, "Could not allocate buffers for a new session\n");
    buffer_state_free(s);
    ecma48_state_free(&s->vt);
    free(s);
    return NULL;
  }

  ++session_name_counter;
  if (kind == SHELL_ZSH) {
    snprintf(namebuf, sizeof(namebuf), "zsh %d", session_name_counter);
  } else if (kind == SHELL_MKSH_UTF8) {
    snprintf(namebuf, sizeof(namebuf), "utf8 %d", session_name_counter);
  } else {
    snprintf(namebuf, sizeof(namebuf), "shell %d", session_name_counter);
  }
  session_rename(s, namebuf);

  s->index = session_count;
  sessions[session_count++] = s;

  /* The pty gets the *current* geometry. rows/cols are global and always
   * describe the visible grid, which every session shares. */
  session_spawn_shell(s);

  if(activate){
    session_switch(s);
  }
  return s;
}

void session_close(session_t *s){
  int i, idx;

  if(s == NULL){ return; }
  idx = session_index_of(s);
  if(idx < 0){ return; }

  if(session_count == 1 && cur_session == s){
    /* Closing the last tab means "quit", like the single-session app
     * always did. The live engine state still points into this session,
     * and the render thread may be drawing it right now, so it is not
     * freed here: the event loop exits and session_shutdown() cleans up
     * after the render thread is gone. */
    if(s->master_fd >= 0){
      queue_close_fd(s->master_fd);
      s->master_fd = -1;
    }
    if(s->alive && s->child_pid > 0){
      kill(s->child_pid, SIGHUP);
      s->alive = 0;
    }
    session_exit_requested = 1;
    return;
  }

  if(s->master_fd >= 0){
    /* the render thread may be blocked in select() on this fd; it closes
     * queued fds at the top of its loop instead */
    queue_close_fd(s->master_fd);
    s->master_fd = -1;
  }
  if(s->alive && s->child_pid > 0){
    kill(s->child_pid, SIGHUP);
    s->alive = 0;
  }

  for(i = idx; i < session_count - 1; ++i){
    sessions[i] = sessions[i + 1];
    sessions[i]->index = i;
  }
  sessions[--session_count] = NULL;

  if(cur_session == s){
    /* Pick the tab that took its place, else the one before it. */
    session_t *next = NULL;
    if(session_count > 0){
      next = sessions[idx < session_count ? idx : session_count - 1];
    }
    cur_session = NULL;
    if(next != NULL){
      cur_session = next;
      session_load_live(next);
      io_set_master(next->master_fd);
    }
  }

  buffer_state_free(s);
  ecma48_state_free(&s->vt);
  free(s);
}

int session_switch(session_t *s){
  if(s == NULL || s == cur_session){ return 0; }
  session_save_live();
  cur_session = s;
  session_load_live(s);
  /* an unfinished input-method composition belongs to the old tab */
  pinyin_reset();
  s->has_output = 0;
  io_set_master(s->master_fd);
  /* the pty may have been sized while this session was parked */
  session_push_winsize(s);
  return 1;
}

int session_switch_index(int i){
  if(i < 0 || i >= session_count){ return 0; }
  return session_switch(sessions[i]);
}

void session_next(){
  int i;
  if(session_count < 2){ return; }
  i = session_index_of(cur_session);
  session_switch(sessions[(i + 1) % session_count]);
}

void session_prev(){
  int i;
  if(session_count < 2){ return; }
  i = session_index_of(cur_session);
  session_switch(sessions[(i + session_count - 1) % session_count]);
}

void session_rename(session_t *s, const char *name){
  if(s == NULL || name == NULL){ return; }
  while(*name == ' '){ ++name; }
  if(*name == '\0'){
    snprintf(s->name, sizeof(s->name), SESSION_UNNAMED " %d", s->index + 1);
    return;
  }
  strncpy(s->name, name, sizeof(s->name) - 1);
  s->name[sizeof(s->name) - 1] = '\0';
}

int session_handle_sigchld(){
  int n, i, changed = 0;

  for(;;){
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if(pid <= 0){ break; }
    n = session_reaped_count;
    if(n < SESSION_REAP_SLOTS){
      session_reaped_pid[n] = pid;
      session_reaped_status[n] = status;
      session_reaped_count = n + 1;
    }
  }

  for(i = 0; i < session_reaped_count; ++i){
    pid_t pid = session_reaped_pid[i];
    int k;
    for(k = 0; k < session_count; ++k){
      if(sessions[k]->child_pid == pid){
        sessions[k]->alive = 0;
        sessions[k]->exit_status = session_reaped_status[i];
        ++changed;
        break;
      }
    }
  }
  session_reaped_count = 0;
  return changed;
}

void session_clear_renders(){
  int i, n, r, c;
  for(i = 0; i < session_count; ++i){
    session_t *s = sessions[i];
    if(s == cur_session || s->screens == NULL){ continue; }
    for(n = 0; n < NUM_BUFFERS; ++n){
      buf_t *b = &s->screens[n];
      if(b->text == NULL){ continue; }
      for(r = 0; r <= TEXT_BUFFER_SIZE; ++r){
        for(c = 0; c < MAX_COLS; ++c){
          b->text[r][c].surface = NULL;
        }
      }
    }
  }
}

void session_shutdown(){
  int i;
  io_set_master(-1);
  for(i = 0; i < session_count; ++i){
    session_t *s = sessions[i];
    if(s->master_fd >= 0){
      close(s->master_fd);
      s->master_fd = -1;
    }
    if(s->alive && s->child_pid > 0){
      kill(s->child_pid, SIGHUP);
      s->alive = 0;
    }
  }
  for(i = 0; i < session_count; ++i){
    buffer_state_free(sessions[i]);
    ecma48_state_free(&sessions[i]->vt);
    free(sessions[i]);
    sessions[i] = NULL;
  }
  session_count = 0;
  cur_session = NULL;
}
