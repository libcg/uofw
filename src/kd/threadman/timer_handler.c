/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/timer_handler.c
 *
 * Timers, continued (original address range 0x000112F8 - 0x00011A24): the suspend and resume handlers of
 * the system timer, the system clock division, the alarm and virtual timer handler calls, sceKernelSetAlarm()'s
 * common function and the timer UID type functions.
 *
 * In the original module these functions follow sub_000112E0 (timer_syscall.S), which itself follows the
 * functions of timer.c: timer.c, timer_syscall.S and this file are linked in this order to reproduce it.
 *
 * PARITY-Q021 (ac3e625): the split of the timer functions into timer.c / timer_handler.c reproduces the original's
 * function order around sub_000112E0.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <loadcore.h>
#include <sysmem_kernel.h>
#include <sysmem_suspend_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

// 0x00019F00
/** The counter of the system timer, saved on suspend and restored on resume. */
u32 g_sysTimerSavedCounter;

/*
 * PARITY-Q025 (696dede): Uses nextCompare volatile.
 * PARITY-Q063 (696dede): Single read and s32 difference of the compare value (the original's code).
 */
/**
 * The suspend handler of the system timer: save its counter.
 *
 * @param unk Unknown.
 * @param param Unused.
 *
 * @return 0.
 */
// Subroutine sub_000112F8 - Address 0x000112F8
s32 _sceKernelSysTimerSuspend(s32 unk, void *param)
{
    s32 diff;

    (void)unk;
    (void)param;

    g_sysTimerSavedCounter = HW(SYSTIMER_COUNTER);
    sceKernelDisableIntr(g_threadman.sysTimerIntr);

    /* Make sure the counter is restored before the compare value so that the interrupt is not missed. */
    diff = g_threadman.nextCompare.low - g_sysTimerSavedCounter;
    if (diff == 0 || (u32)diff > SYSTIMER_OVF_PERIOD)
        g_sysTimerSavedCounter = g_threadman.nextCompare.low - 1;
    return 0;
}

/*
 * PARITY-Q001 (91cfbbf): Uses divu asm in the system clock division.
 */
/**
 * The resume handler of the system timer: restore its state.
 *
 * @param unk Unknown.
 * @param param Unused.
 *
 * @return 0.
 */
// Subroutine sub_0001136C - Address 0x0001136C
s32 _sceKernelSysTimerResume(s32 unk, void *param)
{
    (void)unk;
    (void)param;

    HW(SYSTIMER_UNKC) = 1;
    HW(SYSTIMER_UNK8) = 0x30;
    HW(SYSTIMER_UNK10) = 0;
    pspSync();
    HW(SYSTIMER_COUNTER) = g_sysTimerSavedCounter;
    pspSync();
    HW(SYSTIMER_COMPARE) = g_threadman.nextCompare.low;
    pspSync();
    pspSync();
    sceKernelEnableIntr(g_threadman.sysTimerIntr);
    return 0;
}

/*
 * 32-bit unsigned division through inline asm, as in the original module: the two-operand
 * `divu rs, rt` is expanded by the assembler as its divide macro (`divu rs, rs, rt`: zero-divisor
 * check, `divu $zero`, `break 7`, `mflo rs`), which GCC never emits itself. The quotient and remainder
 * are then read from LO/HI, so the macro's own `mflo rs` is dead. See threadman-deviations.md.
 * As in the original, the asm does not declare that the macro also writes its first operand: the dividend
 * must be dead after each use (it is at both uses below; checked in the compiled code).
 * GCC 4.4 and later have no "h" constraint: they use the C division, which is equivalent.
 */
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
/*
 * PARITY-Q001: divu through inline asm: the original's _sceKernelSysClockDivide has a bare divu/mflo/mfhi with no
 * zero-divide trap
 */
#define THREADMAN_DIVU(quot, rem, a, b) do { \
    u32 _dividend = (a), _divisor = (b); \
    (quot) = _dividend / _divisor; \
    (rem) = _dividend % _divisor; \
} while (0)
#else
#define THREADMAN_DIVU(quot, rem, a, b) \
    __asm__ __volatile__ ("divu %2, %3" : "=l" (quot), "=h" (rem) : "r" (a), "r" (b))
#endif

/*
 * PARITY-Q001 (91cfbbf): Uses divu asm in the system clock division.
 * PARITY-Q121: quot->hi is stored before quot->low. The scheduler then emits the stores in the original's order
 *     (sw 0x0 then sw 0x4). wsim +0.028, csim 0, still equiv. Residue: the quot argument is copied to $t3 and $a0 is
 *     reused for quotHi; the remainder stays in HI across the loop (the original keeps it in $v1 with an mfhi in
 *     the loop); and beqz/nop is used where the original has bnel for the rem store.
 *     Rejected: the three orders of the low/quotHi/first-divide statements (0 or down).
 */
/**
 * Divide a system clock value by a 32-bit value.
 *
 * @param quot Pointer receiving the quotient (may be NULL, may be clock).
 * @param clock The dividend.
 * @param div The divisor.
 * @param rem Pointer receiving the remainder (may be NULL).
 */
// Subroutine sub_000113F0 - Address 0x000113F0
void _sceKernelSysClockDivide(SceKernelSysClock *quot, SceKernelSysClock *clock, u32 div, unsigned int *rem)
{
    s32 i;
    u32 low;
    u32 cur;
    u32 r;
    u32 quotLow;
    u32 quotHi;
    u32 q;

    low = clock->low;
    quotHi = 0;
    THREADMAN_DIVU(quotLow, r, clock->hi, div);
    for (i = 3; i >= 0; i--) {
        cur = (r << 8) | (low >> 24);
        low <<= 8;
        THREADMAN_DIVU(q, r, cur, div);
        quotHi = (quotHi << 8) | (quotLow >> 24);
        quotLow = (quotLow << 8) + q;
    }
    if (quot != NULL) {
        quot->hi = quotHi;
        quot->low = quotLow;
    }
    if (rem != NULL)
        *rem = r;
}

/*
 * PARITY-Q005 (3d248ac): Uses volatile $gp swap around handler calls.
 */
/**
 * Call the handler of an expired alarm.
 *
 * @param uid The control block of the alarm.
 * @param intr The interrupt state to restore while calling a kernel mode handler.
 *
 * @return The return value of the handler: the delay until its next call, 0 to delete the alarm.
 */
// Subroutine sub_0001147C - Address 0x0001147C
SceUInt _sceKernelCallAlarmHandler(SceSysmemUidCB *uid, s32 intr)
{
    u32 oldGp;
    SceUInt ret;
    SceThreadmanAlarm *alarm;

    alarm = UID_CB_TO_DATA(uid, g_threadman.alarmType, SceThreadmanAlarm);
    g_threadman.curTimerUid = uid->uid;

    THREADMAN_LOG(SCE_THREADMAN_LOG_CALL_ALARM_HANDLER, 3, uid->uid, alarm->common, alarm->handler);

    oldGp = threadmanSwapGp(alarm->gp);
    if (!alarm->isUser) {
        sceKernelCpuResumeIntr(intr);
        ret = alarm->handler(alarm->common);
        sceKernelCpuSuspendIntr();
    } else
        ret = sceKernelCallUserIntrHandler((s32)alarm->common, 0, 0, 0, (s32)alarm->handler,
            sceKernelGetUserIntrStack());
    pspSetGp(oldGp);

    g_threadman.curTimerUid = 0;
    return ret;
}

/*
 * PARITY-Q005 (3d248ac): Uses volatile $gp swap around handler calls.
 */
/**
 * Call the handler of an expired virtual timer.
 *
 * @param uid The control block of the virtual timer.
 * @param intr The interrupt state to restore while calling a kernel mode handler.
 *
 * @return The return value of the handler: the delay until its next call, 0 to stop calling it.
 */
// Subroutine sub_00011574 - Address 0x00011574
SceUInt _sceKernelCallVTimerHandler(SceSysmemUidCB *uid, s32 intr)
{
    u32 oldGp;
    u64 time;
    SceUInt ret;
    SceKernelSysClock actual;
    SceKernelSysClock *frame;
    SceThreadmanVTimer *vtimer;

    vtimer = UID_CB_TO_DATA(uid, g_threadman.vtimerType, SceThreadmanVTimer);
    g_threadman.curTimerUid = uid->uid;

    time = _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base);
    threadmanSysClockSet(&actual, time);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CALL_VTIMER_HANDLER, 5, uid->uid, vtimer->schedule.low, actual.low,
        vtimer->common, vtimer->handler);

    oldGp = threadmanSwapGp(vtimer->gp);
    if (!vtimer->isUser) {
        sceKernelCpuResumeIntr(intr);
        ret = vtimer->handler(uid->uid, &vtimer->schedule, &actual, vtimer->common);
        sceKernelCpuSuspendIntr();
    } else {
        /* Pass the schedule and the actual time on the user interrupt stack. */
        frame = (SceKernelSysClock *)sceKernelGetUserIntrStack();
        frame -= 2;
        frame[0] = vtimer->schedule;
        frame[1] = actual;
        ret = sceKernelCallUserIntrHandler(uid->uid, (s32)&frame[0], (s32)&frame[1], (s32)vtimer->common,
            (s32)vtimer->handler, (s32)frame);
    }
    pspSetGp(oldGp);

    g_threadman.curTimerUid = 0;
    return ret;
}

/*
 * PARITY-Q053 (acc54fa, 99612ae): Error/success paths share one return (the original's join).
 * PARITY-Q065 (99612ae): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
/**
 * Create an alarm.
 *
 * @param clock The number of microseconds until the alarm expires.
 * @param handler The handler called on expiration.
 * @param common The argument passed to the handler.
 *
 * @return The UID of the new alarm on success, < 0 on error.
 */
// Subroutine sub_000116F4 - Address 0x000116F4
SceUID _sceKernelSetAlarm(u64 clock, SceKernelAlarmHandler handler, void *common)
{
    s32 intr;
    s32 status;
    u64 schedule;
    SceSysmemUidCB *uid;
    SceThreadmanTimer *timer;
    SceThreadmanAlarm *alarm;

    /* A kernel mode alarm handler must be in kernel memory. */
    if (!pspK1IsUserMode() && (s32)handler >= 0)
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelCreateUID(g_threadman.alarmType, "SceThreadmanAlarm", pspGetK1() >> 31, &uid);
    if (status != SCE_ERROR_OK)
        goto end;

    alarm = UID_CB_TO_DATA(uid, g_threadman.alarmType, SceThreadmanAlarm);
    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    schedule = clock + _sceKernelGetSystemTime64();
    threadmanSysClockSet(&timer->schedule, schedule);
    alarm->common = common;
    alarm->isUser = pspK1IsUserMode();
    alarm->handler = handler;
    alarm->gp = sceKernelGetModuleGPByAddressForKernel((u32)handler);
    if (alarm->gp == 0)
        alarm->gp = threadmanGetGp();
    _sceKernelInsertTimer(timer);
    _sceKernelUpdateSysTimerCompare();
    status = uid->uid;
    sceKernelCpuResumeIntr(intr);
    return status;

end:
    sceKernelCpuResumeIntr(intr);
    return status;
}

// Subroutine sub_00011834 - Address 0x00011834
s32 _sceKernelDelayInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_0001185C - Address 0x0001185C
s32 _sceKernelDelayDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

// Subroutine sub_0001187C - Address 0x0001187C
s32 _sceKernelAlarmInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_000118A4 - Address 0x000118A4
s32 _sceKernelAlarmDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

// Subroutine sub_000118C4 - Address 0x000118C4
s32 _sceKernelVTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_000118EC - Address 0x000118EC
s32 _sceKernelVTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    vtimer = UID_CB_TO_DATA(uid, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL) {
        _sceKernelRemoveTimer(timer);
        _sceKernelUpdateSysTimerCompare();
    }
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q042 (df2331f): Statement/store order taken from the original's store order.
 */
// Subroutine sub_000119B8 - Address 0x000119B8
s32 _sceKernelTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanTimer *timer;

    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    timer = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanTimer);
    timer->deletePending = 0;
    timer->link.prev = &timer->link;
    timer->link.next = &timer->link;
    return uid->uid;
}

// Subroutine sub_00011A04 - Address 0x00011A04
s32 _sceKernelTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

