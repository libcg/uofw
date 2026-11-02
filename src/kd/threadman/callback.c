/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/callback.c
 *
 * Thread event handlers and callbacks (original address range 0x00001140 - 0x00002DBC; 0x00002DBC is in
 * callback_ret.S). The two are in one source file in the original: their functions alternate in the module
 * (0x1140 - 0x19D8, 0x2570 - 0x26CC and 0x2B3C - 0x2C7C are thread event handler functions, the others callback
 * functions) and their UID function tables are adjacent (0x00019AFC, 0x00019B14).
 *
 * A thread event handler is a "ThreadEventHandler" UID object (inheriting the "WaitQ" type) which is
 * called when a thread is created, started, exits or is deleted. It monitors a single thread or all
 * the kernel and/or user threads.
 *
 * A callback is a "Callback" UID object (inheriting the "WaitQ" type) belonging to the thread which
 * created it. Notifying it records the notification; the callback function is then called by its
 * thread when it checks its callbacks (sceKernelCheckCallback() or a *CB wait function).
 */

#include <common_imp.h>
#include <interruptman.h>
#include <loadcore.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/*
 * PARITY-Q020 (ac3e625): the thread event handlers and the callbacks are one file, as in the original's function
 * order (0x1140 - 0x2DBC, one object: the only padded boundary, see PARITY-Q014, falls at its start).
 */

/*
 * PARITY-Q014: The original object for this file has a 16-byte aligned .text: 6.60 pads one nop at 0x113C so that it
 * starts at 0x1140, the only padded file boundary in threadman (see threadman-deviations.md section 4). GCC 3.3 cannot
 * align a function (aligned() on a function is an error), so the section alignment is set here.
 */
asm(".balign 16");

static s32 _sceKernelThreadEventHandlerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap);
static s32 _sceKernelThreadEventHandlerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap);
static s32 _sceKernelCallbackInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelCallbackDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019AFC
SceSysmemUidLookupFunc g_ThreadEventHandlerFuncs[] = {
    { 0xD310D2D9, _sceKernelThreadEventHandlerInitialize },
    { 0x87089863, _sceKernelThreadEventHandlerDelete },
    { 0, NULL }
};

// 0x00019B14
SceSysmemUidLookupFunc g_CallbackFuncs[] = {
    { 0xD310D2D9, _sceKernelCallbackInitialize },
    { 0x87089863, _sceKernelCallbackDelete },
    { 0, NULL }
};

/* Unlink a deleted handler whose last running call has ended (its link is made empty). */
static inline void _sceKernelThreadEventHandlerUnlink(SceThreadmanThreadEventHandler *handler)
{
    threadmanListRemove(&handler->link);
    handler->link.prev = &handler->link;
    handler->link.next = &handler->link;
}

/* Unlink a deleted callback whose last running call has ended (its link is made empty). */
static inline void _sceKernelCallbackUnlink(SceThreadmanCallback *callback)
{
    threadmanListRemove(&callback->link);
    callback->link.prev = &callback->link;
    callback->link.next = &callback->link;
}

/*
 * PARITY-Q045 (e3e6f07): The new UID kept in a local id read before the data pointer (the original's register use).
 * PARITY-Q054: Comparison operand order as the original: the current thread's mode on the left.
 * PARITY-Q076: Caller mode read once into a userMode local after the thread lookup, for the permission and mode checks.
 *   (Superseded by Q163: the local is gone again.)
 * PARITY-Q163: pspK1IsUserMode() written at both checks instead of a userMode local: the caller mode is then
 *   a CSE/GCSE temporary created after the threadUid load, so the global allocator's equal-priority tie
 *   (allocno number order) gives the two the original's a0/a1.
 */
/**
 * Register a thread event handler.
 *
 * @param name The name of the thread event handler.
 * @param threadID The thread to monitor, or one of SCE_KERNEL_THREAD_EVENT_ID_*.
 * @param mask The events to handle (SCE_KERNEL_THREAD_EVENT_*).
 * @param handler The handler.
 * @param common The common pointer passed to the handler.
 *
 * @return The UID of the thread event handler on success, < 0 on error.
 */
// Subroutine sceKernelRegisterThreadEventHandler - Address 0x00001140
SceUID sceKernelRegisterThreadEventHandler(const char *name, SceUID threadID, s32 mask,
    SceKernelThreadEventHandler handler, void *common)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID id;
    SceSysmemUidCB *uid;
    SceSysmemUidCB *threadUid;
    SceThreadmanThread *thread;
    SceThreadmanThreadEventHandler *eventHandler;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1PtrOk(handler) || (!pspK1IsUserMode() && (s32)handler >= 0)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT() || g_threadman.curThread->threadEventMask != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (mask & ~SCE_KERNEL_THREAD_EVENT_MASK) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MASK;
    }

    intr = sceKernelCpuSuspendIntr();

    if (threadID == SCE_KERNEL_THREAD_EVENT_ID_USER || threadID == SCE_KERNEL_THREAD_EVENT_ID_KERN
        || threadID == SCE_KERNEL_THREAD_EVENT_ID_ALL) {
        /* User mode callers can only monitor the user threads. */
        if (pspK1IsUserMode() && threadID != SCE_KERNEL_THREAD_EVENT_ID_USER) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
        }
        thread = (SceThreadmanThread *)threadID;
    } else {
        if (threadID == SCE_KERNEL_THREAD_EVENT_ID_CURRENT) {
            if (mask != SCE_KERNEL_THREAD_EVENT_EXIT) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
            }
            threadID = g_threadman.curThread->uid;
        }
        if (sceKernelGetUIDcontrolBlockWithType(threadID, g_threadman.threadType, &threadUid) != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
        }
        if (!threadmanUidPermOkMode(threadUid, SCE_THREADMAN_UID_ATTR_SIGNAL, pspK1IsUserMode())) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
        }
        thread = UID_CB_TO_DATA(threadUid, g_threadman.threadType, SceThreadmanThread);
        if (pspK1IsUserMode() && THREADMAN_THREAD_MODE(g_threadman.curThread) != THREADMAN_THREAD_MODE(thread)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
        }
    }

    status = sceKernelCreateUID(g_threadman.threadEventHandlerType, name, pspGetK1() >> 31, &uid);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    id = uid->uid;
    eventHandler = UID_CB_TO_DATA(uid, g_threadman.threadEventHandlerType, SceThreadmanThreadEventHandler);
    eventHandler->thread = thread;
    eventHandler->userMode = pspK1IsUserMode();
    eventHandler->uid = id;
    eventHandler->mask = mask;
    eventHandler->handler = handler;
    eventHandler->common = common;
    eventHandler->gp = sceKernelGetModuleGPByAddressForKernel((u32)handler);
    if (eventHandler->gp == 0)
        eventHandler->gp = threadmanGetGp();
    threadmanListAddTail(&g_threadman.threadEventHandlers, &eventHandler->link);

    THREADMAN_LOG(SCE_THREADMAN_LOG_REGISTER_THREAD_EVENT_HANDLER, 5, threadID, mask, handler, common, id);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return id;
}

/*
 * PARITY-Q051: interrupt state and the current thread read into locals before the check (the original loads
 *     curThread in the branch delay slot, ahead of the threadEventMask test).
 */
/**
 * Release a thread event handler.
 *
 * @param uid The UID of the thread event handler.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReleaseThreadEventHandler - Address 0x0000143C
s32 sceKernelReleaseThreadEventHandler(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 isIntr;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    /*
     * PARITY-Q012: per-site: the interrupt check here uses the volatile cfc0 (Q011); the only one of the 98
     * interrupt checks where the original's order needs it
     */
    isIntr = threadmanCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    thread = g_threadman.curThread;
    if (isIntr || thread->threadEventMask) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    /* Deleting the handler may wait for its running calls to end. */
    if (intr == 0 || g_threadman.dispatchSuspended) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }
    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.threadEventHandlerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD_EVENT_HANDLER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_RELEASE_THREAD_EVENT_HANDLER, 1, uid);

    status = sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/*
 * PARITY-Q126: The skip paths of the handler loop jump to a shared `skip:` label at the end of the loop body
 * (`next = entry->next;`) instead of a `next = entry->next; continue;` in each failed check. Block layout now
 * matches the original (mask test falls through into the checks, deleted test as bnel to the latch).
 * wsim +0.099, csim -0.017 (size 162 -> 158, orig 160). Residue: the %hi(g_threadman) reuse (orig: fresh lui) and
 * the s-register choice.
 */
// Subroutine sub_00001580 - Address 0x00001580
/* Call the handlers of the thread event event which occurred on thread. Always return 0. */
s32 sub_00001580(u32 event, SceThreadmanThread *thread, s32 intr)
{
    s32 ret;
    s32 numWokenUp;
    SceThreadmanThread *curThread;
    SceThreadmanListEntry *entry;
    SceThreadmanListEntry *next;
    SceSysmemUidCB *uid;
    SceThreadmanThreadEventHandler *eventHandler;

    curThread = g_threadman.curThread;
    numWokenUp = 0;
    curThread->threadEventMask |= event;
    for (entry = g_threadman.threadEventHandlers.next; entry != &g_threadman.threadEventHandlers; entry = next) {
        eventHandler = (SceThreadmanThreadEventHandler *)entry;
        uid = THREADMAN_UID_DATA_TO_CB(eventHandler, g_threadman.threadEventHandlerType);
        if (!(event & eventHandler->mask) || eventHandler->deleted
            || (THREADMAN_IS_USER_THREAD(g_threadman.curThread)
                && THREADMAN_THREAD_MODE(g_threadman.curThread) != THREADMAN_THREAD_MODE(thread))) {
            goto skip;
        }
        if (eventHandler->thread != (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_ALL
            && (THREADMAN_IS_USER_THREAD(thread)
                ? (eventHandler->thread != (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_USER
                    || !THREADMAN_IS_USER_THREAD(g_threadman.curThread))
                : eventHandler->thread != (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_KERN)
            && eventHandler->thread != thread) {
            goto skip;
        }

        eventHandler->runCount++;
        curThread->threadEventHandler = uid;
        THREADMAN_LOG(SCE_THREADMAN_LOG_CALL_THREAD_EVENT_HANDLER, 4, uid->uid, event, eventHandler->common,
            eventHandler->handler);
        ret = sub_00002944(eventHandler->userMode, eventHandler->handler, event, thread->uid,
            (u32)eventHandler->common, 0, eventHandler->gp);
        eventHandler->runCount--;
        curThread->threadEventHandler = NULL;
        next = entry->next;
        if (eventHandler->deleted && eventHandler->runCount == 0) {
            /* The handler has been released while running: wake up the releasing thread. */
            _sceKernelThreadEventHandlerUnlink(eventHandler);
            numWokenUp += sub_00000B48(uid, SCE_ERROR_OK);
        } else if (!eventHandler->deleted && ret != 0)
            sceKernelDeleteUID(eventHandler->uid);
        continue;
skip:
        next = entry->next;
    }

    if (g_threadman.dispatchSuspended)
        sub_00014124(1, 0, intr);
    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }
    curThread->threadEventMask &= ~event;
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q043 (76cf7f6): Struct fields filled in struct order (the original's store order).
 * PARITY-Q059 (6810a13): ThreadId selected with a switch over the special IDs (GCC's case tree = the original's
 *     -8/-7/-16/-1 tests).
 */
/**
 * Get the status of a thread event handler.
 *
 * @param uid The UID of the thread event handler.
 * @param info Pointer receiving the status.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferThreadEventHandlerStatus - Address 0x00001800
s32 sceKernelReferThreadEventHandlerStatus(SceUID uid, SceKernelThreadEventHandlerInfo *info)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanThreadEventHandler *eventHandler;
    SceKernelThreadEventHandlerInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.threadEventHandlerType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD_EVENT_HANDLER;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_THREAD_EVENT_HANDLER_STATUS, 2, uid, info);

    eventHandler = UID_CB_TO_DATA(cb, g_threadman.threadEventHandlerType, SceThreadmanThreadEventHandler);
    buf = &g_threadman.statusBuf.threadEventHandlerInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    switch ((SceUID)eventHandler->thread) {
    case SCE_KERNEL_THREAD_EVENT_ID_KERN:
    case SCE_KERNEL_THREAD_EVENT_ID_USER:
    case SCE_KERNEL_THREAD_EVENT_ID_ALL:
        buf->threadId = (SceUID)eventHandler->thread;
        break;
    default:
        buf->threadId = eventHandler->thread->uid;
        break;
    }
    buf->mask = eventHandler->mask;
    buf->handler = eventHandler->handler;
    buf->common = eventHandler->common;
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.threadEventHandlerInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q045 (a4d5017): The new UID kept in a local id read before the data pointer (the original's register use).
 */
/**
 * Create a callback belonging to the current thread.
 *
 * @param name The name of the callback.
 * @param func The callback function.
 * @param common The common pointer passed to the callback function.
 *
 * @return The UID of the callback on success, < 0 on error.
 */
// Subroutine sceKernelCreateCallback - Address 0x000019D8
SceUID sceKernelCreateCallback(const char *name, SceKernelCallbackFunction func, void *common)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID id;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;
    SceThreadmanCallback *callback;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    if (!pspK1PtrOk(name) || !pspK1PtrOk(func) || (!pspK1IsUserMode() && (s32)func >= 0)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelCreateUID(g_threadman.callbackType, name, pspGetK1() >> 31, &uid);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    id = uid->uid;
    callback = UID_CB_TO_DATA(uid, g_threadman.callbackType, SceThreadmanCallback);
    callback->userMode = pspK1IsUserMode();
    callback->uid = id;
    callback->thread = thread;
    callback->notifyCount = 0;
    callback->notifyArg = 0;
    callback->func = func;
    callback->common = common;
    callback->gp = sceKernelGetModuleGPByAddressForKernel((u32)func);
    if (callback->gp == 0)
        callback->gp = threadmanGetGp();
    threadmanListAddTail(&thread->callbacks, &callback->link);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_CALLBACK, 3, func, common, id);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return id;
}

/**
 * Delete a callback.
 *
 * @param uid The UID of the callback.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteCallback - Address 0x00001B80
s32 sceKernelDeleteCallback(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT() || g_threadman.curThread->curCallback != NULL) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    /* Deleting the callback may wait for its running calls to end. */
    if (intr == 0 || g_threadman.dispatchSuspended) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }
    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.callbackType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_CALLBACK;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_CALLBACK, 1, uid);

    status = sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Notify a callback. If its thread waits with callbacks enabled, its wait is released.
 *
 * @param uid The UID of the callback.
 * @param arg The second argument of the callback function.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelNotifyCallback - Address 0x00001CC0
s32 sceKernelNotifyCallback(SceUID uid, s32 arg)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 numWokenUp;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanCallback *callback;

    oldK1 = pspShiftK1();
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.callbackType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_CALLBACK;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    callback = UID_CB_TO_DATA(cb, g_threadman.callbackType, SceThreadmanCallback);

    THREADMAN_LOG(SCE_THREADMAN_LOG_NOTIFY_CALLBACK, 2, uid, arg);

    thread = callback->thread;
    thread->callbackNotified = 1;
    callback->notifyArg = arg;
    callback->notifyCount++;
    if (thread->waitWithCallbacks
        && (thread->status == SCE_THREADMAN_STATUS_WAIT || thread->status == SCE_THREADMAN_STATUS_WAIT_SUSPEND)) {
        /* Release the wait of the thread so that it runs its callbacks. */
        thread->waitStatus = SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK;
        numWokenUp = sub_00000924(thread);
        if (thread->waitObj != NULL)
            numWokenUp += sub_00000C30(thread->waitObj);
        if (numWokenUp != 0) {
            g_threadman.nextThread = NULL;
            if (!isIntr)
                sub_00000FE4(0, intr);
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q042 (578dde3): Statement/store order taken from the original's store order.
 */
/**
 * Cancel the pending notifications of a callback.
 *
 * @param uid The UID of the callback.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelCallback - Address 0x00001E74
s32 sceKernelCancelCallback(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanCallback *callback;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.callbackType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_CALLBACK;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    callback = UID_CB_TO_DATA(cb, g_threadman.callbackType, SceThreadmanCallback);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_CALLBACK, 1, uid);

    callback->notifyCount = 0;
    callback->notifyArg = 0;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the number of pending notifications of a callback.
 *
 * @param uid The UID of the callback.
 *
 * @return The notification count on success, < 0 on error.
 */
// Subroutine sceKernelGetCallbackCount - Address 0x00001F78
s32 sceKernelGetCallbackCount(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 count;
    SceSysmemUidCB *cb;
    SceThreadmanCallback *callback;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.callbackType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_CALLBACK;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    callback = UID_CB_TO_DATA(cb, g_threadman.callbackType, SceThreadmanCallback);
    count = callback->notifyCount;

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_CALLBACK_COUNT, 2, uid, count);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return count;
}

/*
 * PARITY-Q042 (18b8790): Statement/store order taken from the original's store order.
 */
/**
 * Run the notified callbacks of the current thread.
 *
 * @return 1 if callbacks were run, 0 if none, < 0 on error.
 */
// Subroutine sceKernelCheckCallback - Address 0x00002078
s32 sceKernelCheckCallback(void)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    ret = 0;
    if (THREADMAN_IS_INTR_CONTEXT() || thread->curCallback != NULL) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (intr == 0 || g_threadman.dispatchSuspended) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }
    if (thread->callbackNotified)
        ret = (sub_00002170(intr) != 0);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CHECK_CALLBACK, 1, ret);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q042 (116eeb6): Statement/store order taken from the original's store order.
 * PARITY-Q050 (116eeb6): Loop shape (guarded block instead of continue / separate next cursor / head pointer walk) as
 *     the original's layout.
 */
// Subroutine sub_00002170 - Address 0x00002170
/* Call the notified callbacks of the current thread. Return the number of callbacks called. */
s32 sub_00002170(s32 intr)
{
    s32 ret;
    s32 numCalled;
    s32 numWokenUp;
    s32 notifyCount;
    s32 notifyArg;
    SceThreadmanThread *thread;
    SceThreadmanListEntry *entry;
    SceThreadmanListEntry *next;
    SceSysmemUidCB *uid;
    SceSysmemUidCB *prevCallback;
    SceThreadmanCallback *callback;

    numWokenUp = 0;
    numCalled = 0;
    thread = g_threadman.curThread;
    thread->callbackNotified = 0;
    for (entry = thread->callbacks.next; entry != &thread->callbacks; entry = next) {
        callback = (SceThreadmanCallback *)entry;
        uid = THREADMAN_UID_DATA_TO_CB(callback, g_threadman.callbackType);
        notifyCount = callback->notifyCount;
        if (notifyCount != 0 && !callback->deleted) {
            callback->notifyCount = 0;
            notifyArg = callback->notifyArg;
            callback->notifyArg = 0;
            thread->callbackDepth++;
            prevCallback = thread->curCallback;
            thread->curCallback = uid;
            callback->runCount++;
            THREADMAN_LOG(SCE_THREADMAN_LOG_CALL_CALLBACK, 5, uid->uid, notifyCount, notifyArg, callback->common,
                callback->func);
            ret = sub_00002944(callback->userMode, callback->func, notifyCount, notifyArg, (u32)callback->common, 0,
                callback->gp);
            thread->curCallback = prevCallback;
            callback->runCount--;
            thread->callbackDepth--;
            next = entry->next;
            numCalled++;
            if (callback->deleted && callback->runCount == 0) {
                /* The callback has been deleted while running: wake up the deleting thread. */
                _sceKernelCallbackUnlink(callback);
                numWokenUp += sub_00000B48(uid, SCE_ERROR_OK);
            } else if (!callback->deleted && ret != 0)
                sceKernelDeleteUID(callback->uid);
        } else
            next = entry->next;
    }

    if (g_threadman.dispatchSuspended)
        sub_00014124(1, 0, intr);
    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }
    return numCalled;
}

/**
 * Get the status of a callback.
 *
 * @param uid The UID of the callback.
 * @param info Pointer receiving the status.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferCallbackStatus - Address 0x00002384
s32 sceKernelReferCallbackStatus(SceUID uid, SceKernelCallbackInfo *info)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanCallback *callback;
    SceKernelCallbackInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.callbackType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_CALLBACK;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_CALLBACK_STATUS, 2, uid, info);

    callback = UID_CB_TO_DATA(cb, g_threadman.callbackType, SceThreadmanCallback);
    buf = &g_threadman.statusBuf.callbackInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->threadId = callback->thread->uid;
    buf->callback = callback->func;
    buf->notifyCount = callback->notifyCount;
    buf->common = callback->common;
    buf->notifyArg = callback->notifyArg;
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.callbackInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Return from a user mode callback. The return address of user mode callbacks points to a syscall
 * of this function; the result of the callback is in the $v0 register of the syscall frame.
 */
// Subroutine _sceKernelReturnFromCallback - Address 0x0000253C
s32 _sceKernelReturnFromCallback(void)
{
    SceThreadmanThread *thread;

    sceKernelCpuSuspendIntr();
    thread = g_threadman.curThread;
    thread->waitStatus = ((SceThreadmanSyscallFrame *)thread->syscallFrame)->v0;
    return sub_000028C0();
}

/*
 * PARITY-Q042 (578dde3): Statement/store order taken from the original's store order.
 */
// Subroutine sub_00002570 - Address 0x00002570
/* End the thread event handler call of thread, which exits from within the handler. */
void sub_00002570(SceThreadmanThread *thread)
{
    SceSysmemUidCB *uid;
    SceThreadmanThreadEventHandler *eventHandler;

    uid = thread->threadEventHandler;
    if (uid == NULL)
        return;

    eventHandler = UID_CB_TO_DATA(uid, g_threadman.threadEventHandlerType, SceThreadmanThreadEventHandler);
    eventHandler->runCount--;
    if (eventHandler->deleted && eventHandler->runCount == 0) {
        _sceKernelThreadEventHandlerUnlink(eventHandler);
        sub_00000B48(uid, SCE_ERROR_OK);
    }
    thread->threadEventMask = 0;
    thread->threadEventHandler = NULL;
}

/*
 * PARITY-Q040 (b287e0d): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q042 (3908d6a, 034d6ec): Statement/store order taken from the original's store order.
 * PARITY-Q050 (034d6ec, 3908d6a): Loop shape (guarded block instead of continue / separate next cursor / head pointer
 *     walk) as the original's layout.
 */
// Subroutine sub_000025FC - Address 0x000025FC
/* Return 1 if a thread event handler would be called for the thread event event on thread, 0 otherwise. */
s32 sub_000025FC(u32 event, SceThreadmanThread *thread)
{
    SceThreadmanListEntry *entry;
    SceThreadmanListEntry *next;
    SceThreadmanThreadEventHandler *eventHandler;

    for (entry = g_threadman.threadEventHandlers.next; entry != &g_threadman.threadEventHandlers; entry = next) {
        next = entry->next;
        eventHandler = (SceThreadmanThreadEventHandler *)entry;
        if ((event & eventHandler->mask) && !eventHandler->deleted
            && (!THREADMAN_IS_USER_THREAD(g_threadman.curThread) || THREADMAN_THREAD_MODE(g_threadman.curThread) == THREADMAN_THREAD_MODE(thread))) {
            if (eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_ALL
                || (THREADMAN_IS_USER_THREAD(thread)
                    && eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_USER
                    && THREADMAN_IS_USER_THREAD(g_threadman.curThread))
                || (!THREADMAN_IS_USER_THREAD(thread)
                    && eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_KERN)
                || eventHandler->thread == thread)
                return 1;
        }
    }
    return 0;
}

/*
 * PARITY-Q042 (4e268f7): Statement/store order taken from the original's store order.
 */
// Subroutine sub_000026CC - Address 0x000026CC
/* Delete the callbacks of thread (which is being deleted). */
void sub_000026CC(SceThreadmanThread *thread)
{
    SceThreadmanListEntry *entry;
    SceThreadmanCallback *callback;

    entry = thread->callbacks.next;
    while (entry != &thread->callbacks) {
        callback = (SceThreadmanCallback *)entry;
        callback->runCount = 0;
        entry = entry->next;
        sceKernelDeleteUID(callback->uid);
    }
    thread->callbackNotified = 0;
    thread->curCallback = NULL;
    thread->callbackDepth = 0;
}

/**
 * Get the number of nested callbacks running on the current thread.
 *
 * @return The callback depth on success, < 0 on error.
 */
// Subroutine ThreadManForUser_8672E3D0 - Address 0x0000272C
s32 ThreadManForUser_8672E3D0(void)
{
    s32 oldK1;
    s32 depth;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    depth = g_threadman.curThread->callbackDepth;

    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_CALLBACK_DEPTH, 2, g_threadman.curThread->uid, depth);

    pspSetK1(oldK1);
    return depth;
}

s32 ThreadManForKernel_8672E3D0(void) __attribute__((alias("ThreadManForUser_8672E3D0")));

// Subroutine sub_000027B4 - Address 0x000027B4
/* End the callback call of thread, which exits from within the callback. */
void sub_000027B4(SceThreadmanThread *thread)
{
    SceSysmemUidCB *uid;
    SceThreadmanCallback *callback;

    uid = thread->curCallback;
    if (uid == NULL)
        return;

    callback = UID_CB_TO_DATA(uid, g_threadman.callbackType, SceThreadmanCallback);
    callback->runCount--;
    if (callback->deleted && callback->runCount == 0) {
        _sceKernelCallbackUnlink(callback);
        sub_00000B48(uid, SCE_ERROR_OK);
    }
    thread->curCallback = NULL;
}

// Subroutine sub_0000283C - Address 0x0000283C
s32 _sceKernelCallbackInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "ThreadEventHandler", 0x30, g_ThreadEventHandlerFuncs, NULL,
        &g_threadman.threadEventHandlerType);
    sceKernelCreateUIDtypeInherit("WaitQ", "Callback", 0x30, g_CallbackFuncs, NULL, &g_threadman.callbackType);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q052 (5248502): Fatal break loop placed at the end of the function, as the original.
 */
// Subroutine sub_000028C0 - Address 0x000028C0
/* Return from the current callback (or thread event handler) to the context which called it. */
s32 sub_000028C0(void)
{
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    if (THREADMAN_IS_INTR_CONTEXT() || thread->callbackContext == NULL)
        goto fatal;

    /* Restore the VFPU state of the returned to context. */
    if (thread->callbackContext->status & SCE_THREADMAN_CONTEXT_STATUS_CU2) {
        thread->attr |= SCE_KERNEL_TH_USE_VFPU;
        sub_00012C04(thread);
    } else
        thread->attr &= ~SCE_KERNEL_TH_USE_VFPU;
    InterruptManagerForKernel_551097F2(thread->callbackContext);
    return SCE_ERROR_OK;

fatal:
    for (;;)
        pspBreak(THREADMAN_BREAKCODE_FATAL);
}

/*
 * PARITY-Q038 (c9b95ca): Saved thread state kept in a local struct (the original's stack layout and reload order).
 * PARITY-Q117: thread->fpuUsed is saved before the other fields. The scheduler then loads it with the others before
 *     the stack stores (lw 0x18 before the sw's, the last sw in the sub_00016FA4 call's delay slot), as in the
 *     original. csim +0.016. Residue: the 0x24/0x28 loads are swapped, and the restore block's first load fills the
 *     beqz delay slot (the original has a nop there). The 18 orders of the save/restore lines on top of this are all down.
 * PARITY-Q156: The callback context's gpr[29] is set before gpr[30] and gpr[26] (store order).
 * PARITY-Q176: The saved struct holds syscallFrame before userSp. Member order only; csim +0.016. This is NOT the
 *     original's layout: the original keeps userSp (thread+0x28) at 0x0($sp) and syscallFrame (thread+0x24) at
 *     0x4($sp) on both save and restore, which is the userSp-first order. That order scores lower here
 *     (0.8127 -> 0.7968) because of the remaining load-order/rename residue, so this order stays for now.
 */
// Subroutine sub_00002944 - Address 0x00002944
/*
 * Call func(arg0, arg1, arg2, arg3) on the current thread with $gp set to gp, in user mode if userMode is
 * set. Return the result of func, set by _sceKernelReturnFromCallback() (user mode) or sub_00002DBC()
 * (kernel mode) which both resume the context saved here through sub_000028C0().
 */
s32 sub_00002944(s32 userMode, void *func, u32 arg0, u32 arg1, u32 arg2, u32 arg3, u32 gp)
{
    struct {
        void *syscallFrame;
        u32 userSp;
        SceThreadmanContext *callbackContext;
    } saved;
    u8 contextBuf[sizeof(SceThreadmanContext) + 63];
    u32 stack;
    u32 status;
    u32 savedFpuUsed;
    SceThreadmanContext *context;
    SceThreadmanContext *callerContext;
    SceThreadmanThread *thread;

    /* The callback entry context (64 bytes aligned). */
    context = (SceThreadmanContext *)(((u32)contextBuf + 63) & ~63);
    sub_000134A8(context);
    context->epc = (u32)func;
    context->gpr[4] = arg0;
    context->gpr[5] = arg1;
    context->gpr[6] = arg2;
    context->gpr[7] = arg3;
    context->gpr[28] = gp;

    thread = g_threadman.curThread;
    savedFpuUsed = thread->fpuUsed;
    saved.userSp = thread->userSp;
    saved.syscallFrame = thread->syscallFrame;
    saved.callbackContext = thread->callbackContext;
    sub_00016FA4();
    thread->callbackContext = thread->context;
    if (SaveThreadContext(thread->context) != 0) {
        /* The callback has returned (through sub_000028C0()). */
        callerContext = thread->callbackContext;
        thread->userSp = saved.userSp;
        thread->syscallFrame = saved.syscallFrame;
        thread->callbackContext = saved.callbackContext;
        thread->fpuUsed = savedFpuUsed;
        thread->context = callerContext;
        if (threadmanCop0CtrlGet(COP0_CTRL_EXC_TABLE) == (s32)context)
            threadmanCop0CtrlSet(COP0_CTRL_EXC_TABLE, 0);
        threadmanCop0CtrlSet(COP0_CTRL_SP_KERNEL, (u32)callerContext);
        threadmanCop0CtrlSet(COP0_CTRL_SP_USER, thread->userSp);
        return thread->waitStatus;
    }

    thread->context = context;
    if (userMode) {
        /*
         * Build a "syscall _sceKernelReturnFromCallback" instruction on the user stack, used as the
         * return address of the callback.
         */
        stack = (thread->syscallFrame != NULL) ? ((SceThreadmanSyscallFrame *)thread->syscallFrame)->sp : thread->initSp;
        stack -= 64;
        ((u32 *)stack)[0] = ((g_threadman.returnFromCallbackSyscall << 6) & 0x03FFFFC0) | 0xC;
        ((u32 *)stack)[1] = 0;
        ((u32 *)stack)[2] = 0;
        ((u32 *)stack)[3] = 0;
        pspCache(0x1B, (void *)stack);
        pspCache(0x08, (void *)stack);
        pspSync();
        context->gpr[31] = stack;
        status = SCE_THREADMAN_CONTEXT_STATUS_USER;
        thread->userSp = thread->callbackContext->gpr[29];
    } else {
        stack = thread->callbackContext->gpr[29];
        context->gpr[31] = (u32)sub_00002DBC;
        status = SCE_THREADMAN_CONTEXT_STATUS_KERNEL;
        thread->userSp = 0;
    }
    context->status = status;
    context->type = SCE_THREADMAN_CONTEXT_TYPE_FULL;
    context->gpr[29] = stack;
    context->gpr[30] = stack;
    context->gpr[26] = (u32)thread->k0;
    thread->fpuUsed = 0;
    thread->attr &= ~SCE_KERNEL_TH_USE_VFPU;
    threadmanCop0CtrlSet(COP0_CTRL_SP_USER, thread->userSp);
    threadmanCop0CtrlSet(COP0_CTRL_SP_KERNEL, (u32)context);
    InterruptManagerForKernel_551097F2(context);
    return 0;
}

// Subroutine sub_00002B3C - Address 0x00002B3C
static s32 _sceKernelThreadEventHandlerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00002B64 - Address 0x00002B64
static s32 _sceKernelThreadEventHandlerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap)
{
    s32 intr;
    s32 status;
    SceThreadmanThreadEventHandler *eventHandler;

    intr = sceKernelCpuSuspendIntr();
    eventHandler = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanThreadEventHandler);
    if (eventHandler->deleted) {
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD_EVENT_HANDLER;
    }

    /* Wait for the running calls of the handler to end. */
    eventHandler->deleted = 1;
    while (eventHandler->runCount > 0) {
        sub_00016FA4();
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_THREAD_EVENT_HANDLER, 0, NULL, NULL);
        if (status != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            return status;
        }
    }
    threadmanListRemove(&eventHandler->link);

    sceKernelCpuResumeIntr(intr);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

// Subroutine sub_00002C7C - Address 0x00002C7C
static s32 _sceKernelCallbackInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00002CA4 - Address 0x00002CA4
static s32 _sceKernelCallbackDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    s32 intr;
    s32 status;
    SceThreadmanCallback *callback;

    intr = sceKernelCpuSuspendIntr();
    callback = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanCallback);
    if (callback->deleted) {
        sceKernelCpuResumeIntr(intr);
        return SCE_ERROR_KERNEL_NOT_FOUND_CALLBACK;
    }

    /* Wait for the running calls of the callback to end. */
    callback->deleted = 1;
    while (callback->runCount > 0) {
        sub_00016FA4();
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_CALLBACK, 0, NULL, NULL);
        if (status != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            return status;
        }
    }
    threadmanListRemove(&callback->link);

    sceKernelCpuResumeIntr(intr);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
