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

/* PARITY-Q036: wait data accessed through the thread (the original's addressing) */
#define MSG_PIPE_WAIT_DATA(thread) ((SceThreadmanMsgPipeWaitData *)(thread)->waitData)

static s32 __attribute__((used)) _sceKernelSendMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result, SceUInt *timeout);
static s32 __attribute__((used)) _sceKernelReceiveMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result, SceUInt *timeout);
static void _sceKernelMsgPipeWrite(SceThreadmanMsgPipe *msgPipe, const u8 *data, SceSize size);
static void _sceKernelMsgPipeRead(SceThreadmanMsgPipe *msgPipe, u8 *data, SceSize size);
static s32 _sceKernelMsgPipeInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelMsgPipeDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
/*
 * PARITY-Q041: Checks combined in one condition / merged identical error checks: the original shares one branch.
 * PARITY-Q065 (993611e): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
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

/*
 * PARITY-Q037 (cf293fa): UID attr | (k1 >> 31) passed to sceKernelCreateUID, as the original computes it.
 * PARITY-Q045 (58379bc): The new UID kept in a local id read before the data pointer (the original's register use).
 * PARITY-Q051: The wait queue reached through a waitQ local derived before msgPipe, and the UID read before the
 * fields are filled (the original's addressing and load order).
 * PARITY-Q065: blockId = 0 in the then-arm of `bufSize == 0`, allocation in the else-arm (the original's arm layout).
 */
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
/*
 * PARITY-Q042: msgPipe control block resolved before waitQ, and the field stores in the order that
 *     schedules as the original (readPtr, attr, writePtr before buf; used before blockId).
 */
// Subroutine sceKernelCreateMsgPipe - Address 0x00009468
SceUID sceKernelCreateMsgPipe(const char *name, int part, int attr, SceSize bufSize,
    SceKernelMppOptParam *option)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID id;
    SceUID blockId;
    u8 *buf;
    SceSysmemUidCB *uid;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
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

    if (bufSize == 0)
        blockId = 0;
    else {
        blockId = sceKernelAllocPartitionMemory(part, (char *)name,
            (attr >> 14) & 1, bufSize, 0); /* SCE_KERNEL_MPA_HIGHMEM: allocate from the top of the partition */
        if (blockId <= 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_NO_MEMORY;
        }
        buf = sceKernelGetBlockHeadAddr(blockId);
    }

    status = sceKernelCreateUID(g_threadman.msgPipeType, name, attr | (pspGetK1() >> 31), &uid);
    if (status != SCE_ERROR_OK) {
        if (blockId > 0)
            sceKernelFreePartitionMemory(blockId);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    msgPipe = UID_CB_TO_DATA(uid, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    id = uid->uid;
    msgPipe->option = option;
    msgPipe->readPtr = buf;
    waitQ->attr = attr;
    msgPipe->writePtr = buf;
    msgPipe->buf = buf;
    msgPipe->used = 0;
    msgPipe->blockId = blockId;
    msgPipe->bufSize = bufSize;
    msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_EMPTY;
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_MSG_PIPE, 1, id);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return id;
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
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelSendMsgPipe, _sceKernelSendMsgPipe, 0);

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
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelSendMsgPipeCB, _sceKernelSendMsgPipe, 1);

/*
 * Give data to the receiving threads waiting on a message pipe, in their wait order, until there is no data
 * left or no receiving thread left. A receiver is woken up once its buffer is full, or at once if it waits
 * with SCE_KERNEL_MPW_ASAP. Updates DATA and SIZE and sets NW to the number of threads to reschedule
 * (a macro: the original keeps the caller's data pointer and size in registers, they are never address-taken).
 */
/*
 * PARITY-Q036: give/take as macros on the caller's data pointer and size: the original inlines both into each
 * caller
 */
#define MSG_PIPE_GIVE_TO_RECEIVERS(MP, WQ, DATA, SIZE, NW) do {                                           \
    SceSize n;                                                                                            \
    SceThreadmanThread *thread;                                                                           \
    SceThreadmanThread *next;                                                                             \
                                                                                                          \
    (NW) = 0;                                                                                             \
    thread = (SceThreadmanThread *)(WQ)->waitList.next;                                                   \
    while ((MP)->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING && (WQ)->numWaitThreads > 0) {   \
        next = (SceThreadmanThread *)thread->link.next;                                                   \
        n = (MSG_PIPE_WAIT_DATA(thread)->remaining < (SIZE)) ? MSG_PIPE_WAIT_DATA(thread)->remaining : (SIZE); \
        memcpy(MSG_PIPE_WAIT_DATA(thread)->buf, (DATA), n);                                               \
        MSG_PIPE_WAIT_DATA(thread)->buf += n;                                                             \
        MSG_PIPE_WAIT_DATA(thread)->remaining -= n;                                                       \
        (SIZE) -= n;                                                                                      \
        (DATA) += n;                                                                                      \
        if (MSG_PIPE_WAIT_DATA(thread)->result != NULL)                                                   \
            *MSG_PIPE_WAIT_DATA(thread)->result = MSG_PIPE_WAIT_DATA(thread)->size - MSG_PIPE_WAIT_DATA(thread)->remaining; \
        if (MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_ASAP || MSG_PIPE_WAIT_DATA(thread)->remaining == 0) { \
            (WQ)->firstServing = 0;                                                                       \
            (NW) += sub_00000924(thread);                                                                 \
        } else                                                                                            \
            (WQ)->firstServing = 1;                                                                       \
        if ((SIZE) == 0)                                                                                  \
            break;                                                                                        \
        thread = next;                                                                                    \
    }                                                                                                     \
} while (0)

/*
 * PARITY-Q174: sceKernelTrySendMsgPipe's copy of the give loop. The give/take loops are uOFW's macros (the
 *     original inlines its own copy into each caller), and the copies diverge to match the original: here the loop is
 *     an `if (receivers waiting) for (;;)` with the waiting test repeated at the end of the iteration (as in
 *     MSG_PIPE_TAKE_FROM_SENDERS); _sceKernelSendMsgPipe keeps the while loop above. TrySend wsim +0.030,
 *     csim +0.051, 264 -> 266 instructions (= original); 1 up / 0 down. In _sceKernelSendMsgPipe this form
 *     (and the next-first loop of Q173) is down: wsim -0.014 / csim -0.073 (not taken).
 */
#define MSG_PIPE_TRY_GIVE_TO_RECEIVERS(MP, WQ, DATA, SIZE, NW) do {                                       \
    SceSize n;                                                                                            \
    SceThreadmanThread *thread;                                                                           \
    SceThreadmanThread *next;                                                                             \
                                                                                                          \
    (NW) = 0;                                                                                             \
    thread = (SceThreadmanThread *)(WQ)->waitList.next;                                                   \
    if ((MP)->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING && (WQ)->numWaitThreads > 0) {      \
        for (;;) {                                                                                        \
            next = (SceThreadmanThread *)thread->link.next;                                               \
            n = (MSG_PIPE_WAIT_DATA(thread)->remaining < (SIZE)) ? MSG_PIPE_WAIT_DATA(thread)->remaining : (SIZE); \
            memcpy(MSG_PIPE_WAIT_DATA(thread)->buf, (DATA), n);                                           \
            MSG_PIPE_WAIT_DATA(thread)->buf += n;                                                         \
            MSG_PIPE_WAIT_DATA(thread)->remaining -= n;                                                   \
            (SIZE) -= n;                                                                                  \
            (DATA) += n;                                                                                  \
            if (MSG_PIPE_WAIT_DATA(thread)->result != NULL)                                               \
                *MSG_PIPE_WAIT_DATA(thread)->result = MSG_PIPE_WAIT_DATA(thread)->size - MSG_PIPE_WAIT_DATA(thread)->remaining; \
            if (MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_ASAP || MSG_PIPE_WAIT_DATA(thread)->remaining == 0) { \
                (WQ)->firstServing = 0;                                                                   \
                (NW) += sub_00000924(thread);                                                             \
            } else                                                                                        \
                (WQ)->firstServing = 1;                                                                   \
            if ((SIZE) == 0)                                                                              \
                break;                                                                                    \
            if ((MP)->state != SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING || (WQ)->numWaitThreads <= 0) \
                break;                                                                                    \
            thread = next;                                                                                \
        }                                                                                                 \
    }                                                                                                     \
} while (0)

/*
 * PARITY-Q147: In the copy loop, copied = 0 comes before the next-thread read, and SIZE is decremented
 * before the sender's remaining count (statement order).
 */
/*
 * PARITY-Q172: the per-sender firstServing test is written with the ASAP-and-copied case first, as an
 *     else-if chain (`if (ASAP && copied) fs = 0; else if (remaining == 0) fs = 0; else { fs = 1; break; }`).
 *     This gives the original's `xori; sltiu; and; bnel` test (ours computed the De Morgan inverse with
 *     `sltu; or; beql`); sceKernelTryReceiveMsgPipe control flow becomes exact, _sceKernelReceiveMsgPipe goes
 *     from 6 control-flow hunks to 5. Try +0.022/+0.011, Receive +0.009/+0.006 (wsim/csim), 2 up / 0 down.
 *     Known open residue: each function loses one spill reload (Receive 375 vs 377, Try 289 vs 290 instructions;
 *     a downstream register-allocation difference, not the source shape).
 */
/*
 * PARITY-Q173: the sender loop reads the first waiting thread into `next` before the loop and starts each
 *     iteration with `thread = next` (instead of `thread = next` at the end of the iteration). The original copies
 *     the first thread from a separate register at the loop entry (`move s1,s3`). _sceKernelReceiveMsgPipe
 *     +0.012/+0.002 and one instruction closer (376 vs 377), sceKernelTryReceiveMsgPipe +0.003/+0.014;
 *     2 up / 0 down. Q172's one-instruction gap in _sceKernelReceiveMsgPipe is closed; TryReceive is still 289 vs 290.
 */
/*
 * Take data from the sending threads waiting on a message pipe, in their wait order: their data is copied
 * to the receiver's buffer and, once it is full, to the message pipe buffer. Updates DATA and
 * SIZE and sets NW to the number of threads to reschedule (a macro, see MSG_PIPE_GIVE_TO_RECEIVERS).
 */
#define MSG_PIPE_TAKE_FROM_SENDERS(MP, WQ, DATA, SIZE, NW) do {                                           \
    s32 copied;                                                                                           \
    SceSize n;                                                                                            \
    SceThreadmanThread *thread;                                                                           \
    SceThreadmanThread *next;                                                                             \
                                                                                                          \
    (NW) = 0;                                                                                             \
    next = (SceThreadmanThread *)(WQ)->waitList.next;                                                     \
    if ((MP)->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING && (WQ)->numWaitThreads > 0) {        \
        for (;;) {                                                                                        \
            thread = next;                                                                                \
            copied = 0;                                                                                   \
            next = (SceThreadmanThread *)thread->link.next;                                               \
            if ((SIZE) != 0) {                                                                            \
                n = (MSG_PIPE_WAIT_DATA(thread)->remaining < (SIZE)) ? MSG_PIPE_WAIT_DATA(thread)->remaining : (SIZE); \
                memcpy((DATA), MSG_PIPE_WAIT_DATA(thread)->buf, n);                                       \
                (DATA) += n;                                                                              \
                MSG_PIPE_WAIT_DATA(thread)->buf += n;                                                     \
                (SIZE) -= n;                                                                              \
                MSG_PIPE_WAIT_DATA(thread)->remaining -= n;                                               \
                copied = 1;                                                                               \
            }                                                                                             \
            n = (MP)->bufSize - (MP)->used;                                                               \
            if (MSG_PIPE_WAIT_DATA(thread)->remaining < n)                                                \
                n = MSG_PIPE_WAIT_DATA(thread)->remaining;                                                \
            if ((MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_ASAP && n != 0)                   \
                || (MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_FULL && n == MSG_PIPE_WAIT_DATA(thread)->remaining)) { \
                _sceKernelMsgPipeWrite((MP), MSG_PIPE_WAIT_DATA(thread)->buf, n);                         \
                MSG_PIPE_WAIT_DATA(thread)->remaining -= n;                                               \
                copied = 1;                                                                               \
            }                                                                                             \
            if (MSG_PIPE_WAIT_DATA(thread)->result != NULL)                                               \
                *MSG_PIPE_WAIT_DATA(thread)->result = MSG_PIPE_WAIT_DATA(thread)->size - MSG_PIPE_WAIT_DATA(thread)->remaining; \
            if (MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_ASAP && copied)                    \
                (WQ)->firstServing = 0;                                                                   \
            else if (MSG_PIPE_WAIT_DATA(thread)->remaining == 0)                                          \
                (WQ)->firstServing = 0;                                                                   \
            else {                                                                                        \
                (WQ)->firstServing = 1;                                                                   \
                break;                                                                                    \
            }                                                                                             \
            (NW) += sub_00000924(thread);                                                                 \
            if ((MP)->state != SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING || (WQ)->numWaitThreads <= 0) \
                break;                                                                                    \
        }                                                                                                 \
    }                                                                                                     \
} while (0)

/*
 * PARITY-Q145: the overtake check as a static inline shared by the four send / receive paths (uOFW open-coded
 *     it in each). The inline's argument pseudos shift the allocation ranking in sceKernelTrySendMsgPipe and
 *     sceKernelTryReceiveMsgPipe toward the original. Plain C.
 */
/**
 * Check whether a thread must wait behind the threads already waiting on a message pipe: it may only overtake
 * them when they wait in priority order (prioAttr: SCE_KERNEL_MPA_THPRI_S or _R), none is being served and its
 * priority is higher than the first waiting thread's.
 */
static inline s32 threadmanMsgPipeMustWait(SceThreadmanWaitQ *waitQ, SceThreadmanThread *thread, u32 prioAttr)
{
    return (waitQ->attr & prioAttr) == 0 || waitQ->firstServing != 0
        || thread->currentPriority >= ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority;
}

/*
 * PARITY-Q054: Comparison operand order as the original: the free room clamped with `if (n >= size) n = size`.
 * PARITY-Q129: Wait data stored with isReceiving before waitMode, as in the receive path (sub_0000A1B4):
 *   the isReceiving zero store then precedes the reloaded waitMode store, as in the original.
 * PARITY-Q149: After the buffered write, data is advanced before size is decremented (statement order).
 * PARITY-Q167: mustWait declared after origSize: spilled pseudos get their stack slots in pseudo
 *   (declaration) order (as Q164/Q166), which moves one spill slot to the original's.
 * PARITY-Q175: msgPipe declared first and withCallbacks (with its $t3 read) after mustWait: by the same
 *   declaration-order rule the msgPipe spill slot comes before intr's and withCallbacks' after mustWait's,
 *   as in the original (0x20 / 0x34). Plain C, no code moves (the initializer still runs first).
 */
/*
 * Common part of sceKernelSendMsgPipe() and sceKernelSendMsgPipeCB().
 * Note: the original passes withCallbacks in $t3 (an unused $t2 argument).
 */
// Subroutine sub_000097E0 - Address 0x000097E0
static s32 __attribute__((used)) _sceKernelSendMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result, SceUInt *timeout)
{
    SceThreadmanMsgPipe *msgPipe;
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 numWokenUp;
    SceSize n;
    SceSize origSize;
    s32 mustWait;
    s32 withCallbacks = threadmanGetWithCallbacks();
    u8 *data;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanWaitQ *waitQ;

    data = message;
    origSize = size;
    oldK1 = pspShiftK1();
    if (!threadmanK1DynBufOk(data, size) || !pspK1StaBufOk(result, 4) || !pspK1StaBufOk(timeout, 4)) {
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
            if (threadmanMsgPipeMustWait(waitQ, thread, SCE_KERNEL_MPA_THPRI_S))
                mustWait = 1;
        }
        if (!mustWait) {
            if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
                MSG_PIPE_GIVE_TO_RECEIVERS(msgPipe, waitQ, data, size, numWokenUp);
            if (size != 0) {
                n = msgPipe->bufSize - msgPipe->used;
                if (n >= size)
                    n = size;
                if ((waitMode == SCE_KERNEL_MPW_ASAP && n != 0) || (waitMode == SCE_KERNEL_MPW_FULL && n == size)) {
                    _sceKernelMsgPipeWrite(msgPipe, data, n);
                    data += n;
                    size -= n;
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
        MSG_PIPE_WAIT_DATA(thread)->buf = data;
        MSG_PIPE_WAIT_DATA(thread)->remaining = size;
        MSG_PIPE_WAIT_DATA(thread)->result = result;
        MSG_PIPE_WAIT_DATA(thread)->size = origSize;
        MSG_PIPE_WAIT_DATA(thread)->isReceiving = 0;
        MSG_PIPE_WAIT_DATA(thread)->waitMode = waitMode;
        if (!mustWait)
            waitQ->firstServing = (origSize != size);
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_MSG_PIPE, waitQ->attr & SCE_KERNEL_MPA_THPRI_S,
            &timeoutClock, timeout);
        data = MSG_PIPE_WAIT_DATA(thread)->buf;
        size = MSG_PIPE_WAIT_DATA(thread)->remaining;
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/*
 * PARITY-Q047 (2356fce): Interrupt state read raw / at the original's position.
 * PARITY-Q054: Comparison operand order as the original: the free room clamped with `if (n >= size) n = size`.
 */
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
    s32 lookup;
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
    if (!threadmanK1DynBufOk(data, size) || !pspK1StaBufOk(result, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((waitMode & ~SCE_KERNEL_MPW_ASAP) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
    }

    intr = sceKernelCpuSuspendIntr();

    lookup = sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb);
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (lookup != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
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
        if (threadmanMsgPipeMustWait(waitQ, g_threadman.curThread, SCE_KERNEL_MPA_THPRI_S)) {
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
        MSG_PIPE_TRY_GIVE_TO_RECEIVERS(msgPipe, waitQ, data, size, numWokenUp);
    if (size != 0) {
        n = msgPipe->bufSize - msgPipe->used;
        if (n >= size)
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
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelReceiveMsgPipe, _sceKernelReceiveMsgPipe, 0);

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
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelReceiveMsgPipeCB, _sceKernelReceiveMsgPipe, 1);

/*
 * PARITY-Q148: After the buffered read, buf is advanced before size is decremented (statement order).
 * PARITY-Q164: mustWait declared before numWokenUp: both are spilled, and reload assigns their stack
 *   slots in pseudo (declaration) order, so the two 0x28/0x2C slots swap into the original's assignment.
 * PARITY-Q171: in the wait loop the overtake check is written out, with the not-by-priority case as the else arm
 *     (`if (attr & THPRI_R) { if (firstServing || prio >= first) mustWait = 1; } else mustWait = 1;`). The two
 *     `mustWait = 1` stores stay separate blocks (the else arm's 1 set in the branch delay slot), as in the original;
 *     through the || helper they share one block reached by a threaded bnel. wsim +0.003, csim +0.002, 2 instructions
 *     closer, 1 up / 0 down; still equiv. Same form in _sceKernelSendMsgPipe: csim -0.046 (not taken); the plain
 *     if/else-if order and the three-arm chain: down.
 */
/*
 * Common part of sceKernelReceiveMsgPipe() and sceKernelReceiveMsgPipeCB().
 * Note: the original passes withCallbacks in $t3 (an unused $t2 argument).
 */
// Subroutine sub_0000A1B4 - Address 0x0000A1B4
static s32 __attribute__((used)) _sceKernelReceiveMsgPipe(SceUID uid, void *message, SceSize size, s32 waitMode, int *result, SceUInt *timeout)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 mustWait;
    s32 numWokenUp;
    SceSize n;
    SceSize origSize;
    u8 *buf;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;

    buf = message;
    origSize = size;
    oldK1 = pspShiftK1();
    if (!threadmanK1DynBufOk(buf, size) || !pspK1StaBufOk(result, 4) || !pspK1StaBufOk(timeout, 4)) {
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
            if ((waitQ->attr & SCE_KERNEL_MPA_THPRI_R) != 0) {
                if (waitQ->firstServing != 0
                    || thread->currentPriority >= ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority)
                    mustWait = 1;
            } else
                mustWait = 1;
        }
        if (!mustWait) {
            if (msgPipe->used != 0) {
                n = (msgPipe->used < size) ? msgPipe->used : size;
                _sceKernelMsgPipeRead(msgPipe, buf, n);
                buf += n;
                size -= n;
            }
            MSG_PIPE_TAKE_FROM_SENDERS(msgPipe, waitQ, buf, size, numWokenUp);
        }
        if (result != NULL)
            *result = origSize - size;
        if ((waitMode != SCE_KERNEL_MPW_ASAP || origSize != size) && (waitMode != SCE_KERNEL_MPW_FULL || size == 0)) {
            status = SCE_ERROR_OK;
            break;
        }

        msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING;
        numWokenUp = 0;
        MSG_PIPE_WAIT_DATA(thread)->buf = buf;
        MSG_PIPE_WAIT_DATA(thread)->remaining = size;
        MSG_PIPE_WAIT_DATA(thread)->size = origSize;
        MSG_PIPE_WAIT_DATA(thread)->result = result;
        MSG_PIPE_WAIT_DATA(thread)->isReceiving = 1;
        MSG_PIPE_WAIT_DATA(thread)->waitMode = waitMode;
        if (!mustWait)
            waitQ->firstServing = (origSize != size);
        thread->waitWithCallbacks = withCallbacks;
        /* The receivers wait in priority order with SCE_KERNEL_MPA_THPRI_R. */
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_MSG_PIPE, (waitQ->attr >> 4) & SCE_KERNEL_MPA_THPRI_S,
            &timeoutClock, timeout);
        buf = MSG_PIPE_WAIT_DATA(thread)->buf;
        size = MSG_PIPE_WAIT_DATA(thread)->remaining;
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    if (numWokenUp != 0) {
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/*
 * PARITY-Q047 (2356fce): Interrupt state read raw / at the original's position.
 * PARITY-Q166: numWokenUp declared before intr: both are spilled, and reload assigns their stack slots in
 *   pseudo (declaration) order, so intr gets the original's 0xC slot.
 */
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
    s32 numWokenUp;
    s32 intr;
    s32 lookup;
    s32 isIntr;
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
    if (!threadmanK1DynBufOk(buf, size) || !pspK1StaBufOk(result, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((waitMode & ~SCE_KERNEL_MPW_ASAP) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
    }

    intr = sceKernelCpuSuspendIntr();

    lookup = sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.msgPipeType, &cb);
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (lookup != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_MESSAGE_PIPE;
    }
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
        if (threadmanMsgPipeMustWait(waitQ, g_threadman.curThread, SCE_KERNEL_MPA_THPRI_R)) {
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
    MSG_PIPE_TAKE_FROM_SENDERS(msgPipe, waitQ, buf, size, numWokenUp);
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

/*
 * PARITY-Q042 (71a2a63): Statement/store order taken from the original's store order.
 */
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
    numReceive = 0;
    numSend = 0;
    if (state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING)
        numSend = numWaitThreads;
    else if (state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
        numReceive = numWaitThreads;
    if (numSendWaitThreads != NULL)
        *numSendWaitThreads = numSend;
    if (numReceiveWaitThreads != NULL)
        *numReceiveWaitThreads = numReceive;

    msgPipe->used = 0;
    msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_EMPTY;
    msgPipe->readPtr = msgPipe->buf;
    msgPipe->writePtr = msgPipe->buf;

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
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
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
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.msgPipeInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/* Append data to the ring buffer of a message pipe (the caller checked there is enough room). */
/*
 * PARITY-Q042 (40373d4): Statement/store order taken from the original's store order.
 */
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
        data += n;
        size -= n;
        if (msgPipe->writePtr >= msgPipe->buf + msgPipe->bufSize)
            msgPipe->writePtr = msgPipe->buf;
    }
    memcpy(msgPipe->writePtr, data, size);
    msgPipe->writePtr += size;
    msgPipe->used += size;
}

/* Take data from the ring buffer of a message pipe (the caller checked there is enough data). */
/*
 * PARITY-Q042 (40373d4): Statement/store order taken from the original's store order.
 */
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
        data += n;
        size -= n;
        if (msgPipe->readPtr >= msgPipe->buf + msgPipe->bufSize)
            msgPipe->readPtr = msgPipe->buf;
    }
    memcpy(data, msgPipe->readPtr, size);
    msgPipe->readPtr += size;
    msgPipe->used -= size;
}

/*
 * Find the message pipe whose buffer is a given memory block.
 *
 * @return The UID of the message pipe, or -1 if not found.
 */
/*
 * PARITY-Q048 (905162f, e0f7084): Value read once into a local (the original keeps it in one register).
 */
// Subroutine sub_0000B16C - Address 0x0000B16C
SceUID _sceKernelGetMsgPipeByBlockId(SceUID blockId)
{
    SceSysmemUidCB *cb;
    SceSysmemUidCB *type = g_threadman.msgPipeType;
    SceThreadmanMsgPipe *msgPipe;

    for (cb = type->PARENT0; cb != type; cb = cb->PARENT0) {
        msgPipe = UID_CB_TO_DATA(cb, type, SceThreadmanMsgPipe);
        if (msgPipe->blockId == blockId)
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
 * PARITY-Q040: Separate ifs for the early checks (each returns 0 on its own): the original gives each check its own branch.
 * PARITY-Q054: Comparison operand order as the original: n starts as the remaining size and is clamped to the free room.
 */
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

    numWokenUp = 0;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    msgPipe = UID_CB_TO_DATA(uid, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    if (waitQ->numWaitThreads <= 0)
        return 0;
    if (msgPipe->state != SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING)
        return 0;
    /*
     * Note: the original never moves to the next thread, it handles the first waiting sender again until it
     * stops (it was woken up, and is still first in the list as the wait list is only updated later).
     */
    thread = (SceThreadmanThread *)waitQ->waitList.next;
    while (thread != (SceThreadmanThread *)&waitQ->waitList) {
        n = MSG_PIPE_WAIT_DATA(thread)->remaining;
        if (msgPipe->bufSize - msgPipe->used < n)
            n = msgPipe->bufSize - msgPipe->used;
        if ((MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_ASAP && n != 0)
            || (MSG_PIPE_WAIT_DATA(thread)->waitMode == SCE_KERNEL_MPW_FULL && n == MSG_PIPE_WAIT_DATA(thread)->remaining)) {
            _sceKernelMsgPipeWrite(msgPipe, MSG_PIPE_WAIT_DATA(thread)->buf, n);
            MSG_PIPE_WAIT_DATA(thread)->remaining -= n;
            if (MSG_PIPE_WAIT_DATA(thread)->result != NULL)
                *MSG_PIPE_WAIT_DATA(thread)->result = MSG_PIPE_WAIT_DATA(thread)->size - MSG_PIPE_WAIT_DATA(thread)->remaining;
            numWokenUp += sub_00000924(thread);
        } else
            break;
    }
    return numWokenUp;
}

/* Called when a thread stops waiting on a message pipe. */
/*
 * PARITY-Q040 (eb31f8d): Separate ifs for the early error checks: the original gives each check its own branch.
 */
// Subroutine sub_0000B3C4 - Address 0x0000B3C4
static s32 _sceKernelMsgPipeEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    SceThreadmanMsgPipe *msgPipe;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    msgPipe = UID_CB_TO_DATA(uid, g_threadman.msgPipeType, SceThreadmanMsgPipe);
    waitQ->firstServing = 0;
    thread = (SceThreadmanThread *)waitQ->waitList.next;
    if (thread != (SceThreadmanThread *)&waitQ->waitList) {
        /* The first waiting thread may already have transferred a part of its data. */
        if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING)
            waitQ->firstServing = (MSG_PIPE_WAIT_DATA(thread)->size != MSG_PIPE_WAIT_DATA(thread)->remaining);
        else if (msgPipe->state == SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING)
            waitQ->firstServing = (MSG_PIPE_WAIT_DATA(thread)->size != MSG_PIPE_WAIT_DATA(thread)->remaining);
    }
    waitQ->numWaitThreads--;
    if (waitQ->numWaitThreads == 0)
        msgPipe->state = SCE_THREADMAN_MSG_PIPE_STATE_EMPTY;
    return SCE_ERROR_OK;
}
