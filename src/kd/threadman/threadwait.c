/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/threadwait.c
 *
 * Thread sleep, wakeup, suspend, resume, end wait and delay (original address range 0x00002DF0 -
 * 0x00004248).
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

static s32 __attribute__((used)) _sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout);
static s32 __attribute__((used)) _sceKernelDelayThread(SceUInt delay);
static s32 __attribute__((used)) _sceKernelDelaySysClockThread(SceKernelSysClock *delay);
static void _sceKernelDelayThreadTimeout(void *common);

/**
 * Put the current thread to sleep until it is woken up by sceKernelWakeupThread(). If it has pending
 * wakeups, one of them is consumed instead.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSleepThread - Address 0x00002DF0
int sceKernelSleepThread(void)
{
    s32 oldK1;
    s32 intr;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SLEEP_THREAD, 0);

    sub_00016FA4();

    thread = g_threadman.curThread;
    thread->waitStatus = SCE_ERROR_OK;
    if (thread->wakeupCount != 0)
        thread->wakeupCount--;
    else {
        sub_00000858(&g_threadman.sleepingThreads, SCE_THREADMAN_WAIT_SLEEP);
        g_threadman.nextThread = NULL;
        sub_00000FE4(1, 1);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return thread->waitStatus;
}

/**
 * Put the current thread to sleep, running the notified callbacks. See sceKernelSleepThread().
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSleepThreadCB - Address 0x00002EF0
int sceKernelSleepThreadCB(void)
{
    s32 oldK1;
    s32 intr;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SLEEP_THREAD_CB, 0);

    sub_00016FA4();

    thread = g_threadman.curThread;
    do {
        while (thread->callbackNotified)
            sub_00002170(intr);
        thread->waitStatus = SCE_ERROR_OK;
        if (thread->wakeupCount != 0)
            thread->wakeupCount--;
        else {
            sub_00000858(&g_threadman.sleepingThreads, SCE_THREADMAN_WAIT_SLEEP);
            thread->waitWithCallbacks = 1;
            g_threadman.nextThread = NULL;
            sub_00000FE4(1, 1);
        }
    } while (thread->waitStatus == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return thread->waitStatus;
}

/**
 * Wake up a sleeping thread. If the thread is not sleeping, the wakeup is recorded and consumed by
 * its next sleep.
 *
 * @param thid The UID of the thread to wake up.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelWakeupThread - Address 0x00003050
int sceKernelWakeupThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 isIntr;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (thid == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid);
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (!isIntr && thread == g_threadman.curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (thread->status == SCE_THREADMAN_STATUS_DORMANT || thread->status == SCE_THREADMAN_STATUS_DEAD) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_WAKEUP_THREAD, 1, thid);

    if ((thread->status == SCE_THREADMAN_STATUS_WAIT || thread->status == SCE_THREADMAN_STATUS_WAIT_SUSPEND)
        && thread->waitType == SCE_THREADMAN_WAIT_SLEEP)
        sub_00000A08(thread, isIntr, intr);
    else
        thread->wakeupCount++;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q047 (09d9be8): Interrupt state read raw / at the original's position.
 */
/**
 * Cancel the pending wakeups of a thread.
 *
 * @param thid The UID of the thread (0 for the current thread).
 *
 * @return The number of cancelled wakeups on success, < 0 on error.
 */
// Subroutine sceKernelCancelWakeupThread - Address 0x00003218
int sceKernelCancelWakeupThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 count;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (thid == 0) {
        if (isIntr) {
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
        }
        thid = g_threadman.curThread->uid;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_WAKEUP_THREAD, 1, thid);

    count = thread->wakeupCount;
    thread->wakeupCount = 0;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return count;
}

/**
 * Give the pending wakeups of the current thread to another thread (waking it up if it is sleeping),
 * then put the current thread to sleep.
 *
 * @param thid The UID of the thread receiving the wakeups.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDonateWakeupThread - Address 0x0000333C
s32 sceKernelDonateWakeupThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 count;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;
    SceThreadmanThread *curThread;

    oldK1 = pspShiftK1();
    if (thid == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    sub_00016FA4();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    curThread = g_threadman.curThread;
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (thread == curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (thread->status == SCE_THREADMAN_STATUS_DORMANT || thread->status == SCE_THREADMAN_STATUS_DEAD) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
    }

    count = curThread->wakeupCount;

    THREADMAN_LOG(SCE_THREADMAN_LOG_DONATE_WAKEUP_THREAD, 2, thid, count);

    curThread->wakeupCount = 0;
    curThread->waitStatus = SCE_ERROR_OK;
    sub_00000858(&g_threadman.sleepingThreads, SCE_THREADMAN_WAIT_SLEEP);
    thread->wakeupCount += count;
    if ((thread->status == SCE_THREADMAN_STATUS_WAIT || thread->status == SCE_THREADMAN_STATUS_WAIT_SUSPEND)
        && thread->waitType == SCE_THREADMAN_WAIT_SLEEP)
        sub_00000924(thread);
    g_threadman.nextThread = NULL;
    sub_00000FE4(1, 1);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return curThread->waitStatus;
}

/*
 * PARITY-Q048: Value read once into a local (the original keeps it in one register).
 * PARITY-Q064 (a8737ef): Switch result kept in its own variable / lookup result and owner type in one variable (the
 *     original's register use).
 */
/**
 * Suspend a thread.
 *
 * @param thid The UID of the thread to suspend (not the current thread).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSuspendThread - Address 0x0000357C
int sceKernelSuspendThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 ret;
    s32 isIntr;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (thid == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid);
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (!isIntr && thread == g_threadman.curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SUSPEND_THREAD, 1, thid);

    switch (thread->status) {
    case SCE_THREADMAN_STATUS_RUNNING:
        /* The interrupted thread: if dispatching is suspended, it is suspended once dispatching resumes. */
        if (g_threadman.dispatchSuspended != 0) {
            if (g_threadman.dispatchSuspended != 3)
                g_threadman.dispatchSuspended = 2;
            ret = SCE_ERROR_OK;
            break;
        }
        /* Fall through. */
    case SCE_THREADMAN_STATUS_READY:
        if (isIntr && thread == g_threadman.curThread)
            g_threadman.nextThread = NULL;
        else
            sub_00016BF4(thread, thread->currentPriority);
        threadmanListAddTail(&g_threadman.suspendedThreads, &thread->link);
        thread->status = SCE_THREADMAN_STATUS_SUSPEND;
        ret = SCE_ERROR_OK;
        break;
    case SCE_THREADMAN_STATUS_WAIT:
        thread->status = SCE_THREADMAN_STATUS_WAIT_SUSPEND;
        ret = SCE_ERROR_OK;
        break;
    case SCE_THREADMAN_STATUS_SUSPEND:
    case SCE_THREADMAN_STATUS_WAIT_SUSPEND:
        ret = SCE_ERROR_KERNEL_THREAD_ALREADY_SUSPEND;
        break;
    case SCE_THREADMAN_STATUS_DORMANT:
    case SCE_THREADMAN_STATUS_DEAD:
    default:
        ret = SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
        break;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q048: Value read once into a local (the original keeps it in one register).
 * PARITY-Q064 (d3844c2): Switch result kept in its own variable / lookup result and owner type in one variable (the
 *     original's register use).
 */
/**
 * Resume a thread suspended by sceKernelSuspendThread().
 *
 * @param thid The UID of the thread to resume.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelResumeThread - Address 0x00003790
int sceKernelResumeThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 ret;
    s32 isIntr;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (thid == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid);
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (!isIntr && thread == g_threadman.curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_RESUME_THREAD, 1, thid);

    switch (thread->status) {
    case SCE_THREADMAN_STATUS_SUSPEND:
        threadmanListRemove(&thread->link);
        thread->status = SCE_THREADMAN_STATUS_READY;
        if (!isIntr)
            sub_00016C48(thread, intr);
        else {
            sub_000127D0(thread);
            g_threadman.nextThread = NULL;
        }
        ret = SCE_ERROR_OK;
        break;
    case SCE_THREADMAN_STATUS_WAIT_SUSPEND:
        thread->status = SCE_THREADMAN_STATUS_WAIT;
        ret = SCE_ERROR_OK;
        break;
    case SCE_THREADMAN_STATUS_RUNNING:
        /* The interrupted thread, whose suspension is pending (see sceKernelSuspendThread()). */
        if (g_threadman.dispatchSuspended == 2) {
            g_threadman.dispatchSuspended = 1;
            ret = SCE_ERROR_OK;
            break;
        }
        /* FALLTHROUGH */
    case SCE_THREADMAN_STATUS_READY:
    case SCE_THREADMAN_STATUS_WAIT:
    case SCE_THREADMAN_STATUS_DORMANT:
    case SCE_THREADMAN_STATUS_DEAD:
    default:
        ret = SCE_ERROR_KERNEL_THREAD_IS_NOT_SUSPEND;
        break;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q051: Access through a local/named pointer (the original's addressing): the thread UID type read once.
 */
/**
 * Suspend all the user mode threads, and skip them when waking up waiting threads from now on.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSuspendAllUserThreads - Address 0x00003984
int sceKernelSuspendAllUserThreads(void)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceSysmemUidCB *uidType;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    /* Go through all the thread objects (the instances of the "Thread" UID type). */
    uidType = g_threadman.threadType;
    for (uid = uidType->PARENT0; uid != uidType; uid = uid->PARENT0) {
        thread = UID_CB_TO_DATA(uid, uidType, SceThreadmanThread);
        if (!THREADMAN_IS_USER_THREAD(thread) && !(thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000))
            continue;

        switch (thread->status) {
        case SCE_THREADMAN_STATUS_READY:
            sub_00016BF4(thread, thread->currentPriority);
            threadmanListAddTail(&g_threadman.suspendedThreads, &thread->link);
            thread->status = SCE_THREADMAN_STATUS_SUSPEND;
            break;
        case SCE_THREADMAN_STATUS_WAIT:
            thread->status = SCE_THREADMAN_STATUS_WAIT_SUSPEND;
            break;
        case SCE_THREADMAN_STATUS_RUNNING:
        case SCE_THREADMAN_STATUS_SUSPEND:
        case SCE_THREADMAN_STATUS_WAIT_SUSPEND:
        case SCE_THREADMAN_STATUS_DORMANT:
        case SCE_THREADMAN_STATUS_DEAD:
        default:
            break;
        }
    }
    g_threadman.skipUserThreads = 1;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Wait until a thread ends.
 *
 * @param thid The UID of the thread to wait for.
 * @param timeout The timeout in microseconds (receives the remaining time), or NULL for no timeout.
 *
 * @return The exit status of the thread on success, < 0 on error.
 */
// Subroutine sceKernelWaitThreadEnd - Address 0x00003AC0
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelWaitThreadEnd, _sceKernelWaitThreadEnd, 0);

/**
 * Wait until a thread ends, running the notified callbacks. See sceKernelWaitThreadEnd().
 *
 * @param thid The UID of the thread to wait for.
 * @param timeout The timeout in microseconds (receives the remaining time), or NULL for no timeout.
 *
 * @return The exit status of the thread on success, < 0 on error.
 */
// Subroutine sceKernelWaitThreadEndCB - Address 0x00003AC8
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelWaitThreadEndCB, _sceKernelWaitThreadEnd, 1);

/*
 * PARITY-Q054 (d07dd48): Comparison operand order as the original's branch operands.
 * PARITY-Q066 (d07dd48): The target compared with g_threadman.curThread directly (curThread only loaded before the
 *     loop, as the original).
 */
// Subroutine sub_00003AD0 - Address 0x00003AD0
static s32 __attribute__((used)) _sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    s32 status;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;
    SceThreadmanThread *curThread;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(timeout, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (thid == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    sub_00016FA4();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (thread == g_threadman.curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_WAIT_THREAD_END_CB, 1, thid);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_WAIT_THREAD_END, 1, thid);
    }

    curThread = g_threadman.curThread;
    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && curThread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the thread. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &uid) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        if (thread->status == SCE_THREADMAN_STATUS_DORMANT || thread->status == SCE_THREADMAN_STATUS_DEAD) {
            status = thread->exitStatus;
            break;
        }
        curThread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_THREAD_END, 0, &timeoutClock, timeout);
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Delay the current thread.
 *
 * @param delay The delay in microseconds.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDelayThread - Address 0x00003D78
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelDelayThread, _sceKernelDelayThread, 0);

/**
 * Delay the current thread, running the notified callbacks.
 *
 * @param delay The delay in microseconds.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDelayThreadCB - Address 0x00003D80
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelDelayThreadCB, _sceKernelDelayThread, 1);

/*
 * PARITY-Q042 (96e1ce5): Statement/store order taken from the original's store order.
 */
// Subroutine sub_00003D88 - Address 0x00003D88
static s32 __attribute__((used)) _sceKernelDelayThread(SceUInt delay)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    SceUID alarmId;
    SceKernelSysClock endClock;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    sub_00016FA4();

    thread = g_threadman.curThread;
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_DELAY_THREAD_CB, 1, delay);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_DELAY_THREAD, 1, delay);
    }

    sub_00010D34(&endClock, delay);
    do {
        if (withCallbacks && thread->callbackNotified != 0)
            sub_00002170(intr);
        thread->timeout = NULL;
        alarmId = sub_00010FAC(&endClock, _sceKernelDelayThreadTimeout, thread);
        if (alarmId < 0) {
            /* SCE_ERROR_KERNEL_WAIT_TIMEOUT: the delay has already elapsed. */
            if (alarmId == (s32)SCE_ERROR_KERNEL_WAIT_TIMEOUT)
                alarmId = SCE_ERROR_OK;
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return alarmId;
        }
        if (withCallbacks && thread->callbackNotified != 0) {
            /* A callback has been notified meanwhile: cancel the delay to run it first. */
            sub_00011104(alarmId, NULL);
            thread->timeoutAlarmId = 0;
            thread->waitStatus = SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK;
        } else {
            thread->timeoutAlarmId = alarmId;
            thread->waitData[0] = delay;
            thread->waitData[1] = 0;
            sub_00000858(&g_threadman.delayedThreads, SCE_THREADMAN_WAIT_DELAY);
            thread->waitWithCallbacks = withCallbacks;
            g_threadman.nextThread = NULL;
            sub_00000FE4(1, 1);
        }
    } while (withCallbacks && thread->waitStatus == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return thread->waitStatus;
}

/**
 * Delay the current thread.
 *
 * @param delay The delay in system clocks (microseconds).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDelaySysClockThread - Address 0x00003F9C
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelDelaySysClockThread, _sceKernelDelaySysClockThread, 0);

/**
 * Delay the current thread, running the notified callbacks.
 *
 * @param delay The delay in system clocks (microseconds).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDelaySysClockThreadCB - Address 0x00003FA4
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelDelaySysClockThreadCB, _sceKernelDelaySysClockThread, 1);

/*
 * PARITY-Q042 (96e1ce5): Statement/store order taken from the original's store order.
 */
// Subroutine sub_00003FAC - Address 0x00003FAC
static s32 __attribute__((used)) _sceKernelDelaySysClockThread(SceKernelSysClock *delay)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    SceUID alarmId;
    SceKernelSysClock endClock;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (!pspK1StaBufOk(delay, sizeof(*delay))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    sub_00016FA4();

    thread = g_threadman.curThread;
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_DELAY_SYS_CLOCK_THREAD_CB, 2, delay->hi, delay->low);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_DELAY_SYS_CLOCK_THREAD, 2, delay->hi, delay->low);
    }

    sub_00010D8C(&endClock, delay);
    do {
        if (withCallbacks && thread->callbackNotified != 0)
            sub_00002170(intr);
        thread->timeout = NULL;
        alarmId = sub_00010FAC(&endClock, _sceKernelDelayThreadTimeout, thread);
        if (alarmId < 0) {
            if (alarmId == (s32)SCE_ERROR_KERNEL_WAIT_TIMEOUT)
                alarmId = SCE_ERROR_OK;
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return alarmId;
        }
        if (withCallbacks && thread->callbackNotified != 0) {
            sub_00011104(alarmId, NULL);
            thread->timeoutAlarmId = 0;
            thread->waitStatus = SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK;
        } else {
            thread->timeoutAlarmId = alarmId;
            thread->waitData[0] = delay->low;
            thread->waitData[1] = delay->hi;
            sub_00000858(&g_threadman.delayedThreads, SCE_THREADMAN_WAIT_DELAY);
            thread->waitWithCallbacks = withCallbacks;
            g_threadman.nextThread = NULL;
            sub_00000FE4(1, 1);
        }
    } while (withCallbacks && thread->waitStatus == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return thread->waitStatus;
}

// Subroutine sub_000041F4 - Address 0x000041F4
/* The alarm handler ending the delay of the thread common. */
static void _sceKernelDelayThreadTimeout(void *common)
{
    s32 intr;
    SceThreadmanThread *thread;

    thread = common;
    intr = sceKernelCpuSuspendIntr();
    thread->timeoutAlarmId = 0;
    if (sub_00000924(thread) > 0)
        g_threadman.nextThread = NULL;
    sceKernelCpuResumeIntr(intr);
}
