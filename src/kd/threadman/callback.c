/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/callback.c
 *
 * Callbacks (original address ranges 0x000019D8 - 0x00002570, 0x000026CC - 0x00002B3C and
 * 0x00002C7C - 0x00002DBC; 0x00002DBC is in callback_ret.S).
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

static s32 _sceKernelCallbackInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelCallbackDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019B14
SceSysmemUidLookupFunc g_CallbackFuncs[] = {
    { 0xD310D2D9, _sceKernelCallbackInitialize },
    { 0x87089863, _sceKernelCallbackDelete },
    { 0, NULL }
};

/* Unlink a deleted callback whose last running call has ended (its link is made empty). */
static inline void _sceKernelCallbackUnlink(SceThreadmanCallback *callback)
{
    threadmanListRemove(&callback->link);
    callback->link.prev = &callback->link;
    callback->link.next = &callback->link;
}

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

    callback = UID_CB_TO_DATA(uid, g_threadman.callbackType, SceThreadmanCallback);
    callback->userMode = pspK1IsUserMode();
    callback->uid = uid->uid;
    callback->thread = thread;
    callback->notifyCount = 0;
    callback->notifyArg = 0;
    callback->func = func;
    callback->common = common;
    callback->gp = sceKernelGetModuleGPByAddressForKernel((u32)func);
    if (callback->gp == 0)
        callback->gp = pspGetGp();
    threadmanListAddTail(&thread->callbacks, &callback->link);

    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_CALLBACK, 3, func, common, uid->uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return uid->uid;
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

    callback->notifyArg = 0;
    callback->notifyCount = 0;

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
    ret = 0;
    thread = g_threadman.curThread;
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

    numCalled = 0;
    thread = g_threadman.curThread;
    numWokenUp = 0;
    thread->callbackNotified = 0;
    for (entry = thread->callbacks.next; entry != &thread->callbacks; entry = next) {
        callback = (SceThreadmanCallback *)entry;
        uid = THREADMAN_UID_DATA_TO_CB(callback, g_threadman.callbackType);
        notifyCount = callback->notifyCount;
        if (notifyCount == 0 || callback->deleted) {
            next = entry->next;
            continue;
        }

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
    u32 *ptr;
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
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->threadId = callback->thread->uid;
    buf->callback = callback->func;
    buf->notifyCount = callback->notifyCount;
    buf->common = callback->common;
    buf->notifyArg = callback->notifyArg;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

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
    thread->waitStatus = thread->syscallFrame->v0;
    return sub_000028C0();
}

/* sub_00002570 and sub_000025FC are implemented in threadevent.c. */

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
    thread->callbackDepth = 0;
    thread->callbackNotified = 0;
    thread->curCallback = NULL;
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

// Subroutine sub_000028C0 - Address 0x000028C0
/* Return from the current callback (or thread event handler) to the context which called it. */
s32 sub_000028C0(void)
{
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    if (THREADMAN_IS_INTR_CONTEXT() || thread->callbackContext == NULL) {
        for (;;)
            pspBreak(0x3FF);
    }

    /* Restore the VFPU state of the returned to context. */
    if (thread->callbackContext->status & SCE_THREADMAN_CONTEXT_STATUS_CU2) {
        thread->attr |= SCE_KERNEL_TH_USE_VFPU;
        sub_00012C04(thread);
    } else
        thread->attr &= ~SCE_KERNEL_TH_USE_VFPU;
    InterruptManagerForKernel_551097F2(thread->callbackContext);
    return SCE_ERROR_OK;
}

// Subroutine sub_00002944 - Address 0x00002944
/*
 * Call func(arg0, arg1, arg2, arg3) on the current thread with $gp set to gp, in user mode if userMode is
 * set. Return the result of func, set by _sceKernelReturnFromCallback() (user mode) or sub_00002DBC()
 * (kernel mode) which both resume the context saved here through sub_000028C0().
 */
s32 sub_00002944(s32 userMode, void *func, u32 arg0, u32 arg1, u32 arg2, u32 arg3, u32 gp)
{
    u8 contextBuf[sizeof(SceThreadmanContext) + 63];
    u32 stack;
    u32 status;
    u32 savedUserSp;
    u32 savedUnk24;
    SceThreadmanSyscallFrame *savedSyscallFrame;
    SceThreadmanContext *savedCallbackContext;
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
    savedUserSp = thread->userSp;
    savedSyscallFrame = thread->syscallFrame;
    savedCallbackContext = thread->callbackContext;
    savedUnk24 = thread->unk24;
    sub_00016FA4();
    thread->callbackContext = thread->context;
    if (SaveThreadContext(thread->context) != 0) {
        /* The callback has returned (through sub_000028C0()). */
        callerContext = thread->callbackContext;
        thread->userSp = savedUserSp;
        thread->syscallFrame = savedSyscallFrame;
        thread->callbackContext = savedCallbackContext;
        thread->unk24 = savedUnk24;
        thread->context = callerContext;
        if (pspCop0CtrlGet(COP0_CTRL_EXC_TABLE) == (s32)context)
            pspCop0CtrlSet(COP0_CTRL_EXC_TABLE, 0);
        pspCop0CtrlSet(COP0_CTRL_SP_KERNEL, (u32)callerContext);
        pspCop0CtrlSet(COP0_CTRL_SP_USER, thread->userSp);
        return thread->waitStatus;
    }

    thread->context = context;
    if (userMode) {
        /*
         * Build a "syscall _sceKernelReturnFromCallback" instruction on the user stack, used as the
         * return address of the callback.
         */
        stack = (thread->syscallFrame != NULL) ? thread->syscallFrame->sp : thread->unk200;
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
    context->gpr[30] = stack;
    context->gpr[26] = (u32)thread->k0;
    context->gpr[29] = stack;
    thread->unk24 = 0;
    thread->attr &= ~SCE_KERNEL_TH_USE_VFPU;
    pspCop0CtrlSet(COP0_CTRL_SP_USER, thread->userSp);
    pspCop0CtrlSet(COP0_CTRL_SP_KERNEL, (u32)context);
    InterruptManagerForKernel_551097F2(context);
    return 0;
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
