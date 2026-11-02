/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/fpl.c
 *
 * Fixed-length memory pools (original address range 0x0000C408 - 0x0000D574).
 *
 * A fixed-length memory pool is a "Fpl" UID object which inherits the "WaitQ" type. Its memory is a
 * partition memory block split into blocks of the same size; the free blocks are kept in a circular
 * list.
 *
 * The free list functions of the TLS pools (tlspl.c) are also here.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <sysmem_user.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid fixed-length memory pool attributes. */
#define SCE_THREADMAN_FPL_ATTR_MASK     (0x41FF)

static s32 _sceKernelAllocateFpl(SceUID uid, void **data, unsigned int *timeout, s32 withCallbacks);
static s32 _sceKernelFplFreeBlock(SceThreadmanFpl *fpl, s32 index);
static s32 _sceKernelFplAllocBlock(SceThreadmanFpl *fpl);
static s32 _sceKernelFplInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelFplDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019DA0
SceSysmemUidLookupFunc g_FplFuncs[] = {
    { 0xD310D2D9, _sceKernelFplInitialize },
    { 0x87089863, _sceKernelFplDelete },
    { 0, NULL }
};

/**
 * Create a fixed-length memory pool.
 *
 * @param name The name of the pool.
 * @param part The memory partition to allocate the pool from.
 * @param attr The attributes (SCE_KERNEL_FPA_*).
 * @param size The size of a block.
 * @param blocks The number of blocks.
 * @param opt The option parameter (can be NULL), specifying the alignment of the blocks.
 *
 * @return The UID of the pool on success, < 0 on error.
 */
// Subroutine sceKernelCreateFpl - Address 0x0000C408
int sceKernelCreateFpl(const char *name, int part, int attr, unsigned int size, unsigned int blocks,
    SceKernelFplOptParam *opt)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 i;
    u32 type;
    SceSize alignment;
    SceSize blockSize;
    SceUID blockId;
    void *base;
    SceThreadmanFplLink *links;
    SceSysmemUidCB *uid;
    SceThreadmanFpl *fpl;
    SceThreadmanWaitQ *waitQ;
    SceSysmemPartitionInfo partInfo;
    SceKernelFplOptParam *option;

    option = &g_threadman.statusBuf.fplOption;
    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(opt, sizeof(*opt))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_FPL_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if ((s32)blocks <= 0 || size == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMSIZE;
    }

    alignment = 0;
    intr = sceKernelCpuSuspendIntr();

    option->size = 0;
    option->alignment = 0;
    if (opt != NULL) {
        memcpy(option, opt, (opt->size > sizeof(*option)) ? sizeof(*option) : opt->size);
        alignment = option->alignment;
    }

    type = (attr >> 14) & 1; /* SCE_KERNEL_SMEM_High with SCE_KERNEL_FPA_HIGHMEM */
    blockSize = (size + 3) & ~3;
    if (alignment != 0) {
        if ((alignment & (alignment - 1)) != 0) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT;
        }
        if (alignment < 4)
            alignment = 4;
        blockSize = (size + alignment - 1) & ~(alignment - 1);
        type = (attr & SCE_KERNEL_FPA_HIGHMEM) ? SCE_KERNEL_SMEM_HIGHALIGNED : SCE_KERNEL_SMEM_LOWALIGNED;
    }
    /* The size of the pool must fit in 32 bits. */
    if (blockSize == 0 || (((u64)blockSize * (s32)blocks) >> 32) != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMSIZE;
    }

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

    /* The links of the first two blocks are stored in the pool itself. */
    links = NULL;
    if ((s32)blocks >= 3) {
        links = sceKernelAllocHeapMemory(g_threadman.heapId, blocks * sizeof(*links));
        if (links == NULL) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_NO_MEMORY;
        }
        sceKernelMemset32(links, 0, blocks * sizeof(*links));
    }

    blockId = sceKernelAllocPartitionMemory(part, (char *)name, type, blockSize * blocks, alignment);
    if (blockId <= 0) {
        if (links != NULL)
            sceKernelFreeHeapMemory(g_threadman.heapId, links);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }
    base = sceKernelGetBlockHeadAddr(blockId);

    status = sceKernelCreateUID(g_threadman.fplType, name, (pspGetK1() >> 31) | attr, &uid);
    if (status != SCE_ERROR_OK) {
        if (links != NULL)
            sceKernelFreeHeapMemory(g_threadman.heapId, links);
        sceKernelFreePartitionMemory(blockId);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    fpl = UID_CB_TO_DATA(uid, g_threadman.fplType, SceThreadmanFpl);
    status = uid->uid;
    fpl->base = base;
    waitQ->attr = attr;
    if (links == NULL) {
        fpl->inlineLinks[0].next = NULL;
        fpl->inlineLinks[1].next = NULL;
        links = fpl->inlineLinks;
    }
    fpl->blockSize = blockSize;
    fpl->links = links;
    fpl->size = size;
    fpl->option = opt;
    fpl->blockId = blockId;
    fpl->numBlocks = blocks;
    fpl->lastFree = NULL;
    for (i = 0; i < (s32)blocks; i++)
        _sceKernelFplFreeBlock(fpl, i);
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_FPL, 1, status);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Delete a fixed-length memory pool. The threads waiting on it are released with
 * SCE_ERROR_KERNEL_WAIT_DELETE.
 *
 * @param uid The UID of the pool.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteFpl - Address 0x0000C82C
int sceKernelDeleteFpl(SceUID uid)
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_FPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_FPL, 1, uid);
    sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Allocate a block from a fixed-length memory pool, waiting until one is available.
 *
 * @param uid The UID of the pool.
 * @param data Receives the address of the block.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelAllocateFpl - Address 0x0000C940
int sceKernelAllocateFpl(SceUID uid, void **data, unsigned int *timeout)
{
    return _sceKernelAllocateFpl(uid, data, timeout, 0);
}

/**
 * Allocate a block from a fixed-length memory pool, waiting until one is available and running the
 * callbacks notified meanwhile.
 *
 * @param uid The UID of the pool.
 * @param data Receives the address of the block.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelAllocateFplCB - Address 0x0000C948
int sceKernelAllocateFplCB(SceUID uid, void **data, unsigned int *timeout)
{
    return _sceKernelAllocateFpl(uid, data, timeout, 1);
}

/*
 * Common part of sceKernelAllocateFpl() and sceKernelAllocateFplCB().
 * Note: the original passes withCallbacks in $t3.
 */
// Subroutine sub_0000C950 - Address 0x0000C950
static s32 _sceKernelAllocateFpl(SceUID uid, void **data, unsigned int *timeout, s32 withCallbacks)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 index;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanFpl *fpl;
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_FPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    fpl = UID_CB_TO_DATA(cb, g_threadman.fplType, SceThreadmanFpl);
    if (withCallbacks) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_ALLOCATE_FPL_CB, 1, uid);
    } else {
        THREADMAN_LOG(SCE_THREADMAN_LOG_ALLOCATE_FPL, 1, uid);
    }

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    do {
        if (withCallbacks && thread->callbackNotified != 0) {
            /* Run the pending callbacks; they might have deleted the pool. */
            sub_00002170(intr);
            if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_WAIT_DELETE;
            }
        }
        if (fpl->lastFree != NULL) {
            index = _sceKernelFplAllocBlock(fpl);
            status = SCE_ERROR_OK;
            *data = fpl->base + fpl->blockSize * index;
            break;
        }
        thread->waitWithCallbacks = withCallbacks;
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_FPL, waitQ->attr, &timeoutClock, (SceUInt *)timeout);
        if (status == SCE_ERROR_OK)
            *data = (void *)thread->waitData[0];
    } while (withCallbacks && status == (s32)SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED_CALLBACK);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Allocate a block from a fixed-length memory pool without waiting.
 *
 * @param uid The UID of the pool.
 * @param data Receives the address of the block.
 *
 * @return 0 on success, SCE_ERROR_KERNEL_NO_MEMORY if no block is free, < 0 on error.
 */
// Subroutine sceKernelTryAllocateFpl - Address 0x0000CC1C
int sceKernelTryAllocateFpl(SceUID uid, void **data)
{
    s32 oldK1;
    s32 intr;
    s32 index;
    SceSysmemUidCB *cb;
    SceThreadmanFpl *fpl;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(data, 4)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_FPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    /* Note: the waiting threads can be overtaken. */
    fpl = UID_CB_TO_DATA(cb, g_threadman.fplType, SceThreadmanFpl);
    if (fpl->lastFree == NULL) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_ALLOCATE_FPL, 2, uid, 0);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }
    index = _sceKernelFplAllocBlock(fpl);
    *data = fpl->base + fpl->blockSize * index;
    THREADMAN_LOG(SCE_THREADMAN_LOG_TRY_ALLOCATE_FPL, 2, uid, index);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Free a block of a fixed-length memory pool. The block is given to the first waiting thread, if any.
 *
 * @param uid The UID of the pool.
 * @param data The address of the block.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelFreeFpl - Address 0x0000CDBC
int sceKernelFreeFpl(SceUID uid, void *data)
{
    s32 oldK1;
    s32 intr;
    s32 index;
    SceSysmemUidCB *cb;
    SceThreadmanFpl *fpl;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_FPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    fpl = UID_CB_TO_DATA(cb, g_threadman.fplType, SceThreadmanFpl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    /* The address must be the start of an allocated block of the pool. */
    if (data == NULL || (u8 *)data < fpl->base || (u8 *)data >= fpl->base + fpl->blockSize * fpl->numBlocks) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK;
    }
    index = (s32)((u8 *)data - fpl->base) / (s32)fpl->blockSize;
    if ((u8 *)data != fpl->base + fpl->blockSize * index || fpl->links[index].next != NULL) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK;
    }
    THREADMAN_LOG(SCE_THREADMAN_LOG_FREE_FPL, 2, uid, data);

    if (waitQ->numWaitThreads > 0) {
        /* Give the block to the first waiting thread. */
        thread = (SceThreadmanThread *)waitQ->waitList.next;
        thread->waitData[0] = (s32)data;
        sub_00000B08(cb, SCE_ERROR_OK, 0, intr);
    } else
        _sceKernelFplFreeBlock(fpl, index);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Cancel the waits on a fixed-length memory pool. The waiting threads are released with
 * SCE_ERROR_KERNEL_WAIT_CANCEL.
 *
 * @param uid The UID of the pool.
 * @param pnum Receives the number of threads released (can be NULL).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelCancelFpl - Address 0x0000CFE0
int sceKernelCancelFpl(SceUID uid, int *pnum)
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

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_FPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_CANCEL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CANCEL_FPL, 2, uid, pnum);
    numWaitThreads = sceKernelCallUIDObjFunction(cb, SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL);
    if (pnum != NULL)
        *pnum = numWaitThreads;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a fixed-length memory pool.
 *
 * @param uid The UID of the pool.
 * @param info Receives the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferFplStatus - Address 0x0000D118
int sceKernelReferFplStatus(SceUID uid, SceKernelFplInfo *info)
{
    s32 oldK1;
    s32 intr;
    u32 *ptr;
    SceSysmemUidCB *cb;
    SceThreadmanFpl *fpl;
    SceThreadmanWaitQ *waitQ;
    SceKernelFplInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.fplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_FPOOL;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_FPL_STATUS, 2, uid, info);

    fpl = UID_CB_TO_DATA(cb, g_threadman.fplType, SceThreadmanFpl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.fplInfo;
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->attr = waitQ->attr;
    buf->blockSize = fpl->size;
    buf->numBlocks = fpl->numBlocks;
    buf->freeBlocks = fpl->numFreeBlocks;
    buf->numWaitThreads = waitQ->numWaitThreads;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * Put a block in the free list.
 *
 * @return 0 on success, -1 if the block is already free.
 */
// Subroutine sub_0000D2E4 - Address 0x0000D2E4
static s32 _sceKernelFplFreeBlock(SceThreadmanFpl *fpl, s32 index)
{
    SceThreadmanFplLink *link;
    SceThreadmanFplLink *last;

    link = &fpl->links[index];
    if (link->next != NULL)
        return -1;
    fpl->numFreeBlocks++;
    last = fpl->lastFree;
    if (last == NULL) {
        fpl->lastFree = link;
        link->next = link;
    } else {
        fpl->lastFree = link;
        link->next = last->next;
        last->next = link;
    }
    return 0;
}

/*
 * Put a block of a TLS pool in its free list (the same as _sceKernelFplFreeBlock()).
 *
 * @return 0 on success, -1 if the block is already free.
 */
// Subroutine sub_0000D338 - Address 0x0000D338
s32 _sceKernelTlsplFreeBlock(SceThreadmanTlspl *tlspl, s32 index)
{
    SceThreadmanFplLink *link;
    SceThreadmanFplLink *last;

    link = &tlspl->links[index];
    if (link->next != NULL)
        return -1;
    tlspl->numFreeBlocks++;
    last = tlspl->lastFree;
    if (last == NULL) {
        tlspl->lastFree = link;
        link->next = link;
    } else {
        tlspl->lastFree = link;
        link->next = last->next;
        last->next = link;
    }
    return 0;
}

/*
 * Take the first block of the free list.
 *
 * @return The index of the block, or -1 if no block is free.
 */
// Subroutine sub_0000D38C - Address 0x0000D38C
static s32 _sceKernelFplAllocBlock(SceThreadmanFpl *fpl)
{
    SceThreadmanFplLink *last;
    SceThreadmanFplLink *first;

    last = fpl->lastFree;
    if (last == NULL)
        return -1;
    fpl->numFreeBlocks--;
    first = last->next;
    if (last == first)
        last = NULL;
    else
        last->next = first->next;
    fpl->lastFree = last;
    first->next = NULL;
    return first - fpl->links;
}

/*
 * Take the first block of the free list of a TLS pool (the same as _sceKernelFplAllocBlock()).
 *
 * @return The index of the block, or -1 if no block is free.
 */
// Subroutine sub_0000D3DC - Address 0x0000D3DC
s32 _sceKernelTlsplAllocBlock(SceThreadmanTlspl *tlspl)
{
    SceThreadmanFplLink *last;
    SceThreadmanFplLink *first;

    last = tlspl->lastFree;
    if (last == NULL)
        return -1;
    tlspl->numFreeBlocks--;
    first = last->next;
    if (last == first)
        last = NULL;
    else
        last->next = first->next;
    tlspl->lastFree = last;
    first->next = NULL;
    return first - tlspl->links;
}

/*
 * Find the fixed-length memory pool using a given partition memory block.
 *
 * @return The UID of the pool, or -1 if not found.
 */
// Subroutine sub_0000D42C - Address 0x0000D42C
SceUID _sceKernelGetFplByBlockId(SceUID blockId)
{
    SceSysmemUidCB *cb;

    for (cb = g_threadman.fplType->PARENT0; cb != g_threadman.fplType; cb = cb->PARENT0) {
        if (UID_CB_TO_DATA(cb, g_threadman.fplType, SceThreadmanFpl)->blockId == blockId)
            return cb->uid;
    }
    return -1;
}

/**
 * Register the "Fpl" UID type.
 *
 * @return 0.
 */
// Subroutine sub_0000D474 - Address 0x0000D474
s32 _sceKernelFplInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Fpl", 0x30, g_FplFuncs, NULL, &g_threadman.fplType);
    return SCE_ERROR_OK;
}

// Subroutine sub_0000D4B8 - Address 0x0000D4B8
static s32 _sceKernelFplInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_0000D4E0 - Address 0x0000D4E0
static s32 _sceKernelFplDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanFpl *fpl;

    fpl = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanFpl);
    if (fpl->links != fpl->inlineLinks)
        sceKernelFreeHeapMemory(g_threadman.heapId, fpl->links);
    sceKernelFreePartitionMemory(fpl->blockId);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
