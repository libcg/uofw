/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/mutex.c
 *
 * Mutexes (original address range 0x00006284 - 0x000076D8).
 *
 * A mutex is a "Mutex" UID object which inherits the "WaitQ" type: the WaitQ part holds the attributes
 * and the list of waiting threads, the Mutex part (SceThreadmanMutex) holds the owner and the lock count.
 *
 * A locked mutex is linked in the ownedMutexes list of its owner. With
 * SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING, this list is sorted by ceiling priority and the owner runs at
 * least at the highest ceiling priority of the mutexes it owns.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid mutex attributes. */
#define SCE_THREADMAN_MUTEX_ATTR_MASK   (0xFFF)

/** The default ceiling priority of a mutex (the lowest priority). */
#define SCE_THREADMAN_MUTEX_NO_PRIORITY (0x7F)

static void _sceKernelMutexLock(SceSysmemUidCB *uid, SceThreadmanThread *thread, s32 count);
static s32 __attribute__((used)) _sceKernelLockMutex(SceUID mutexId, s32 count, SceUInt *timeout);
static void _sceKernelMutexUnlock(SceSysmemUidCB *uid);
static s32 _sceKernelMutexInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelMutexDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelMutexWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019CF8
SceSysmemUidLookupFunc g_MutexFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS, _sceKernelMutexWakeupWaiters },
    { 0xD310D2D9, _sceKernelMutexInitialize },
    { 0x87089863, _sceKernelMutexDelete },
    { 0, NULL }
};

/*
 * PARITY-Q037 (cf293fa): UID attr | (k1 >> 31) passed to sceKernelCreateUID, as the original computes it.
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q051 (173e6b7): Access through a local/named pointer (the original's addressing).
 * PARITY-Q079 (a72237c): memcpy() destination named through g_threadman, not the option local (the original
 *     re-derives that address from the %hi part).
 * PARITY-Q085: Creation failure returns through its own resume/return tail (success path dedented).
 * PARITY-Q087: the new UID kept in its own `id` local instead of reusing the CreateUID status. csim only, still equiv
 *     (intr is still spilled to the stack).
 * PARITY-Q123: option ceilingPriority cleared before size, and lockCount set before initCount. With this order the
 *     scheduler emits the original's store order (size, then ceilingPriority; initCount, then lockCount). wsim
 *     only, still equiv (intr is still spilled).
 */
/*
 * PARITY-Q144: the mutex data pointer is computed before the wait queue's, and the uid is stored last (after
 *     owner, initCount, lockCount). The original loads the mutex type's size first and stores initCount after
 *     the uid; GCC's scheduler reverses both relative to the source order. Plain C.
 */
/**
 * Create a mutex.
 *
 * @param name The name of the mutex.
 * @param attr The mutex attributes (SCE_KERNEL_MUTEX_ATTR_*).
 * @param initCount The initial lock count (a positive count locks the mutex for the calling thread).
 * @param pOption The option parameter (can be NULL).
 *
 * @return The UID of the new mutex on success, < 0 on error.
 */
// Subroutine sceKernelCreateMutex - Address 0x00006284
s32 sceKernelCreateMutex(char *name, s32 attr, s32 initCount, const SceKernelMutexOptParam *pOption)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID id;
    s32 priority;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceKernelMutexOptParam *option = &g_threadman.statusBuf.mutexOption;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(pOption, 8)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_MUTEX_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if (pspK1IsUserMode() && (attr & SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if (initCount < 0 || (initCount >= 2 && (attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    intr = sceKernelCpuSuspendIntr();

    option->ceilingPriority = 0;
    option->size = 0;
    if (pOption != NULL)
        memcpy(&g_threadman.statusBuf.mutexOption, pOption, (pOption->size < 9) ? pOption->size : 8);

    thread = g_threadman.curThread;
    priority = SCE_THREADMAN_MUTEX_NO_PRIORITY;
    if (attr & SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING) {
        priority = g_threadman.statusBuf.mutexOption.ceilingPriority;
        if (priority == 0)
            priority = thread->currentPriority;
        /* A thread creating a locked mutex must not already run above its ceiling priority. */
        if (!THREADMAN_IS_VALID_PRIORITY(priority)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
        }
        if (initCount > 0 && thread->basePriority < priority) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
        }
    }

    status = sceKernelCreateUID(g_threadman.mutexType, name, attr | (pspGetK1() >> 31), &uid);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }
    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    id = uid->uid;
    waitQ->attr = attr;
    mutex->option = pOption;
    mutex->ceilingPriority = priority;
    mutex->owner = NULL;
    mutex->initCount = initCount;
    mutex->lockCount = initCount;
    mutex->uid = id;
    if (initCount > 0)
        _sceKernelMutexLock(uid, thread, initCount);
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_MUTEX, 1, id);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return id;
}

/**
 * Delete a mutex. The threads waiting on it are released with SCE_ERROR_KERNEL_WAIT_DELETE.
 *
 * @param mutexId The UID of the mutex.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteMutex - Address 0x000064F0
s32 sceKernelDeleteMutex(SceUID mutexId)
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

    if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_MUTEX, 1, mutexId);
    sceKernelDeleteUID(mutexId);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q065: Guard and arm shape taken from the original's layout (positive priority-ceiling guard, plain
 *     add in the else arm).
 */
/*
 * Make thread the owner of a mutex with the lock count count, raising the priority of the thread to the
 * ceiling priority of the mutex if needed.
 */
// Subroutine sub_00006604 - Address 0x00006604
static void _sceKernelMutexLock(SceSysmemUidCB *uid, SceThreadmanThread *thread, s32 count)
{
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanListEntry *entry;

    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    mutex->lockCount = count;
    mutex->owner = thread;
    if ((waitQ->attr & SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING) != 0) {
        /* Keep the owned mutexes sorted by ceiling priority, the highest priority first. */
        for (entry = thread->ownedMutexes.next; entry != &thread->ownedMutexes; entry = entry->next) {
            if (mutex->ceilingPriority < ((SceThreadmanMutex *)entry)->ceilingPriority)
                break;
        }
        threadmanListAddTail(entry, &mutex->ownerLink);

        if (mutex->ceilingPriority < thread->mutexPriority)
            thread->mutexPriority = mutex->ceilingPriority;
        if (thread->mutexPriority < thread->currentPriority)
            sub_00014598(thread, thread->mutexPriority, 0);
    } else
        threadmanListAddTail(&thread->ownedMutexes, &mutex->ownerLink);
}

/*
 * PARITY-Q042 (1d26f02): Statement/store order taken from the original's store order.
 * PARITY-Q050 (1d26f02): Loop shape (guarded block instead of continue / separate next cursor / head pointer walk) as
 *     the original's layout.
 */
/**
 * Release the mutexes owned by an exiting thread: the mutexes with SCE_THREADMAN_MUTEX_ATTR_KEEP_ON_EXIT stay
 * locked (their owner becomes SCE_THREADMAN_MUTEX_OWNER_EXITED), the others are unlocked and given to their
 * waiting threads. The thread gets back its base priority.
 *
 * @param thread The exiting thread.
 */
// Subroutine sub_000066F0 - Address 0x000066F0
void _sceKernelMutexReleaseOwned(SceThreadmanThread *thread)
{
    s32 reschedule;
    SceThreadmanListEntry *entry;
    SceThreadmanListEntry *next;
    SceThreadmanMutex *mutex;
    SceSysmemUidCB *uid;
    SceThreadmanWaitQ *waitQ;

    reschedule = 0;
    entry = thread->ownedMutexes.next;
    while (entry != &thread->ownedMutexes) {
        mutex = (SceThreadmanMutex *)entry;
        uid = (SceSysmemUidCB *)((u32 *)mutex - g_threadman.mutexType->size);
        waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
        entry = entry->next;
        threadmanListRemove(&mutex->ownerLink);
        if (waitQ->attr & SCE_THREADMAN_MUTEX_ATTR_KEEP_ON_EXIT) {
            mutex->owner = SCE_THREADMAN_MUTEX_OWNER_EXITED;
        } else {
            mutex->owner = NULL;
            mutex->lockCount = 0;
            reschedule += sub_00000C30(uid);
        }
    }

    thread->mutexPriority = SCE_THREADMAN_MUTEX_NO_PRIORITY;
    if (thread->currentPriority != thread->basePriority) {
        thread->currentPriority = thread->basePriority;
        reschedule = 1;
    }
    if (reschedule) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, 0);
    }
}

/*
 * PARITY-Q046 (1598082): Object pointer derived before waitQ (the original loads the object's type word first).
 * PARITY-Q051: curThread read into a local after the mutex/waitQ pointers (the original loads it in the delay
 *     slot of the count check, before the owner test).
 * PARITY-Q083: still-locked case as `goto end` (goto prediction: the original's block frequencies, hence its $s1/$s2
 *     allocation).
 */
/**
 * Unlock a mutex owned by the current thread. When its lock count reaches 0, the mutex is given to the first
 * waiting thread, if any.
 *
 * @param mutexId The UID of the mutex.
 * @param unlockCount The count to subtract from the lock count.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelUnlockMutex - Address 0x000067F4
s32 sceKernelUnlockMutex(SceUID mutexId, s32 unlockCount)
{
    s32 oldK1;
    s32 intr;
    s32 newCount;
    s32 wokenUp;
    SceSysmemUidCB *uid;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *wokenUpThread;
    SceThreadmanThread *curThread;

    oldK1 = pspShiftK1();
    wokenUpThread = NULL;
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    curThread = g_threadman.curThread;
    if (unlockCount <= 0 || (unlockCount >= 2 && (waitQ->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }
    if (mutex->owner != curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_UNLOCKED;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_UNLOCK_MUTEX, 2, mutexId, unlockCount);

    if (mutex->lockCount < unlockCount) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_UNLOCK_UNDERFLOW;
    }
    newCount = mutex->lockCount - unlockCount;
    mutex->lockCount = newCount;
    if (newCount > 0)
        goto end;
    _sceKernelMutexUnlock(uid);
    wokenUp = 0;
    /* Give the mutex to the first waiting thread. */
    for (thread = (SceThreadmanThread *)waitQ->waitList.next;
         thread != (SceThreadmanThread *)&waitQ->waitList; thread = (SceThreadmanThread *)thread->link.next) {
        /*
         * PARITY-Q015: skipUserThreads is predicted false so the skip test's block layout matches
         * the original
         */
        if (__builtin_expect(g_threadman.skipUserThreads != 0, 0)
            && (THREADMAN_IS_USER_THREAD(thread) || (thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000)))
            continue;
        _sceKernelMutexLock(uid, thread, thread->waitData[0]);
        if (sub_00000924(thread) > 0) {
            wokenUpThread = thread;
            wokenUp = 1;
        }
        break;
    }
    if (wokenUp == 1) {
        sub_00016BF4(wokenUpThread, wokenUpThread->currentPriority);
        sub_00016C48(wokenUpThread, intr);
    }

end:
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Lock a mutex, waiting until it is available.
 *
 * @param mutexId The UID of the mutex.
 * @param lockCount The lock count.
 * @param pTimeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelLockMutex - Address 0x00006A6C
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelLockMutex, _sceKernelLockMutex, 0);

/**
 * Lock a mutex, waiting until it is available and running the callbacks notified meanwhile.
 *
 * @param mutexId The UID of the mutex.
 * @param lockCount The lock count.
 * @param pTimeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelLockMutexCB - Address 0x00006A74
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelLockMutexCB, _sceKernelLockMutex, 1);

/*
 * PARITY-Q056 (c3b7a81): Must-wait priority checks as if / else if (the original's branch chain).
 */
// Subroutine sub_00006A7C - Address 0x00006A7C
static s32 __attribute__((used)) _sceKernelLockMutex(SceUID mutexId, s32 count, SceUInt *timeout)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 mustWait;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *uid;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *waiter;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(timeout, 4)) {
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

    if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_NOT_FOUND;
    }
    thread = g_threadman.curThread;
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (count <= 0 || (count >= 2 && (waitQ->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_LOCK_MUTEX_CB, 2, mutexId, count);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_LOCK_MUTEX, 2, mutexId, count);
    }

    if (mutex->owner == thread) {
        /* Recursive lock. */
        if ((waitQ->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MUTEX_RECURSIVE_NOT_ALLOWED;
        }
        /*
         * PARITY-Q177: The new lock count is accumulated in count itself (no separate newCount): the original
         *     keeps it in count's register ($s5) across the overflow check and store. Whole function exact.
         */
        count += mutex->lockCount;
        if (count < 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MUTEX_LOCK_OVERFLOW;
        }
        mutex->lockCount = count;
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
            if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        /* Do not overtake a waiting thread which has at least our priority. */
        mustWait = 0;
        if (waitQ->numWaitThreads > 0) {
            for (waiter = (SceThreadmanThread *)waitQ->waitList.next;
                 waiter != (SceThreadmanThread *)&waitQ->waitList; waiter = (SceThreadmanThread *)waiter->link.next) {
                /*
                 * PARITY-Q015: skipUserThreads is predicted false so the skip test's block layout
                 * matches the original
                 */
                if (__builtin_expect(g_threadman.skipUserThreads != 0, 0)
                    && (THREADMAN_IS_USER_THREAD(waiter) || (waiter->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000)))
                    continue;
                if ((waitQ->attr & SCE_THREADMAN_WAITQ_ATTR_PRIORITY) == 0)
                    mustWait = 1;
                else if (waiter->currentPriority <= thread->currentPriority)
                    mustWait = 1;
                break;
            }
        }
        if (!mustWait && mutex->owner == NULL) {
            _sceKernelMutexLock(uid, thread, count);
            status = SCE_ERROR_OK;
            break;
        }
        thread->waitData[0] = count;
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_MUTEX, waitQ->attr, &timeoutClock, timeout);
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/*
 * PARITY-Q042 (096181d): Statement/store order taken from the original's store order.
 */
/**
 * Lock a mutex without waiting.
 *
 * @param mutexId The UID of the mutex.
 * @param lockCount The lock count.
 *
 * @return 0 on success, SCE_ERROR_KERNEL_MUTEX_LOCKED if another thread owns the mutex, < 0 on error.
 */
// Subroutine sceKernelTryLockMutex - Address 0x00006E54
s32 sceKernelTryLockMutex(SceUID mutexId, s32 lockCount)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (lockCount <= 0 || (lockCount >= 2 && (waitQ->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0)) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_LOCK_MUTEX, 2, mutexId, lockCount);

    if (mutex->owner == thread) {
        /* Recursive lock. */
        if ((waitQ->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MUTEX_RECURSIVE_NOT_ALLOWED;
        }
        lockCount += mutex->lockCount;
        if (lockCount < 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MUTEX_LOCK_OVERFLOW;
        }
        mutex->lockCount = lockCount;
    } else if (mutex->owner != NULL) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_LOCKED;
    } else {
        _sceKernelMutexLock(uid, thread, lockCount);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Cancel a mutex: release all its waiting threads with SCE_ERROR_KERNEL_WAIT_CANCEL and reset its lock count.
 *
 * @param mutexId The UID of the mutex.
 * @param newLockCount The new lock count (a positive count locks the mutex for the calling thread, a negative
 * count means the initial count).
 * @param pNumWaitThreads Receives the number of released threads (can be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelMutex - Address 0x0000705C
s32 sceKernelCancelMutex(SceUID mutexId, s32 newLockCount, s32 *pNumWaitThreads)
{
    s32 oldK1;
    s32 intr;
    s32 numCancelled;
    SceSysmemUidCB *uid;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT() && newLockCount != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (!pspK1StaBufOk(pNumWaitThreads, 4)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_NOT_FOUND;
    }
    thread = g_threadman.curThread;
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (newLockCount < 0)
        newLockCount = mutex->initCount;
    if (newLockCount >= 2 && (waitQ->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE) == 0) {
        pspSetK1(oldK1);
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_ILLEGAL_COUNT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_MUTEX, 3, mutexId, newLockCount, pNumWaitThreads);

    _sceKernelMutexUnlock(uid);
    numCancelled = sceKernelCallUIDObjFunction(uid, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    if (newLockCount != 0)
        _sceKernelMutexLock(uid, thread, newLockCount);
    if (pNumWaitThreads != NULL)
        *pNumWaitThreads = numCancelled;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a mutex.
 *
 * @param mutexId The UID of the mutex.
 * @param pInfo Receives the status (pInfo->size must be set).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferMutexStatus - Address 0x00007258
s32 sceKernelReferMutexStatus(SceUID mutexId, SceKernelMutexInfo *pInfo)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceKernelMutexInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(pInfo, sizeof(*pInfo))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mutexId, g_threadman.mutexType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MUTEX_NOT_FOUND;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_MUTEX_STATUS, 2, mutexId, pInfo);

    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.mutexInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (uid->name != NULL)
        strncpy(buf->name, uid->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    buf->numWaitThreads = waitQ->numWaitThreads;
    if (mutex->owner == NULL)
        buf->currentOwner = -1;
    else if (mutex->owner == SCE_THREADMAN_MUTEX_OWNER_EXITED)
        buf->currentOwner = -2;
    else
        buf->currentOwner = mutex->owner->uid;
    buf->currentCount = mutex->lockCount;
    buf->initCount = mutex->initCount;
    THREADMAN_COPY_STATUS(pInfo, &g_threadman.statusBuf.mutexInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Register the "Mutex" UID type.
 *
 * @return 0.
 */
// Subroutine sub_0000743C - Address 0x0000743C
s32 _sceKernelMutexInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Mutex", 0x30, g_MutexFuncs, NULL, &g_threadman.mutexType);
    return SCE_ERROR_OK;
}

/*
 * Unlock a mutex (lock count 0, no owner) and remove it from the owned mutexes of its owner, whose priority is
 * lowered back to the ceiling priority of the mutexes it still owns, or to its base priority.
 */
/*
 * PARITY-Q048 (fe568a5): Value read once into a local (the original keeps it in one register).
 * PARITY-Q049 (67a73b6): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q057 (a5007fb): The first owned mutex's UID CB computed into uid before UID_CB_TO_DATA (the original's
 *     order).
 * PARITY-Q065 (67a73b6): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
// Subroutine sub_00007480 - Address 0x00007480
static void _sceKernelMutexUnlock(SceSysmemUidCB *uid)
{
    s32 priority;
    SceThreadmanMutex *mutex;
    SceThreadmanMutex *first;
    SceThreadmanThread *owner;
    SceThreadmanWaitQ *waitQ;

    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    owner = mutex->owner;
    mutex->lockCount = 0;
    mutex->owner = NULL;
    if (owner != NULL && owner != SCE_THREADMAN_MUTEX_OWNER_EXITED) {
    threadmanListRemove(&mutex->ownerLink);
    first = (SceThreadmanMutex *)owner->ownedMutexes.next;
    if (&first->ownerLink != &owner->ownedMutexes) {
        /* The UID control block of the first owned mutex. */
        uid = (SceSysmemUidCB *)((u32 *)first - g_threadman.mutexType->size);
        waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
        if (waitQ->attr & SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING) {
            /* The first owned mutex has the highest ceiling priority. */
            priority = first->ceilingPriority;
            owner->mutexPriority = priority;
            if (priority < owner->basePriority)
                sub_00014598(owner, priority, 0);
            else
                sub_00014598(owner, owner->basePriority, 0);
            return;
        }
    }
    owner->mutexPriority = SCE_THREADMAN_MUTEX_NO_PRIORITY;
    sub_00014598(owner, owner->basePriority, 0);
    }
}

// Subroutine sub_0000755C - Address 0x0000755C
static s32 _sceKernelMutexInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00007584 - Address 0x00007584
static s32 _sceKernelMutexDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    _sceKernelMutexUnlock(uid);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

/*
 * Give an unlocked mutex to its first waiting thread (called by the WaitQ code, for example after a waiting
 * thread was released or its priority changed). Returns 1 if a thread has to be rescheduled, 0 otherwise.
 */
// Subroutine sub_000075E4 - Address 0x000075E4
static s32 _sceKernelMutexWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    s32 reschedule;
    SceThreadmanMutex *mutex;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    reschedule = 0;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    mutex = UID_CB_TO_DATA(uid, g_threadman.mutexType, SceThreadmanMutex);
    if (waitQ->numWaitThreads <= 0)
        return 0;

    for (thread = (SceThreadmanThread *)waitQ->waitList.next;
         thread != (SceThreadmanThread *)&waitQ->waitList; thread = (SceThreadmanThread *)thread->link.next) {
        /*
         * PARITY-Q015: skipUserThreads is predicted false so the skip test's block layout matches the
         * original
         */
        if (__builtin_expect(g_threadman.skipUserThreads != 0, 0)
            && (THREADMAN_IS_USER_THREAD(thread) || (thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000)))
            continue;
        if (mutex->owner != NULL)
            break;
        if (sub_00000924(thread) > 0)
            reschedule = 1;
        _sceKernelMutexLock(uid, thread, thread->waitData[0]);
        break;
    }
    return reschedule;
}
