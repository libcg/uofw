# matching-toolchain

Rebuilds the compiler used by the `MATCHING=1` build (see `lib/build.mak`): a recreation of the
toolchain the original modules were compiled with, so that a module's C sources can be compiled to the
exact original machine code.

It consists of:

- `cc1` from GCC 3.3.6 (C only, `mipsel-elf` target) with `patches/gcc-3.3.6-allegrex.patch`: Allegrex
  instructions (`clz`, `ext`, `ins`, `seb`, `seh`, `min`, `max`, ...), Allegrex scheduling and costs, and the
  code generation choices of the original compiler.
- `psp-as` from the pspdev binutils (`allegrex-v2.44` branch) with the `patches/binutils-allegrex-*.patch`
  patches: `halt` and the VFPU sync register, `move` as `addu`, coprocessor interlocks, no delay slot
  filling after cop0 writes, and the `%hi`/`%lo` relocation order of binutils 2.14.
- `stdint.h` from newlib 1.20.0 as installed by the PSP toolchain (`patches/newlib-1.20.0-stdint.patch`).
- `oldcc.py`, a small driver which runs `cc1` and then `psp-as`, as `gcc -c` would.

The `.S` files and the link still use the normal pspdev toolchain.

## Building

    utils/matching-toolchain/build.sh <OLDGCC>

where `<OLDGCC>` is the install directory. The script downloads GCC 3.3.6 and newlib 1.20.0 from
ftp.gnu.org and sourceware.org (checked against their SHA-256) and fetches a pinned commit of
https://github.com/pspdev/binutils-gdb, applies the patches and installs `bin/oldcc.py` and
`3.3.6-sony/{bin/cc1,bin/psp-as,include,sysinclude,defines,cc1.cmd}`. Build files go to `<OLDGCC>/_work`
(`WORK=` to change it), which can be deleted afterwards. Set `DOWNLOADS=` to a directory holding the
tarballs to skip downloading them.

Host requirements: gcc, make, python3, git, curl, bison and flex. GCC 3.3.6 is built with
`-std=gnu89 -fcommon -fpermissive` so that a current host gcc accepts it.

## Using

    make MATCHING=1 OLDGCC=<OLDGCC>

in the directory of a module which defines `MATCHING_CFLAGS`.
