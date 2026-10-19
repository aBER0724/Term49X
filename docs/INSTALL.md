# Building and installing Term49X

The result of a build is an **unsigned** `.bar`. BlackBerry no longer signs for
hobbyists, so it only installs on a **rooted** BB10 10.3+ device (which is what
the Q20/Q30/Q10 MOD OS images and the `bb10mt` flashing tooling produce).

## Build (no NDK needed on the host)

```sh
cd term49x/repo

# first time only: rebuild the patched SDL (trackpad events + belt keys)
BBNDK_IMAGE=delaya73/bbndk:latest ./scripts/docker-build.sh sdl

# every build afterwards (~20 s)
./scripts/docker-build.sh
```

Outputs:

* `Device-Debug/Term49` — the armle-v7 binary
* `Term49X.bar` — the package (app id `com.example.Term49X`, version 2.0.0)

The container image (`delaya73/bbndk`, BB10 NDK 10.3.1.995) runs as
`linux/amd64`; on Apple silicon that is emulation, which works but is slow for
the SDL build. Only the first `sdl` build is slow.

`external/lib/*.so`, `share/root/*` (ssh/scp/sftp/mksh) and the bundled fonts
are not tracked by git; restore them from an upstream bar:

```sh
unzip -oq Term49-all-permissions.bar -d /tmp/upstream
cp /tmp/upstream/native/lib/libconfig.so.11          external/lib/libconfig.so
cp /tmp/upstream/native/lib/libTouchControlOverlay.so external/lib/libTouchControlOverlay.so
cp -a /tmp/upstream/native/root/.                     share/root/
```

(The patched `libSDL12.so` is built by `./scripts/docker-build.sh sdl`, do not
copy the upstream one — it drops trackpad/joystick events.)

## Install

Sideload the bar the same way any other Term49C build is installed on your
device (bb10d / Sachesi / a device-side installer). It has its **own package
id** (`com.example.Term49X`), so it installs next to an existing Term49 or
Term49C instead of replacing it.

`~/.term49rc` is shared with Term49C: the new session/search features need no
new settings, and the new metamode keys (`n`, `b`, `r`, `x`, `f`, `[`, `]`,
`1`-`9`, `0`) are built in, so an existing rc file keeps working.

### Built-in metamode keys (top-left corner of the grid, below the tab bar)

| key | action |
| --- | ------ |
| `n` | new session |
| `b` | session switcher overlay |
| `r` | rename current session |
| `x` | close current session |
| `f` | scrollback search |
| `[` `]` | previous / next session |
| `1`-`9` | jump to session N |
| `0` | show / hide the tab bar |

## What was verified here

* cross-compiles and links cleanly with the BB10 NDK (`qcc`, armle-v7);
  only pre-existing warnings from the upstream code remain
* `META-INF/MANIFEST.MF` is now valid (no packager error): `Package-Author-Id`
  had to be a 27-char urlsafe-base64 id, the upstream placeholder is rejected
* the bar contains the new binary, the trackpad-patched `libSDL12.so`
  (with `handleTrackpadEvent`/`handleSyskeyEvent`), both fonts, the terminfo
  database (including `x/xterm-256color`) and the updated on-device README
* `nm` shows the new session/sessionui/buffer-state/ecma48-state symbols in
  the packaged binary

Not verified: runtime behaviour on hardware (no device attached to this
build host). First things to exercise on the device: open/switch/close/rename
tabs, rotation with several tabs open, background output indicator, search,
and `exit` in the last tab.
