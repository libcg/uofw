# Copyright (C) 2016 - 2026 The uOFW team
# See the file COPYING for copying permission.

#include "common_asm.h"

    .set noreorder
    .set noat

    .text

# Subroutine sub_00000000 - Address 0x00000000
# The entry of the idle thread (see ThreadManInit()). Forever: write back some data cache lines, write 0x10100
# to the hardware register 0xBC200000 and put the CPU to sleep, then restore the register and briefly enable
# the interrupts. The second written back line is pointed to by the first word of the exception manager's state
# (ExcepManCB, a variable imported from ExceptionManagerForKernel). The instructions with no assembler mnemonic
# (sleep) are written as raw words.
    .globl sub_00000000
sub_00000000:
    jal         ExceptionManagerForKernel_96D47F7E
    nop
    lui         $t1, 0xBC20
    mfic $t2, $0
    mtic $zero, $0
    lui         $t3, 0x1
    ori         $t3, $t3, 0x100
    lui         $t4, %hi(ExcepManCB)
    # The sleep loop is aligned on a data cache line (64 bytes).
    .p2align 6
.L00040:
    lw          $t6, %lo(ExcepManCB)($t4)
    mtc0        $v0, $25
    cache       0x0A, 0x0($v0)
    cache       0x0A, 0x40($v0)
    cache       0x0A, 0x0($t6)
    cache       0x0A, 0x40($t6)
    lw          $t5, 0x0($t1)
    sw          $t3, 0x0($t1)
    .word 0x70000000  # sleep
    sw          $t5, 0x0($t1)
    mtic $t2, $0
    mtic $zero, $0
    j           .L00040
    nop
    .p2align 6

# Subroutine sub_00000080 - Address 0x00000080
# Save the VFPU registers into a VFPU context (a0, SceThreadmanVfpuContext): the 128 vector registers
# and the VFPU control registers. The VFPU is enabled (Status.CU2) during the copy.
    .globl sub_00000080
sub_00000080:
    mfc0        $t0, $12
    lui         $at, 0x4000
    or          $t7, $t0, $at
    mtc0        $t7, $12
    nop
    cache       0x18, 0x0($a0)
    sv.q        C000, 0x0($a0)
    sv.q        C010, 0x10($a0)
    sv.q        C020, 0x20($a0)
    sv.q        C030, 0x30($a0)
    cache       0x18, 0x40($a0)
    sv.q        C100, 0x40($a0)
    sv.q        C110, 0x50($a0)
    sv.q        C120, 0x60($a0)
    sv.q        C130, 0x70($a0)
    cache       0x18, 0x80($a0)
    sv.q        C200, 0x80($a0)
    sv.q        C210, 0x90($a0)
    sv.q        C220, 0xA0($a0)
    sv.q        C230, 0xB0($a0)
    cache       0x18, 0xC0($a0)
    sv.q        C300, 0xC0($a0)
    sv.q        C310, 0xD0($a0)
    sv.q        C320, 0xE0($a0)
    sv.q        C330, 0xF0($a0)
    cache       0x18, 0x100($a0)
    sv.q        C400, 0x100($a0)
    sv.q        C410, 0x110($a0)
    sv.q        C420, 0x120($a0)
    sv.q        C430, 0x130($a0)
    cache       0x18, 0x140($a0)
    sv.q        C500, 0x140($a0)
    sv.q        C510, 0x150($a0)
    sv.q        C520, 0x160($a0)
    sv.q        C530, 0x170($a0)
    cache       0x18, 0x180($a0)
    sv.q        C600, 0x180($a0)
    sv.q        C610, 0x190($a0)
    sv.q        C620, 0x1A0($a0)
    sv.q        C630, 0x1B0($a0)
    cache       0x18, 0x1C0($a0)
    sv.q        C700, 0x1C0($a0)
    sv.q        C710, 0x1D0($a0)
    sv.q        C720, 0x1E0($a0)
    sv.q        C730, 0x1F0($a0)
    cache       0x18, 0x200($a0)
    mfvc        $t7, $128
    mfvc        $t6, $129
    mfvc        $t5, $130
    mfvc        $t4, $131
    mfvc        $t3, $132
    sw          $t7, 0x200($a0)
    sw          $t6, 0x204($a0)
    sw          $t5, 0x208($a0)
    sw          $t4, 0x20C($a0)
    sw          $t3, 0x210($a0)
    mfvc        $t7, $136
    mfvc        $t6, $137
    mfvc        $t5, $138
    mfvc        $t4, $139
    sw          $t7, 0x220($a0)
    sw          $t6, 0x224($a0)
    sw          $t5, 0x228($a0)
    sw          $t4, 0x22C($a0)
    mfvc        $t7, $140
    mfvc        $t6, $141
    mfvc        $t5, $142
    mfvc        $t4, $143
    sw          $t7, 0x230($a0)
    sw          $t6, 0x234($a0)
    sw          $t5, 0x238($a0)
    sw          $t4, 0x23C($a0)
    vsync
    vnop
    mtc0        $t0, $12
    nop
    jr          $ra
    nop

# Subroutine sub_000001B8 - Address 0x000001B8
# Restore the VFPU registers from a VFPU context (a0, SceThreadmanVfpuContext). See sub_00000080().
    .globl sub_000001B8
sub_000001B8:
    mfc0        $t0, $12
    lui         $at, 0x4000
    or          $t7, $t0, $at
    mtc0        $t7, $12
    nop
    vsync
    lv.q        C000, 0x0($a0)
    lv.q        C010, 0x10($a0)
    lv.q        C020, 0x20($a0)
    lv.q        C030, 0x30($a0)
    lv.q        C100, 0x40($a0)
    lv.q        C110, 0x50($a0)
    lv.q        C120, 0x60($a0)
    lv.q        C130, 0x70($a0)
    lv.q        C200, 0x80($a0)
    lv.q        C210, 0x90($a0)
    lv.q        C220, 0xA0($a0)
    lv.q        C230, 0xB0($a0)
    lv.q        C300, 0xC0($a0)
    lv.q        C310, 0xD0($a0)
    lv.q        C320, 0xE0($a0)
    lv.q        C330, 0xF0($a0)
    lv.q        C400, 0x100($a0)
    lv.q        C410, 0x110($a0)
    lv.q        C420, 0x120($a0)
    lv.q        C430, 0x130($a0)
    lv.q        C500, 0x140($a0)
    lv.q        C510, 0x150($a0)
    lv.q        C520, 0x160($a0)
    lv.q        C530, 0x170($a0)
    lv.q        C600, 0x180($a0)
    lv.q        C610, 0x190($a0)
    lv.q        C620, 0x1A0($a0)
    lv.q        C630, 0x1B0($a0)
    lv.q        C700, 0x1C0($a0)
    lv.q        C710, 0x1D0($a0)
    lv.q        C720, 0x1E0($a0)
    lv.q        C730, 0x1F0($a0)
    lw          $t7, 0x200($a0)
    lw          $t6, 0x204($a0)
    lw          $t5, 0x208($a0)
    lw          $t4, 0x20C($a0)
    lw          $t3, 0x210($a0)
    mtvc        $t7, $128
    mtvc        $t6, $129
    mtvc        $t5, $130
    mtvc        $t4, $131
    mtvc        $t3, $132
    lw          $t7, 0x220($a0)
    lw          $t6, 0x224($a0)
    lw          $t5, 0x228($a0)
    lw          $t4, 0x22C($a0)
    mtvc        $t7, $136
    mtvc        $t6, $137
    mtvc        $t5, $138
    mtvc        $t4, $139
    lw          $t7, 0x230($a0)
    lw          $t6, 0x234($a0)
    lw          $t5, 0x238($a0)
    lw          $t4, 0x23C($a0)
    mtvc        $t7, $140
    mtvc        $t6, $141
    mtvc        $t5, $142
    mtvc        $t4, $143
    mtc0        $t0, $12
    nop
    jr          $ra
    nop

# Subroutine sub_000002C8 - Address 0x000002C8
# Flush the VFPU (wait for the pending VFPU operations), with the VFPU enabled (Status.CU2) meanwhile.
# vsync2 has no assembler mnemonic and is written as a raw word.
    .globl sub_000002C8
sub_000002C8:
    mfc0        $t0, $12
    lui         $at, 0x4000
    or          $t7, $t0, $at
    mtc0        $t7, $12
    nop
    vsync
    vflush
    .word 0x486000FF  # vsync2
    mtc0        $t0, $12
    nop
    jr          $ra
    nop

# Part of the exception handler below (reached from sub_000004B8): decrement the counter at v1 + 8 and,
# when it reaches 0, store the word at v1 + 4 to the address at v1 + 0.
    .p2align 6
.L00300:
    lw          $v0, 0x8($v1)
    beqz        $v0, .L004CC
    addiu       $v0, $v0, -0x1
    bnez        $v0, .L004CC
    sw          $v0, 0x8($v1)
    lw          $v0, 0x4($v1)
    lw          $v1, 0x0($v1)
    sw          $v0, 0x0($v1)
    j           .L004CC
    sync

# Not a coprocessor 1 unusable exception: run the next handler of the chain.
.L00328:
    lui         $v0, %hi(sub_00000338)
    lw          $v0, %lo(sub_00000338)($v0)
    jr          $v0
    nop

# Subroutine sub_00000338 - Address 0x00000338
# The coprocessor unusable exception handler (registered for EXCEP_CPU by ThreadManInit()). It starts with
# the exception handler header (the next handler of the chain, set by the exception manager), followed by
# the code. For a coprocessor 1 (FPU) unusable exception, it enables the FPU (Status.CU1) and lazily switches
# the FPU registers: they are saved into the context of their previous owner (THREADMAN_COP0_CTRL_FPU_OWNER)
# and loaded from the current context (COP0_CTRL_SP_KERNEL), which becomes the owner. The other exceptions
# go to the next handler.
    .globl sub_00000338
sub_00000338:
    .word       0  # the next handler
    .word       0
    mfc0        $v1, $13
    ext         $v1, $v1, 28, 2
    xori        $v0, $v1, 0x1
    bnez        $v0, .L00328
    cfc0        $v0, $13
    bnez        $v0, .L004E0
    lui         $v0, %hi(g_threadman)
    lw          $v0, %lo(g_threadman)($v0)
    lw          $v1, 0x14($v0)
    andi        $v1, $v1, 0x2000
    bnez        $v1, .L00328
    addiu       $v1, $zero, 0x1
    sw          $v1, 0x18($v0)
    cfc0        $v0, $2
    ins         $v0, $v1, 29, 1
    mtc0        $v0, $12
    cfc0        $v1, $8
    bgez        $v1, .L00420
    ctc0        $v0, $2
    lb          $v0, 0x24($v1)
    xori        $v0, $v0, 0x1
    beqz        $v0, .L003F0
    cache       0x18, 0x100($v1)
    cache       0x18, 0xC0($v1)
    swc1        $f0, 0xC0($v1)
    swc1        $f1, 0xC4($v1)
    swc1        $f2, 0xC8($v1)
    swc1        $f3, 0xCC($v1)
    swc1        $f4, 0xD0($v1)
    swc1        $f5, 0xD4($v1)
    swc1        $f6, 0xD8($v1)
    swc1        $f7, 0xDC($v1)
    swc1        $f8, 0xE0($v1)
    swc1        $f9, 0xE4($v1)
    swc1        $f10, 0xE8($v1)
    swc1        $f11, 0xEC($v1)
    swc1        $f12, 0xF0($v1)
    swc1        $f13, 0xF4($v1)
    swc1        $f14, 0xF8($v1)
    swc1        $f15, 0xFC($v1)
    swc1        $f16, 0x100($v1)
    swc1        $f17, 0x104($v1)
    swc1        $f18, 0x108($v1)
    swc1        $f19, 0x10C($v1)
.L003F0:
    swc1        $f20, 0x110($v1)
    swc1        $f21, 0x114($v1)
    swc1        $f22, 0x118($v1)
    swc1        $f23, 0x11C($v1)
    swc1        $f24, 0x120($v1)
    swc1        $f25, 0x124($v1)
    swc1        $f26, 0x128($v1)
    swc1        $f27, 0x12C($v1)
    swc1        $f28, 0x130($v1)
    swc1        $f29, 0x134($v1)
    swc1        $f30, 0x138($v1)
    swc1        $f31, 0x13C($v1)
.L00420:
    cfc0        $v0, $14
    ctc0        $v0, $8
    lb          $v1, 0x24($v0)
    lwc1        $f20, 0x110($v0)
    lwc1        $f21, 0x114($v0)
    lwc1        $f22, 0x118($v0)
    lwc1        $f23, 0x11C($v0)
    lwc1        $f24, 0x120($v0)
    lwc1        $f25, 0x124($v0)
    lwc1        $f26, 0x128($v0)
    lwc1        $f27, 0x12C($v0)
    lwc1        $f28, 0x130($v0)
    lwc1        $f29, 0x134($v0)
    lwc1        $f30, 0x138($v0)
    lwc1        $f31, 0x13C($v0)
    xori        $v1, $v1, 0x1
    beqz        $v1, .L004B8
    lw          $v1, 0x28($v0)
    lwc1        $f0, 0xC0($v0)
    lwc1        $f1, 0xC4($v0)
    lwc1        $f2, 0xC8($v0)
    lwc1        $f3, 0xCC($v0)
    lwc1        $f4, 0xD0($v0)
    lwc1        $f5, 0xD4($v0)
    lwc1        $f6, 0xD8($v0)
    lwc1        $f7, 0xDC($v0)
    lwc1        $f8, 0xE0($v0)
    lwc1        $f9, 0xE4($v0)
    lwc1        $f10, 0xE8($v0)
    lwc1        $f11, 0xEC($v0)
    lwc1        $f12, 0xF0($v0)
    lwc1        $f13, 0xF4($v0)
    lwc1        $f14, 0xF8($v0)
    lwc1        $f15, 0xFC($v0)
    lwc1        $f16, 0x100($v0)
    lwc1        $f17, 0x104($v0)
    lwc1        $f18, 0x108($v0)
    lwc1        $f19, 0x10C($v0)

# Subroutine sub_000004B8 - Address 0x000004B8
# Part of sub_00000338() (a branch target the analyser sees as a function): load FCR31 (v1), then run the
# code at 0x300 if the COP0 control register 25 is set.
    .globl sub_000004B8
sub_000004B8:
.L004B8:
    ctc1        $zero, $31
    ctc1        $v1, $31
    cfc0        $v1, $25
    bnez        $v1, .L00300
    nop

# Subroutine sub_000004CC - Address 0x000004CC
# Part of sub_00000338() (a branch target the analyser sees as a function): return from the exception.
    .globl sub_000004CC
sub_000004CC:
.L004CC:
    cfc0        $v1, $0
    mtc0        $v1, $14
    cfc0        $v0, $4
    cfc0        $v1, $5
    eret
.L004E0:
    cfc0        $v0, $2
    ins         $v0, $v1, 29, 1
    mtc0        $v0, $12
    ctc0        $v0, $2
    cfc0        $v0, $8
    bgezl       $v0, .L004B8
    addiu       $v1, $zero, 0xE00
    sb          $v1, 0x25($v0)
    ctc0        $zero, $8
    lb          $v1, 0x24($v0)
    cache       0x18, 0x100($v0)
    beqz        $v1, .L00328
    xori        $v1, $v1, 0x1
    swc1        $f20, 0x110($v0)
    swc1        $f21, 0x114($v0)
    swc1        $f22, 0x118($v0)
    swc1        $f23, 0x11C($v0)
    swc1        $f24, 0x120($v0)
    swc1        $f25, 0x124($v0)
    swc1        $f26, 0x128($v0)
    swc1        $f27, 0x12C($v0)
    swc1        $f28, 0x130($v0)
    swc1        $f29, 0x134($v0)
    swc1        $f30, 0x138($v0)
    swc1        $f31, 0x13C($v0)
    beqz        $v1, .L004B8
    addiu       $v1, $zero, 0xE00
    cache       0x18, 0xC0($v0)
    swc1        $f0, 0xC0($v0)
    swc1        $f1, 0xC4($v0)
    swc1        $f2, 0xC8($v0)
    swc1        $f3, 0xCC($v0)
    swc1        $f4, 0xD0($v0)
    swc1        $f5, 0xD4($v0)
    swc1        $f6, 0xD8($v0)
    swc1        $f7, 0xDC($v0)
    swc1        $f8, 0xE0($v0)
    swc1        $f9, 0xE4($v0)
    swc1        $f10, 0xE8($v0)
    swc1        $f11, 0xEC($v0)
    swc1        $f12, 0xF0($v0)
    swc1        $f13, 0xF4($v0)
    swc1        $f14, 0xF8($v0)
    swc1        $f15, 0xFC($v0)
    swc1        $f16, 0x100($v0)
    swc1        $f17, 0x104($v0)
    swc1        $f18, 0x108($v0)
    j           .L004B8
    swc1        $f19, 0x10C($v0)

# Subroutine sub_000005A4 - Address 0x000005A4
# Save the FPU registers into a context (a0, SceThreadmanContext), with the FPU enabled (Status.CU1)
# meanwhile, and clear THREADMAN_COP0_CTRL_FPU_OWNER. $f0 - $f19 are only saved if the byte at offset 36 of
# the context (its type) is not 1.
    .globl sub_000005A4
sub_000005A4:
    cache       0x18, 0x100($a0)
    mfc0        $v0, $12
    lui         $v1, 0x2000
    or          $v1, $v0, $v1
    mtc0        $v1, $12
    lb          $v1, 0x24($a0)
    swc1        $f20, 0x110($a0)
    swc1        $f21, 0x114($a0)
    swc1        $f22, 0x118($a0)
    swc1        $f23, 0x11C($a0)
    swc1        $f24, 0x120($a0)
    swc1        $f25, 0x124($a0)
    swc1        $f26, 0x128($a0)
    swc1        $f27, 0x12C($a0)
    swc1        $f28, 0x130($a0)
    swc1        $f29, 0x134($a0)
    swc1        $f30, 0x138($a0)
    xori        $v1, $v1, 0x1
    beqz        $v1, .L00648
    swc1        $f31, 0x13C($a0)
    cache       0x18, 0xC0($a0)
    swc1        $f0, 0xC0($a0)
    swc1        $f1, 0xC4($a0)
    swc1        $f2, 0xC8($a0)
    swc1        $f3, 0xCC($a0)
    swc1        $f4, 0xD0($a0)
    swc1        $f5, 0xD4($a0)
    swc1        $f6, 0xD8($a0)
    swc1        $f7, 0xDC($a0)
    swc1        $f8, 0xE0($a0)
    swc1        $f9, 0xE4($a0)
    swc1        $f10, 0xE8($a0)
    swc1        $f11, 0xEC($a0)
    swc1        $f12, 0xF0($a0)
    swc1        $f13, 0xF4($a0)
    swc1        $f14, 0xF8($a0)
    swc1        $f15, 0xFC($a0)
    swc1        $f16, 0x100($a0)
    swc1        $f17, 0x104($a0)
    swc1        $f18, 0x108($a0)
    swc1        $f19, 0x10C($a0)
.L00648:
    mtc0        $v0, $12
    jr          $ra
    ctc0        $zero, $8
    .p2align 6
