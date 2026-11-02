# Copyright (C) 2016 - 2026 The uOFW team
# See the file COPYING for copying permission.

#include "common_asm.h"

    .set noreorder

    .text

# Subroutine sub_000112E0 - Address 0x000112E0
# The primary syscall handler registered by _sceKernelSysTimerInit() (see timer.c): return the
# lower 32 bits of the system time (the counter of the system timer) directly from the exception
# handler. This cannot be written in C as it returns with eret.
    .globl sub_000112E0
sub_000112E0:
    lui   $v0, 0xBC60
    lw    $v0, 0($v0)
    eret
    nop
    jr    $ra
    nop
