/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/threadevent.c
 *
 * Thread event handlers (original address ranges 0x00001140 - 0x000019D8, 0x00002570 - 0x000026CC
 * and 0x00002B3C - 0x00002C7C).
 *
 * A thread event handler is a "ThreadEventHandler" UID object (inheriting the "WaitQ" type) which is
 * called when a thread is created, started, exits or is deleted. It monitors a single thread or all
 * the kernel and/or user threads.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <loadcore.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

static s32 _sceKernelThreadEventHandlerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap);
static s32 _sceKernelThreadEventHandlerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap);

// 0x00019AFC
SceSysmemUidLookupFunc g_ThreadEventHandlerFuncs[] = {
    { 0xD310D2D9, _sceKernelThreadEventHandlerInitialize },
    { 0x87089863, _sceKernelThreadEventHandlerDelete },
    { 0, NULL }
};

/* Unlink a deleted handler whose last running call has ended (its link is made empty). */
static inline void _sceKernelThreadEventHandlerUnlink(SceThreadmanThreadEventHandler *handler)
{
    threadmanListRemove(&handler->link);
    handler->link.prev = &handler->link;
    handler->link.next = &handler->link;
}

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
        if (!THREADMAN_UID_PERM_OK(threadUid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
        }
        thread = UID_CB_TO_DATA(threadUid, g_threadman.threadType, SceThreadmanThread);
        if (pspK1IsUserMode() && THREADMAN_THREAD_MODE(thread) != THREADMAN_THREAD_MODE(g_threadman.curThread)) {
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

    eventHandler = UID_CB_TO_DATA(uid, g_threadman.threadEventHandlerType, SceThreadmanThreadEventHandler);
    eventHandler->thread = thread;
    eventHandler->userMode = pspK1IsUserMode();
    eventHandler->uid = uid->uid;
    eventHandler->mask = mask;
    eventHandler->handler = handler;
    eventHandler->common = common;
    eventHandler->gp = sceKernelGetModuleGPByAddressForKernel((u32)handler);
    if (eventHandler->gp == 0)
        eventHandler->gp = pspGetGp();
    threadmanListAddTail(&g_threadman.threadEventHandlers, &eventHandler->link);

    THREADMAN_LOG(SCE_THREADMAN_LOG_REGISTER_THREAD_EVENT_HANDLER, 5, threadID, mask, handler, common, uid->uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return uid->uid;
}

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
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT() || g_threadman.curThread->threadEventMask != 0) {
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
            next = entry->next;
            continue;
        }
        if (eventHandler->thread != (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_ALL
            && (THREADMAN_IS_USER_THREAD(thread)
                ? (eventHandler->thread != (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_USER
                    || !THREADMAN_IS_USER_THREAD(g_threadman.curThread))
                : eventHandler->thread != (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_KERN)
            && eventHandler->thread != thread) {
            next = entry->next;
            continue;
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
    u32 *ptr;
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
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    if (eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_KERN
        || eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_USER
        || eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_ALL)
        buf->threadId = (SceUID)eventHandler->thread;
    else
        buf->threadId = eventHandler->thread->uid;
    buf->common = eventHandler->common;
    buf->mask = eventHandler->mask;
    buf->handler = eventHandler->handler;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

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
    thread->threadEventHandler = NULL;
    thread->threadEventMask = 0;
}

// Subroutine sub_000025FC - Address 0x000025FC
/* Return 1 if a thread event handler would be called for the thread event event on thread, 0 otherwise. */
s32 sub_000025FC(u32 event, SceThreadmanThread *thread)
{
    SceThreadmanThread *curThread;
    SceThreadmanListEntry *entry;
    SceThreadmanThreadEventHandler *eventHandler;

    curThread = g_threadman.curThread;
    for (entry = g_threadman.threadEventHandlers.next; entry != &g_threadman.threadEventHandlers;
        entry = entry->next) {
        eventHandler = (SceThreadmanThreadEventHandler *)entry;
        if (!(event & eventHandler->mask) || eventHandler->deleted
            || (THREADMAN_IS_USER_THREAD(curThread) && THREADMAN_THREAD_MODE(curThread) != THREADMAN_THREAD_MODE(thread)))
            continue;
        if (eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_ALL
            || (THREADMAN_IS_USER_THREAD(thread)
                ? (eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_USER
                    && THREADMAN_IS_USER_THREAD(curThread))
                : eventHandler->thread == (SceThreadmanThread *)SCE_KERNEL_THREAD_EVENT_ID_KERN)
            || eventHandler->thread == thread)
            return 1;
    }
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
