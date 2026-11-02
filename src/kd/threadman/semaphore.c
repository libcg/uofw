/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/semaphore.c
 *
 * Counting semaphores (original address range 0x00004248 - 0x00005138).
 *
 * A semaphore is a "Semaphore" UID object which inherits the "WaitQ" type: the WaitQ part holds the
 * attributes and the list of waiting threads, the Semaphore part (SceThreadmanSema) holds the counts.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid semaphore attributes. */
#define SCE_THREADMAN_SEMA_ATTR_MASK    (0x1FF)

static s32 _sceKernelWaitSema(SceUID semaid, s32 signal, SceUInt *timeout, s32 withCallbacks);
static s32 _sceKernelSemaMustWait(SceThreadmanWaitQ *waitQ);
static s32 _sceKernelSemaWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelSemaInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelSemaDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019CB8
SceSysmemUidLookupFunc g_SemaFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS, _sceKernelSemaWakeupWaiters },
    { 0xD310D2D9, _sceKernelSemaInitialize },
    { 0x87089863, _sceKernelSemaDelete },
    { 0, NULL }
};

// Subroutine sceKernelCreateSema - Address 0x00004248
SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, SceKernelSemaOptParam *option)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceSysmemUidCB *uid;
    SceThreadmanSema *sema;
    SceThreadmanWaitQ *waitQ;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(option, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_SEMA_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelCreateUID(g_threadman.semaType, name, (pspGetK1() >> 31) | attr, &uid);
    if (status == SCE_ERROR_OK) {
        sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
        waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
        sema->option = option;
        waitQ->attr = attr;
        sema->maxCount = maxVal;
        sema->curCount = initVal;
        sema->initCount = initVal;
        status = uid->uid;
        THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_SEMA, 1, status);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

// Subroutine sceKernelDeleteSema - Address 0x000043C4
int sceKernelDeleteSema(SceUID semaid)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_SEMAPHORE;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_SEMA, 1, semaid);
    /* Threads still waiting on the semaphore are released by the WaitQ delete function. */
    sceKernelDeleteUID(semaid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelSignalSema - Address 0x000044D8
int sceKernelSignalSema(SceUID semaid, int signal)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 newCount;
    s32 numWokenUp;
    SceSysmemUidCB *uid;
    SceThreadmanSema *sema;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;
    SceThreadmanThread *lastWokenUp;

    oldK1 = pspShiftK1();
    lastWokenUp = NULL;

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_SEMAPHORE;
    }
    isIntr = THREADMAN_IS_INTR_CONTEXT();
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    THREADMAN_LOG(SCE_THREADMAN_LOG_SIGNAL_SEMA, 2, semaid, signal);

    /* The count must not overflow, even after all the waiting threads took what they asked for. */
    newCount = sema->curCount + signal;
    for (thread = (SceThreadmanThread *)waitQ->waitList.next; thread != (SceThreadmanThread *)&waitQ->waitList;
         thread = (SceThreadmanThread *)thread->link.next)
        newCount -= thread->waitData[0];
    if (sema->maxCount < newCount) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_SEMA_OVERFLOW;
    }

    sema->curCount += signal;

    /* Wake up the waiting threads, in order, as long as their request can be satisfied. */
    numWokenUp = 0;
    for (thread = (SceThreadmanThread *)waitQ->waitList.next; thread != (SceThreadmanThread *)&waitQ->waitList;
         thread = next) {
        next = (SceThreadmanThread *)thread->link.next;
        if (g_threadman.skipUserThreads
            && (THREADMAN_IS_USER_THREAD(thread) || (thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000)))
            continue;
        if (sema->curCount < thread->waitData[0])
            break;
        sema->curCount -= thread->waitData[0];
        if (sub_00000924(thread) > 0) {
            lastWokenUp = thread;
            numWokenUp++;
        }
    }

    if (isIntr) {
        /* Reschedule when leaving the interrupt. */
        if (numWokenUp > 0)
            g_threadman.nextThread = NULL;
    } else if (numWokenUp == 1) {
        sub_00016BF4(lastWokenUp, lastWokenUp->currentPriority);
        sub_00016C48(lastWokenUp, intr);
    } else if (numWokenUp > 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelWaitSema - Address 0x0000474C
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout)
{
    return _sceKernelWaitSema(semaid, signal, timeout, 0);
}

// Subroutine sceKernelWaitSemaCB - Address 0x00004754
int sceKernelWaitSemaCB(SceUID semaid, int signal, SceUInt *timeout)
{
    return _sceKernelWaitSema(semaid, signal, timeout, 1);
}

// Subroutine sub_0000475C - Address 0x0000475C
static s32 _sceKernelWaitSema(SceUID semaid, s32 signal, SceUInt *timeout, s32 withCallbacks)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *uid;
    SceThreadmanSema *sema;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(timeout, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (signal <= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    sub_00016FA4();

    if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_SEMAPHORE;
    }
    thread = g_threadman.curThread;
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (sema->maxCount < signal) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_WAIT_SEMA_CB, 2, semaid, signal);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_WAIT_SEMA, 2, semaid, signal);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the semaphore. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        if (!_sceKernelSemaMustWait(waitQ) && sema->curCount >= signal) {
            sema->curCount -= signal;
            status = SCE_ERROR_OK;
            break;
        }
        thread->waitData[0] = signal;
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_SEMA, waitQ->attr, &timeoutClock, timeout);
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

// Subroutine sceKernelPollSema - Address 0x00004A34
int sceKernelPollSema(SceUID semaid, int signal)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanSema *sema;
    SceThreadmanWaitQ *waitQ;

    oldK1 = pspShiftK1();
    if (signal <= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_SEMAPHORE;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (sema->maxCount < signal) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_POLL_SEMA, 2, semaid, signal);

    if (_sceKernelSemaMustWait(waitQ) || sema->curCount < signal) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_SEMA_ZERO;
    }
    sema->curCount -= signal;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelCancelSema - Address 0x00004BCC
int sceKernelCancelSema(SceUID semaid, int setCount, int *numWaitThreads)
{
    s32 oldK1;
    s32 intr;
    s32 numCancelled;
    SceSysmemUidCB *uid;
    SceThreadmanSema *sema;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(numWaitThreads, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_SEMAPHORE;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
    if (sema->maxCount < setCount) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_SEMA, 3, semaid, setCount, numWaitThreads);

    /* Release all the waiting threads (their wait returns SCE_ERROR_KERNEL_WAIT_CANCELLED). */
    numCancelled = sceKernelCallUIDObjFunction(uid, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    if (setCount < 0)
        sema->curCount = sema->initCount;
    else
        sema->curCount = setCount;
    if (numWaitThreads != NULL)
        *numWaitThreads = numCancelled;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * Check whether a thread trying to take the semaphore has to wait even if the count is large enough, because
 * other threads are already waiting (in FIFO mode), or because a waiting thread has a higher or the same priority.
 */
// Subroutine sub_00004D68 - Address 0x00004D68
static s32 _sceKernelSemaMustWait(SceThreadmanWaitQ *waitQ)
{
    s32 mustWait;
    SceThreadmanThread *thread;

    mustWait = 0;
    if (waitQ->numWaitThreads > 0) {
        for (thread = (SceThreadmanThread *)waitQ->waitList.next;
             thread != (SceThreadmanThread *)&waitQ->waitList; thread = (SceThreadmanThread *)thread->link.next) {
            if (g_threadman.skipUserThreads
                && (THREADMAN_IS_USER_THREAD(thread) || (thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000)))
                continue;
            if ((waitQ->attr & SCE_THREADMAN_WAITQ_ATTR_PRIORITY) == 0
                || thread->currentPriority <= g_threadman.curThread->currentPriority)
                mustWait = 1;
            break;
        }
    }
    return mustWait;
}

// Subroutine sceKernelReferSemaStatus - Address 0x00004DF0
int sceKernelReferSemaStatus(SceUID semaid, SceKernelSemaInfo *info)
{
    s32 oldK1;
    s32 intr;
    u32 *ptr;
    SceSysmemUidCB *uid;
    SceThreadmanSema *sema;
    SceThreadmanWaitQ *waitQ;
    SceKernelSemaInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(semaid, g_threadman.semaType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_SEMAPHORE;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_SEMA_STATUS, 2, semaid, info);

    sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.semaInfo;
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (uid->name != NULL)
        strncpy(buf->name, uid->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    buf->numWaitThreads = waitQ->numWaitThreads;
    buf->currentCount = sema->curCount;
    buf->maxCount = sema->maxCount;
    buf->initCount = sema->initCount;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sub_00004FBC - Address 0x00004FBC
s32 _sceKernelSemaInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Semaphore", 0x30, g_SemaFuncs, NULL, &g_threadman.semaType);
    return SCE_ERROR_OK;
}

// Subroutine sub_00005000 - Address 0x00005000
static s32 _sceKernelSemaInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00005028 - Address 0x00005028
static s32 _sceKernelSemaDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

/*
 * Wake up the waiting threads whose request can now be satisfied (called by the WaitQ code, for example after
 * a waiting thread was released or its priority changed). Returns the number of threads woken up.
 */
// Subroutine sub_00005048 - Address 0x00005048
static s32 _sceKernelSemaWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    s32 numWokenUp;
    SceThreadmanSema *sema;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;

    numWokenUp = 0;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    sema = UID_CB_TO_DATA(uid, g_threadman.semaType, SceThreadmanSema);
    if (waitQ->numWaitThreads <= 0)
        return 0;

    for (thread = (SceThreadmanThread *)waitQ->waitList.next; thread != (SceThreadmanThread *)&waitQ->waitList;
         thread = next) {
        next = (SceThreadmanThread *)thread->link.next;
        if (g_threadman.skipUserThreads
            && (THREADMAN_IS_USER_THREAD(thread) || (thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000)))
            continue;
        if (sema->curCount < thread->waitData[0])
            break;
        sema->curCount -= thread->waitData[0];
        numWokenUp += (sub_00000924(thread) > 0);
    }
    return numWokenUp;
}
