/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/vpl.c
 *
 * Variable-length memory pools (original address range 0x0000B44C - 0x0000C408).
 *
 * A variable-length memory pool is a "Vpl" UID object which inherits the "WaitQ" type. Its memory is a
 * system memory heap; the threads waiting on the pool wait for a block of a given size.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid variable-length memory pool attributes. */
#define SCE_THREADMAN_VPL_ATTR_MASK     (0x43FF)

static s32 __attribute__((used)) _sceKernelAllocateVpl(SceUID uid, SceSize size, void **data, unsigned int *timeout);
static s32 _sceKernelVplInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelVplDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelVplWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019D80
SceSysmemUidLookupFunc g_VplFuncs[] = {
    { SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS, _sceKernelVplWakeupWaiters },
    { 0xD310D2D9, _sceKernelVplInitialize },
    { 0x87089863, _sceKernelVplDelete },
    { 0, NULL }
};

/*
 * PARITY-Q037 (cf293fa): UID attr | (k1 >> 31) passed to sceKernelCreateUID, as the original computes it.
 * PARITY-Q045 (58379bc): The new UID kept in a local id read before the data pointer (the original's register use).
 * PARITY-Q083: sceKernelCreateUID failure as `goto error` (goto prediction: the original's block order).
 */
/**
 * Create a variable-length memory pool.
 *
 * @param name The name of the pool.
 * @param part The memory partition to allocate the pool from.
 * @param attr The attributes (SCE_KERNEL_VPA_*).
 * @param size The size of the pool.
 * @param opt The option parameter (can be NULL).
 *
 * @return The UID of the pool on success, < 0 on error.
 */
// Subroutine sceKernelCreateVpl - Address 0x0000B44C
SceUID sceKernelCreateVpl(const char *name, int part, int attr, unsigned int size, SceKernelVplOptParam *opt)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    SceUID id;
    SceUID heapId;
    SceSysmemUidCB *uid;
    SceSysmemUidCB *heapUid;
    SceThreadmanVpl *vpl;
    SceThreadmanWaitQ *waitQ;
    SceSysmemPartitionInfo partInfo;

    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(opt, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_VPL_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if (size == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMSIZE;
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

    status = sceKernelCreateUID(g_threadman.vplType, name, attr | (pspGetK1() >> 31), &uid);
    if (status != SCE_ERROR_OK)
        goto error;
    vpl = UID_CB_TO_DATA(uid, g_threadman.vplType, SceThreadmanVpl);
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    id = uid->uid;
    /* SCE_KERNEL_VPA_HIGHMEM: allocate the heap from the top of the partition. */
    heapId = sceKernelCreateHeap(part, size, ((u32)attr >> 13) & 2, name);
    vpl->heapId = heapId;
    if (heapId <= 0) {
        sceKernelDeleteUID(id);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }
    vpl->poolSize = sceKernelHeapTotalFreeSize(heapId);
    vpl->option = opt;
    waitQ->attr = attr;
    if (((attr | (pspGetK1() >> 31)) & 1) != 0) {
        /* Let the heap be accessed from user mode too. */
        if (sceKernelGetUIDcontrolBlock(vpl->heapId, &heapUid) == SCE_ERROR_OK)
            heapUid->attr |= 1;
    }
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_VPL, 1, id);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return id;

error:
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Delete a variable-length memory pool. The threads waiting on it are released with
 * SCE_ERROR_KERNEL_WAIT_DELETE.
 *
 * @param uid The UID of the pool.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteVpl - Address 0x0000B6C0
int sceKernelDeleteVpl(SceUID uid)
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_VPL, 1, uid);
    sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Allocate a block from a variable-length memory pool, waiting until it is available.
 *
 * @param uid The UID of the pool.
 * @param size The size of the block.
 * @param data Receives the address of the block.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelAllocateVpl - Address 0x0000B7D4
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelAllocateVpl, _sceKernelAllocateVpl, 0);

/**
 * Allocate a block from a variable-length memory pool, waiting until it is available and running the
 * callbacks notified meanwhile.
 *
 * @param uid The UID of the pool.
 * @param size The size of the block.
 * @param data Receives the address of the block.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelAllocateVplCB - Address 0x0000B7DC
THREADMAN_WITH_CALLBACKS_THUNK(sceKernelAllocateVplCB, _sceKernelAllocateVpl, 1);

/*
 * Common part of sceKernelAllocateVpl() and sceKernelAllocateVplCB().
 * Note: the original passes withCallbacks in $t3.
 */
/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q041 (e093285): Checks combined in one condition / merged identical error checks: the original shares one
 *     branch.
 */
// Subroutine sub_0000B7E4 - Address 0x0000B7E4
static s32 __attribute__((used)) _sceKernelAllocateVpl(SceUID uid, SceSize size, void **data, unsigned int *timeout)
{
    s32 withCallbacks = threadmanGetWithCallbacks();
    s32 oldK1;
    s32 intr;
    s32 status;
    void *addr;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanVpl *vpl;
    SceThreadmanWaitQ *waitQ;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(data, 4) || !pspK1StaBufOk(timeout, 4)) {
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    vpl = UID_CB_TO_DATA(cb, g_threadman.vplType, SceThreadmanVpl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    if (size == 0 || vpl->poolSize < size) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMSIZE;
    }
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_ALLOCATE_VPL_CB, 2, uid, size);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_ALLOCATE_VPL, 2, uid, size);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the pool. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        /* Do not overtake the waiting threads, unless allowed or our priority is higher. */
        if ((waitQ->attr & SCE_KERNEL_VPA_PASS) || waitQ->numWaitThreads <= 0
            || ((waitQ->attr & SCE_KERNEL_VPA_THPRI)
                && thread->currentPriority < ((SceThreadmanThread *)waitQ->waitList.next)->currentPriority)) {
            addr = sceKernelAllocHeapMemory(vpl->heapId, size);
            if (addr != NULL) {
                *data = addr;
                status = SCE_ERROR_OK;
                break;
            }
        }
        thread->waitData[1] = size;
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_VPL, waitQ->attr, &timeoutClock, (SceUInt *)timeout);
        if (status == SCE_ERROR_OK)
            *data = (void *)thread->waitData[0];
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Allocate a block from a variable-length memory pool without waiting.
 *
 * @param uid The UID of the pool.
 * @param size The size of the block.
 * @param data Receives the address of the block.
 *
 * @return 0 on success, SCE_ERROR_KERNEL_NO_MEMORY if no block is available, < 0 on error.
 */
// Subroutine sceKernelTryAllocateVpl - Address 0x0000BB14
int sceKernelTryAllocateVpl(SceUID uid, unsigned int size, void **data)
{
    s32 oldK1;
    s32 intr;
    void *addr;
    SceSysmemUidCB *cb;
    SceThreadmanVpl *vpl;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(data, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    vpl = UID_CB_TO_DATA(cb, g_threadman.vplType, SceThreadmanVpl);
    if (size == 0 || vpl->poolSize < size) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMSIZE;
    }

    /* Note: unlike sceKernelAllocateVpl(), the waiting threads can always be overtaken. */
    addr = sceKernelAllocHeapMemory(vpl->heapId, size);
    THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_ALLOCATE_VPL, 3, uid, size, addr);
    if (addr == NULL) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }
    *data = addr;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q046 (3b08adb): Object pointer derived before waitQ (the original loads the object's type word first).
 * PARITY-Q050 (3b08adb): Loop shape (guarded block instead of continue / separate next cursor / head pointer walk) as
 *     the original's layout.
 * PARITY-Q065 (3b08adb): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
/**
 * Free a block of a variable-length memory pool, and give the memory to the waiting threads.
 *
 * @param uid The UID of the pool.
 * @param data The address of the block.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelFreeVpl - Address 0x0000BCB4
int sceKernelFreeVpl(SceUID uid, void *data)
{
    s32 oldK1;
    s32 intr;
    s32 numWokenUp;
    void *addr;
    SceSysmemUidCB *cb;
    SceThreadmanVpl *vpl;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;
    SceThreadmanThread *lastWokenUp;

    lastWokenUp = NULL;
    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(data)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    vpl = UID_CB_TO_DATA(cb, g_threadman.vplType, SceThreadmanVpl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    if (data == NULL || sceKernelFreeHeapMemory(vpl->heapId, data) < 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK;
    }
    THREADMAN_LOG(SCE_THREADMAN_LOG_FREE_VPL, 2, uid, data);

    /* Satisfy the waiting threads in order (skipping the ones which cannot be with SCE_KERNEL_VPA_PASS). */
    numWokenUp = 0;
    next = (SceThreadmanThread *)waitQ->waitList.next;
    while (next != (SceThreadmanThread *)&waitQ->waitList) {
        thread = next;
        next = (SceThreadmanThread *)thread->link.next;
        addr = sceKernelAllocHeapMemory(vpl->heapId, thread->waitData[1]);
        /*
         * PARITY-Q203 (source form): the failed allocation is handled first
         * (break, or continue with SCE_KERNEL_VPA_PASS); the changed block
         * frequencies give the original's register allocation.
         */
        if (addr == NULL) {
            if (!(waitQ->attr & SCE_KERNEL_VPA_PASS))
                break;
            continue;
        }
        thread->waitData[0] = (s32)addr;
        if (sub_00000924(thread) > 0) {
            lastWokenUp = thread;
            numWokenUp++;
        }
    }

    if (numWokenUp == 1) {
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

/**
 * Cancel the waits on a variable-length memory pool. The waiting threads are released with
 * SCE_ERROR_KERNEL_WAIT_CANCEL.
 *
 * @param uid The UID of the pool.
 * @param pnum Receives the number of threads released (can be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelVpl - Address 0x0000BEFC
int sceKernelCancelVpl(SceUID uid, int *pnum)
{
    s32 oldK1;
    s32 intr;
    s32 numWaitThreads;
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(pnum, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_VPL, 2, uid, pnum);
    numWaitThreads = sceKernelCallUIDObjFunction(cb, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    if (pnum != NULL)
        *pnum = numWaitThreads;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a variable-length memory pool.
 *
 * @param uid The UID of the pool.
 * @param info Receives the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferVplStatus - Address 0x0000C034
int sceKernelReferVplStatus(SceUID uid, SceKernelVplInfo *info)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanVpl *vpl;
    SceThreadmanWaitQ *waitQ;
    SceKernelVplInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.vplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_VPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_VPL_STATUS, 2, uid, info);

    vpl = UID_CB_TO_DATA(cb, g_threadman.vplType, SceThreadmanVpl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.vplInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    buf->poolSize = vpl->poolSize;
    buf->freeSize = sceKernelHeapTotalFreeSize(vpl->heapId);
    buf->numWaitThreads = waitQ->numWaitThreads;
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.vplInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * Find the variable-length memory pool using a given heap.
 *
 * @return The UID of the pool, or -1 if not found.
 */
/*
 * PARITY-Q048 (905162f, e0f7084): Value read once into a local (the original keeps it in one register).
 */
// Subroutine sub_0000C1FC - Address 0x0000C1FC
SceUID _sceKernelGetVplByHeapId(SceUID heapId)
{
    SceSysmemUidCB *cb;
    SceSysmemUidCB *type = g_threadman.vplType;
    SceThreadmanVpl *vpl;

    for (cb = type->PARENT0; cb != type; cb = cb->PARENT0) {
        vpl = UID_CB_TO_DATA(cb, type, SceThreadmanVpl);
        if (vpl->heapId == heapId)
            return cb->uid;
    }
    return -1;
}

/**
 * Register the "Vpl" UID type.
 *
 * @return 0.
 */
// Subroutine sub_0000C244 - Address 0x0000C244
s32 _sceKernelVplInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Vpl", 0x30, g_VplFuncs, NULL, &g_threadman.vplType);
    return SCE_ERROR_OK;
}

// Subroutine sub_0000C288 - Address 0x0000C288
static s32 _sceKernelVplInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_0000C2B0 - Address 0x0000C2B0
static s32 _sceKernelVplDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanVpl *vpl;

    vpl = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanVpl);
    if (vpl->heapId > 0)
        sceKernelDeleteHeap(vpl->heapId);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}

/*
 * Give the free memory to the waiting threads (called by the WaitQ code, for example after a waiting thread
 * was released). Returns the number of threads to reschedule.
 */
/*
 * PARITY-Q050 (4e5b0b9, bef87e6): Loop shape (guarded block instead of continue / separate next cursor / head pointer
 *     walk) as the original's layout.
 * PARITY-Q065 (bef87e6): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
// Subroutine sub_0000C32C - Address 0x0000C32C
static s32 _sceKernelVplWakeupWaiters(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc __attribute__((unused)),
    s32 funcId __attribute__((unused)), va_list ap __attribute__((unused)))
{
    s32 numWokenUp;
    void *addr;
    SceThreadmanVpl *vpl;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;

    numWokenUp = 0;
    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    vpl = UID_CB_TO_DATA(uid, g_threadman.vplType, SceThreadmanVpl);
    if (waitQ->numWaitThreads <= 0)
        return 0;

    next = (SceThreadmanThread *)waitQ->waitList.next;
    while (next != (SceThreadmanThread *)&waitQ->waitList) {
        thread = next;
        next = (SceThreadmanThread *)thread->link.next;
        addr = sceKernelAllocHeapMemory(vpl->heapId, thread->waitData[1]);
        if (addr != NULL) {
            thread->waitData[0] = (s32)addr;
            if (sub_00000924(thread) > 0)
                numWokenUp++;
        } else if (!(waitQ->attr & SCE_KERNEL_VPA_PASS))
            break;
    }
    return numWokenUp;
}
