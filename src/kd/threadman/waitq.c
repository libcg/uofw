/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/waitq.c
 *
 * Wait queues and thread dispatching (original address range 0x00000680 - 0x00001140).
 *
 * All the synchronization objects (semaphores, event flags, ...) inherit the "WaitQ" UID type, whose data
 * (SceThreadmanWaitQ) holds the list of threads waiting on the object. This file implements putting the
 * current thread to wait on such an object, ending waits (wakeup, timeout, cancel, delete) and switching
 * to the next thread.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

static void sub_00000C8C(void *common);
static s32 _sceKernelWaitQEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelWaitQWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap);
static s32 _sceKernelWaitQCancel(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelWaitQInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelWaitQDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019ACC
SceSysmemUidLookupFunc g_WaitQFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_END_WAIT, _sceKernelWaitQEndWait },
    { SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS, _sceKernelWaitQWakeupWaiters },
    { SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL, _sceKernelWaitQCancel },
    { 0xD310D2D9, _sceKernelWaitQInitialize },
    { 0x87089863, _sceKernelWaitQDelete },
    { 0, NULL }
};

/* Insert thread in the wait list of waitQ, behind the waiting threads with the same or a higher priority. */
static inline void _sceKernelWaitQInsertByPriority(SceThreadmanWaitQ *waitQ, SceThreadmanThread *thread)
{
    SceThreadmanListEntry *pos;

    for (pos = waitQ->waitList.next; pos != &waitQ->waitList; pos = pos->next) {
        if (thread->currentPriority < ((SceThreadmanThread *)pos)->currentPriority)
            break;
    }
    /* The first waiting thread is being served: never insert in front of it. */
    if (waitQ->unk4 != 0 && pos->prev == &waitQ->waitList)
        pos = pos->next;
    threadmanListAddTail(pos, &thread->link);
}

/* Stop the timeout of the wait of thread and notify its wait object that the wait ends. */
static inline void _sceKernelEndWait(SceThreadmanThread *thread)
{
    thread->waitWithCallbacks = 0;
    if (thread->timeoutAlarmId != 0) {
        sub_00011104(thread->timeoutAlarmId, thread->timeout);
        thread->timeoutAlarmId = 0;
        thread->timeout = NULL;
    }
    if (thread->waitObj != NULL)
        sceKernelCallUIDObjFunction(thread->waitObj, SCE_THREADMAN_UID_FUNC_WAITQ_END_WAIT, thread);
}

/* Move a thread whose wait ended while it was suspended to the suspended threads. */
static inline void _sceKernelSuspendWaitEnded(SceThreadmanThread *thread)
{
    threadmanListAddTail(&g_threadman.suspendedThreads, &thread->link);
    thread->status = SCE_THREADMAN_STATUS_SUSPEND;
}

/* End the wait of thread. Return 1 when it has been made ready, 0 otherwise. */
static inline s32 _sceKernelReleaseWait(SceThreadmanThread *thread)
{
    if (thread->status == SCE_THREADMAN_STATUS_DEAD)
        return 0;

    threadmanListRemove(&thread->link);
    _sceKernelEndWait(thread);
    if (thread->status == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
        _sceKernelSuspendWaitEnded(thread);
        return 0;
    }
    thread->status = SCE_THREADMAN_STATUS_READY;
    sub_000127D0(thread);
    return 1;
}

/* End the waits of all the threads waiting on uid with the wait status waitStatus. Return their number. */
static inline s32 _sceKernelReleaseAllWaits(SceSysmemUidCB *uid, s32 waitStatus)
{
    s32 numReleased;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanListEntry *entry;
    SceThreadmanThread *thread;

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    numReleased = 0;
    entry = waitQ->waitList.next;
    while (entry != &waitQ->waitList) {
        thread = (SceThreadmanThread *)entry;
        thread->waitStatus = waitStatus;
        numReleased++;
        entry = entry->next;
        sub_00000924(thread);
    }
    return numReleased;
}

// Subroutine sub_00000680 - Address 0x00000680
/*
 * Make the current thread wait on uid (the wait type specific data must already be set) and switch to
 * the next thread. Return the wait status once the wait has ended.
 */
s32 sub_00000680(SceSysmemUidCB *uid, s32 waitType, u32 waitAttr, SceKernelSysClock *timeoutClock,
    SceUInt *timeout, s32 *waitCount)
{
    SceUID alarmId;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    thread->timeoutAlarmId = 0;
    alarmId = 0;
    if (timeout != NULL) {
        alarmId = sub_00010FAC(timeoutClock, sub_00000C8C, thread);
        if (alarmId < 0)
            return alarmId;
        thread->timeout = timeout;
        thread->timeoutAlarmId = alarmId;
    }

    if (thread->waitWithCallbacks && thread->callbackNotified) {
        /* A callback has been notified: do not wait but process it. */
        thread->waitWithCallbacks = 0;
        if (alarmId != 0) {
            sub_00011104(alarmId, NULL);
            thread->timeout = NULL;
            thread->timeoutAlarmId = 0;
        }
        return SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK;
    }

    thread->status = SCE_THREADMAN_STATUS_WAIT;
    thread->waitType = waitType;
    thread->waitStatus = SCE_ERROR_OK;
    thread->waitObj = uid;
    waitQ->numWaitThreads++;
    if (waitAttr & SCE_THREADMAN_WAITQ_ATTR_PRIORITY)
        _sceKernelWaitQInsertByPriority(UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ), thread);
    else
        threadmanListAddTail(&waitQ->waitList, &thread->link);

    if (waitCount != NULL)
        (*waitCount)++;

    g_threadman.nextThread = NULL;
    sub_00000FE4(1, 1);
    return thread->waitStatus;
}

// Subroutine sub_00000858 - Address 0x00000858
/* Make the current thread wait in the list waitList, without wait object. */
void sub_00000858(SceThreadmanListEntry *waitList, s32 waitType)
{
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    threadmanListAddTail(waitList, &thread->link);
    thread->status = SCE_THREADMAN_STATUS_WAIT;
    thread->waitType = waitType;
    thread->waitObj = NULL;
    thread->waitStatus = SCE_ERROR_OK;
}

// Subroutine sub_00000890 - Address 0x00000890
/* Insert thread in the wait list of uid in priority order (used when its priority changes). */
void sub_00000890(SceThreadmanThread *thread, SceSysmemUidCB *uid)
{
    _sceKernelWaitQInsertByPriority(UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ), thread);
}

// Subroutine sub_00000908 - Address 0x00000908
/* Make the current thread wait on uid. See sub_00000680(). */
s32 sub_00000908(SceSysmemUidCB *uid, s32 waitType, u32 waitAttr, SceKernelSysClock *timeoutClock,
    SceUInt *timeout)
{
    return sub_00000680(uid, waitType, waitAttr, timeoutClock, timeout, NULL);
}

// Subroutine sub_00000924 - Address 0x00000924
/* End the wait of thread. Return 1 when it has been made ready (a reschedule is needed), 0 otherwise. */
s32 sub_00000924(SceThreadmanThread *thread)
{
    return _sceKernelReleaseWait(thread);
}

// Subroutine sub_00000A08 - Address 0x00000A08
/*
 * End the wait of thread. If dispatch is set, the thread is made ready and a reschedule is requested,
 * otherwise the thread may run immediately (intr is the interrupt state to restore then).
 */
void sub_00000A08(SceThreadmanThread *thread, s32 dispatch, s32 intr)
{
    if (thread->status == SCE_THREADMAN_STATUS_DEAD)
        return;

    threadmanListRemove(&thread->link);
    _sceKernelEndWait(thread);
    if (thread->status == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
        _sceKernelSuspendWaitEnded(thread);
        return;
    }
    thread->status = SCE_THREADMAN_STATUS_READY;
    if (dispatch) {
        sub_000127D0(thread);
        g_threadman.nextThread = NULL;
    } else
        sub_00016C48(thread, intr);
}

// Subroutine sub_00000B08 - Address 0x00000B08
/* End the wait of the first thread waiting on uid with the wait status waitStatus. See sub_00000A08(). */
void sub_00000B08(SceSysmemUidCB *uid, s32 waitStatus, s32 dispatch, s32 intr)
{
    SceThreadmanThread *thread;

    thread = (SceThreadmanThread *)UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ)->waitList.next;
    thread->waitStatus = waitStatus;
    sub_00000A08(thread, dispatch, intr);
}

// Subroutine sub_00000B48 - Address 0x00000B48
/* End the waits of all the threads waiting on uid with the wait status waitStatus. Return their number. */
s32 sub_00000B48(SceSysmemUidCB *uid, s32 waitStatus)
{
    return _sceKernelReleaseAllWaits(uid, waitStatus);
}

// Subroutine sub_00000BC4 - Address 0x00000BC4
/* Stop the timeout of the wait of thread and notify its wait object (the thread is already unlinked). */
void sub_00000BC4(SceThreadmanThread *thread)
{
    _sceKernelEndWait(thread);
}

// Subroutine sub_00000C30 - Address 0x00000C30
/* Wake up the threads waiting on uid whose wait condition is now satisfied. Return their number. */
s32 sub_00000C30(SceSysmemUidCB *uid)
{
    return sceKernelCallUIDObjFunction(uid, SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS);
}

// Subroutine sub_00000C50 - Address 0x00000C50
/* Create the "WaitQ" UID type. */
s32 sub_00000C50(void)
{
    sceKernelCreateUIDtype("WaitQ", sizeof(SceThreadmanWaitQ), g_WaitQFuncs, NULL, &g_threadman.waitQType);
    return SCE_ERROR_OK;
}

// Subroutine sub_00000C8C - Address 0x00000C8C
/* The timer handler ending a wait on timeout (common is the waiting thread). */
static void sub_00000C8C(void *common)
{
    s32 intr;
    s32 needDispatch;
    SceThreadmanThread *thread;

    thread = common;
    intr = sceKernelCpuSuspendIntr();
    thread->timeoutAlarmId = 0;
    if (thread->timeout != NULL)
        *thread->timeout = 0;
    thread->waitStatus = SCE_ERROR_KERNEL_WAIT_TIMEOUT;
    thread->timeout = NULL;

    needDispatch = _sceKernelReleaseWait(thread);
    if (thread->waitObj != NULL)
        needDispatch += sub_00000C30(thread->waitObj);
    if (needDispatch)
        g_threadman.nextThread = NULL;

    sceKernelCpuResumeIntr(intr);
}

// Subroutine sub_00000DC8 - Address 0x00000DC8
static s32 _sceKernelWaitQInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanWaitQ *waitQ;

    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    waitQ = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanWaitQ);
    waitQ->unk4 = 0;
    waitQ->numWaitThreads = 0;
    waitQ->attr = 0;
    waitQ->waitList.next = &waitQ->waitList;
    waitQ->waitList.prev = &waitQ->waitList;
    return uid->uid;
}

// Subroutine sub_00000E20 - Address 0x00000E20
static s32 _sceKernelWaitQDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    s32 numReleased;

    numReleased = _sceKernelReleaseAllWaits(uid, SCE_ERROR_KERNEL_WAIT_DELETE);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    if (numReleased != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, 0);
    }
    return SCE_ERROR_OK;
}

// Subroutine sub_00000F10 - Address 0x00000F10
static s32 _sceKernelWaitQCancel(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    s32 numReleased;

    numReleased = _sceKernelReleaseAllWaits(uid, SCE_ERROR_KERNEL_WAIT_CANCELLED);
    if (numReleased != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, 0);
    }
    return numReleased;
}

// Subroutine sub_00000FBC - Address 0x00000FBC
static s32 _sceKernelWaitQWakeupWaiters(SceSysmemUidCB *uid __attribute__((unused)),
    SceSysmemUidCB *uidWithFunc __attribute__((unused)), s32 funcId __attribute__((unused)),
    va_list ap __attribute__((unused)))
{
    return 0;
}

// Subroutine sub_00000FC4 - Address 0x00000FC4
static s32 _sceKernelWaitQEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc,
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanWaitQ)->numWaitThreads--;
    return SCE_ERROR_OK;
}

// Subroutine sub_00000FE4 - Address 0x00000FE4
/*
 * Switch to the next thread. release is set when the current thread releases the CPU (by waiting) rather
 * than being preempted. When intr is 0 (called with interrupts disabled from an interrupt-free context?),
 * the switch is deferred to the thread switch software interrupt.
 */
void sub_00000FE4(s32 release, s32 intr)
{
    s32 reason;
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    if (release) {
        thread->switchCounter = &thread->releaseCount;
        reason = 2;
    } else {
        thread->switchCounter = &thread->threadPreemptCount;
        reason = 1;
    }

    if (!intr) {
        pspCop0StateSet(COP0_STATE_CAUSE, pspCop0StateGet(COP0_STATE_CAUSE) | 0x200);
        sceKernelEnableIntr(SCE_THREAD1_INT);
        return;
    }
    if (g_threadman.dispatchSuspended)
        return;

    threadmanProfilerPause();
    if (SaveThreadContext(thread->context) == 0)
        InterruptManagerForKernel_551097F2(sub_00012828(thread->context, reason));
    thread->waitWithCallbacks = 0;
    threadmanProfilerResume();
}

// Subroutine sub_00001100 - Address 0x00001100
/* Check whether the current thread is preempted by an interrupt. */
s32 sub_00001100(void)
{
    s32 preempted;
    SceThreadmanThread *thread;

    if (g_threadman.dispatchSuspended)
        return 0;

    thread = g_threadman.curThread;
    preempted = (g_threadman.nextThread != thread);
    if (preempted)
        thread->switchCounter = &thread->intrPreemptCount;
    return preempted;
}
