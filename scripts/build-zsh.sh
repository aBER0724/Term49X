#!/bin/bash
# Build the zsh 5.9 that ships in the bar (share/root/bin/zsh).
#
# Why a custom build: BB10's BerryCore zsh 5.0.2 was compiled without
# MULTIBYTE_SUPPORT, so it edits bytes and CJK in the line editor turns into
# boxes. This builds 5.9 with multibyte on, links it statically (the device
# has no discoverable libc.so.3/libsocket.so.3 for a dynamic link from the
# app sandbox), and supplies the three things QNX's libc lacks:
#
#   wcwidth/wcswidth  - there is no width function at all
#   iswprint          - QNX says every non-ASCII wide char is unprintable
#   (see tools/zsh-wcwidth.c and patches/zsh-5.9-qnx-printable.patch)
#
# Run inside the BB10 NDK container with the NDK environment sourced:
#   scripts/docker-build.sh zsh
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO_ROOT/.zsh-build"
VERSION=5.9
TARBALL="$BUILD/zsh-$VERSION.tar.xz"
URL="https://sourceforge.net/projects/zsh/files/zsh/$VERSION/zsh-$VERSION.tar.xz/download"

mkdir -p "$BUILD"
if [ ! -f "$TARBALL" ]; then
	echo "Downloading zsh $VERSION"
	curl -sL -o "$TARBALL" "$URL"
fi
rm -rf "$BUILD/zsh-$VERSION"
tar xf "$TARBALL" -C "$BUILD"

SRC="$BUILD/zsh-$VERSION"
cd "$SRC"

# QNX iswprint workaround
patch -p1 < "$REPO_ROOT/patches/zsh-5.9-qnx-printable.patch"

CCBIN="$(ls "$QNX_HOST"/usr/bin/arm-unknown-nto-qnx*eabi-gcc | head -1)"
T="$QNX_TARGET/armle-v7"

# width/isprint shim, compiled for the target
"$CCBIN" -O2 -I"$QNX_TARGET/usr/include" -include "$REPO_ROOT/tools/zsh-wide.h" \
	-c "$REPO_ROOT/tools/zsh-wcwidth.c" -o "$BUILD/zsh-wcwidth.o"
ar rcs "$BUILD/libzsh-wcwidth.a" "$BUILD/zsh-wcwidth.o"

# configure cannot run target binaries, so tell it what we know
export ac_cv_func_wcwidth=yes ac_cv_func_wcswidth=yes

CC="$CCBIN" \
CFLAGS="-O2 -fcommon -DHAVE_STRUCT_TIMESPEC -DHAVE_STRUCT_TIMEZONE -I$QNX_TARGET/usr/include -include $REPO_ROOT/tools/zsh-wide.h" \
LDFLAGS="-static -L$T/lib -L$T/usr/lib" \
LIBS="$BUILD/libzsh-wcwidth.a $T/usr/lib/libncursesw.a $T/usr/lib/libiconv.a $T/lib/libsocket.a $T/lib/libm.a $T/lib/libc.a" \
./configure --host=arm-unknown-nto-qnx8.0.0eabi \
	--enable-multibyte --disable-dynamic --with-term-lib="ncursesw" --disable-gdbm

make -j"$(nproc)" LIBS="$BUILD/libzsh-wcwidth.a $T/usr/lib/libncursesw.a $T/usr/lib/libiconv.a $T/lib/libsocket.a $T/lib/libm.a $T/lib/libc.a"

install -m 755 "$SRC/Src/zsh" "$REPO_ROOT/share/root/bin/zsh"
echo "Built $REPO_ROOT/share/root/bin/zsh"
