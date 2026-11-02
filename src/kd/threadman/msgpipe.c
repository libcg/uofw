/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/msgpipe.c
 *
 * Message pipes (original address range 0x00009468 - 0x0000B44C).
 *
 * A message pipe is a "MsgPipe" UID object which inherits the "WaitQ" type. The data is stored in an
 * optional ring buffer allocated in a memory partition. The threads waiting on a message pipe are either all
 * senders or all receivers (see SceThreadmanMsgPipe.state), and data is directly copied between the
 * sending and the receiving threads when possible.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid message pipe attributes. */
#define SCE_THREADMAN_MSG_PIPE_ATTR_MASK    (0x51FF)

/** The wait data of a thread waiting on a message pipe. */
typedef struct {
    /** The address of the data still to send or the buffer still to fill. */
    u8 *buf; // 96
    /** The number of bytes still to send or receive. */
    SceSize remaining; // 100
    /** The number of bytes requested. */
    SceSize size; // 104
    /** Receives the number of bytes sent or received (can be NULL). */
    int *result; // 108
    /** The wait mode (SCE_KERNEL_MPW_*). */
    s32 waitMode; // 112
    /** 1 for a receiving thread, 0 for a sending thread. */
    s32 isReceiving; // 116
} SceThreadmanMsgPipeWaitData;

#define MSG_PIPE_WAIT_DATA(thread) ((SceThreadmanMsgPipeWaitData *)(thread)->waitData)

static s32 _sceKernelSendMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result,
    SceUInt *timeout, s32 withCallbacks);
static s32 _sceKernelReceiveMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result,
    SceUInt *timeout, s32 withCallbacks);
static void _sceKernelMsgPipeWrite(SceThreadmanMsgPipe *msgPipe, const u8 *data, SceSize size);
static void _sceKernelMsgPipeRead(SceThreadmanMsgPipe *msgPipe, u8 *data, SceSize size);
static s32 _sceKernelMsgPipeInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelMsgPipeDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelMsgPipeWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId,
    va_list ap);
static s32 _sceKernelMsgPipeEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019D58
SceSysmemUidLookupFunc g_MsgPipeFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_END_WAIT, _sceKernelMsgPipeEndWait },
    { SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS, _sceKernelMsgPipeWakeupWaiters },
    { 0xD310D2D9, _sceKernelMsgPipeInitialize },
    { 0x87089863, _sceKernelMsgPipeDelete },
    { 0, NULL }
};

/**
 * Create a message pipe.
 *
 * @param name The name of the message pipe.
 * @param part The memory partition to allocate the buffer from.
 * @param attr The attributes (SCE_KERNEL_MPA_*).
 * @param bufSize The size of the buffer (0 for no buffer: the data is only transferred between threads).
 * @param option The option parameter (can be NULL).
 *
 * @return The UID of the message pipe on success, < 0 on error.
 */
// Subroutine sceKernelCreateMsgPipe - Address 0x00009468
SceUID sceKernelCreateMsgPipe(const char *name, int part, int attr, SceSize bufSize,
    SceKernelMppOptParam *option)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID blockId;
    u8 *buf;
    SceSysmemUidCB *uid;
    SceThreadmanMsgPipe *msgPipe;
    SceSysmemPartitionInfo partInfo;

    buf = NULL;
    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(option, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_MSG_PIPE_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }

    intr = sceKernelCpuSuspendIntr();

    partInfo.size = sizeof(partInfo);
    status = sceKernelQueryMemoryPartitionInfo(part, &partInfo);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }
    /* User threads can only use partitions accessible from user mode. */
    if (pspK1IsUserMode() && (partInfo.attr & 3) != 3) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    blockId = 0;
    if (bufSize != 0) {
        blockId = sceKernelAllocPartitionMemory(part, (char *)name,
            (attr >> 14) & 1, bufSize, 0); /* SCE_KERNEL_MPA_HIGHMEM: allocate from the top of the partition */
        if (blockId <= 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_NO_MEMORY;
        }
        buf = sceKernelGetBlockHeadAddr(blockId);
    }

    status = sceKernelCreateUID(g_threadman.msgPipeType, name, (pspGetK1() >> 31) | attr, &uid);
    if (status != SCE_ERROR_OK) {
        if (blockId > 0)
            sceKernelFreePartitionMemory(blockId);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    msgPipe = UID_CB_TO_DATA(uid, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    msgPipe->option = option;
    msgPipe->buf = buf;
    msgPipe->readPtr = buf;
    UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ)->attr = attr;
    msgPipe->writePtr = buf;
    msgPipe->blockId = blockId;
    msgPipe->bufSize = bufSize;
    msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_EMPTY;
    msgPipe->used = 0;
    status = uid->uid;
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_MSG_PIPE, 1, status);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Delete a message pipe. The threads waiting on it are released with SCE_ERROR_KERNEL_WAIT_DELETE.
 *
 * @param uid The UID of the message pipe.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteMsgPipe - Address 0x000096BC
int sceKernelDeleteMsgPipe(SceUID uid)
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_MSG_PIPE, 1, uid);
    sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Send data to a message pipe, waiting until it can be sent.
 *
 * @param uid The UID of the message pipe.
 * @param message The data to send.
 * @param size The size of the data.
 * @param waitMode SCE_KERNEL_MPW_FULL to wait until all the data is sent, SCE_KERNEL_MPW_ASAP to wait until
 * a part of it is sent.
 * @param result Receives the number of bytes sent (can be NULL).
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSendMsgPipe - Address 0x000097D0
int sceKernelSendMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout)
{
    return _sceKernelSendMsgPipe(uid, message, size, waitMode, result, timeout, 0);
}

/**
 * Send data to a message pipe, waiting until it can be sent and running the callbacks notified meanwhile.
 *
 * @param uid The UID of the message pipe.
 * @param message The data to send.
 * @param size The size of the data.
 * @param waitMode SCE_KERNEL_MPW_FULL to wait until all the data is sent, SCE_KERNEL_MPW_ASAP to wait until
 * a part of it is sent.
 * @param result Receives the number of bytes sent (can be NULL).
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelSendMsgPipeCB - Address 0x000097D8
int sceKernelSendMsgPipeCB(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout)
{
    return _sceKernelSendMsgPipe(uid, message, size, waitMode, result, timeout, 1);
}

/*
 * Give data to the receiving threads waiting on a message pipe, in their wait order, until there is no data
 * left or no receiving thread left. A receiver is woken up once its buffer is full, or at once if it waits
 * with SCE_KERNEL_MPW_ASAP. Updates the data pointer and size, and returns the number of threads
 * to reschedule.
 */
static inline s32 _sceKernelMsgPipeGiveToReceivers(SceThreadmanMsgPipe *msgPipe, SceThreadmanWaitQ *waitQ,
    u8 **pdata, SceSize *psize)
{
    s32 numWokenUp;
    SceSize n;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;
    SceThreadmanMsgPipeWaitData *waitData;

    numWokenUp = 0;
    thread = (SceThreadmanThread *)waitQ->waitList.next;
    while (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING && waitQ->numWaitThreads > 0) {
        next = (SceThreadmanThread *)thread->link.next;
        waitData = MSG_PIPE_WAIT_DATA(thread);
        n = (waitData->remaining < *psize) ? waitData->remaining : *psize;
        memcpy(waitData->buf, *pdata, n);
        waitData->buf += n;
        waitData->remaining -= n;
        *psize -= n;
        *pdata += n;
        if (waitData->result != NULL)
            *waitData->result = waitData->size - waitData->remaining;
        if (waitData->waitMode == SCE_KERNEL_MPW_ASAP || waitData->remaining == 0) {
            waitQ->unk4 = 0;
            numWokenUp += sub_00000924(thread);
        } else
            waitQ->unk4 = 1;
        if (*psize == 0)
            break;
        thread = next;
    }
    return numWokenUp;
}

/*
 * Take data from the sending threads waiting on a message pipe, in their wait order: their data is copied
 * to the receiver's buffer and, once it is full, to the message pipe buffer. Updates the buffer pointer and
 * size, and returns the number of threads to reschedule.
 */
static inline s32 _sceKernelMsgPipeTakeFromSenders(SceThreadmanMsgPipe *msgPipe, SceThreadmanWaitQ *waitQ,
    u8 **pbuf, SceSize *psize)
{
    s32 numWokenUp;
    s32 copied;
    SceSize n;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;
    SceThreadmanMsgPipeWaitData *waitData;

    numWokenUp = 0;
    thread = (SceThreadmanThread *)waitQ->waitList.next;
    if (msgPipe->state != SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING || waitQ->numWaitThreads <= 0)
        return 0;
    for (;;) {
        next = (SceThreadmanThread *)thread->link.next;
        waitData = MSG_PIPE_WAIT_DATA(thread);
        copied = 0;
        if (*psize != 0) {
            n = (waitData->remaining < *psize) ? waitData->remaining : *psize;
            memcpy(*pbuf, waitData->buf, n);
            *pbuf += n;
            waitData->buf += n;
            waitData->remaining -= n;
            *psize -= n;
            copied = 1;
        }
        n = msgPipe->bufSize - msgPipe->used;
        if (waitData->remaining < n)
            n = waitData->remaining;
        if ((waitData->waitMode == SCE_KERNEL_MPW_ASAP && n != 0)
            || (waitData->waitMode == SCE_KERNEL_MPW_FULL && n == waitData->remaining)) {
            _sceKernelMsgPipeWrite(msgPipe, waitData->buf, n);
            waitData->remaining -= n;
            copied = 1;
        }
        if (waitData->result != NULL)
            *waitData->result = waitData->size - waitData->remaining;
        if ((waitData->waitMode != SCE_KERNEL_MPW_ASAP || !copied) && waitData->remaining != 0) {
            waitQ->unk4 = 1;
            break;
        }
        waitQ->unk4 = 0;
        numWokenUp += sub_00000924(thread);
        if (msgPipe->state != SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING || waitQ->numWaitThreads <= 0)
            break;
        thread = next;
    }
    return numWokenUp;
}

/*
 * Common part of sceKernelSendMsgPipe() and sceKernelSendMsgPipeCB().
 * Note: the original passes withCallbacks in $t3 (an unused $t2 argument).
 */
// Subroutine sub_000097E0 - Address 0x000097E0
static s32 _sceKernelSendMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result,
    SceUInt *timeout, s32 withCallbacks)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 numWokenUp;
    s32 mustWait;
    SceSize n;
    SceSize origSize;
    u8 *data;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanMsgPipeWaitData *waitData;

    data = message;
    origSize = size;
    oldK1 = pspShiftK1();
    if (!pspK1DynBufOk(data, size) || !pspK1StaBufOk(result, 4) || !pspK1StaBufOk(timeout, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((waitMode & ~SCE_KERNEL_MPW_ASAP) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    msgPipe = UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    if (msgPipe->bufSize != 0 && msgPipe->bufSize < size) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_SIZE;
    }
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_SEND_MSG_PIPE_CB, 4, uid, data, size, waitMode);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_SEND_MSG_PIPE, 4, uid, data, size, waitMode);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the message pipe. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        numWokenUp = 0;
        mustWait = 0;
        if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING) {
            /* Only overtake the waiting senders when they wait by priority and ours is higher. */
            if ((waitQ->attr & SCE_KERNEL_MPA_THPRI_S) == 0 || waitQ->unk4 != 0
                || thread->currentPriority >= ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority)
                mustWait = 1;
        }
        if (!mustWait) {
            if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
                numWokenUp = _sceKernelMsgPipeGiveToReceivers(msgPipe, waitQ, &data, &size);
            if (size != 0) {
                n = msgPipe->bufSize - msgPipe->used;
                if (size < n)
                    n = size;
                if ((waitMode == SCE_KERNEL_MPW_ASAP && n != 0) || (waitMode == SCE_KERNEL_MPW_FULL && n == size)) {
                    _sceKernelMsgPipeWrite(msgPipe, data, n);
                    size -= n;
                    data += n;
                }
            }
        }
        if (result != NULL)
            *result = origSize - size;
        if ((waitMode != SCE_KERNEL_MPW_ASAP || origSize != size) && (waitMode != SCE_KERNEL_MPW_FULL || size == 0)) {
            status = SCE_ERROR_OK;
            break;
        }

        msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING;
        numWokenUp = 0;
        waitData = MSG_PIPE_WAIT_DATA(thread);
        waitData->buf = data;
        waitData->remaining = size;
        waitData->result = result;
        waitData->size = origSize;
        waitData->waitMode = waitMode;
        waitData->isReceiving = 0;
        if (!mustWait)
            waitQ->unk4 = (origSize != size);
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_MSG_PIPE, waitQ->attr & SCE_KERNEL_MPA_THPRI_S,
            &timeoutClock, timeout);
        data = waitData->buf;
        size = waitData->remaining;
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Send data to a message pipe without waiting.
 *
 * @param uid The UID of the message pipe.
 * @param message The data to send.
 * @param size The size of the data.
 * @param waitMode SCE_KERNEL_MPW_FULL to only send all the data, SCE_KERNEL_MPW_ASAP to send a part of it.
 * @param result Receives the number of bytes sent (can be NULL).
 *
 * @return 0 on success, SCE_ERROR_KERNEL_MESSAGE_PIPE_FULL if the data cannot be sent, < 0 on error.
 */
// Subroutine sceKernelTrySendMsgPipe - Address 0x00009D7C
int sceKernelTrySendMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 numWokenUp;
    SceSize n;
    SceSize origSize;
    u8 *data;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;

    data = message;
    origSize = size;
    oldK1 = pspShiftK1();
    if (!pspK1DynBufOk(data, size) || !pspK1StaBufOk(result, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((waitMode & ~SCE_KERNEL_MPW_ASAP) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    isIntr = THREADMAN_IS_INTR_CONTEXT();
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    msgPipe = UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    if (msgPipe->bufSize != 0 && msgPipe->bufSize < size) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_SIZE;
    }
    THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_SEND_MSG_PIPE, 4, uid, data, size, waitMode);

    if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING) {
        /* Only overtake the waiting senders when they wait by priority and ours is higher. */
        if ((waitQ->attr & SCE_KERNEL_MPA_THPRI_S) == 0 || waitQ->unk4 != 0
            || g_threadman.curThread->currentPriority
                >= ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MESSAGE_PIPE_FULL;
        }
    }
    if (waitMode == SCE_KERNEL_MPW_FULL) {
        /* All the data must fit in the buffer and the waiting receivers. */
        n = msgPipe->bufSize - msgPipe->used;
        if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING) {
            for (thread = (SceThreadmanThread *)waitQ->waitList.next;
                 thread != (SceThreadmanThread *)&waitQ->waitList; thread = (SceThreadmanThread *)thread->link.next)
                n += MSG_PIPE_WAIT_DATA(thread)->remaining;
        }
        if (n < size) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MESSAGE_PIPE_FULL;
        }
    }

    numWokenUp = 0;
    if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
        numWokenUp = _sceKernelMsgPipeGiveToReceivers(msgPipe, waitQ, &data, &size);
    if (size != 0) {
        n = msgPipe->bufSize - msgPipe->used;
        if (size < n)
            n = size;
        if ((waitMode == SCE_KERNEL_MPW_ASAP && n != 0) || (waitMode == SCE_KERNEL_MPW_FULL && n == size)) {
            _sceKernelMsgPipeWrite(msgPipe, data, n);
            size -= n;
        }
    }
    if (result != NULL)
        *result = origSize - size;
    /* Note: the threads woken up above are not rescheduled on failure. */
    if ((waitMode == SCE_KERNEL_MPW_ASAP && origSize == size) || (waitMode == SCE_KERNEL_MPW_FULL && size != 0)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MESSAGE_PIPE_FULL;
    }

    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        if (!isIntr)
            sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Receive data from a message pipe, waiting until it is available.
 *
 * @param uid The UID of the message pipe.
 * @param message The buffer receiving the data.
 * @param size The size of the buffer.
 * @param waitMode SCE_KERNEL_MPW_FULL to wait until the buffer is full, SCE_KERNEL_MPW_ASAP to wait until
 * some data is received.
 * @param result Receives the number of bytes received (can be NULL).
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReceiveMsgPipe - Address 0x0000A1A4
int sceKernelReceiveMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout)
{
    return _sceKernelReceiveMsgPipe(uid, message, size, waitMode, result, timeout, 0);
}

/**
 * Receive data from a message pipe, waiting until it is available and running the callbacks notified
 * meanwhile.
 *
 * @param uid The UID of the message pipe.
 * @param message The buffer receiving the data.
 * @param size The size of the buffer.
 * @param waitMode SCE_KERNEL_MPW_FULL to wait until the buffer is full, SCE_KERNEL_MPW_ASAP to wait until
 * some data is received.
 * @param result Receives the number of bytes received (can be NULL).
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReceiveMsgPipeCB - Address 0x0000A1AC
int sceKernelReceiveMsgPipeCB(SceUID uid, void *message, SceSize size, int waitMode, int *result,
    SceUInt *timeout)
{
    return _sceKernelReceiveMsgPipe(uid, message, size, waitMode, result, timeout, 1);
}

/*
 * Common part of sceKernelReceiveMsgPipe() and sceKernelReceiveMsgPipeCB().
 * Note: the original passes withCallbacks in $t3 (an unused $t2 argument).
 */
// Subroutine sub_0000A1B4 - Address 0x0000A1B4
static s32 _sceKernelReceiveMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result,
    SceUInt *timeout, s32 withCallbacks)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 numWokenUp;
    s32 mustWait;
    SceSize n;
    SceSize origSize;
    u8 *buf;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanMsgPipeWaitData *waitData;

    buf = message;
    origSize = size;
    oldK1 = pspShiftK1();
    if (!pspK1DynBufOk(buf, size) || !pspK1StaBufOk(result, 4) || !pspK1StaBufOk(timeout, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((waitMode & ~SCE_KERNEL_MPW_ASAP) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    msgPipe = UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    if (msgPipe->bufSize != 0 && msgPipe->bufSize < size) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_SIZE;
    }
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_RECEIVE_MSG_PIPE_CB, 4, uid, buf, size, waitMode);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_RECEIVE_MSG_PIPE, 4, uid, buf, size, waitMode);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the message pipe. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        numWokenUp = 0;
        mustWait = 0;
        if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING) {
            /* Only overtake the waiting receivers when they wait by priority and ours is higher. */
            if ((waitQ->attr & SCE_KERNEL_MPA_THPRI_R) == 0 || waitQ->unk4 != 0
                || thread->currentPriority >= ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority)
                mustWait = 1;
        }
        if (!mustWait) {
            if (msgPipe->used != 0) {
                n = (msgPipe->used < size) ? msgPipe->used : size;
                _sceKernelMsgPipeRead(msgPipe, buf, n);
                size -= n;
                buf += n;
            }
            numWokenUp = _sceKernelMsgPipeTakeFromSenders(msgPipe, waitQ, &buf, &size);
        }
        if (result != NULL)
            *result = origSize - size;
        if ((waitMode != SCE_KERNEL_MPW_ASAP || origSize != size) && (waitMode != SCE_KERNEL_MPW_FULL || size == 0)) {
            status = SCE_ERROR_OK;
            break;
        }

        msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING;
        numWokenUp = 0;
        waitData = MSG_PIPE_WAIT_DATA(thread);
        waitData->buf = buf;
        waitData->remaining = size;
        waitData->size = origSize;
        waitData->result = result;
        waitData->isReceiving = 1;
        waitData->waitMode = waitMode;
        if (!mustWait)
            waitQ->unk4 = (origSize != size);
        thread->waitWithCallbacks = withCallbacks;
        /* The receivers wait in priority order with SCE_KERNEL_MPA_THPRI_R. */
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_MSG_PIPE, (waitQ->attr >> 4) & SCE_KERNEL_MPA_THPRI_S,
            &timeoutClock, timeout);
        buf = waitData->buf;
        size = waitData->remaining;
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Receive data from a message pipe without waiting.
 *
 * @param uid The UID of the message pipe.
 * @param message The buffer receiving the data.
 * @param size The size of the buffer.
 * @param waitMode SCE_KERNEL_MPW_FULL to only fill the whole buffer, SCE_KERNEL_MPW_ASAP to receive the
 * available data.
 * @param result Receives the number of bytes received (can be NULL).
 *
 * @return 0 on success, SCE_ERROR_KERNEL_MESSAGE_PIPE_EMPTY if no data can be received, < 0 on error.
 */
// Subroutine sceKernelTryReceiveMsgPipe - Address 0x0000A798
int sceKernelTryReceiveMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 numWokenUp;
    SceSize n;
    SceSize origSize;
    u8 *buf;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;

    buf = message;
    origSize = size;
    oldK1 = pspShiftK1();
    if (!pspK1DynBufOk(buf, size) || !pspK1StaBufOk(result, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((waitMode & ~SCE_KERNEL_MPW_ASAP) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    isIntr = THREADMAN_IS_INTR_CONTEXT();
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    msgPipe = UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    if (msgPipe->bufSize != 0 && msgPipe->bufSize < size) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_SIZE;
    }
    THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_RECEIVE_MSG_PIPE, 4, uid, buf, size, waitMode);

    if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING) {
        /* Only overtake the waiting receivers when they wait by priority and ours is higher. */
        if ((waitQ->attr & SCE_KERNEL_MPA_THPRI_R) == 0 || waitQ->unk4 != 0
            || g_threadman.curThread->currentPriority
                >= ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MESSAGE_PIPE_EMPTY;
        }
    }
    if (waitMode == SCE_KERNEL_MPW_FULL) {
        /* The buffer and the waiting senders must hold enough data. */
        n = msgPipe->used;
        if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING) {
            for (thread = (SceThreadmanThread *)waitQ->waitList.next;
                 thread != (SceThreadmanThread *)&waitQ->waitList; thread = (SceThreadmanThread *)thread->link.next)
                n += MSG_PIPE_WAIT_DATA(thread)->remaining;
        }
        if (n < size) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_MESSAGE_PIPE_EMPTY;
        }
    }

    if (msgPipe->used != 0) {
        n = (msgPipe->used < size) ? msgPipe->used : size;
        _sceKernelMsgPipeRead(msgPipe, buf, n);
        size -= n;
        buf += n;
    }
    numWokenUp = _sceKernelMsgPipeTakeFromSenders(msgPipe, waitQ, &buf, &size);
    if (result != NULL)
        *result = origSize - size;
    /* Note: the threads woken up above are not rescheduled on failure. */
    if ((waitMode == SCE_KERNEL_MPW_ASAP && origSize == size) || (waitMode == SCE_KERNEL_MPW_FULL && size != 0)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_MESSAGE_PIPE_EMPTY;
    }

    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        if (!isIntr)
            sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Cancel the waits on a message pipe and empty its buffer. The waiting threads are released with
 * SCE_ERROR_KERNEL_WAIT_CANCEL.
 *
 * @param uid The UID of the message pipe.
 * @param numSendWaitThreads Receives the number of sending threads released (can be NULL).
 * @param numReceiveWaitThreads Receives the number of receiving threads released (can be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelMsgPipe - Address 0x0000AC20
int sceKernelCancelMsgPipe(SceUID uid, int *numSendWaitThreads, int *numReceiveWaitThreads)
{
    s32 oldK1;
    s32 intr;
    s32 state;
    s32 numWaitThreads;
    s32 numSend;
    s32 numReceive;
    SceSysmemUidCB *cb;
    SceThreadmanMsgPipe *msgPipe;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(numSendWaitThreads, 4) || !pspK1StaBufOk(numReceiveWaitThreads, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    msgPipe = UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_MSG_PIPE, 3, uid, numSendWaitThreads, numReceiveWaitThreads);

    state = msgPipe->state;
    numWaitThreads = sceKernelCallUIDObjFunction(cb, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    numSend = 0;
    numReceive = 0;
    if (state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING)
        numSend = numWaitThreads;
    else if (state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
        numReceive = numWaitThreads;
    if (numSendWaitThreads != NULL)
        *numSendWaitThreads = numSend;
    if (numReceiveWaitThreads != NULL)
        *numReceiveWaitThreads = numReceive;

    msgPipe->used = 0;
    msgPipe->writePtr = msgPipe->buf;
    msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_EMPTY;
    msgPipe->readPtr = msgPipe->buf;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a message pipe.
 *
 * @param uid The UID of the message pipe.
 * @param info Receives the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferMsgPipeStatus - Address 0x0000ADD0
int sceKernelReferMsgPipeStatus(SceUID uid, SceKernelMppInfo *info)
{
    s32 oldK1;
    s32 intr;
    u32 *ptr;
    SceSysmemUidCB *cb;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
    SceKernelMppInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_MSG_PIPE_STATUS, 2, uid, info);

    msgPipe = UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.msgPipeInfo;
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING)
        buf->numSendWaitThreads = waitQ->numWaitThreads;
    else if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
        buf->numReceiveWaitThreads = waitQ->numWaitThreads;
    buf->bufSize = msgPipe->bufSize;
    buf->freeSize = msgPipe->bufSize - msgPipe->used;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/* Append data to the ring buffer of a message pipe (the caller checked there is enough room). */
// Subroutine sub_0000AFC0 - Address 0x0000AFC0
static void _sceKernelMsgPipeWrite(SceThreadmanMsgPipe *msgPipe, const u8 *data, SceSize size)
{
    SceSize n;

    if (msgPipe->writePtr >= msgPipe->readPtr) {
        /* Fill up to the end of the buffer first. */
        n = msgPipe->bufSize - (msgPipe->writePtr - msgPipe->buf);
        if (size < n)
            n = size;
        memcpy(msgPipe->writePtr, data, n);
        msgPipe->used += n;
        msgPipe->writePtr += n;
        size -= n;
        data += n;
        if (msgPipe->writePtr >= msgPipe->buf + msgPipe->bufSize)
            msgPipe->writePtr = msgPipe->buf;
    }
    memcpy(msgPipe->writePtr, data, size);
    msgPipe->used += size;
    msgPipe->writePtr += size;
}

/* Take data from the ring buffer of a message pipe (the caller checked there is enough data). */
// Subroutine sub_0000B094 - Address 0x0000B094
static void _sceKernelMsgPipeRead(SceThreadmanMsgPipe *msgPipe, u8 *data, SceSize size)
{
    SceSize n;

    if (msgPipe->readPtr >= msgPipe->writePtr) {
        /* Read up to the end of the buffer first. */
        n = msgPipe->bufSize - (msgPipe->readPtr - msgPipe->buf);
        if (size < n)
            n = size;
        memcpy(data, msgPipe->readPtr, n);
        msgPipe->used -= n;
        msgPipe->readPtr += n;
        size -= n;
        data += n;
        if (msgPipe->readPtr >= msgPipe->buf + msgPipe->bufSize)
            msgPipe->readPtr = msgPipe->buf;
    }
    memcpy(data, msgPipe->readPtr, size);
    msgPipe->used -= size;
    msgPipe->readPtr += size;
}

/*
 * Find the message pipe whose buffer is a given memory block.
 *
 * @return The UID of the message pipe, or -1 if not found.
 */
// Subroutine sub_0000B16C - Address 0x0000B16C
SceUID _sceKernelGetMsgPipeByBlockId(SceUID blockId)
{
    SceSysmemUidCB *cb;

    for (cb = g_threadman.msgPipeType->PARENT0; cb != g_threadman.msgPipeType; cb = cb->PARENT0) {
        if (UID_CB_TO_DATA(cb, g_threadman.msgPipeType, SceThreadmanMsgPipe)->blockId == blockId)
            return cb->uid;
    }
    return -1;
}

/**
 * Register the "MsgPipe" UID type.
 *
 * @return 0.
 */
// Subroutine sub_0000B1B4 - Address 0x0000B1B4
s32 _sceKernelMsgPipeInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "MsgPipe", 0x30, g_MsgPipeFuncs, NULL, &g_threadman.msgPipeType);
    return SCE_ERROR_OK;
}

// Subroutine sub_0000B1F8 - Address 0x0000B1F8
static s32 _sceKernelMsgPipeInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_0000B220 - Address 0x0000B220
static s32 _sceKernelMsgPipeDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanMsgPipe *msgPipe;

    msgPipe = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanMsgPipe);
    if (msgPipe->bufSize != 0)
        sceKernelFreePartitionMemory(msgPipe->blockId);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

/*
 * Move the data of the waiting senders to the buffer once room was made (called by the WaitQ code, for
 * example after a waiting thread was released). Returns the number of threads to reschedule.
 */
// Subroutine sub_0000B2A0 - Address 0x0000B2A0
static s32 _sceKernelMsgPipeWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    s32 numWokenUp;
    SceSize n;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanMsgPipeWaitData *waitData;

    numWokenUp = 0;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    msgPipe = UID_CB_TO_DATA(uid, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    if (waitQ->numWaitThreads <= 0 || msgPipe->state != SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING)
        return 0;

    /*
     * Note: the original never moves to the next thread, it handles the first waiting sender again until it
     * stops (it was woken up, and is still first in the list as the wait list is only updated later).
     */
    thread = (SceThreadmanThread *)waitQ->waitList.next;
    while (thread != (SceThreadmanThread *)&waitQ->waitList) {
        waitData = MSG_PIPE_WAIT_DATA(thread);
        n = msgPipe->bufSize - msgPipe->used;
        if (waitData->remaining < n)
            n = waitData->remaining;
        if ((waitData->waitMode != SCE_KERNEL_MPW_ASAP || n == 0)
            && (waitData->waitMode != SCE_KERNEL_MPW_FULL || n != waitData->remaining))
            break;
        _sceKernelMsgPipeWrite(msgPipe, waitData->buf, n);
        waitData->remaining -= n;
        if (waitData->result != NULL)
            *waitData->result = waitData->size - waitData->remaining;
        numWokenUp += sub_00000924(thread);
    }
    return numWokenUp;
}

/* Called when a thread stops waiting on a message pipe. */
// Subroutine sub_0000B3C4 - Address 0x0000B3C4
static s32 _sceKernelMsgPipeEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    msgPipe = UID_CB_TO_DATA(uid, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ->unk4 = 0;
    thread = (SceThreadmanThread *)waitQ->waitList.next;
    if (thread != (SceThreadmanThread *)&waitQ->waitList && (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING
        || msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING))
        /* The first waiting thread already transferred a part of its data. */
        waitQ->unk4 = (MSG_PIPE_WAIT_DATA(thread)->size != MSG_PIPE_WAIT_DATA(thread)->remaining);
    waitQ->numWaitThreads--;
    if (waitQ->numWaitThreads == 0)
        msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_EMPTY;
    return SCE_ERROR_OK;
}
