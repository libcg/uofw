# Copyright (C) 2016 The uOFW team
# See the file COPYING for copying permission.

TARGET = threadman
# The objects are linked in the order of the original module (its code, strings, tables and .bss variables follow
# this order).
OBJS = threadman_asm.o waitq.o callback.o callback_ret.o threadwait.o semaphore.o eventflag.o mutex.o \
	   lwmutex.o mbx.o msgpipe.o vpl.o fpl.o tlspl.o timer.o timer_syscall.o timer_handler.o ktls.o threadman.o

#DEBUG = 1

LIBS = 	-lInterruptManagerForKernel -lExceptionManagerForKernel -lSysMemForKernel -lKDebugForKernel \
		-lsceSuspendForKernel -lSysclibForKernel -lLoadCoreForKernel

# Flags for MATCHING=1 with the recreated original GCC 3.3.6 compiler (the primary matching toolchain; the GCC 4.0.2
# flags previously here are in git history). Build as the parity scoring does:
#   make MATCHING=1 OLDGCC=<oldgcc> MATCHING_CC="<oldgcc>/bin/oldcc.py <3.3.6-sony dir>" PSP_AS=<oldgcc>/4.0.2-sony/bin/psp-as
# (the patched assembler: COP0 interlocks, move as addu, no delay slot after COP0 writes).
# -O2 -G0, EABI 32-bit, no string merging (the original stores "WaitQ" once per source file), no sibling calls,
# register renaming, -march=allegrex. Builtins off as in the regular uOFW build (BUILTINS_DISABLE) except memset:
# constant-size word-aligned clears expand into the step-to-base store loop of the original.
# -g: the original was compiled with DWARF2 debug info (stripped from the PRX) by a cc1 configured for a .loc-capable
# gas (HAVE_AS_DWARF2_DEBUG_LINE): the $LBB/$LBE block labels keep gas from filling the delay slot of a labelled
# branch, e.g. after an inlined threadmanSysClockSet().
# PARITY-Q028 (-ffunction-sections): one gas fixup chain per function. With the binutils-2.14 %hi/%lo pairing of the patched
# assembler (oldgcc patches/binutils-allegrex-hilo-pairing-2.14.patch) this gives the HI16/LO16 order of the
# original's relocation stream (475/475 HI16 positions in 6.60 threadman; per-function chains fit 102 of the 172 6.60 kd
# modules, per-object 31). Code and linked image are unchanged; the top-level asm goes to its own sections (PARITY-Q186).
# PARITY-Q028: flags reproducing the original compiler's choices (see the comment above)
MATCHING_CFLAGS = -O2 -G0 -g -ffunction-sections -mabi=eabi -mgp32 -mlong32 -msingle-float -fno-merge-constants -fno-optimize-sibling-calls \
				  -frename-registers $(filter-out -fno-builtin-memset,$(BUILTINS_DISABLE)) -march=allegrex

# PARITY-Q031: imports, lib.ent NID index and module data laid out as in the original
# MATCHING=1: the imports are linked in the layout of the original module (imports.S, see there) instead of
# from the stub libraries.
ifeq ($(MATCHING),1)
OBJS += imports.o
LIBS =
endif

# ROOT_DIR is exported by the top-level Makefile; default it to the repository root for a build run from this
# directory (make MATCHING=1 ... here, or make -C src/kd/threadman ... from the root).
ROOT_DIR ?= $(abspath $(CURDIR)/../../..)

include $(ROOT_DIR)/lib/build.mak
# Code generation closest to the original module (measured against the 6.60 binary): -O2 without the
# optimisations that the original compiler did not perform (inlining of small functions, identical code
# folding, IPA register allocation, shrink wrapping, caller-saved register spilling, jump threading,
# statement sinking, value range propagation, sibling calls). No strict aliasing: the UID control blocks
# are accessed through casts.
CFLAGS += -O2 -fno-inline-small-functions -fno-ipa-icf -fno-ipa-ra -fno-shrink-wrap -fno-caller-saves \
		  -fno-thread-jumps -fno-tree-sink -fno-tree-vrp -fno-optimize-sibling-calls -fno-strict-aliasing
# The original module does not merge its string literals (e.g. "WaitQ" is stored once per source file).
CFLAGS += -fno-merge-constants
