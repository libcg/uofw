# Copyright (C) 2016 - 2026 The uOFW team
# See the file COPYING for copying permission.

#include "common_asm.h"

    .set noreorder

    .text

# Subroutine sub_00002DBC - Address 0x00002DBC
# The return address of the kernel mode callbacks (see sub_00002944()): the result of the callback,
# in $v0 on entry, is stored as the result of the call before returning to its caller. This cannot be
# written in C as $v0 is read on entry.
    .globl sub_00002DBC
sub_00002DBC:
    addiu $sp, $sp, -16
    sw    $ra, 4($sp)
    sw    $s0, 0($sp)
    jal   sceKernelCpuSuspendIntr
    addu  $s0, $v0, $zero
    lui   $v0, %hi(g_threadman)
    lw    $a0, %lo(g_threadman)($v0)
    jal   sub_000028C0
    sw    $s0, 128($a0) # g_threadman.curThread->waitStatus
    lw    $ra, 4($sp)
    lw    $s0, 0($sp)
    jr    $ra
    addiu $sp, $sp, 16
