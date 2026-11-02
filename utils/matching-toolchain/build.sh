#!/bin/bash
# Build the toolchain used by the MATCHING=1 build (see README.md).
#
# Usage: utils/matching-toolchain/build.sh <OLDGCC>
#
# Installs into <OLDGCC>:
#   bin/oldcc.py                 the compiler driver called by lib/build.mak
#   3.3.6-sony/bin/cc1           GCC 3.3.6 cc1 with the Allegrex patch
#   3.3.6-sony/bin/psp-as        pspdev binutils 2.44 (Allegrex) assembler with the patches below
#   3.3.6-sony/include           GCC's private headers (stddef.h, stdarg.h, ...)
#   3.3.6-sony/sysinclude        stdint.h of the PSP toolchain's newlib 1.20.0
#   3.3.6-sony/defines           the predefined macros passed to cc1
#   3.3.6-sony/cc1.cmd           the command that runs cc1
#
# Environment: WORK (build directory, default <OLDGCC>/_work), JOBS (default nproc),
# DOWNLOADS (directory searched for the GCC and newlib tarballs before downloading them).
# Host requirements: gcc, make, python3, git, curl, bison, flex.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <OLDGCC install directory>" >&2
    exit 1
fi

for tool in gcc make python3 git curl bison flex; do
    command -v $tool > /dev/null || { echo "$tool not found" >&2; exit 1; }
done

HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
OLDGCC=$(cd "$1" && pwd)
WORK=${WORK:-$OLDGCC/_work}
JOBS=${JOBS:-$(nproc)}
DEST=$OLDGCC/3.3.6-sony

GCC_URL=https://ftp.gnu.org/gnu/gcc/gcc-3.3.6/gcc-core-3.3.6.tar.bz2
GCC_SHA256=4f9bee8ac57711508d6b8031d5ecfefc16fcf37ec81568b3f8344ef5f4cdfeb6
NEWLIB_URL=https://sourceware.org/pub/newlib/newlib-1.20.0.tar.gz
NEWLIB_SHA256=c644b2847244278c57bec2ddda69d8fab5a7c767f3b9af69aa7aa3da823ff692
BINUTILS_GIT=https://github.com/pspdev/binutils-gdb.git
BINUTILS_COMMIT=a8b53fe2b5825fa86337623c743d21a19aeb0daf  # branch allegrex-v2.44

mkdir -p "$WORK"
cd "$WORK"

# --- sources -----------------------------------------------------------------------------------

# download <url> <sha256>
download() {
    local f
    f=$(basename "$1")
    if [ ! -f "$f" ] && [ -n "${DOWNLOADS:-}" ] && [ -f "$DOWNLOADS/$f" ]; then
        cp "$DOWNLOADS/$f" .
    fi
    [ -f "$f" ] || curl -fsSLO "$1"
    echo "$2  $f" | sha256sum -c --quiet -
}

download "$GCC_URL" "$GCC_SHA256"
download "$NEWLIB_URL" "$NEWLIB_SHA256"

# --- GCC 3.3.6 cc1 -----------------------------------------------------------------------------

rm -rf gcc-3.3.6 build-gcc
python3 -c "import sys, tarfile; tarfile.open(sys.argv[1]).extractall()" "$(basename "$GCC_URL")"
(cd gcc-3.3.6 && patch -p1 -s < "$HERE/patches/gcc-3.3.6-allegrex.patch")

# Only cc1 is needed: all-gcc stops at libgcc, which needs a target assembler, so its status is ignored.
mkdir build-gcc
(
    cd build-gcc
    export CC="gcc -std=gnu89 -fcommon -fpermissive -w -O1 -fno-strict-aliasing"
    ../gcc-3.3.6/configure --target=mipsel-elf --prefix="$DEST/install" --enable-languages=c \
        --with-newlib --without-headers --disable-nls --disable-shared --disable-threads \
        --disable-multilib --with-gnu-as --with-gnu-ld \
        --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-gnu > configure.log 2>&1
    # The assembler built below supports .loc: with -g, line info goes out as .loc directives instead of
    # $LM labels (configure cannot detect this as the assembler is not installed yet). This changes where
    # the assembler can fill branch delay slots, so it matters for the generated code.
    sed -i 's|^/\* #undef HAVE_AS_DWARF2_DEBUG_LINE \*/$|#define HAVE_AS_DWARF2_DEBUG_LINE 1|' gcc/auto-host.h
    grep -q '^#define HAVE_AS_DWARF2_DEBUG_LINE 1$' gcc/auto-host.h
    make -j"$JOBS" all-gcc > build.log 2>&1 || true
    make -C gcc cc1 >> build.log 2>&1 || { echo "cc1 build failed, see $WORK/build-gcc/build.log" >&2; exit 1; }
)

# --- assembler ---------------------------------------------------------------------------------

rm -rf binutils build-binutils
git init -q binutils
git -C binutils fetch -q --depth 1 "$BINUTILS_GIT" "$BINUTILS_COMMIT"
git -C binutils -c advice.detachedHead=false checkout -q FETCH_HEAD
test "$(git -C binutils rev-parse HEAD)" = "$BINUTILS_COMMIT"
for p in insns cop-interlocks move-addu cop0-write-no-dslot hilo-pairing-2.14; do
    git -C binutils apply "$HERE/patches/binutils-allegrex-$p.patch"
done

mkdir build-binutils
(
    cd build-binutils
    ../binutils/configure --target=psp --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng \
        --disable-nls --disable-werror --disable-libdecnumber --disable-readline --disable-gold \
        --disable-ld --disable-binutils --disable-gprof > configure.log 2>&1
    make -j"$JOBS" all-gas MAKEINFO=true > build.log 2>&1 || { echo "assembler build failed, see $WORK/build-binutils/build.log" >&2; exit 1; }
)

# --- stdint.h ----------------------------------------------------------------------------------

rm -rf newlib
python3 -c "import sys, tarfile; tarfile.open(sys.argv[1]).extractall(members=[m for m in tarfile.open(sys.argv[1]) if m.name == sys.argv[2]])" \
    "$(basename "$NEWLIB_URL")" newlib-1.20.0/newlib/libc/include/stdint.h
mv newlib-1.20.0 newlib
(cd newlib && patch -p1 -s < "$HERE/patches/newlib-1.20.0-stdint.patch")

# --- install -----------------------------------------------------------------------------------

rm -rf "$DEST"
mkdir -p "$DEST/bin" "$OLDGCC/bin"
cp build-gcc/gcc/cc1 "$DEST/bin/cc1"
cp build-binutils/gas/as-new "$DEST/bin/psp-as"
cp -rL build-gcc/gcc/include "$DEST/include"
mkdir "$DEST/sysinclude"
cp newlib/newlib/libc/include/stdint.h "$DEST/sysinclude/stdint.h"
echo "$DEST/bin/cc1" > "$DEST/cc1.cmd"
echo "-D__GNUC__=3 -D__GNUC_MINOR__=3 -D__GNUC_PATCHLEVEL__=6 -DPSP=1 -D__psp__=1 -D_PSP=1 '-D__builtin_offsetof(t,m)=((unsigned long)&((t *)0)->m)'" > "$DEST/defines"
install -m 755 "$HERE/oldcc.py" "$OLDGCC/bin/oldcc.py"

echo "Installed into $OLDGCC (build with: make MATCHING=1 OLDGCC=$OLDGCC)"
