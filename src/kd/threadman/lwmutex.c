/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/lwmutex.c
 *
 * Lightweight mutexes (original address range 0x000076D8 - 0x00008650).
 *
 * A lightweight mutex is a "LwMutex" UID object which inherits the "WaitQ" type. Its state (lock count, owner
 * and number of waiting threads) lives in a work area of the caller (SceLwMutex), so that the user mode
 * library can lock and unlock an uncontended mutex without a syscall; the functions below handle the
 * contended cases.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid lightweight mutex attributes. */
#define SCE_THREADMAN_LW_MUTEX_ATTR_MASK    (0x3FF)

static s32 _sceKernelWaitLwMutex(SceLwMutex *mutex, s32 count, SceUInt *timeout, s32 withCallbacks);
static s32 _sceKernelLwMutexEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelLwMutexWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelLwMutexInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelLwMutexDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019D18
SceSysmemUidLookupFunc g_LwMutexFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_END_WAIT, _sceKernelLwMutexEndWait },
    { SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS, _sceKernelLwMutexWakeupWaiters },
    { 0xD310D2D9, _sceKernelLwMutexInitialize },
    { 0x87089863, _sceKernelLwMutexDelete },
    { 0, NULL }
};

/**
 * Create a lightweight mutex.
 *
 * @param mutex The work area of the mutex (32 bytes, word aligned).
 * @param name The name of the mutex.
 * @param attr The mutex attributes (SCE_KERNEL_LWMUTEX_*).
 * @param initCount The initial lock count (a positive count locks the mutex for the calling thread).
 * @param option The option parameter (can be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCreateLwMutex - Address 0x000076D8
s32 sceKernelCreateLwMutex(SceLwMutex *mutex, const char *name, u32 attr, s32 initCount,
    SceKernelLwMutexOptParam *option)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceSysmemUidCB *uid;
    SceThreadmanLwMutex *lwMutex;
    SceThreadmanWaitQ *waitQ;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(mutex) || !pspK1PtrOk(name) || !pspK1StaBufOk(option, 4) || ((u32)mutex & 3) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_LW_MUTEX_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if (initCount < 0 || (initCount >= 2 && (attr & SCE_KERNEL_LWMUTEX_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    intr = sceKernelCpuSuspendIntr();

    g_threadman.statusBuf.lwMutexOption.size = 0;
    if (option != NULL)
        memcpy(&g_threadman.statusBuf.lwMutexOption, option, (option->size < 5) ? option->size : 4);

    status = sceKernelCreateUID(g_threadman.lwMutexType, name, (pspGetK1() >> 31) | attr, &uid);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    lwMutex = UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex);
    waitQ->attr = attr;
    lwMutex->option = option;
    lwMutex->initCount = initCount;
    lwMutex->uid = uid->uid;
    lwMutex->workarea = mutex;
    mutex->flags = attr;
    mutex->lockCount = initCount;
    mutex->thid = 0;
    mutex->unk3 = 0;
    mutex->id = uid->uid;
    mutex->unk20[0] = 0;
    mutex->unk20[1] = 0;
    mutex->unk20[2] = 0;
    if (initCount > 0)
        mutex->thid = g_threadman.curThread->uid;
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_LW_MUTEX, 1, uid->uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Delete a lightweight mutex. The threads waiting on it are released with SCE_ERROR_KERNEL_WAIT_DELETE.
 *
 * @param mutex The work area of the mutex.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteLwMutex - Address 0x00007908
s32 sceKernelDeleteLwMutex(SceLwMutex *mutex)
{
    s32 oldK1;
    s32 intr;
    SceUID id;
    SceSysmemUidCB *uid;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(mutex)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    id = mutex->id;
    if (sceKernelGetUIDcontrolBlockWithType(id, g_threadman.lwMutexType, &uid) != SCE_ERROR_OK
        || UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea != mutex) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_LW_MUTEX, 1, id);
    sceKernelDeleteUID(id);
    mutex->id = -1;
    mutex->thid = -1;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Unlock a lightweight mutex owned by the current thread. When its lock count reaches 0, the mutex is given
 * to the first waiting thread, if any.
 *
 * @param mutex The work area of the mutex.
 * @param count The count to subtract from the lock count.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine _sceKernelUnlockLwMutex - Address 0x00007A5C
s32 _sceKernelUnlockLwMutex(SceLwMutex *mutex, s32 count)
{
    s32 oldK1;
    s32 intr;
    s32 newCount;
    s32 wokenUp;
    SceUID id;
    SceSysmemUidCB *uid;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *wokenUpThread;

    oldK1 = pspShiftK1();
    wokenUpThread = NULL;
    if (!pspK1PtrOk(mutex)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    id = mutex->id;
    if (sceKernelGetUIDcontrolBlockWithType(id, g_threadman.lwMutexType, &uid) != SCE_ERROR_OK
        || UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea != mutex) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (count <= 0 || (count >= 2 && (waitQ->attr & SCE_KERNEL_LWMUTEX_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }
    if (mutex->thid != g_threadman.curThread->uid) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_UNLOCKED;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_UNLOCK_LW_MUTEX, 2, id, count);

    if (mutex->lockCount < count) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_UNLOCK_UNDERFLOW;
    }
    newCount = mutex->lockCount - count;
    mutex->lockCount = newCount;
    if (newCount <= 0) {
        mutex->thid = 0;
        mutex->lockCount = 0;
        wokenUp = 0;
        /* Give the mutex to the first waiting thread. */
        thread = (SceThreadmanThread *)waitQ->waitList.next;
        if (thread != (SceThreadmanThread *)&waitQ->waitList) {
            mutex->lockCount = thread->waitData[0];
            mutex->thid = thread->uid;
            if (sub_00000924(thread) > 0) {
                wokenUpThread = thread;
                wokenUp = 1;
            }
        }
        if (wokenUp == 1) {
            sub_00016BF4(wokenUpThread, wokenUpThread->currentPriority);
            sub_00016C48(wokenUpThread, intr);
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Lock a lightweight mutex, waiting until it is available.
 *
 * @param mutex The work area of the mutex.
 * @param count The lock count.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine _sceKernelLockLwMutex - Address 0x00007CD4
s32 _sceKernelLockLwMutex(SceLwMutex *mutex, s32 count, SceUInt *timeout)
{
    return _sceKernelWaitLwMutex(mutex, count, timeout, 0);
}

/**
 * Lock a lightweight mutex, waiting until it is available and running the callbacks notified meanwhile.
 *
 * @param mutex The work area of the mutex.
 * @param count The lock count.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine _sceKernelLockLwMutexCB - Address 0x00007CDC
s32 _sceKernelLockLwMutexCB(SceLwMutex *mutex, s32 count, SceUInt *timeout)
{
    return _sceKernelWaitLwMutex(mutex, count, timeout, 1);
}

// Subroutine sub_00007CE4 - Address 0x00007CE4
static s32 _sceKernelWaitLwMutex(SceLwMutex *mutex, s32 count, SceUInt *timeout, s32 withCallbacks)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 newCount;
    SceUID id;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *uid;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(mutex, sizeof(*mutex)) || !pspK1StaBufOk(timeout, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
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

    id = mutex->id;
    if (sceKernelGetUIDcontrolBlockWithType(id, g_threadman.lwMutexType, &uid) != SCE_ERROR_OK
        || UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea != mutex) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    thread = g_threadman.curThread;
    if (count <= 0 || (count >= 2 && (waitQ->attr & SCE_KERNEL_LWMUTEX_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_LOCK_LW_MUTEX_CB, 2, id, count);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_LOCK_LW_MUTEX, 2, id, count);
    }

    if (mutex->thid == thread->uid) {
        /* Recursive lock. */
        if ((waitQ->attr & SCE_KERNEL_LWMUTEX_RECURSIVE) == 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_LWMUTEX_RECURSIVE_NOT_ALLOWED;
        }
        newCount = count + mutex->lockCount;
        if (newCount < 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_LWMUTEX_LOCK_OVERFLOW;
        }
        mutex->lockCount = newCount;
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_OK;
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the mutex. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(id, g_threadman.lwMutexType, &uid) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        /* Do not overtake the first waiting thread unless it has a lower priority. */
        if ((waitQ->numWaitThreads <= 0
             || ((waitQ->attr & SCE_THREADMAN_WAITQ_ATTR_PRIORITY)
                 && thread->currentPriority < ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority))
            && mutex->thid == 0) {
            mutex->lockCount = count;
            mutex->thid = thread->uid;
            status = SCE_ERROR_OK;
            break;
        }
        thread->waitData[0] = count;
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000680(uid, SCE_THREADMAN_WAIT_LW_MUTEX, waitQ->attr, &timeoutClock, timeout, &mutex->unk3);
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Lock a lightweight mutex without waiting.
 *
 * @param mutex The work area of the mutex.
 * @param count The lock count.
 *
 * @return 0 on success, SCE_ERROR_KERNEL_LWMUTEX_LOCKED if another thread owns the mutex, < 0 on error.
 */
// Subroutine _sceKernelTryLockLwMutex - Address 0x00008094
s32 _sceKernelTryLockLwMutex(SceLwMutex *mutex, s32 count)
{
    s32 oldK1;
    s32 intr;
    s32 newCount;
    SceUID id;
    SceSysmemUidCB *uid;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(mutex)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    id = mutex->id;
    if (sceKernelGetUIDcontrolBlockWithType(id, g_threadman.lwMutexType, &uid) != SCE_ERROR_OK
        || UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea != mutex) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    thread = g_threadman.curThread;
    if (count <= 0 || (count >= 2 && (waitQ->attr & SCE_KERNEL_LWMUTEX_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_LOCK_LW_MUTEX, 2, id, count);

    if (mutex->thid == thread->uid) {
        /* Recursive lock. */
        if ((waitQ->attr & SCE_KERNEL_LWMUTEX_RECURSIVE) == 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_LWMUTEX_RECURSIVE_NOT_ALLOWED;
        }
        newCount = count + mutex->lockCount;
        if (newCount < 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_LWMUTEX_LOCK_OVERFLOW;
        }
        mutex->lockCount = newCount;
    } else if (mutex->thid != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_LOCKED;
    } else {
        mutex->lockCount = count;
        mutex->thid = thread->uid;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a lightweight mutex.
 *
 * @param id The UID of the mutex.
 * @param info Receives the status (info->size must be set).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferLwMutexStatusByID - Address 0x000082C0
s32 sceKernelReferLwMutexStatusByID(SceUID id, SceKernelLwMutexInfo *info)
{
    s32 oldK1;
    s32 intr;
    u32 *ptr;
    SceSysmemUidCB *uid;
    SceThreadmanLwMutex *lwMutex;
    SceThreadmanWaitQ *waitQ;
    SceLwMutex *mutex;
    SceKernelLwMutexInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(id, g_threadman.lwMutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_LWMUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_LW_MUTEX_STATUS_BY_ID, 2, id, info);

    lwMutex = UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    mutex = lwMutex->workarea;
    buf = &g_threadman.statusBuf.lwMutexInfo;
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (uid->name != NULL)
        strncpy(buf->name, uid->name, SCE_UID_NAME_LEN);
    buf->workarea = mutex;
    buf->attr = waitQ->attr;
    buf->uid = uid->uid;
    buf->numWaitThreads = waitQ->numWaitThreads;
    buf->currentOwner = (mutex->thid == 0) ? -1 : mutex->thid;
    buf->currentCount = mutex->lockCount;
    buf->initCount = lwMutex->initCount;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Register the "LwMutex" UID type.
 *
 * @return 0.
 */
// Subroutine sub_000084B4 - Address 0x000084B4
s32 _sceKernelLwMutexInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "LwMutex", 0x30, g_LwMutexFuncs, NULL, &g_threadman.lwMutexType);
    return SCE_ERROR_OK;
}

// Subroutine sub_000084F8 - Address 0x000084F8
static s32 _sceKernelLwMutexInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00008520 - Address 0x00008520
static s32 _sceKernelLwMutexDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceLwMutex *mutex;

    mutex = UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea;
    mutex->lockCount = 0;
    mutex->thid = 0;
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

/* A thread stopped waiting on the mutex: update the number of waiting threads, also in the work area. */
// Subroutine sub_0000855C - Address 0x0000855C
static s32 _sceKernelLwMutexEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    SceThreadmanWaitQ *waitQ;

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea->unk3--;
    waitQ->numWaitThreads--;
    return SCE_ERROR_OK;
}

/*
 * Give an unlocked mutex to its first waiting thread (called by the WaitQ code, for example after a waiting
 * thread was released or its priority changed). Returns 1 if a thread has to be rescheduled, 0 otherwise.
 */
// Subroutine sub_000085A8 - Address 0x000085A8
static s32 _sceKernelLwMutexWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    s32 reschedule;
    SceLwMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    reschedule = 0;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    mutex = UID_CB_TO_DATA(uid, g_threadman.lwMutexType, SceThreadmanLwMutex)->workarea;
    if (waitQ->numWaitThreads <= 0)
        return 0;

    thread = (SceThreadmanThread *)waitQ->waitList.next;
    if (thread == (SceThreadmanThread *)&waitQ->waitList || mutex->thid != 0)
        return reschedule;

    reschedule = (sub_00000924(thread) > 0);
    mutex->thid = thread->uid;
    mutex->lockCount = thread->waitData[0];
    return reschedule;
}
