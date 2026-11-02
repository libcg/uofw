/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/eventflag.c
 *
 * Event flags (original address range 0x00005138 - 0x00006284).
 *
 * An event flag is an "EventFlag" UID object which inherits the "WaitQ" type: the WaitQ part holds the
 * attributes and the list of waiting threads, the EventFlag part (SceThreadmanEventFlag) holds the bit pattern.
 *
 * Without SCE_KERNEL_EA_MULTI only one thread may wait on an event flag: the first waiting thread becomes
 * its owner (linked in the thread's ownedEventFlags list) until all of its pending waits have ended.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid event flag attributes. */
#define SCE_THREADMAN_EVENT_FLAG_ATTR_MASK  (0x2FF)

/** The valid wait modes. */
#define SCE_THREADMAN_EVENT_FLAG_WAIT_MASK  (SCE_KERNEL_EW_OR | SCE_KERNEL_EW_CLEAR_ALL | SCE_KERNEL_EW_CLEAR_PAT)

/** Get the event flag owning the ownedEventFlags list entry. */
#define THREADMAN_EVENT_FLAG_FROM_OWNER_LINK(entry) \
    ((SceThreadmanEventFlag *)((u8 *)(entry) - __builtin_offsetof(SceThreadmanEventFlag, ownerLink)))

static s32 __attribute__((used)) _sceKernelWaitEventFlag(SceUID evid, u32 bits, u32 wait, u32 *outBits, SceUInt *timeout);
static s32 _sceKernelEventFlagEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelEventFlagInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelEventFlagDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019CD8
SceSysmemUidLookupFunc g_EventFlagFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_END_WAIT, _sceKernelEventFlagEndWait },
    { 0xD310D2D9, _sceKernelEventFlagInitialize },
    { 0x87089863, _sceKernelEventFlagDelete },
    { 0, NULL }
};

/* Check whether the bit pattern pattern satisfies a wait for bits with the wait mode wait. */
#define THREADMAN_EVENT_FLAG_MATCH(pattern, bits, wait) \
    (((wait) & SCE_KERNEL_EW_OR) ? ((pattern) & (bits)) : (((pattern) & (bits)) == (bits)))

/*
 * PARITY-Q037 (cf293fa): UID attr | (k1 >> 31) passed to sceKernelCreateUID, as the original computes it.
 * PARITY-Q085: Creation failure returns through its own resume/return tail (success path dedented).
 * PARITY-Q042: uid->uid read first, then the stores in the order that schedules as the original (option, owner,
 *     attr, curPattern, ownerWaitCount, initPattern).
 */
// Subroutine sceKernelCreateEventFlag - Address 0x00005138
SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits, SceKernelEventFlagOptParam *opt)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;
    SceThreadmanWaitQ *waitQ;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(opt, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_EVENT_FLAG_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }

    intr = sceKernelCpuSuspendIntr();

    status = sceKernelCreateUID(g_threadman.eventFlagType, name, attr | (pspGetK1() >> 31), &uid);
    if (status != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }
    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    status = uid->uid;
    eventFlag->option = opt;
    eventFlag->owner = NULL;
    waitQ->attr = attr;
    eventFlag->curPattern = bits;
    eventFlag->ownerWaitCount = 0;
    eventFlag->initPattern = bits;
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_EVENT_FLAG, 1, status);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

// Subroutine sceKernelDeleteEventFlag - Address 0x000052AC
int sceKernelDeleteEventFlag(int evid)
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

    if (sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_EVENT_FLAG, 1, evid);
    /* Threads still waiting on the event flag are released by the WaitQ delete function. */
    sceKernelDeleteUID(evid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q047 (b2ff651): Interrupt state read raw / at the original's position.
 * PARITY-Q060 (6d518d4): A 'matched' value tested / returned directly, as the original.
 * PARITY-Q065: Guard and arm shape taken from the original's layout (!isIntr arm first, interrupt arm last).
 * PARITY-Q077: Wake-up walk driven by the next local (`thread = next` at the top of the body, the original's
 * register use: the next pointer is the loop variable, one instruction less than walking `thread`).
 * PARITY-Q178: Each waiter's wait pattern is loaded into the bits parameter (dead once it is OR-ed into
 * curPattern), not a separate local; the original's register use.
 */
// Subroutine sceKernelSetEventFlag - Address 0x000053C0
int sceKernelSetEventFlag(SceUID evid, u32 bits)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    s32 isIntr;
    s32 numWokenUp;
    s32 matched;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;
    SceThreadmanThread *lastWokenUp;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    ret = sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid);
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    lastWokenUp = NULL;
    if (ret != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    if (bits == 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_OK;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SET_EVENT_FLAG, 2, evid, bits);

    eventFlag->curPattern |= bits;

    /* Wake up the waiting threads, in order, whose wait condition is now satisfied. */
    numWokenUp = 0;
    next = (SceThreadmanThread *)waitQ->waitList.next;
    while (next != (SceThreadmanThread *)&waitQ->waitList && eventFlag->curPattern != 0) {
        thread = next;
        next = (SceThreadmanThread *)thread->link.next;
        bits = thread->waitData[0];
        matched = THREADMAN_EVENT_FLAG_MATCH(eventFlag->curPattern, bits, thread->waitData[1]);
        if (!matched)
            continue;
        if (sub_00000924(thread) > 0) {
            lastWokenUp = thread;
            numWokenUp++;
        }
        if (thread->waitData[1] & SCE_KERNEL_EW_CLEAR_ALL)
            eventFlag->curPattern = 0;
        if (thread->waitData[1] & SCE_KERNEL_EW_CLEAR_PAT)
            eventFlag->curPattern &= ~bits;
    }

    if (!isIntr) {
        if (numWokenUp == 1) {
            sub_00016BF4(lastWokenUp, lastWokenUp->currentPriority);
            sub_00016C48(lastWokenUp, intr);
        } else if (numWokenUp > 0) {
            g_threadman.nextThread = NULL;
            sub_00000FE4(0, intr);
        }
    } else {
        /* Reschedule when leaving the interrupt. */
        if (numWokenUp > 0)
            g_threadman.nextThread = NULL;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelClearEventFlag - Address 0x00005618
int sceKernelClearEventFlag(SceUID evid, u32 bits)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;

    oldK1 = pspShiftK1();

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    THREADMAN_LOG(SCE_THREADMAN_LOG_CLEAR_EVENT_FLAG, 2, evid, bits);

    /* Only the bits set in bits are kept. */
    eventFlag->curPattern &= bits;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelWaitEventFlag - Address 0x00005730
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelWaitEventFlag, _sceKernelWaitEventFlag, 0);

// Subroutine sceKernelWaitEventFlagCB - Address 0x00005738
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelWaitEventFlagCB, _sceKernelWaitEventFlag, 1);

/*
 * PARITY-Q048: Value read once into a local (the original keeps it in one register).
 * PARITY-Q060 (a024847): A 'matched' value tested / returned directly, as the original.
 */
// Subroutine sub_00005740 - Address 0x00005740
static s32 __attribute__((used)) _sceKernelWaitEventFlag(SceUID evid, u32 bits, u32 wait, u32 *outBits, SceUInt *timeout)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 matched;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(outBits, 4) || !pspK1StaBufOk(timeout, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((wait & ~SCE_THREADMAN_EVENT_FLAG_WAIT_MASK) != 0
        || (wait & (SCE_KERNEL_EW_CLEAR_ALL | SCE_KERNEL_EW_CLEAR_PAT))
        == (SCE_KERNEL_EW_CLEAR_ALL | SCE_KERNEL_EW_CLEAR_PAT)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
    }
    if (bits == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_EVENT_FLAG_ILLEGAL_WAIT_PATTERN;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0 || g_threadman.dispatchSuspended != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    sub_00016FA4();

    if (sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    thread = g_threadman.curThread;
    if ((waitQ->attr & SCE_KERNEL_EA_MULTI) == 0) {
        /* Only one thread may wait on the event flag: the first waiting thread becomes its owner. */
        if (eventFlag->owner == NULL) {
            threadmanListAddTail(&thread->ownedEventFlags, &eventFlag->ownerLink);
            eventFlag->owner = thread;
        } else if (eventFlag->owner != thread) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_EVENT_FLAG_NO_MULTI_PERM;
        }
        eventFlag->ownerWaitCount++;
    }

    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_WAIT_EVENT_FLAG_CB, 4, evid, bits, wait, outBits);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_WAIT_EVENT_FLAG, 4, evid, bits, wait, outBits);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the event flag. */
            if (outBits != NULL)
                *outBits = eventFlag->curPattern;
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        matched = THREADMAN_EVENT_FLAG_MATCH(eventFlag->curPattern, bits, wait);
        if (matched) {
            if (outBits != NULL)
                *outBits = eventFlag->curPattern;
            if (wait & SCE_KERNEL_EW_CLEAR_ALL)
                eventFlag->curPattern = 0;
            if (wait & SCE_KERNEL_EW_CLEAR_PAT)
                eventFlag->curPattern &= ~bits;
            if (eventFlag->owner != NULL && --eventFlag->ownerWaitCount == 0) {
                eventFlag->owner = NULL;
                threadmanListRemove(&eventFlag->ownerLink);
            }
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_OK;
        }
        /* PARITY-Q071: outBits is stored before wait (the original's store order) */
        thread->waitData[0] = bits;
        thread->waitData[2] = (s32)outBits;
        thread->waitData[1] = wait;
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(uid, SCE_THREADMAN_WAIT_EVENT_FLAG, 0, &timeoutClock, timeout);
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

// Subroutine sceKernelPollEventFlag - Address 0x00005B70
int sceKernelPollEventFlag(int evid, u32 bits, u32 wait, u32 *outBits)
{
    s32 oldK1;
    s32 intr;
    s32 matched;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(outBits, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if ((wait & ~SCE_THREADMAN_EVENT_FLAG_WAIT_MASK) != 0
        || (wait & (SCE_KERNEL_EW_CLEAR_ALL | SCE_KERNEL_EW_CLEAR_PAT))
        == (SCE_KERNEL_EW_CLEAR_ALL | SCE_KERNEL_EW_CLEAR_PAT)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MODE;
    }
    if (bits == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_EVENT_FLAG_ILLEGAL_WAIT_PATTERN;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_WAIT)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    if ((waitQ->attr & SCE_KERNEL_EA_MULTI) == 0 && eventFlag->owner != NULL && eventFlag->owner != thread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_EVENT_FLAG_NO_MULTI_PERM;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_POLL_EVENT_FLAG, 4, evid, bits, wait, outBits);

    matched = THREADMAN_EVENT_FLAG_MATCH(eventFlag->curPattern, bits, wait);
    if (outBits != NULL)
        *outBits = eventFlag->curPattern;
    if (!matched) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_EVENT_FLAG_POLL_FAILED;
    }
    if (wait & SCE_KERNEL_EW_CLEAR_ALL)
        eventFlag->curPattern = 0;
    if (wait & SCE_KERNEL_EW_CLEAR_PAT)
        eventFlag->curPattern &= ~bits;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelCancelEventFlag - Address 0x00005DB4
int sceKernelCancelEventFlag(SceUID evid, SceUInt setpattern, s32 *numWaitThreads)
{
    s32 oldK1;
    s32 intr;
    s32 numCancelled;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(numWaitThreads, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(evid, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_EVENT_FLAG, 3, evid, setpattern, numWaitThreads);

    eventFlag->curPattern = setpattern;
    /* Release all the waiting threads (their wait returns SCE_ERROR_KERNEL_WAIT_CANCELLED). */
    numCancelled = sceKernelCallUIDObjFunction(uid, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    if (numWaitThreads != NULL)
        *numWaitThreads = numCancelled;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

// Subroutine sceKernelReferEventFlagStatus - Address 0x00005F10
int sceKernelReferEventFlagStatus(SceUID event, SceKernelEventFlagInfo *status)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *uid;
    SceThreadmanEventFlag *eventFlag;
    SceThreadmanWaitQ *waitQ;
    SceKernelEventFlagInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(status, sizeof(*status))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(event, g_threadman.eventFlagType, &uid) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_EVENT_FLAG;
    }
    if (!THREADMAN_UID_PERM_OK(uid, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_EVENT_FLAG_STATUS, 2, event, status);

    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.eventFlagInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (uid->name != NULL)
        strncpy(buf->name, uid->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    buf->initPattern = eventFlag->initPattern;
    buf->currentPattern = eventFlag->curPattern;
    buf->numWaitThreads = waitQ->numWaitThreads;
    THREADMAN_COPY_STATUS(status, &g_threadman.statusBuf.eventFlagInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * Release the ownership of all the event flags owned by a thread (called when the thread terminates, so that
 * other threads can wait on them again).
 */
/*
 * PARITY-Q042 (503f2bb): Statement/store order taken from the original's store order.
 * PARITY-Q050 (503f2bb): Loop shape (guarded block instead of continue / separate next cursor / head pointer walk) as
 *     the original's layout.
 */
// Subroutine sub_000060D4 - Address 0x000060D4
void _sceKernelEventFlagReleaseOwned(SceThreadmanThread *thread)
{
    SceThreadmanListEntry *entry;
    SceThreadmanEventFlag *eventFlag;

    entry = thread->ownedEventFlags.next;
    while (entry != &thread->ownedEventFlags) {
        eventFlag = THREADMAN_EVENT_FLAG_FROM_OWNER_LINK(entry);
        entry = entry->next;
        if (eventFlag->owner != NULL) {
            eventFlag->owner = NULL;
            eventFlag->ownerWaitCount = 0;
            threadmanListRemove(&eventFlag->ownerLink);
        }
    }
}

// Subroutine sub_00006120 - Address 0x00006120
s32 _sceKernelEventFlagInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "EventFlag", 0x30, g_EventFlagFuncs, NULL, &g_threadman.eventFlagType);
    return SCE_ERROR_OK;
}

/*
 * Called when the wait of a thread on the event flag ends: drop the ownership taken by the wait (unless the wait
 * is only interrupted to run callbacks) and return the current bit pattern to the thread.
 */
// Subroutine sub_00006164 - Address 0x00006164
static s32 _sceKernelEventFlagEndWait(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap)
{
    u32 *outBits;
    SceThreadmanEventFlag *eventFlag;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    thread = va_arg(ap, SceThreadmanThread *);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    waitQ->numWaitThreads--;
    if (thread == NULL)
        return SCE_ERROR_OK;

    if (thread->waitStatus != (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK && eventFlag->owner != NULL
        && --eventFlag->ownerWaitCount == 0) {
        eventFlag->owner = NULL;
        threadmanListRemove(&eventFlag->ownerLink);
    }
    outBits = (u32 *)thread->waitData[2];
    if (outBits != NULL)
        *outBits = eventFlag->curPattern;
    return SCE_ERROR_OK;
}

// Subroutine sub_00006208 - Address 0x00006208
static s32 _sceKernelEventFlagInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_00006230 - Address 0x00006230
static s32 _sceKernelEventFlagDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanEventFlag *eventFlag;

    eventFlag = UID_CB_TO_DATA(uid, g_threadman.eventFlagType, SceThreadmanEventFlag);
    if (eventFlag->owner != NULL)
        threadmanListRemove(&eventFlag->ownerLink);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
