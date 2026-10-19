# Term49X

**Term49X** is a terminal emulator for BlackBerry 10, tuned for the BlackBerry
Classic (Q20) hardware — physical keyboard, trackpad, and belt keys — and
usable on other BB10 devices.

It continues the [Term49](https://github.com/BerryFarm/Term49) /
[Term48](https://github.com/mordak/Term48) line by
[mordak](https://github.com/mordak) and BerryFarm (via the Term49C fork).

Term49X adds:

* **multiple sessions** — several shells at once, each with its own screen,
  scrollback, parser state and child process, switched from a tab bar, a
  full-screen switcher overlay, or metamode keys
* **scrollback search** — incremental, case-insensitive, with hit
  highlighting and jump-to-hit
* **tap-to-open URLs** — a short tap on a `http(s)://`, `ftp://`, `www.` or
  `mailto:` token hands it to the browser
* session rename/close from the UI, and per-tab "new output" indicators

It implements (relevant parts of) the [ECMA-48 standard][ecma], plus extra
control sequences for the `xterm-256color` terminfo. It is a work in progress,
but good enough for daily use. Pull requests, feature requests, and bug reports
are welcome.

Requires OS version >= 10.3.

**Install note:** official BlackBerry signing/distribution is no longer
practical. Today Term49X can only be **sideloaded on a rooted BlackBerry 10
OS**. Unsigned bars will not install on a stock, non-rooted device.

## Pinyin input bar (小鹤双拼)

Term49X carries its own input method: BB10's system IME does not deliver text
to SDL apps. The bottom row shows the composition and the candidate list:

```
[鹤] ni hc | 1.你 2.尼 3.泥 4.拟 >
```

| key | action |
| --- | ------ |
| letters | build the xiaohe shuangpin code (`nihc` → 你好) |
| `1`–`9` | pick candidate 1–9 of the current page |
| space | pick candidate 1 |
| `.` / `=` | next page |
| `,` / `-` | previous page |
| backspace | delete a letter |
| enter | commit the raw letters |
| Esc / belt Back | cancel the composition |
| any other key | pick candidate 1, then act normally |

Candidates come from `$HOME/term49_xiaohe.dict`, falling back to
`app/native/xiaohe.dict` and `/accounts/1000/shared/misc/term49_xiaohe.dict`
(one `code<TAB>word word ...` line per key, words ordered by frequency). When
the full code has no entry, the longest matching prefix is used and the
remaining letters stay in the buffer, so phrases can be typed in pieces.

It is on by default. `TERM49_PINYIN=0` starts with a plain terminal, and the
Menu-key popup (`CN`) toggles it at runtime.

### 全拼 / 双拼

两种输入方式，同一份词典：小鹤双拼（默认）按键固定两下，全拼按整串拼音查词。

| 模式 | 输入 | 结果 |
| --- | --- | --- |
| 小鹤双拼 | `nihc` | 你好 |
| 全拼 | `nihao` | 你好 |

切换：Menu 键 → `f`（菜单标签 `全/鹤`）。想固定下来，在 `~/.term49rc` 加一行：

```
pinyin_mode = "full";    /* 默认 "xiaohe"，小鹤双拼 */
```

单次运行时也可以用 `TERM49_PINYIN_FULL=1` 启动。

全拼会把字母自动切成音节（`fangan` → 方案，`xian` → 先），所以个别需要断字的地方要自己分次选词。没有匹配的英文单词原样传给终端（`ls`、`git` 不受影响）。

### Backspace byte

The Backspace key sends **DEL (0x7f)** and the session's tty is programmed with
`VERASE = 0x7f` to match, which is what full-screen programs (claude, vim,
less, htop) expect. Older Term48/49 builds sent `^H` (0x08), which those
programs read as Ctrl-H — "typing works, deleting does not". If you prefer the
old byte for an old shell setup, put this in `~/.term49rc`:

```
backspace = "bs";   /* 0x08 instead of "del" (default) */
```

## Menu key popup

The BlackBerry/Menu key opens a small action grid instead of sticky Alt:
Paste, `CN` (pinyin bar), `Kbd` (virtual keyboard), `Cls` (clear screen),
`Fix` (rescreen), Close. The grid is configured by `menu_actions` in
`~/.term49rc` and accepts the same action names as `metamode_func_keys`
(`paste_clipboard`, `pinyin_toggle`, `vkb_toggle`, `clear_screen`,
`lang_toggle`, `rescreen`, `menu_close`, plus the `session_*` actions).
`TERM49_MENU_ALT=1` restores the old "Menu = sticky Alt" behaviour.

## Sessions (tabs)

Term49X runs up to 8 shells concurrently. Every session keeps its own screen
buffer, scrollback, VT parser state, pty and child process; switching is a
context switch of the live engine state (a handful of memcpys), so it feels
like flipping a view rather than starting anything.

The tab bar occupies the top strip of the screen (one text row). The terminal
grid is sized below it, so no terminal line is ever covered by chrome. The
metamode hitbox (top-left corner, 100×100 px by default) is measured from the
top of the *grid*, so the corner gesture now lives just below the bar.

* **+** creates a new shell in `$HOME`.
* A **tab** switches to that shell; tapping the *active* tab (or the
  hamburger button on the left) opens the switcher.
* **swipe left/right** across the bar goes to the previous/next shell.
* A **dot** on a tab means that shell produced output while you were looking
  somewhere else.
* A shell that exits is shown in red and keeps its last output readable;
  closing the tab kills the process (`SIGHUP`). If the exiting shell was the
  only session, the app exits as it always has.

The switcher overlay lists every session with per-row **R** (rename) and
**x** (close) buttons, plus **+ New session**.

### Session keys

Available inside metamode (top-left corner / hold Space), after the user's own
`metamode_keys` and `metamode_func_keys` bindings:

| key | action |
| --- | ------ |
| `n` | new session |
| `b` | session switcher |
| `r` | rename current session |
| `x` | close current session |
| `f` | search the scrollback |
| `[` / `]` | previous / next session |
| `1`–`9` | switch to session 1–9 |
| `0` | show/hide the tab bar |

These are compiled in, so they work with a `~/.term49rc` that predates them.
They can also be bound to any key through `metamode_func_keys` (the same list
that holds `alt_down`, `ctrl_down`, `rescreen`, `paste_clipboard`): use the
values `session_new`, `session_menu`, `session_prev`, `session_next`,
`session_close`, `session_rename`, `session_search`, `session_bar`.

### Background output

A session that is not visible does not drain its pty. Its output waits in the
kernel tty buffer and is read the moment you switch to it — nothing is lost,
but a program that writes continuously (`yes`, a chatty log) blocks on its
next write until you look at that tab. This keeps the render thread on one
pty and avoids parsing eight streams of output behind your back.

## Scrollback search

Metamode `f` opens the search bar over the tab bar. Typing searches the whole
scrollback, case-insensitively, as you type:

* `Enter` / `Tab` / `>` — next hit
* `<` / `Left` / `Up` — previous hit
* `x` / `Escape` — close

Hits are inverted in the terminal; the current one is underlined and scrolled
to the middle of the screen. The search bar and the rename prompt take over
the keyboard, so nothing typed there reaches the shell.

## URLs

A short tap on a URL in the terminal opens it with the system browser
(`navigator_invoke`). A tap that scrolled the view, that is part of a text
selection, or that lands in a mouse-tracking app is ignored, so this does not
fight the existing touch gestures.

## Terminal compatibility

- **Colors**: 16, 256 (`SGR 38;5;n`) and 24-bit truecolor (`SGR 38;2;r;g;b`),
  bold/italic/underline/strikethrough/reverse/dim.
- **Wide characters**: East-Asian wide/fullwidth cells take two columns,
  including emoji outside the BMP (shipped fonts may not have emoji glyphs,
  but the layout is right).
- **Combining marks**: accents and other zero-width marks attach to the
  preceding cell instead of taking their own, so `e` + U+0301 renders as one
  character.
- `clear` is bundled (`native/root/bin/clear`) and also drops the scrollback
  (`CSI 3J`).

## Trackpad support (BlackBerry Classic / Q20)

On devices with a trackpad, swiping the trackpad sends arrow keys (useful
for shell history and cursor movement, since the Classic has no arrow
keys), and clicking the trackpad sends Enter. Application cursor key mode
(DECCKM) is respected, so arrows also work in `vi`, `less`, etc.

Three settings in `~/.term49rc` control this behaviour:

* `trackpad_enabled` (default `true`): set to `false` to ignore the trackpad.
* `trackpad_sensitivity` (default `60`): trackpad displacement counts per
  arrow keystroke. Lower values mean more keystrokes per swipe.
* `trackpad_click_keys` (default `"kent"`, i.e. Enter): keystrokes sent on
  trackpad click. Accepts the same values as other keymaps, including
  terminfo names such as `"kcuu1"`; set to `""` to disable the click.

The trackpad arrives at the app as raw `SCREEN_EVENT_JOYSTICK` events via a
patched libSDL (see `patches/sdl-term48-trackpad.patch`, already applied to
the prebuilt `external/lib/libSDL12.so`).

## Belt keys (BlackBerry Classic / Q20)

* **Back** sends Esc. Configurable via `back_button_keys` in `~/.term49rc`
  (default `"\x1b"`; accepts the same values as other keymaps, e.g.
  terminfo names).
* **Menu (BlackBerry key)** toggles a persistent Alt mode: every key maps
  through the alt table (and Alt+trackpad scrolling is armed) until you
  press Menu again. The `a` indicator shows while it is active. This is
  different from the hardware Alt key, which stays one-shot. The
  swipe-down-from-bezel gesture does the same.
* Send and End keep their system behaviour.

## Shift+Tab (back-tab)

Key tables are shift-aware. With Shift active — held down, or armed via
the sticky shift key (`↑` indicator) — any metamode, sym-menu, or alt
key bound to Tab sends back-tab (`ESC [ Z`) instead, i.e. Shift+Tab.
So: arm Shift, then arm metamode (hold Space, or tap the top-left
corner), then `t` (the default Tab binding) — or arm Shift and pick
Tab from the sym menu.

**Note:** the optional double-tap right-Shift metamode toggle does
**not** work on BlackBerry Classic (Q20); the keyboard does not
report a distinct right-Shift event. Use hold-Space or the corner
hitbox on Classic.

For custom shifted bindings, an uppercase entry in `metamode_keys` wins
when Shift is active, e.g. `("T", "...")` alongside `("t", "\x09")`.
Shift+Tab from the virtual keyboard also sends back-tab.

## Mouse support

Term49X implements xterm mouse reporting (DECSET 9/1000/1002/1003, plus
SGR 1006 coordinates). When an application enables mouse tracking (tmux
with `set -g mouse on`, vim with `set mouse=a`, htop, mc, ...):

* the **trackpad** automatically switches from arrow keys to driving a
  mouse pointer (shown as an inverse `+`); trackpad click = left click.
  When the application turns mouse tracking off, the trackpad reverts to
  arrow keys — there is nothing to configure.
* **touching the screen** sends a click at the touched cell, and dragging
  reports mouse motion (e.g. selecting text in vim).
* **Alt + trackpad** swipes send wheel events (apps treat these as
  scrolling) instead of using the local scrollback.

Line drawing also works: SI/SO and the SCS charset designations
(`ESC ( 0` / `ESC ( B`) are honoured and DEC Special Graphics characters
are mapped to Unicode box-drawing glyphs, so ncurses borders and
separators render as real lines instead of letters.

## Fallback font

The default terminal font may lack glyphs modern TUI programs draw with:
braille patterns, rounded box corners, block elements, powerline symbols.
Characters the main font cannot draw are rendered with a bundled fallback
font ([Cascadia Mono](https://github.com/microsoft/cascadia-code), OFL
licensed), instead of showing empty squares. Configure with
`fallback_font_path` in `~/.term49rc` (set to `""` to disable).

## Copy and paste

* **Copy**: hold Alt (or turn on Menu's persistent Alt mode), then drag a
  finger across the screen to select text. Selected cells are highlighted;
  lifting your finger copies the text to the system clipboard (trailing
  spaces trimmed, rows joined with newlines). A plain tap clears the
  selection.
* **Paste**: the existing paste action (metamode `v`) writes the clipboard
  to the terminal. When an application enables **bracketed paste**
  (DECSET 2004, e.g. vim, zsh, bash), the paste is wrapped so the
  application treats it as literal text rather than typed commands.
* **Remote clipboard (OSC 52)**: programs can set the phone's clipboard
  with an `OSC 52` escape — so `tmux` (`set -s set-clipboard on`), vim
  (`set clipboard=unnamed` with an OSC 52 plugin), and similar tools can
  copy to the BlackBerry clipboard even over SSH. Clipboard *reads* via
  OSC 52 are ignored for privacy.

## Cursor styles

Term49X honours `DECSCUSR` (`CSI Ps SP q`): applications can request a
block, underline, or bar cursor (vim, for example, uses this to show
insert vs normal mode). Blinking variants render steady.

## Scrollback

Term49X keeps scrollback history (`scrollback_lines` in `~/.term49rc`,
default `500` extra lines; `0` disables). To scroll:

* **Touch**: drag a finger up/down on the screen — the content follows
  your finger.
* **Alt + trackpad**: press Alt (sticky), then swipe the trackpad up/down.
  Press Alt again to leave scroll mode.

While scrolled back, an inverse `▲` shows in the top-right corner, and the
view stays anchored even as new output arrives. Typing (or sending arrows
with the trackpad) snaps back to the live screen. Full-screen applications
(`less`, `vim`, ...) use the alternate screen and are unaffected.

## Development

To compile Term49X you will need:

* [libSDL][libsdl]
* [Touch Control Overlay][tco]
* [libconfig][libconfig]

Prebuilt shared libraries are available in `external/lib` (see Makefile). To
build them from source, check out the submodules (`git clone --recursive`) and
build with the Momentics IDE. When compiling SDL, define
`-D__PLAYBOOK__ -DRAW_KEYBOARD_EVENTS`.

**Deployment:** there is no supported signing path for BlackBerry World or
stock devices anymore. Build an unsigned bar and **sideload it onto a rooted
BB10 OS** (for example with community tools such as
[bb10d](https://github.com/jxw1102) / device-side package installers). Stock,
non-rooted devices will reject the package.

### Building with Docker

If you don't have (or can't install) the BlackBerry NDK locally, you can
build with Docker using a community BB10 NDK 10.3.1 image:

* `./scripts/docker-build.sh` — builds `Device-Debug/Term49` and packages an
  unsigned `Term49X.bar`
* `./scripts/docker-build.sh sdl` — additionally rebuilds the patched
  `external/lib/libSDL12.so` from the `SDL` tree (`term48` branch plus
  `patches/sdl-term48-trackpad.patch`)

Rebuild SDL at least once (or when the patch changes): the prebuilt
`libSDL12.so` taken from an upstream bar does not deliver
`SCREEN_EVENT_JOYSTICK` to the app, and without it the Classic's trackpad
does nothing.

The image (`delaya73/bbndk` by default, override with `BBNDK_IMAGE`) runs
under x86 emulation on arm64 hosts, which is slow but works. By using the
NDK you accept the BlackBerry SDK license. Sideload the resulting
`Term49X.bar` on a rooted device — do not expect stock install or store
signing to work.

### Building locally

You can build Term49X without Momentics:

* Load the proper `bbndk-env` file
* `make` — produces the binary under `Device-Debug/`
* Package with the Docker/container scripts, or your own `blackberry-nativepackager` invocation, to get `Term49X.bar`
* Sideload the bar on a **rooted** BB10 device

### Debugging with GDB

On a rooted device with a working SSH/debug channel:

* Load `bbndk-env` on the host
* Deploy and launch the app stopped if your tooling supports it
* Attach `ntoarm-gdb` to the process on the device

Exact steps depend on your root/debug setup; the historical Momentics
debug-token + `blackberry-connect` signing flow is no longer documented here.

## Source layout

| file | contents |
| ---- | -------- |
| `main.c` | SDL/QNX screen setup, rendering, input (keyboard, trackpad, belt keys), event loop |
| `ecma48.c` | VT/ECMA-48 parser, modes, OSC handling, mouse reports |
| `buffer.c` | screen grid, scrollback, tab stops, selection |
| `session.c` | the session table: pty + shell per session, save/restore of the live engine state on a switch, SIGCHLD bookkeeping |
| `sessionui.c` | tab bar, session switcher, rename prompt, scrollback search, URL detection |
| `preferences.c` | `~/.term49rc` (libconfig) |
| `SDL_ttf.c` | patched SDL_ttf with glyph fallbacks |

The engine in `buffer.c`/`ecma48.c` is intentionally still written against
file-scope state: a session switch copies that state in and out of the
`session_t` block (`buffer_state_save/load`, `ecma48_state_save/load`)
instead of parametrising 6 000 lines of parser code. Only the active session
is parsed and rendered, which is why background shells block on their pty
rather than piling up parsed output (see "Sessions" above).

## See also

* [BerryFarm Term49](https://github.com/BerryFarm/Term49) (upstream)
* [Term48](https://github.com/mordak/Term48) (original)
* [Term48 on BlackBerry AppWorld](http://appworld.blackberry.com/webstore/content/26272878/) (historical)

[ecma]: http://www.ecma-international.org/publications/standards/Ecma-048.htm
[libsdl]: https://github.com/mordak/SDL/tree/term48
[tco]: https://github.com/blackberry/TouchControlOverlay
[libconfig]: http://www.hyperrealm.com/libconfig/
