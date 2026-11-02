/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/mbx.c
 *
 * Message boxes (original address range 0x00008650 - 0x00009468).
 *
 * A message box is a "Mbx" UID object which inherits the "WaitQ" type. The messages are user allocated
 * SceKernelMsgPacket headers chained in a circular list, the message box only keeping a pointer to the last
 * message.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid message box attributes. */
#define SCE_THREADMAN_MBX_ATTR_MASK     (0x5FF)

static s32 __attribute__((used)) _sceKernelReceiveMbx(SceUID mbxid, void **pmessage, SceUInt *timeout);
static s32 _sceKernelMbxAddMessage(SceThreadmanMbx *mbx, SceThreadmanWaitQ *waitQ, SceKernelMsgPacket *message);
static s32 _sceKernelMbxInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelMbxDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019D40
SceSysmemUidLookupFunc g_MbxFuncs[] = {
    { 0xD310D2D9, _sceKernelMbxInitialize },
    { 0x87089863, _sceKernelMbxDelete },
    { 0, NULL }
};

/*
 * PARITY-Q037 (cf293fa): UID attr | (k1 >> 31) passed to sceKernelCreateUID, as the original computes it.
 * PARITY-Q051 (17a1ee9): Access through a local/named pointer (the original's addressing).
 */
/**
 * Create a message box.
 *
 * @param name The name of the message box.
 * @param attr The attributes (SCE_KERNEL_MBA_*).
 * @param option The option parameter (can be NULL).
 *
 * @return The UID of the message box on success, < 0 on error.
 */
/*
 * PARITY-Q085: Creation failure returns through its own resume/return tail (success path dedented).
 * PARITY-Q042: uid->uid read between the option and attr stores.
 * PARITY-Q087: the new UID kept in its own `id` local instead of reusing the CreateUID status: the shorter status
 *     lifetime gives the original's register assignment (name/status in s0, option in fp).
 */
// Subroutine sceKernelCreateMbx - Address 0x00008650
SceUID sceKernelCreateMbx(const char *name, SceUInt attr, SceKernelMbxOptParam *option)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID id;
    SceSysmemUidCB *uid;
    SceThreadmanMbx *mbx;
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
    if ((attr & ~SCE_THREADMAN_MBX_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelCreateUID(g_threadman.mbxType, name, attr | (pspGetK1() >> 31), &uid);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }
    mbx = UID_CB_TO_DATA(uid, g_threadman.mbxType, SceThreadmanMbx);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    mbx->option = option;
    id = uid->uid;
    waitQ->attr = attr;
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_MBX, 1, id);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return id;
}

/**
 * Delete a message box. The threads waiting on it are released with SCE_ERROR_KERNEL_WAIT_DELETE.
 *
 * @param mbxid The UID of the message box.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteMbx - Address 0x000087A8
int sceKernelDeleteMbx(SceUID mbxid)
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

    if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_BOX;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_MBX, 1, mbxid);
    sceKernelDeleteUID(mbxid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q046 (93831a4): Object pointer derived before waitQ (the original loads the object's type word first).
 * PARITY-Q047 (ce8a24a): Interrupt state read raw / at the original's position.
 */
/**
 * Send a message to a message box. If a thread is waiting on the message box, the message is directly
 * given to it, otherwise it is queued.
 *
 * @param mbxid The UID of the message box.
 * @param message The message, starting with a SceKernelMsgPacket header.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSendMbx - Address 0x000088BC
int sceKernelSendMbx(SceUID mbxid, void *message)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 status;
    SceSysmemUidCB *uid;
    SceThreadmanMbx *mbx;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (!pspK1StaBufOk(message, sizeof(SceKernelMsgPacket))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_BOX;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    mbx = UID_CB_TO_DATA(uid, g_threadman.mbxType, SceThreadmanMbx);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    THREADMAN_LOG(SCE_THREADMAN_LOG_SEND_MBX, 2, mbxid, message);

    if (waitQ->numWaitThreads > 0) {
        /* Give the message to the first waiting thread. */
        thread = (SceThreadmanThread *)waitQ->waitList.next;
        if (thread->waitData[0] != 0)
            *(void **)thread->waitData[0] = message;
        sub_00000B08(uid, SCE_ERROR_OK, isIntr, intr);
    } else {
        status = _sceKernelMbxAddMessage(mbx, waitQ, message);
        if (status != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return status;
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Receive a message from a message box, waiting until one is available.
 *
 * @param mbxid The UID of the message box.
 * @param pmessage Receives the address of the message.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReceiveMbx - Address 0x00008A68
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelReceiveMbx, _sceKernelReceiveMbx, 0);

/**
 * Receive a message from a message box, waiting until one is available and running the callbacks notified
 * meanwhile.
 *
 * @param mbxid The UID of the message box.
 * @param pmessage Receives the address of the message.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReceiveMbxCB - Address 0x00008A70
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelReceiveMbxCB, _sceKernelReceiveMbx, 1);

/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q041 (455e483): Checks combined in one condition / merged identical error checks: the original shares one
 *     branch.
 * PARITY-Q065 (455e483): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
// Subroutine sub_00008A78 - Address 0x00008A78
static s32 __attribute__((used)) _sceKernelReceiveMbx(SceUID mbxid, void **pmessage, SceUInt *timeout)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    s32 status;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *uid;
    SceThreadmanThread *thread;
    SceThreadmanMbx *mbx;
    SceThreadmanWaitQ *waitQ;
    SceKernelMsgPacket *last;
    SceKernelMsgPacket *message;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(pmessage, 4) || !pspK1StaBufOk(timeout, 4)) {
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

    if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_BOX;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    mbx = UID_CB_TO_DATA(uid, g_threadman.mbxType, SceThreadmanMbx);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_RECEIVE_MBX_CB, 2, mbxid, pmessage);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_RECEIVE_MBX, 2, mbxid, pmessage);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the message box. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        if (mbx->numMessages > 0)
            goto receive;
        thread->waitData[0] = (s32)pmessage;
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_MBX, waitQ->attr, &timeoutClock, timeout);
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

end:
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;

receive:

    /* Take the first message. */
    last = mbx->lastMessage;
    message = last->next;
    if (message == NULL || !pspK1PtrOk(message) || message->next == NULL || !pspK1PtrOk(message->next)) {
        status = SCE_ERROR_KERNEL_ILLEGAL_ADDR;
        goto end;
    }
    if (message == message->next) {
        if (mbx->numMessages != 1) {
            status = SCE_ERROR_KERNEL_MESSAGEBOX_DUPLICATE_MESSAGE;
            goto end;
        }
        mbx->lastMessage = NULL;
    } else
        last->next = message->next;
    mbx->numMessages--;
    THREADMAN_LOG(SCE_THREADMAN_LOG_MESSAGE, 2, " recvmsg = 0x%x", message);
    *pmessage = message;
    status = SCE_ERROR_OK;
    goto end;
}

/**
 * Receive a message from a message box without waiting.
 *
 * @param mbxid The UID of the message box.
 * @param pmessage Receives the address of the message.
 *
 * @return 0 on success, SCE_ERROR_KERNEL_MESSAGEBOX_NO_MESSAGE if the message box is empty, < 0 on error.
 */
/*
 * PARITY-Q183: The message is taken inside `if (mbx->numMessages > 0)`, with the empty-box path after it. The
 *     taking path then precedes the "nomsg" block in the instruction stream, and register renaming (which picks the
 *     least recently used register, block by block in stream order) gives the original's registers.
 */
// Subroutine sceKernelPollMbx - Address 0x00008DC0
int sceKernelPollMbx(SceUID mbxid, void **pmessage)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanMbx *mbx;
    /* PARITY-Q187 (trick): local register variable: `last` lives in $a1 as in the original (GCC 3.3's allocator picks
     * $a2 for it); it forces the register only, no code is added. */
    register SceKernelMsgPacket *last asm("$5");
    SceKernelMsgPacket *message;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(pmessage, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_BOX;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    mbx = UID_CB_TO_DATA(uid, g_threadman.mbxType, SceThreadmanMbx);
    THREADMAN_LOG(SCE_THREADMAN_LOG_POLL_MBX, 2, mbxid, pmessage);

    if (mbx->numMessages > 0) { /* PARITY-Q183 */
        /* Take the first message. */
        last = mbx->lastMessage;
        message = last->next;
        if (message == NULL || !pspK1PtrOk(message) || message->next == NULL || !pspK1PtrOk(message->next)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
        }
        if (message == message->next) {
            if (mbx->numMessages != 1) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_MESSAGEBOX_DUPLICATE_MESSAGE;
            }
            mbx->lastMessage = NULL;
        } else
            last->next = message->next;
        mbx->numMessages--;
        THREADMAN_LOG(SCE_THREADMAN_LOG_MESSAGE, 2, " recvmsg = 0x%x", message);
        *pmessage = message;

        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_OK;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_MESSAGE, 1, "sceKernelPollMbx nomsg");
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_KERNEL_MESSAGEBOX_NO_MESSAGE;
}

/**
 * Release all the threads waiting on a message box (their wait returns SCE_ERROR_KERNEL_WAIT_CANCELLED).
 *
 * @param mbxid The UID of the message box.
 * @param pnum Receives the number of released threads (can be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelReceiveMbx - Address 0x00008FF0
int sceKernelCancelReceiveMbx(SceUID mbxid, int *pnum)
{
    s32 oldK1;
    s32 intr;
    s32 numCancelled;
    SceSysmemUidCB *uid;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(pnum, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_BOX;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_RECEIVE_MBX, 2, mbxid, pnum);

    numCancelled = sceKernelCallUIDObjFunction(uid, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    if (pnum != NULL)
        *pnum = numCancelled;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a message box.
 *
 * @param mbxid The UID of the message box.
 * @param info Receives the status (info->size must be set).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferMbxStatus - Address 0x00009128
int sceKernelReferMbxStatus(SceUID mbxid, SceKernelMbxInfo *info)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanMbx *mbx;
    SceThreadmanWaitQ *waitQ;
    SceKernelMbxInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(mbxid, g_threadman.mbxType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_BOX;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_MBX_STATUS, 2, mbxid, info);

    mbx = UID_CB_TO_DATA(uid, g_threadman.mbxType, SceThreadmanMbx);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.mbxInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (uid->name != NULL)
        strncpy(buf->name, uid->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    buf->numWaitThreads = waitQ->numWaitThreads;
    buf->numMessages = mbx->numMessages;
    if (mbx->lastMessage != NULL)
        buf->firstMessage = mbx->lastMessage->next;
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.mbxInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Register the "Mbx" UID type.
 *
 * @return 0.
 */
// Subroutine sub_000092F4 - Address 0x000092F4
s32 _sceKernelMbxInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Mbx", 0x30, g_MbxFuncs, NULL, &g_threadman.mbxType);
    return SCE_ERROR_OK;
}

/*
 * Queue a message: at the end of the list (SCE_KERNEL_MBA_MSFIFO), or after the messages with a lower or
 * equal priority (SCE_KERNEL_MBA_MSPRI).
 */
/*
 * PARITY-Q054 (96277f9): Comparison operand order as the original's branch operands.
 * PARITY-Q081 (b232876): Priority scan written as a label/goto loop, not for (;;) (the original's block layout,
 *     no loop pass rotation).
 */
// Subroutine sub_00009338 - Address 0x00009338
static s32 _sceKernelMbxAddMessage(SceThreadmanMbx *mbx, SceThreadmanWaitQ *waitQ, SceKernelMsgPacket *message)
{
    SceKernelMsgPacket *last;
    SceKernelMsgPacket *prev;
    SceKernelMsgPacket *cur;

    if (mbx->lastMessage == NULL) {
        mbx->lastMessage = message;
        message->next = message;
    } else {
        last = mbx->lastMessage;
        if (last == message)
            return SCE_ERROR_KERNEL_MESSAGEBOX_DUPLICATE_MESSAGE;
        if ((waitQ->attr & SCE_KERNEL_MBA_MSPRI) == 0) {
            cur = last->next;
            if (cur == NULL || !pspK1PtrOk(cur))
                return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
            if (cur == message)
                return SCE_ERROR_KERNEL_MESSAGEBOX_DUPLICATE_MESSAGE;
            message->next = cur;
            mbx->lastMessage->next = message;
            mbx->lastMessage = message;
        } else {
            prev = last;
next:
            cur = prev->next;
            if (cur == message)
                return SCE_ERROR_KERNEL_MESSAGEBOX_DUPLICATE_MESSAGE;
            if (cur == NULL || !pspK1PtrOk(cur))
                return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
            if (cur->msgPriority <= message->msgPriority) {
                prev = cur;
                if (cur != last)
                    goto next;
                /* The new message has the highest priority value: it becomes the last one. */
                mbx->lastMessage = message;
                cur = cur->next;
            }
            if (!pspK1PtrOk(cur))
                return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
            message->next = cur;
            prev->next = message;
        }
    }
    mbx->numMessages++;
    return SCE_ERROR_OK;
}

// Subroutine sub_00009420 - Address 0x00009420
static s32 _sceKernelMbxInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00009448 - Address 0x00009448
static s32 _sceKernelMbxDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
