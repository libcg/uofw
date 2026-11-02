/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/timer.c
 *
 * Timers: the system timer, the delay timers, the alarms, the virtual timers and the system time
 * (original address range 0x0000E5EC - 0x00011A24).
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

/** The counter of the system timer (the lower 32 bits of the system time). */
#define SYSTIMER_COUNTER    (0xBC600000)
/** The compare register of the system timer (an interrupt is raised when the counter reaches it). */
#define SYSTIMER_COMPARE    (0xBC600004)
#define SYSTIMER_UNK8       (0xBC600008)
#define SYSTIMER_UNKC       (0xBC60000C)
#define SYSTIMER_UNK10      (0xBC600010)

/** The period of the system timer overflow timer, in microseconds. */
#define SYSTIMER_OVF_PERIOD (0xF0000000)

/** The syscall / suspend / resume handler slot of the system timer. */
#define SYSTIMER_HANDLER_ID (0x18)

void sub_000112E0(void);
static s32 _sceKernelSysTimerIntr(s32 unk, SceThreadman *threadman);
static void _sceKernelInsertTimer(SceThreadmanTimer *timer);
static void _sceKernelUpdateSysTimerCompare(void);
u64 _sceKernelGetSystemTime64(void);
static void _sceKernelFreeDelay(SceSysmemUidCB *uid);
static s32 _sceKernelSysTimerSuspend(s32 unk, void *param);
static s32 _sceKernelSysTimerResume(s32 unk, void *param);
static void _sceKernelSysClockDivide(SceKernelSysClock *quot, SceKernelSysClock *clock, u32 div, unsigned int *rem);
static SceUInt _sceKernelCallAlarmHandler(SceSysmemUidCB *uid, s32 intr);
static SceUInt _sceKernelCallVTimerHandler(SceSysmemUidCB *uid, s32 intr);
static SceUID _sceKernelSetAlarm(u64 clock, SceKernelAlarmHandler handler, void *common);
static s32 _sceKernelDelayInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelDelayDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelAlarmInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelAlarmDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelVTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelVTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

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

// 0x00019F00
/** The counter of the system timer, saved on suspend and restored on resume. */
u32 g_sysTimerSavedCounter;

/** Remove the active timer timer from the timer list (keeping the interrupt handler's iterator valid). */
static inline void _sceKernelRemoveTimer(SceThreadmanTimer *timer)
{
    if (g_threadman.nextTimer == &timer->link)
        g_threadman.nextTimer = timer->link.next;
    threadmanListRemove(&timer->link);
    timer->link.next = &timer->link;
    timer->link.prev = &timer->link;
}

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

    g_threadman.unk2036 = 1;
    g_threadman.unk2032 = 1;
    sceKernelUSec2SysClock(100, &clock);
    g_threadman.getSystemTime = _sceKernelGetSystemTime64;
    g_threadman.minCompareDelay = clock.low * 2;
    g_threadman.getSystemTimeLow = sceKernelGetSystemTimeLow;
    g_threadman.minTimerDelay = clock.low;
    g_threadman.sysTimerIntr = (g_threadman.cop0State22 != 0) ? 4 : 0x13;

    HW(SYSTIMER_UNKC) = 1;
    HW(SYSTIMER_UNK8) = 0x30;
    HW(SYSTIMER_UNK10) = 0;
    pspSync();

    sceKernelRegisterIntrHandler(g_threadman.sysTimerIntr, 1, _sceKernelSysTimerIntr, &g_threadman, NULL);
    sceKernelSetIntrLevel(g_threadman.sysTimerIntr, 2);

    g_threadman.freeDelays.prev = &g_threadman.freeDelays;
    g_threadman.timers.prev = &g_threadman.timers;
    g_threadman.freeDelays.next = &g_threadman.freeDelays;
    g_threadman.timers.next = &g_threadman.timers;
    g_threadman.numFreeDelays = 0;
    g_threadman.nextTimer = NULL;

    intr = sceKernelCpuSuspendIntr();

    /* The overflow timer, which keeps the upper 32 bits of the system time up to date. */
    sceKernelCreateUID(g_threadman.timerType, "SceThreadmanSysTimerOVF", 0, &uid);
    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
    timer->schedule.hi = 0;
    timer->schedule.low = SYSTIMER_OVF_PERIOD;
    g_threadman.sysTimeBase.hi = 0;
    g_threadman.sysTimeBase.low = 0;
    g_threadman.nextCompare = timer->schedule;
    threadmanListAddTail(&g_threadman.timers, &timer->link);

    for (i = SCE_THREADMAN_MAX_FREE_DELAYS - 1; i >= 0; i--) {
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

/**
 * The interrupt handler of the system timer: run the expired timers.
 *
 * @param unk Unknown.
 * @param threadman The global state of the thread manager.
 *
 * @return -1.
 */
// Subroutine sub_0000E80C - Address 0x0000E80C
static s32 _sceKernelSysTimerIntr(s32 unk, SceThreadman *threadman)
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

    (void)unk;

    intr = sceKernelCpuSuspendIntr();

    threadman->sysTimeBase = threadman->nextCompare;
    now = _sceKernelGetSystemTime64();

    for (entry = threadman->timers.next; entry != &threadman->timers; entry = threadman->nextTimer) {
        timer = (SceThreadmanTimer *)entry;
        schedule = THREADMAN_SYSCLOCK_GET(timer->schedule);
        threadman->nextTimer = entry->next;
        if ((s64)(schedule - now) > 0)
            break;

        threadmanListRemove(&timer->link);
        timer->link.next = &timer->link;
        timer->link.prev = &timer->link;

        uid = THREADMAN_UID_DATA_TO_CB(timer, g_threadman.timerType);
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
                if (delay < threadman->minTimerDelay)
                    delay = threadman->minTimerDelay;
                schedule += delay;
                THREADMAN_SYSCLOCK_SET(timer->schedule, schedule);
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
                THREADMAN_SYSCLOCK_SET(timer->schedule, schedule);
                schedule = THREADMAN_SYSCLOCK_GET(vtimer->schedule) + delay;
                THREADMAN_SYSCLOCK_SET(vtimer->schedule, schedule);
                _sceKernelInsertTimer(timer);
            }
        } else {
            /* The overflow timer. */
            schedule += SYSTIMER_OVF_PERIOD;
            THREADMAN_SYSCLOCK_SET(timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
        }
    }

    _sceKernelUpdateSysTimerCompare();
    threadman->nextTimer = NULL;

    sceKernelCpuResumeIntr(intr);
    return -1;
}

/**
 * Insert a timer in the timer list, after the timers expiring before or at the same time.
 *
 * @param timer The timer to insert.
 */
// Subroutine sub_0000EAE0 - Address 0x0000EAE0
static void _sceKernelInsertTimer(SceThreadmanTimer *timer)
{
    SceThreadmanListEntry *entry;

    for (entry = g_threadman.timers.next; entry != &g_threadman.timers; entry = entry->next) {
        if (THREADMAN_SYSCLOCK_GET(timer->schedule) < THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule))
            break;
    }
    timer->link.next = entry;
    timer->link.prev = entry->prev;
    entry->prev = &timer->link;
    timer->link.prev->next = &timer->link;
}

/**
 * Program the compare register of the system timer for the next timer to expire.
 *
 * The timers expiring shortly after the first one are grouped (the compare is set to the last of
 * them), and the compare is never set too close to the current time.
 */
// Subroutine sub_0000EB7C - Address 0x0000EB7C
static void _sceKernelUpdateSysTimerCompare(void)
{
    u64 schedule;
    u64 limit;
    u64 now;
    SceThreadmanListEntry *entry;

    entry = g_threadman.timers.next;
    schedule = THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule);
    limit = schedule + g_threadman.minCompareDelay;
    for (; entry != &g_threadman.timers; entry = entry->next) {
        if ((s64)(limit - THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule)) <= 0)
            break;
        schedule = THREADMAN_SYSCLOCK_GET(((SceThreadmanTimer *)entry)->schedule);
    }

    now = _sceKernelGetSystemTime64();
    if ((s64)(schedule - now) < (s64)g_threadman.minCompareDelay)
        schedule = now + g_threadman.minCompareDelay;

    THREADMAN_SYSCLOCK_SET(g_threadman.nextCompare, schedule);
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
    if (!pspK1StaBufOk(clock, sizeof(*clock)))
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
    if (alarmid == g_threadman.curTimerUid) {
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

    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
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

    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
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
    u32 *ptr;
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
    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
    buf = &g_threadman.statusBuf.alarmInfo;
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    buf->schedule = timer->schedule;
    buf->handler = alarm->handler;
    buf->common = alarm->common;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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
    vtimer->handler = NULL;
    vtimer->active = 0;

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
    else {
        base->low = 0;
        base->hi = 0;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_BASE, 3, uid, base->low, base->hi);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK
        || !THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return -1;
    }

    base = 0;
    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    if (vtimer->active)
        base = THREADMAN_SYSCLOCK_GET(vtimer->base);

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_BASE_WIDE, 2, uid, (u32)base);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return base;
}

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
        now = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
        THREADMAN_SYSCLOCK_SET(*time, now);
    } else
        *time = vtimer->base;

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_TIME, 3, uid, time->low, time->hi);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK
        || !THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return -1;
    }

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    if (vtimer->active)
        time = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
    else
        time = THREADMAN_SYSCLOCK_GET(vtimer->base);

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_VTIMER_TIME_WIDE, 2, uid, (u32)time);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return time;
}

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
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
    if (!vtimer->active) {
        old = THREADMAN_SYSCLOCK_GET(vtimer->base);
        vtimer->base = *time;
    } else {
        old = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
        base = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(*time);
        THREADMAN_SYSCLOCK_SET(vtimer->base, base);
        if (vtimer->handler != NULL) {
            _sceKernelRemoveTimer(timer);
            base = THREADMAN_SYSCLOCK_GET(vtimer->schedule) + THREADMAN_SYSCLOCK_GET(vtimer->base);
            THREADMAN_SYSCLOCK_SET(timer->schedule, base);
            _sceKernelInsertTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    }
    THREADMAN_SYSCLOCK_SET(*time, old);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vtimerType, &cb) != SCE_ERROR_OK
        || !THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return -1;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_VTIMER_TIME_WIDE, 2, uid, (u32)time);

    vtimer = UID_CB_TO_DATA(cb, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
    if (!vtimer->active) {
        old = THREADMAN_SYSCLOCK_GET(vtimer->base);
        THREADMAN_SYSCLOCK_SET(vtimer->base, time);
    } else {
        old = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
        base = _sceKernelGetSystemTime64() - time;
        THREADMAN_SYSCLOCK_SET(vtimer->base, base);
        if (vtimer->handler != NULL) {
            _sceKernelRemoveTimer(timer);
            base = THREADMAN_SYSCLOCK_GET(vtimer->schedule) + THREADMAN_SYSCLOCK_GET(vtimer->base);
            THREADMAN_SYSCLOCK_SET(timer->schedule, base);
            _sceKernelInsertTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return old;
}

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
    u64 time;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (uid == g_threadman.curTimerUid) {
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
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
    ret = 1;
    if (!vtimer->active) {
        vtimer->active = 1;
        ret = 0;
        time = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
        THREADMAN_SYSCLOCK_SET(vtimer->base, time);
        if (vtimer->handler != NULL) {
            time = THREADMAN_SYSCLOCK_GET(vtimer->schedule) + THREADMAN_SYSCLOCK_GET(vtimer->base);
            THREADMAN_SYSCLOCK_SET(timer->schedule, time);
            _sceKernelInsertTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

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
    u64 time;
    SceSysmemUidCB *cb;
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    oldK1 = pspShiftK1();
    if (uid == g_threadman.curTimerUid) {
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
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
    ret = 0;
    if (vtimer->active) {
        vtimer->active = 0;
        ret = 1;
        time = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
        THREADMAN_SYSCLOCK_SET(vtimer->base, time);
        if (vtimer->handler != NULL) {
            _sceKernelRemoveTimer(timer);
            _sceKernelUpdateSysTimerCompare();
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

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
    if (uid == g_threadman.curTimerUid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }
    if (!pspK1StaBufOk(time, sizeof(*time)) || !pspK1PtrOk(handler)
        || (!pspK1IsUserMode() && (s32)handler >= 0)) {
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
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL)
        _sceKernelRemoveTimer(timer);

    vtimer->handler = handler;
    if (handler != NULL) {
        vtimer->schedule = *time;
        vtimer->common = common;
        vtimer->isUser = pspK1IsUserMode();
        vtimer->gp = sceKernelGetModuleGPByAddressForKernel((u32)handler);
        if (vtimer->gp == 0)
            vtimer->gp = pspGetGp();
        if (vtimer->active) {
            schedule = THREADMAN_SYSCLOCK_GET(vtimer->schedule) + THREADMAN_SYSCLOCK_GET(vtimer->base);
            THREADMAN_SYSCLOCK_SET(timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
        }
    }
    if (vtimer->active)
        _sceKernelUpdateSysTimerCompare();

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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
    if (uid == g_threadman.curTimerUid) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_VTIMER;
    }
    if (!pspK1PtrOk(handler) || (!pspK1IsUserMode() && (s32)handler >= 0)) {
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
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL)
        _sceKernelRemoveTimer(timer);

    vtimer->handler = (SceKernelVTimerHandler)(void *)handler;
    if (handler != NULL) {
        THREADMAN_SYSCLOCK_SET(vtimer->schedule, time);
        vtimer->common = common;
        vtimer->isUser = pspK1IsUserMode();
        vtimer->gp = pspGetGp();
        if (vtimer->active) {
            schedule = THREADMAN_SYSCLOCK_GET(vtimer->schedule) + THREADMAN_SYSCLOCK_GET(vtimer->base);
            THREADMAN_SYSCLOCK_SET(timer->schedule, schedule);
            _sceKernelInsertTimer(timer);
        }
    }
    if (vtimer->active)
        _sceKernelUpdateSysTimerCompare();

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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
    if (uid == g_threadman.curTimerUid) {
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
    timer = UID_CB_TO_DATA(cb, g_threadman.timerType, SceThreadmanTimer);
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
    u64 time;
    u32 *ptr;
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
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->active = vtimer->active;
    if (vtimer->active) {
        buf->base = vtimer->base;
        time = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
        THREADMAN_SYSCLOCK_SET(buf->current, time);
    } else {
        buf->base.low = 0;
        buf->base.hi = 0;
        buf->current = vtimer->base;
    }
    buf->schedule = vtimer->schedule;
    buf->handler = vtimer->handler;
    buf->common = vtimer->common;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

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
        THREADMAN_SYSCLOCK_SET(quot, clock);
        _sceKernelSysClockDivide(&quot, &quot, 1000000, high);
        *low = quot.low;
    } else
        *high = (u32)clock;

    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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
    THREADMAN_SYSCLOCK_SET(*time, now);

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_SYSTEM_TIME, 2, time->low, time->hi);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the system time.
 *
 * @return The system time, in microseconds.
 */
// Subroutine sceKernelGetSystemTimeWide - Address 0x00010C98
SceInt64 sceKernelGetSystemTimeWide(void)
{
    s32 intr;
    u64 now;

    intr = sceKernelCpuSuspendIntr();

    now = _sceKernelGetSystemTime64();

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_SYSTEM_TIME_WIDE, 1, (u32)now);

    sceKernelCpuResumeIntr(intr);
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
    THREADMAN_SYSCLOCK_SET(*clock, time);
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

    time = _sceKernelGetSystemTime64() + THREADMAN_SYSCLOCK_GET(*delay);
    THREADMAN_SYSCLOCK_SET(*clock, time);
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
    if (!pspK1StaBufOk(clock, sizeof(*clock)) || handler == NULL || !pspK1PtrOk(handler)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    uid = _sceKernelSetAlarm(THREADMAN_SYSCLOCK_GET(*clock), handler, common);

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_SYS_CLOCK_ALARM, 5, clock->hi, clock->low, handler, common, uid);

    pspSetK1(oldK1);
    return uid;
}

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
    SceThreadmanListEntry *entry;
    SceThreadmanTimer *timer;
    SceThreadmanDelay *delay;

    if ((s64)(THREADMAN_SYSCLOCK_GET(*clock) - _sceKernelGetSystemTime64()) < 0)
        return SCE_ERROR_KERNEL_WAIT_TIMEOUT;

    if (g_threadman.numFreeDelays > 0) {
        entry = g_threadman.freeDelays.next;
        g_threadman.numFreeDelays--;
        threadmanListRemove(entry);
        uid = THREADMAN_UID_DATA_TO_CB(entry, g_threadman.timerType);
    } else if (sceKernelCreateUID(g_threadman.delayType, "SceThreadmanDelay", 0, &uid) != SCE_ERROR_OK)
        return SCE_ERROR_KERNEL_NO_MEMORY;
    if (uid == NULL)
        return SCE_ERROR_KERNEL_NO_MEMORY;

    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
    delay = UID_CB_TO_DATA(uid, g_threadman.delayType, SceThreadmanDelay);
    timer->schedule = *clock;
    delay->common = common;
    delay->handler = handler;
    _sceKernelInsertTimer(timer);
    _sceKernelUpdateSysTimerCompare();
    return uid->uid;
}

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
    SceKernelSysClock clock;
    SceSysmemUidCB *uid;
    SceThreadmanTimer *timer;

    if (sceKernelGetUIDcontrolBlockWithType(alarmId, g_threadman.delayType, &uid) != SCE_ERROR_OK)
        return -1;

    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
    if (remaining != NULL) {
        diff = THREADMAN_SYSCLOCK_GET(timer->schedule) - _sceKernelGetSystemTime64();
        if (diff < 0)
            diff = 0;
        THREADMAN_SYSCLOCK_SET(clock, diff);
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
    sceKernelCreateUIDtype("Timer", sizeof(SceThreadmanTimer), g_TimerFuncs, NULL, &g_threadman.timerType);
    sceKernelCreateUIDtypeInherit("Timer", "Delay", 0x30, g_DelayFuncs, NULL, &g_threadman.delayType);
    sceKernelCreateUIDtypeInherit("Timer", "Alarm", 0x30, g_AlarmFuncs, NULL, &g_threadman.alarmType);
    sceKernelCreateUIDtypeInherit("Timer", "VTimer", 0x30, g_VTimerFuncs, NULL, &g_threadman.vtimerType);
    return 0;
}

/* sub_000112E0 (the syscall handler reading the system time) is implemented in timer_syscall.S. */

/**
 * The suspend handler of the system timer: save its counter.
 *
 * @param unk Unknown.
 * @param param Unused.
 *
 * @return 0.
 */
// Subroutine sub_000112F8 - Address 0x000112F8
static s32 _sceKernelSysTimerSuspend(s32 unk, void *param)
{
    (void)unk;
    (void)param;

    g_sysTimerSavedCounter = HW(SYSTIMER_COUNTER);
    sceKernelDisableIntr(g_threadman.sysTimerIntr);

    /* Make sure the counter is restored before the compare value so that the interrupt is not missed. */
    if (g_threadman.nextCompare.low - g_sysTimerSavedCounter == 0
        || g_threadman.nextCompare.low - g_sysTimerSavedCounter > SYSTIMER_OVF_PERIOD)
        g_sysTimerSavedCounter = g_threadman.nextCompare.low - 1;
    return 0;
}

/**
 * The resume handler of the system timer: restore its state.
 *
 * @param unk Unknown.
 * @param param Unused.
 *
 * @return 0.
 */
// Subroutine sub_0001136C - Address 0x0001136C
static s32 _sceKernelSysTimerResume(s32 unk, void *param)
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

/**
 * Divide a system clock value by a 32-bit value.
 *
 * @param quot Pointer receiving the quotient (may be NULL, may be clock).
 * @param clock The dividend.
 * @param div The divisor.
 * @param rem Pointer receiving the remainder (may be NULL).
 */
// Subroutine sub_000113F0 - Address 0x000113F0
static void _sceKernelSysClockDivide(SceKernelSysClock *quot, SceKernelSysClock *clock, u32 div, unsigned int *rem)
{
    s32 i;
    u32 low;
    u32 cur;
    u32 r;
    u32 quotLow;
    u32 quotHi;

    low = clock->low;
    quotHi = 0;
    quotLow = clock->hi / div;
    r = clock->hi % div;
    for (i = 3; i >= 0; i--) {
        cur = (r << 8) | (low >> 24);
        low <<= 8;
        quotHi = (quotHi << 8) | (quotLow >> 24);
        quotLow = (quotLow << 8) + cur / div;
        r = cur % div;
    }
    if (quot != NULL) {
        quot->low = quotLow;
        quot->hi = quotHi;
    }
    if (rem != NULL)
        *rem = r;
}

/**
 * Call the handler of an expired alarm.
 *
 * @param uid The control block of the alarm.
 * @param intr The interrupt state to restore while calling a kernel mode handler.
 *
 * @return The return value of the handler: the delay until its next call, 0 to delete the alarm.
 */
// Subroutine sub_0001147C - Address 0x0001147C
static SceUInt _sceKernelCallAlarmHandler(SceSysmemUidCB *uid, s32 intr)
{
    u32 oldGp;
    SceUInt ret;
    SceThreadmanAlarm *alarm;

    alarm = UID_CB_TO_DATA(uid, g_threadman.alarmType, SceThreadmanAlarm);
    g_threadman.curTimerUid = uid->uid;

    THREADMAN_LOG(SCE_THREADMAN_LOG_CALL_ALARM_HANDLER, 3, uid->uid, alarm->common, alarm->handler);

    oldGp = pspGetGp();
    pspSetGp(alarm->gp);
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

/**
 * Call the handler of an expired virtual timer.
 *
 * @param uid The control block of the virtual timer.
 * @param intr The interrupt state to restore while calling a kernel mode handler.
 *
 * @return The return value of the handler: the delay until its next call, 0 to stop calling it.
 */
// Subroutine sub_00011574 - Address 0x00011574
static SceUInt _sceKernelCallVTimerHandler(SceSysmemUidCB *uid, s32 intr)
{
    u32 oldGp;
    u64 time;
    SceUInt ret;
    SceKernelSysClock actual;
    SceKernelSysClock *frame;
    SceThreadmanVTimer *vtimer;

    vtimer = UID_CB_TO_DATA(uid, g_threadman.vtimerType, SceThreadmanVTimer);
    g_threadman.curTimerUid = uid->uid;

    time = _sceKernelGetSystemTime64() - THREADMAN_SYSCLOCK_GET(vtimer->base);
    THREADMAN_SYSCLOCK_SET(actual, time);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CALL_VTIMER_HANDLER, 5, uid->uid, vtimer->schedule.low, actual.low,
        vtimer->common, vtimer->handler);

    oldGp = pspGetGp();
    pspSetGp(vtimer->gp);
    if (!vtimer->isUser) {
        sceKernelCpuResumeIntr(intr);
        ret = vtimer->handler(uid->uid, &vtimer->schedule, &actual, vtimer->common);
        sceKernelCpuSuspendIntr();
    } else {
        /* Pass the schedule and the actual time on the user interrupt stack. */
        frame = (SceKernelSysClock *)(sceKernelGetUserIntrStack() - 2 * sizeof(SceKernelSysClock));
        frame[0] = vtimer->schedule;
        frame[1].low = actual.low;
        /* Original bug: the upper 32 bits of the actual time are not copied (a pointer is stored instead). */
        frame[1].hi = (u32)vtimer;
        ret = sceKernelCallUserIntrHandler(uid->uid, (s32)&frame[0], (s32)&frame[1], (s32)vtimer->common,
            (s32)vtimer->handler, (s32)frame);
    }
    pspSetGp(oldGp);

    g_threadman.curTimerUid = 0;
    return ret;
}

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
static SceUID _sceKernelSetAlarm(u64 clock, SceKernelAlarmHandler handler, void *common)
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
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        return status;
    }

    alarm = UID_CB_TO_DATA(uid, g_threadman.alarmType, SceThreadmanAlarm);
    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
    schedule = _sceKernelGetSystemTime64() + clock;
    THREADMAN_SYSCLOCK_SET(timer->schedule, schedule);
    alarm->common = common;
    alarm->isUser = pspK1IsUserMode();
    alarm->handler = handler;
    alarm->gp = sceKernelGetModuleGPByAddressForKernel((u32)handler);
    if (alarm->gp == 0)
        alarm->gp = pspGetGp();
    _sceKernelInsertTimer(timer);
    _sceKernelUpdateSysTimerCompare();

    sceKernelCpuResumeIntr(intr);
    return uid->uid;
}

// Subroutine sub_00011834 - Address 0x00011834
static s32 _sceKernelDelayInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_0001185C - Address 0x0001185C
static s32 _sceKernelDelayDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

// Subroutine sub_0001187C - Address 0x0001187C
static s32 _sceKernelAlarmInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_000118A4 - Address 0x000118A4
static s32 _sceKernelAlarmDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

// Subroutine sub_000118C4 - Address 0x000118C4
static s32 _sceKernelVTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_000118EC - Address 0x000118EC
static s32 _sceKernelVTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanVTimer *vtimer;
    SceThreadmanTimer *timer;

    vtimer = UID_CB_TO_DATA(uid, g_threadman.vtimerType, SceThreadmanVTimer);
    timer = UID_CB_TO_DATA(uid, g_threadman.timerType, SceThreadmanTimer);
    if (vtimer->active && vtimer->handler != NULL) {
        _sceKernelRemoveTimer(timer);
        _sceKernelUpdateSysTimerCompare();
    }
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

// Subroutine sub_000119B8 - Address 0x000119B8
static s32 _sceKernelTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanTimer *timer;

    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    timer = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanTimer);
    timer->deletePending = 0;
    timer->link.next = &timer->link;
    timer->link.prev = &timer->link;
    return uid->uid;
}

// Subroutine sub_00011A04 - Address 0x00011A04
static s32 _sceKernelTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
