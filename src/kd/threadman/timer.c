/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/timer.c
 *
 * Timers: the system timer, the delay timers, the alarms, the virtual timers and the system time
 * (original address range 0x0000E5EC - 0x000112E0; the timers continue in timer_handler.c, 0x000112F8 -
 * 0x00011A24, after sub_000112E0 in timer_syscall.S).
 *
 * The system time is a 64-bit microsecond counter: its lower 32 bits are the counter of the system
 * timer hardware and its upper 32 bits are kept in g_threadman.sysTimeBase, updated on every compare
 * interrupt (an internal "SceThreadmanSysTimerOVF" timer makes sure one happens at least every
 * 0xF0000000 microseconds).
 *
 * All the timers are "Timer" UID objects (SceThreadmanTimer), kept in g_threadman.timers sorted by
 * their schedule. The compare register of the system timer is programmed with the schedule of the
 * first timer, and its interrupt handler runs the expired timers. There are three kinds of timers,
 * inheriting the "Timer" type:
 *  - "Delay": internal one-shot timers, used for the timeouts of the wait functions. Up to 32 free
 *    delay timers are kept for reuse, as they can be needed in interrupt context;
 *  - "Alarm": one-shot timers whose handler can reschedule them (sceKernelSetAlarm());
 *  - "VTimer": virtual timers, which count time only while started, with an optional handler.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <loadcore.h>
#include <sysmem_kernel.h>
#include <sysmem_suspend_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

void sub_000112E0(void);
static s32 _sceKernelSysTimerIntr(s32 intrNum, SceThreadman *threadman);
u64 _sceKernelGetSystemTime64(void);
static void _sceKernelFreeDelay(SceSysmemUidCB *uid);

// 0x00019DD0
SceSysmemUidLookupFunc g_DelayFuncs[] = {
    { 0xD310D2D9, _sceKernelDelayInitialize },
    { 0x87089863, _sceKernelDelayDelete },
    { 0, NULL }
};

// 0x00019DE8
SceSysmemUidLookupFunc g_AlarmFuncs[] = {
    { 0xD310D2D9, _sceKernelAlarmInitialize },
    { 0x87089863, _sceKernelAlarmDelete },
    { 0, NULL }
};

// 0x00019E00
SceSysmemUidLookupFunc g_VTimerFuncs[] = {
    { 0xD310D2D9, _sceKernelVTimerInitialize },
    { 0x87089863, _sceKernelVTimerDelete },
    { 0, NULL }
};

// 0x00019E18
SceSysmemUidLookupFunc g_TimerFuncs[] = {
    { 0xD310D2D9, _sceKernelTimerInitialize },
    { 0x87089863, _sceKernelTimerDelete },
    { 0, NULL }
};

/*
 * PARITY-Q039 (f6c063a): List heads initialised with threadmanListInit (the original's store pattern).
 * PARITY-Q049 (36877a1): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q125: The g_threadman field stores are written in structure-offset order (unk2032/unk2036, then
 *     minTimerDelay, minCompareDelay, getSystemTimeLow, getSystemTime). Prologue now matches. Alternative
 *     (non-natural order minCompareDelay, getSystemTimeLow, getSystemTime, minTimerDelay) scores slightly higher.
 * PARITY-Q161: The overflow timer's schedule is set with threadmanSysClockSet32() (equiv -> sched, size = orig).
 */
/**
 * Initialize the system timer and the timer list.
 */
// Subroutine sub_0000E5EC - Address 0x0000E5EC
void _sceKernelSysTimerInit(void)
{
    s32 i;
    s32 intr;
    SceKernelSysClock clock;
    SceSysmemUidCB *uid;
    SceThreadmanTimer *timer;

    g_threadman.unk2032 = 1;
    g_threadman.unk2036 = 1;
    sceKernelUSec2SysClock(100, &clock);
    g_threadman.minTimerDelay = clock.low;
    g_threadman.minCompareDelay = clock.low * 2;
    g_threadman.getSystemTimeLow = sceKernelGetSystemTimeLow;
    g_threadman.getSystemTime = _sceKernelGetSystemTime64;
    if (g_threadman.cop0State22 == 0)
        g_threadman.sysTimerIntr = 0x13;
    else
        g_threadman.sysTimerIntr = 4;

    HW(SYSTIMER_UNKC) = 1;
    HW(SYSTIMER_UNK8) = 0x30;
    HW(SYSTIMER_UNK10) = 0;
    pspSync();

    sceKernelRegisterIntrHandler(g_threadman.sysTimerIntr, 1, _sceKernelSysTimerIntr, &g_threadman, NULL);
    sceKernelSetIntrLevel(g_threadman.sysTimerIntr, 2);

    /*
     * PARITY-Q198 (source order): timers is initialised before freeDelays. In the natural
     * order the freeDelays head chain closes after the hoisted %hi("SceThreadmanDelay") and
     * regrename hands it $s4 (tick 0), which shifts the later %hi chains ($s4/$a3 -> $a3/$t0)
     * and drops the lui $s4 out of the SetPrimarySyscallHandler delay slot.
     *
     * PARITY-Q201 (source form): the free delay loop counts up and threadmanListInit stores
     * next before prev. loop.c reverses the count-up loop (i is only a counter; the original
     * counts down to bgezl), and with that the i = 31 init no longer precedes the hoisted
     * lui %hi("SceThreadmanDelay"): sched2 then emits lui $s4 before the freeDelays prev
     * store and addiu $s2 after it, as in the original.
     */
    threadmanListInit(&g_threadman.timers);
    threadmanListInit(&g_threadman.freeDelays);
    g_threadman.numFreeDelays = 0;
    g_threadman.nextTimer = NULL;

    intr = sceKernelCpuSuspendIntr();

    /* The overflow timer, which keeps the upper 32 bits of the system time up to date. */
    sceKernelCreateUID(g_threadman.timerType, "SceThreadmanSysTimerOVF", 0, &uid);
    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    threadmanSysClockSet32(&timer->schedule, SYSTIMER_OVF_PERIOD);
    threadmanSysClockSet32(&g_threadman.sysTimeBase, 0);
    /*
     * PARITY-Q197 (source order): the overflow timer is linked into the timers list
     * before nextCompare is copied from its schedule. With the copy first, sched1's
     * register-weight rule puts entry->prev->next = entry ahead of the nextCompare.hi
     * store and ListAddTail's $t1/$t2/$t3 come out permuted against the original.
     */
    threadmanListAddTail(&g_threadman.timers, &timer->link);
    g_threadman.nextCompare = timer->schedule;

    for (i = 0; i < SCE_THREADMAN_MAX_FREE_DELAYS; i++) {
        sceKernelCreateUID(g_threadman.delayType, "SceThreadmanDelay", 0, &uid);
        _sceKernelFreeDelay(uid);
    }

    HW(SYSTIMER_COMPARE) = g_threadman.nextCompare.low;
    pspSync();
    HW(SYSTIMER_COUNTER) = 0;
    pspSync();

    sceKernelSetPrimarySyscallHandler(SYSTIMER_HANDLER_ID, sub_000112E0);
    sceKernelRegisterSuspendHandler(SYSTIMER_HANDLER_ID, _sceKernelSysTimerSuspend, NULL);
    sceKernelRegisterResumeHandler(SYSTIMER_HANDLER_ID, _sceKernelSysTimerResume, NULL);
    sceKernelEnableIntr(g_threadman.sysTimerIntr);

    sceKernelCpuResumeIntr(intr);
}

/*
 * PARITY-Q088: The alarm re-arm adds the clamped delay directly (`schedule += delay < min ? min : delay;`) instead
 *     of reassigning delay (the original's movz into the minTimerDelay register).
 */
/**
 * The interrupt handler of the system timer: run the expired timers.
 *
 * @param intrNum The interrupt number (the interrupt manager passes it in $a0 to a main interrupt handler,
 * as modelled by PPSSPP's IntrHandler::copyArgsToCPU()).
 * @param threadman The global state of the thread manager.
 *
 * @return -1.
 */
// Subroutine sub_0000E80C - Address 0x0000E80C
static s32 _sceKernelSysTimerIntr(s32 intrNum, SceThreadman *threadman)
{
    s32 intr;
    u32 delay;
    u64 now;
    u64 schedule;
    SceThreadmanListEntry *entry;
    SceThreadmanTimer *timer;
    SceThreadmanDelay *delayTimer;
    SceThreadmanVTimer *vtimer;
    SceSysmemUidCB *uid;

    (void)intrNum;

    intr = sceKernelCpuSuspendIntr();

    threadman->sysTimeBase = threadman->nextCompare;
    now = _sceKernelGetSystemTime64();

    for (entry = threadman->timers.next; entry != &threadman->timers; entry = threadman->nextTimer) {
        timer = (SceThreadmanTimer *)entry;
        schedule = threadmanSysClockGet(&timer->schedule);
        threadman->nextTimer = entry->next;
        if ((s64)(schedule - now) > 0)
            break;

        threadmanListRemove(&timer->link);
        timer->link.next = &timer->link;
        timer->link.prev = &timer->link;

        uid = THREADMAN_UID_DATA_TO_CB(timer, ((SceSysmemUidCB *)g_threadman.timerType));
        if (sceKernelIsKindOf(uid, g_threadman.delayType)) {
            delayTimer = UID_CB_TO_DATA(uid, g_threadman.delayType, SceThreadmanDelay);
            sceKernelCpuResumeIntr(intr);
            delayTimer->handler(delayTimer->common);
            intr = sceKernelCpuSuspendIntr();
            _sceKernelFreeDelay(uid);
        } else if (sceKernelIsKindOf(uid, g_threadman.alarmType)) {
            delay = _sceKernelCallAlarmHandler(uid, intr);
            if (delay == 0) {
                /* The alarm is over. */
                timer->deletePending = 1;
                sub_000126AC(uid->uid);
            } else {
                schedule += (delay < threadman->minTimerDelay) ? threadman->minTimerDelay : delay;
                threadmanSysClockSet(&timer->schedule, schedule);
                _sceKernelInsertTimer(timer);
            }
        } else if (sceKernelIsKindOf(uid, g_threadman.vtimerType)) {
            vtimer = UID_CB_TO_DATA(uid, g_threadman.vtimerType, SceThreadmanVTimer);
            delay = _sceKernelCallVTimerHandler(uid, intr);
            if (delay == 0) {
                vtimer->handler = NULL;
            } else {
                if (delay < threadman->minTimerDelay)
                    delay = threadman->minTimerDelay;
                schedule += delay;
                threadmanSysClockSet(&timer->schedule, schedule);
                schedule = threadmanSysClockGet(&vtimer->schedule) + delay;
                threadmanSysClockSet(&vtimer->schedule, schedule);
                _sceKernelInsertTimer(timer);
            }
        } else {
            /* The overflow timer. */
            schedule += SYSTIMER_OVF_PERIOD;
            threadmanSysClockSet(&timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
        }
    }

    _sceKernelUpdateSysTimerCompare();
    threadman->nextTimer = NULL;

    sceKernelCpuResumeIntr(intr);
    return -1;
}

/*
 * PARITY-Q050 (7957ad3): Loop shape (guarded block instead of continue / separate next cursor / head pointer walk) as
 *     the original's layout.
 * PARITY-Q051 (7957ad3): Access through a local/named pointer (the original's addressing).
 * PARITY-Q119: the schedule pointer is a local declared in the loop body, not set once before the loop. Its
 *     address is then recomputed inside the loop and hoisted only late, so the low word is read through timer
 *     (lw 0x8(timer)) and the high word through the pointer, and `move timer` fills the beq delay slot, as in
 *     the original. equiv -> exact. An assignment in the loop to a function-scope pointer matches the same way.
 *     Rejected: dropping the pointer (down), and swapping the head/schedule assignments (0).
 */
/**
 * Insert a timer in the timer list, after the timers expiring before or at the same time.
 *
 * @param timer The timer to insert.
 */
// Subroutine sub_0000EAE0 - Address 0x0000EAE0
void _sceKernelInsertTimer(SceThreadmanTimer *timer)
{
    SceThreadmanListEntry *entry;
    SceThreadmanListEntry *head;

    head = &g_threadman.timers;
    for (entry = head->next; entry != head; entry = entry->next) {
        SceKernelSysClock *schedule = &timer->schedule;
        if (THREADMAN_SYSCLOCK_GET(*schedule) < THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule))
            break;
    }
    timer->link.next = entry;
    timer->link.prev = entry->prev;
    entry->prev = &timer->link;
    timer->link.prev->next = &timer->link;
}

/*
 * PARITY-Q025: Uses nextCompare volatile.
 * PARITY-Q050 (a5da7c8): Loop shape (guarded block instead of continue / separate next cursor / head pointer walk) as
 *     the original's layout.
 * PARITY-Q051 (d2ad177): Access through a local/named pointer (the original's addressing).
 * PARITY-Q120: the first schedule is read through head->next, and entry is set in the for init, not assigned
 *     before. The first timer's address and the loop cursor are then separate values, which gives the original's
 *     register assignment for the loop. equiv -> exact. Assigning `entry = head->next;` after the schedule read
 *     matches the same way; the for init is closer to natural source.
 */
/**
 * Program the compare register of the system timer for the next timer to expire.
 *
 * The timers expiring shortly after the first one are grouped (the compare is set to the last of
 * them), and the compare is never set too close to the current time.
 */
// Subroutine sub_0000EB7C - Address 0x0000EB7C
void _sceKernelUpdateSysTimerCompare(void)
{
    u64 schedule;
    u64 limit;
    u64 now;
    SceThreadmanListEntry *entry;
    SceThreadmanListEntry *head;
    SceKernelSysClock *nextCompare;

    head = &g_threadman.timers;
    schedule = THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)head->next)->schedule);
    limit = schedule + g_threadman.minCompareDelay;
    for (entry = head->next; entry != head; entry = entry->next) {
        if ((s64)(limit - THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule)) <= 0)
            break;
        schedule = THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule);
    }

    now = _sceKernelGetSystemTime64();
    if ((s64)(schedule - now) < (s64)g_threadman.minCompareDelay)
        schedule = now + g_threadman.minCompareDelay;

    nextCompare = (SceKernelSysClock *)&g_threadman.nextCompare;
    THREADMAN_SYSCLOCK_SET(*nextCompare, schedule);
    HW(SYSTIMER_COMPARE) = (u32)schedule;
    pspSync();
}

/**
 * Convert a number of microseconds to a system clock value.
 *
 * @param usec The number of microseconds.
 * @param clock Pointer to a ::SceKernelSysClock receiving the converted value.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelUSec2SysClock - Address 0x0000ECB4
int sceKernelUSec2SysClock(unsigned int usec, SceKernelSysClock *clock)
{
    /* No pspShiftK1() here: the buffer is checked against the shifted $k1 value, $k1 itself is left alone. */
    if (((((s32)clock + (s32)sizeof(*clock)) | (s32)clock) & (pspGetK1() << 11)) < 0)
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;

    clock->low = usec;
    clock->hi = 0;
    return SCE_ERROR_OK;
}

/**
 * Get the 64-bit system time.
 *
 * @return The system time, in microseconds.
 */
// Subroutine sub_0000ECE4 - Address 0x0000ECE4
u64 _sceKernelGetSystemTime64(void)
{
    u32 hi;
    u32 low;

    hi = g_threadman.sysTimeBase.hi;
    low = HW(SYSTIMER_COUNTER);
    hi += (low < g_threadman.sysTimeBase.low);
    return ((u64)hi << 32) + low;
}

/*
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 */
/**
 * Cancel an alarm.
 *
 * @param alarmid The UID of the alarm to cancel.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelAlarm - Address 0x0000ED24
int sceKernelCancelAlarm(SceUID alarmid)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceSysmemUidCB *uid;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();

    /* An alarm cannot be cancelled from its own handler (return 0 from it instead). */
    if (g_threadman.curTimerUid == alarmid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_ALARM;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(alarmid, g_threadman.alarmType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_ALARM;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_ALARM, 1, alarmid);

    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (timer->deletePending) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_ALARM;
    }

    _sceKernelRemoveTimer(timer);
    if (THREADMAN_IS_INTR_CONTEXT()) {
        timer->deletePending = 1;
        status = sub_000126AC(alarmid);
    } else
        status = sceKernelDeleteUID(alarmid);
    if (status == SCE_ERROR_OK)
        _sceKernelUpdateSysTimerCompare();

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Free a delay timer: keep it for reuse if there are not too many free ones, delete it otherwise.
 *
 * @param uid The control block of the delay timer.
 */
// Subroutine sub_0000EEB0 - Address 0x0000EEB0
static void _sceKernelFreeDelay(SceSysmemUidCB *uid)
{
    s32 numFreeDelays;
    SceThreadmanTimer *timer;

    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    numFreeDelays = g_threadman.numFreeDelays;
    if (numFreeDelays < SCE_THREADMAN_MAX_FREE_DELAYS) {
        threadmanListAddTail(&g_threadman.freeDelays, &timer->link);
        g_threadman.numFreeDelays = numFreeDelays + 1;
    } else {
        timer->deletePending = 1;
        sub_000126AC(uid->uid);
    }
}

/**
 * Retrieve the status of an alarm.
 *
 * @param alarmid The UID of the alarm.
 * @param info Pointer to a ::SceKernelAlarmInfo receiving the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferAlarmStatus - Address 0x0000EF24
int sceKernelReferAlarmStatus(SceUID alarmid, SceKernelAlarmInfo *info)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanTimer *timer;
    SceThreadmanAlarm *alarm;
    SceKernelAlarmInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(alarmid, g_threadman.alarmType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_ALARM;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_ALARM_STATUS, 2, alarmid, info);

    alarm = UID_CB_TO_DATA(uid, g_threadman.alarmType, SceThreadmanAlarm);
    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    buf = &g_threadman.statusBuf.alarmInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    buf->schedule = timer->schedule;
    buf->handler = alarm->handler;
    buf->common = alarm->common;
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.alarmInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q042 (3b11026): Statement/store order taken from the original's store order.
 */
/**
 * Create a virtual timer.
 *
 * @param name The name of the virtual timer.
 * @param opt Unused (must be NULL or a valid pointer).
 *
 * @return The UID of the new virtual timer on success, < 0 on error.
 */
// Subroutine sceKernelCreateVTimer - Address 0x0000F0C4
SceUID sceKernelCreateVTimer(const char *name, SceKernelVTimerOptParam *opt)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceSysmemUidCB *uid;
    SceThreadmanVTimer *vtimer;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(opt, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    status = sceKernelCreateUID(g_threadman.vtimerType, name, pspGetK1() >> 31, &uid);
    if (status != SCE_ERROR_OK) {
        pspSetK1(oldK1);
        return status;
    }

    intr = sceKernelCpuSuspendIntr();

    vtimer = UID_CB_TO_DATA(uid, g_threadman.vtimerType, SceThreadmanVTimer);
    vtimer->active = 0;
    vtimer->handler = NULL;

    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_VTIMER, 1, uid->uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return uid->uid;
}

/**
 * Delete a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteVTimer - Address 0x0000F1D4
int sceKernelDeleteVTimer(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_VTIMER, 1, uid);

    sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q042 (4fb02df): Statement/store order taken from the original's store order.
 */
/**
 * Get the base of a virtual timer (the system time at which it was virtually started).
 *
 * @param uid The UID of the virtual timer.
 * @param base Pointer to a ::SceKernelSysClock receiving the base (0 if the timer is stopped).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelGetVTimerBase - Address 0x0000F2E8
int sceKernelGetVTimerBase(SceUID uid, SceKernelSysClock *base)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(base, sizeof(*base))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    if (vtimer->active)
        *base = vtimer->base;
    else
        threadmanSysClockSet32(base, 0);

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_BASE, 3, uid, base->hi, base->low);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q072: Success path inside the then-block of a positive UID/permission check, shared error tail after it
 * (the original's layout: error return out of line at the end).
 * PARITY-Q040: Permission check as its own if with its own error return inside the UID check (the original's
 *     block count keeps the log call setup in its own block).
 * PARITY-Q080: base assigned in both arms of an if/else instead of pre-set to 0 (the original's block layout and
 *     instruction count).
 * PARITY-Q122: UID lookup failure handled first with an early return (the success path follows a jump target).
 *     The label starts a new cselib table in postreload, so the log handler address is rebuilt from the kept
 *     %hi register (lui/addiu pair) instead of being copied from the vtimerType pointer register. Q040's
 *     permission check is now its own if after the lookup check rather than nested inside it.
 */
/**
 * Get the base of a virtual timer (the system time at which it was virtually started).
 *
 * @param uid The UID of the virtual timer.
 *
 * @return The base (0 if the timer is stopped), -1 on error.
 */
// Subroutine sceKernelGetVTimerBaseWide - Address 0x0000F450
SceInt64 sceKernelGetVTimerBaseWide(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    u64 base;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return -1;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return -1;
    }
    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    if (vtimer->active)
        base = threadmanSysClockGet(&vtimer->base);
    else
        base = 0;

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_BASE_WIDE, 2, uid, (u32)base);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return base;
}

/*
 * PARITY-Q042 (e0d596d): Statement/store order taken from the original's store order.
 */
/**
 * Get the current time of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 * @param time Pointer to a ::SceKernelSysClock receiving the time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelGetVTimerTime - Address 0x0000F57C
int sceKernelGetVTimerTime(SceUID uid, SceKernelSysClock *time)
{
    s32 oldK1;
    s32 intr;
    u64 now;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(time, sizeof(*time))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    if (vtimer->active) {
        now = _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base);
        threadmanSysClockSet(time, now);
    } else
        *time = vtimer->base;

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_TIME, 3, uid, time->hi, time->low);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q072: Success path inside the then-block of a positive UID/permission check, shared error tail after it
 * (the original's layout: error return out of line at the end).
 * PARITY-Q040: Permission check as its own if with its own error return inside the UID check (the original's
 *     block count keeps the log call setup in its own block).
 */
/**
 * Get the current time of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 *
 * @return The time, -1 on error.
 */
// Subroutine sceKernelGetVTimerTimeWide - Address 0x0000F71C
SceInt64 sceKernelGetVTimerTimeWide(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    u64 time;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) == SCE_ERROR_OK) {
        if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return -1;
        }
        vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
        if (vtimer->active)
            time = _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base);
        else
            time = threadmanSysClockGet(&vtimer->base);

        THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_TIME_WIDE, 2, uid, (u32)time);

        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return time;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return -1;
}

/*
 * PARITY-Q065 (0cfff40): active arm first (the original's block layout and register choice).
 * PARITY-Q088: The rescheduled expiry goes into its own `schedule` local instead of reusing `base` (separate
 *     lifetimes give the original's register assignment).
 */
/**
 * Set the current time of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 * @param time Pointer to the new time, receiving the previous time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSetVTimerTime - Address 0x0000F874
int sceKernelSetVTimerTime(SceUID uid, SceKernelSysClock *time)
{
    s32 oldK1;
    s32 intr;
    u64 old;
    u64 base;
    u64 schedule;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (!pspK1StaBufOk(time, sizeof(*time))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_VTIMER_TIME, 3, uid, time->hi, time->low);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (vtimer->active) {
        old = _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base);
        base = _sceKernelGetSystemTime64() - threadmanSysClockGet(time);
        threadmanSysClockSet(&vtimer->base, base);
        if (vtimer->handler != NULL) {
            _sceKernelRemoveTimer(timer);
            schedule = threadmanSysClockGet(&vtimer->schedule) + threadmanSysClockGet(&vtimer->base);
            threadmanSysClockSet(&timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    } else {
        old = threadmanSysClockGet(&vtimer->base);
        vtimer->base = *time;
    }
    threadmanSysClockSet(time, old);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q072: Success path inside the then-block of a positive UID/permission check, shared error tail after it
 * (the original's layout: error return out of line at the end).
 * PARITY-Q040: Permission check as its own if with its own error return inside the UID check (the original's
 *     block count keeps the log call setup in its own block).
 * PARITY-Q065 (0cfff40): active arm first (the original's block layout and register choice).
 */
/**
 * Set the current time of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 * @param time The new time.
 *
 * @return The previous time, -1 on error.
 */
// Subroutine sceKernelSetVTimerTimeWide - Address 0x0000FB4C
SceInt64 sceKernelSetVTimerTimeWide(SceUID uid, SceInt64 time)
{
    s32 oldK1;
    s32 intr;
    u64 old;
    u64 base;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return -1;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) == SCE_ERROR_OK) {
        if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return -1;
        }
        THREADMAN_LOG(SCE_THREADMAN_LOG_SET_VTIMER_TIME_WIDE, 2, uid, (u32)time);

        vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
        timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
        if (vtimer->active) {
            old = _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base);
            base = _sceKernelGetSystemTime64() - time;
            threadmanSysClockSet(&vtimer->base, base);
            if (vtimer->handler != NULL) {
                _sceKernelRemoveTimer(timer);
                base = threadmanSysClockGet(&vtimer->schedule) + threadmanSysClockGet(&vtimer->base);
                threadmanSysClockSet(&timer->schedule, base);
                _sceKernelInsertTimer(timer);
                _sceKernelUpdateSysTimerCompare();
            }
        } else {
            old = threadmanSysClockGet(&vtimer->base);
            threadmanSysClockSet(&vtimer->base, time);
        }

        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return old;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return -1;
}


/*
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 * PARITY-Q080 (86f7e58): ret assigned in both arms of an if/else instead of pre-set to 1 (the original
 *     materializes the active = 1 store constant separately from the return value).
 */
/**
 * Start a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 *
 * @return 0 if the timer was started, 1 if it was already running, < 0 on error.
 */
// Subroutine sceKernelStartVTimer - Address 0x0000FDB4
int sceKernelStartVTimer(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (g_threadman.curTimerUid == uid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_START_VTIMER, 1, uid);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (vtimer->active)
        ret = 1;
    else {
        vtimer->active = 1;
        ret = 0;
        threadmanSysClockSet(&vtimer->base, _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base));
        if (vtimer->handler != NULL) {
            threadmanSysClockSet(&timer->schedule, threadmanSysClockGet(&vtimer->schedule) + threadmanSysClockGet(&vtimer->base));
            _sceKernelInsertTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 */
/**
 * Stop a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 *
 * @return 1 if the timer was stopped, 0 if it was not running, < 0 on error.
 */
// Subroutine sceKernelStopVTimer - Address 0x0000FFBC
int sceKernelStopVTimer(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (g_threadman.curTimerUid == uid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_STOP_VTIMER, 1, uid);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    ret = 0;
    if (vtimer->active) {
        vtimer->active = 0;
        ret = 1;
        threadmanSysClockSet(&vtimer->base, _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base));
        if (vtimer->handler != NULL) {
            _sceKernelRemoveTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q040: Separate ifs for the early checks: the kernel-mode handler address check is its own branch, as in
 * sceKernelSetVTimerHandlerWide.
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 */
/**
 * Set the handler of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 * @param time The virtual time at which the handler is called.
 * @param handler The handler (NULL to cancel the current one). Its return value is the delay until its
 * next call, 0 to stop calling it.
 * @param common The argument passed to the handler.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSetVTimerHandler - Address 0x00010194
int sceKernelSetVTimerHandler(SceUID uid, SceKernelSysClock *time, SceKernelVTimerHandler handler, void *common)
{
    s32 oldK1;
    s32 intr;
    u64 schedule;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (g_threadman.curTimerUid == uid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }
    if (!pspK1StaBufOk(time, sizeof(*time)) || !pspK1PtrOk(handler)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (!pspK1IsUserMode() && (s32)handler >= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_VTIMER_HANDLER, 5, uid, time->hi, time->low, handler, common);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL)
        _sceKernelRemoveTimer(timer);

    vtimer->handler = handler;
    if (handler != NULL) {
        vtimer->schedule = *time;
        vtimer->common = common;
        vtimer->isUser = pspK1IsUserMode();
        vtimer->gp = sceKernelGetModuleGPByAddressForKernel((u32)handler);
        if (vtimer->gp == 0)
            vtimer->gp = threadmanGetGp();
        if (vtimer->active) {
            schedule = threadmanSysClockGet(&vtimer->schedule) + threadmanSysClockGet(&vtimer->base);
            threadmanSysClockSet(&timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
        }
    }
    if (vtimer->active)
        _sceKernelUpdateSysTimerCompare();

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 */
/**
 * Set the handler of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 * @param time The virtual time at which the handler is called.
 * @param handler The handler (NULL to cancel the current one).
 * @param common The argument passed to the handler.
 *
 * @return 0 on success, < 0 on error.
 *
 * @note The handler is called like a ::SceKernelVTimerHandler.
 */
// Subroutine sceKernelSetVTimerHandlerWide - Address 0x00010438
int sceKernelSetVTimerHandlerWide(SceUID uid, SceInt64 time, SceKernelVTimerHandlerWide handler, void *common)
{
    s32 oldK1;
    s32 intr;
    u64 schedule;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (g_threadman.curTimerUid == uid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }
    if (!pspK1PtrOk(handler)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (!pspK1IsUserMode() && (s32)handler >= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_VTIMER_HANDLER_WIDE, 4, uid, (u32)time, handler, common);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL)
        _sceKernelRemoveTimer(timer);

    vtimer->handler = (SceKernelVTimerHandler)(void *)handler;
    if (handler != NULL) {
        threadmanSysClockSet(&vtimer->schedule, time);
        vtimer->common = common;
        vtimer->isUser = pspK1IsUserMode();
        vtimer->gp = threadmanGetGp();
        if (vtimer->active) {
            schedule = threadmanSysClockGet(&vtimer->schedule) + threadmanSysClockGet(&vtimer->base);
            threadmanSysClockSet(&timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
        }
    }
    if (vtimer->active)
        _sceKernelUpdateSysTimerCompare();

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 */
/**
 * Cancel the handler of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelVTimerHandler - Address 0x000106B8
int sceKernelCancelVTimerHandler(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (g_threadman.curTimerUid == uid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_VTIMER_HANDLER, 1, uid);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL) {
        _sceKernelRemoveTimer(timer);
        _sceKernelUpdateSysTimerCompare();
    }
    vtimer->handler = NULL;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Retrieve the status of a virtual timer.
 *
 * @param uid The UID of the virtual timer.
 * @param info Pointer to a ::SceKernelVTimerInfo receiving the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferVTimerStatus - Address 0x00010828
int sceKernelReferVTimerStatus(SceUID uid, SceKernelVTimerInfo *info)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceKernelVTimerInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VTIMER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_VTIMER_STATUS, 2, uid, info);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    buf = &g_threadman.statusBuf.vtimerInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->active = vtimer->active;
    if (vtimer->active) {
        buf->base = vtimer->base;
        threadmanSysClockSet(&buf->current, _sceKernelGetSystemTime64() - threadmanSysClockGet(&vtimer->base));
    } else {
        threadmanSysClockSet32(&buf->base, 0);
        buf->current = vtimer->base;
    }
    buf->schedule = vtimer->schedule;
    buf->handler = vtimer->handler;
    buf->common = vtimer->common;
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.vtimerInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * The return address of the user mode alarm and virtual timer handlers (unused: the handlers are
 * called through sceKernelCallUserIntrHandler()).
 *
 * @return 0.
 */
// Subroutine _sceKernelReturnFromTimerHandler - Address 0x00010A60
s32 _sceKernelReturnFromTimerHandler(void)
{
    return 0;
}

/**
 * Convert a number of microseconds to a system clock value.
 *
 * @param usec The number of microseconds.
 *
 * @return The system clock value.
 */
// Subroutine sceKernelUSec2SysClockWide - Address 0x00010A68
SceInt64 sceKernelUSec2SysClockWide(unsigned int usec)
{
    return usec;
}

/**
 * Convert a system clock value to seconds and microseconds.
 *
 * @param clock The system clock value.
 * @param low Pointer receiving the number of seconds (may be NULL).
 * @param high Pointer receiving the number of microseconds (may be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSysClock2USec - Address 0x00010A74
int sceKernelSysClock2USec(SceKernelSysClock *clock, unsigned int *low, unsigned int *high)
{
    s32 oldK1;
    SceKernelSysClock quot;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(clock, sizeof(*clock)) || !pspK1StaBufOk(low, sizeof(*low))
        || !pspK1StaBufOk(high, sizeof(*high))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    if (low != NULL) {
        _sceKernelSysClockDivide(&quot, clock, 1000000, high);
        *low = quot.low;
    } else
        *high = clock->low;

    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q041 (c572844): Checks combined in one condition / merged identical error checks: the original shares one
 *     branch.
 */
/**
 * Convert a system clock value to seconds and microseconds.
 *
 * @param clock The system clock value.
 * @param low Pointer receiving the number of seconds (may be NULL).
 * @param high Pointer receiving the number of microseconds (may be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSysClock2USecWide - Address 0x00010B30
int sceKernelSysClock2USecWide(SceInt64 clock, unsigned int *low, unsigned int *high)
{
    s32 oldK1;
    SceKernelSysClock quot;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(low, sizeof(*low)) || !pspK1StaBufOk(high, sizeof(*high))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    if (low != NULL) {
        threadmanSysClockSet(&quot, clock);
        _sceKernelSysClockDivide(&quot, &quot, 1000000, high);
        *low = quot.low;
    } else
        *high = (u32)clock;

    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q042 (e0d596d): Statement/store order taken from the original's store order.
 */
/**
 * Get the system time.
 *
 * @param time Pointer to a ::SceKernelSysClock receiving the system time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelGetSystemTime - Address 0x00010BE0
int sceKernelGetSystemTime(SceKernelSysClock *time)
{
    s32 oldK1;
    s32 intr;
    u64 now;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(time, sizeof(*time))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    now = _sceKernelGetSystemTime64();
    threadmanSysClockSet(time, now);

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_SYSTEM_TIME, 2, time->hi, time->low);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q069 (57a9a84): K1 shifted and restored around the read, as the original.
 */
/**
 * Get the system time.
 *
 * @return The system time, in microseconds.
 */
// Subroutine sceKernelGetSystemTimeWide - Address 0x00010C98
SceInt64 sceKernelGetSystemTimeWide(void)
{
    s32 oldK1;
    s32 intr;
    u64 now;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    now = _sceKernelGetSystemTime64();

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_SYSTEM_TIME_WIDE, 1, (u32)now);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return now;
}

/**
 * Get the lower 32 bits of the system time.
 *
 * @return The lower 32 bits of the system time, in microseconds.
 */
// Subroutine sceKernelGetSystemTimeLow - Address 0x00010D28
unsigned int sceKernelGetSystemTimeLow(void)
{
    return HW(SYSTIMER_COUNTER);
}

/**
 * Get the system time in usec microseconds.
 *
 * @param clock Pointer to a ::SceKernelSysClock receiving the system time.
 * @param usec The number of microseconds.
 */
// Subroutine sub_00010D34 - Address 0x00010D34
void sub_00010D34(SceKernelSysClock *clock, u32 usec)
{
    u64 time;

    time = _sceKernelGetSystemTime64() + usec;
    threadmanSysClockSet(clock, time);
}

/**
 * Get the system time after a delay.
 *
 * @param clock Pointer to a ::SceKernelSysClock receiving the system time.
 * @param delay The delay.
 */
// Subroutine sub_00010D8C - Address 0x00010D8C
void sub_00010D8C(SceKernelSysClock *clock, SceKernelSysClock *delay)
{
    u64 time;

    time = _sceKernelGetSystemTime64() + threadmanSysClockGet(delay);
    threadmanSysClockSet(clock, time);
}

/**
 * Set an alarm.
 *
 * @param clock The number of microseconds until the alarm expires.
 * @param handler The handler called on expiration. Its return value is the delay until its next
 * call, 0 to delete the alarm.
 * @param common The argument passed to the handler.
 *
 * @return The UID of the new alarm on success, < 0 on error.
 */
// Subroutine sceKernelSetAlarm - Address 0x00010DF4
SceUID sceKernelSetAlarm(SceUInt clock, SceKernelAlarmHandler handler, void *common)
{
    s32 oldK1;
    SceUID uid;

    oldK1 = pspShiftK1();
    if (handler == NULL || !pspK1PtrOk(handler)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    uid = _sceKernelSetAlarm(clock, handler, common);

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_ALARM, 4, clock, handler, common, uid);

    pspSetK1(oldK1);
    return uid;
}

/*
 * PARITY-Q054 (fa733b6): Comparison operand order as the original's branch operands.
 */
/**
 * Set an alarm.
 *
 * @param clock The delay until the alarm expires.
 * @param handler The handler called on expiration. Its return value is the delay until its next
 * call, 0 to delete the alarm.
 * @param common The argument passed to the handler.
 *
 * @return The UID of the new alarm on success, < 0 on error.
 */
// Subroutine sceKernelSetSysClockAlarm - Address 0x00010EBC
SceUID sceKernelSetSysClockAlarm(SceKernelSysClock *clock, SceKernelAlarmHandler handler, void *common)
{
    s32 oldK1;
    SceUID uid;

    oldK1 = pspShiftK1();
    if (handler == NULL || !pspK1StaBufOk(clock, sizeof(*clock)) || !pspK1PtrOk(handler)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    uid = _sceKernelSetAlarm(threadmanSysClockGet(clock), handler, common);

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_SYS_CLOCK_ALARM, 5, clock->hi, clock->low, handler, common, uid);

    pspSetK1(oldK1);
    return uid;
}

/*
 * PARITY-Q043 (71496bf): Struct fields filled in struct order (the original's store order).
 * PARITY-Q044 (71496bf): Register copy cb of the UID control block (the original does not reload the address-taken
 *     uid from the stack).
 */
/*
 * PARITY-Q097: Both out-of-memory exits share one return through a label, the sceKernelCreateUID success branching
 *     forward to `created` (MATCHING FORM, not believed to be the original's literal source). Gives the original's layout: one
 *     shared out-of-line `lui/j/ori` error block, the CreateUID failure as `bnel $v0` to it and the success path
 *     falling through to `j` back into the main path; with separate returns the error block was duplicated and the
 *     return value went through $v1. equiv -> sched, wsim +0.075, 1 up / 0 down (remaining: sched1 order of the
 *     timerType load after the free-list unlink). Same output: the CreateUID test inside a plain else block.
 *     Shared label with the success test as `!= SCE_ERROR_OK` goto (label in either arm or at the end): +0.040, still
 *     equiv (CreateUID success branches back instead of falling through).
 * PARITY-Q127: delay is computed before timer (the two UID_CB_TO_DATA lines swapped): the timerType load of the
 *     data pointers now follows the clock->low load as in the original. wsim +0.011, csim +0.058, still sched,
 *     1 up / 0 down. Rejected: schedule copy right after timer (down), field-wise copy (down), uid computed before the
 *     unlink (down). Residue: the free-list path's timerType load (sched1 places it before the unlink loads).
 * PARITY-Q159: g_threadman.timerType is an s32 cast to SceSysmemUidCB * at each use (threadman_int.h), so its
 *     load conflicts with the s32 numFreeDelays store and stays after it, as in the original (fixes the Q127 residue).
 *     User-approved type-pun exception (19:03 PT 2026-10-27): the s32 alias set is what gives the original load order here.
 */
/**
 * Start a delay timer.
 *
 * @param clock The system time at which the timer expires.
 * @param handler The handler called on expiration (in interrupt context).
 * @param common The argument passed to the handler.
 *
 * @return The UID of the delay timer on success, < 0 on error (SCE_ERROR_KERNEL_WAIT_TIMEOUT if clock is
 * already over).
 */
// Subroutine sub_00010FAC - Address 0x00010FAC
SceUID sub_00010FAC(SceKernelSysClock *clock, SceThreadmanTimerHandler handler, void *common)
{
    SceSysmemUidCB *uid;
    SceSysmemUidCB *cb;
    SceThreadmanListEntry *entry;
    SceThreadmanTimer *timer;
    SceThreadmanDelay *delay;

    if ((s64)(threadmanSysClockGet(clock) - _sceKernelGetSystemTime64()) < 0)
        return SCE_ERROR_KERNEL_WAIT_TIMEOUT;

    if (g_threadman.numFreeDelays > 0) {
        entry = g_threadman.freeDelays.next;
        g_threadman.numFreeDelays--;
        threadmanListRemove(entry);
        uid = THREADMAN_UID_DATA_TO_CB(entry, ((SceSysmemUidCB *)g_threadman.timerType));
    } else if (sceKernelCreateUID(g_threadman.delayType, "SceThreadmanDelay", 0, &uid) == SCE_ERROR_OK)
        goto created;
    else
        goto nomem;
created:
    cb = uid;
    if (cb == NULL) {
nomem:
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }

    delay = UID_CB_TO_DATA(cb, g_threadman.delayType, SceThreadmanDelay);
    timer = UID_CB_TO_DATA(cb, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    timer->schedule = *clock;
    delay->handler = handler;
    delay->common = common;
    _sceKernelInsertTimer(timer);
    _sceKernelUpdateSysTimerCompare();
    return cb->uid;
}

/*
 * PARITY-Q073: The system time read into a local before the schedule is loaded (the original calls
 * _sceKernelGetSystemTime64 first).
 */
/**
 * Cancel a delay timer.
 *
 * @param alarmId The UID of the delay timer.
 * @param remaining Pointer receiving the remaining time in microseconds (may be NULL).
 *
 * @return 0 on success, -1 on error.
 */
// Subroutine sub_00011104 - Address 0x00011104
s32 sub_00011104(SceUID alarmId, SceUInt *remaining)
{
    s64 diff;
    u64 now;
    SceKernelSysClock clock;
    SceSysmemUidCB *uid;
    SceThreadmanTimer *timer;

    if (sceKernelGetUIDcontrolBlockWithType(alarmId, g_threadman.delayType, &uid) != SCE_ERROR_OK)
        return -1;

    timer = UID_CB_TO_DATA(uid, ((SceSysmemUidCB *)g_threadman.timerType), SceThreadmanTimer);
    if (remaining != NULL) {
        now = _sceKernelGetSystemTime64();
        diff = threadmanSysClockGet(&timer->schedule) - now;
        if (diff < 0)
            diff = 0;
        threadmanSysClockSet(&clock, diff);
        sceKernelSysClock2USec(&clock, NULL, (unsigned int *)remaining);
    }

    _sceKernelRemoveTimer(timer);
    _sceKernelFreeDelay(uid);
    _sceKernelUpdateSysTimerCompare();
    return 0;
}

/**
 * Create the timer UID types.
 *
 * @return 0.
 */
// Subroutine sub_0001121C - Address 0x0001121C
s32 _sceKernelTimerInit(void)
{
    sceKernelCreateUIDtype("Timer", sizeof(SceThreadmanTimer), g_TimerFuncs, NULL, (SceSysmemUidCB **)&g_threadman.timerType);
    sceKernelCreateUIDtypeInherit("Timer", "Delay", 0x30, g_DelayFuncs, NULL, &g_threadman.delayType);
    sceKernelCreateUIDtypeInherit("Timer", "Alarm", 0x30, g_AlarmFuncs, NULL, &g_threadman.alarmType);
    sceKernelCreateUIDtypeInherit("Timer", "VTimer", 0x30, g_VTimerFuncs, NULL, &g_threadman.vtimerType);
    return 0;
}
