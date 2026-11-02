/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/tlspl.c
 *
 * TLS pools (original address range 0x0000D574 - 0x0000E5EC).
 *
 * A TLS (thread-local storage) pool is a "Tlspl" UID object which inherits the "WaitQ" type. It is a
 * fixed-length memory pool (see fpl.c) from which each thread gets at most one block. Up to 16 TLS pools
 * can exist; the slot of a pool is stored in its UID, and the block of a thread is stored at that slot in
 * the area pointed to by its $k0 register, so that sceKernelGetTlsAddr() (usersystemlib) can find it
 * without a syscall.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <sysmem_user.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/** The valid TLS pool attributes. */
#define SCE_THREADMAN_TLSPL_ATTR_MASK   (0x41FF)

static s32 _sceKernelTlsplCheckBlock(SceThreadmanTlspl *tlspl, void *data);
static s32 _sceKernelTlsplInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelTlsplDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

// 0x00019DB8
SceSysmemUidLookupFunc g_TlsplFuncs[] = {
    { 0xD310D2D9, _sceKernelTlsplInitialize },
    { 0x87089863, _sceKernelTlsplDelete },
    { 0, NULL }
};

/**
 * Create a TLS pool. Each thread gets its own block of the pool with _sceKernelAllocateTlspl().
 *
 * @param name The name of the pool.
 * @param part The memory partition to allocate the pool from.
 * @param attr The attributes (SCE_KERNEL_TLSPA_*).
 * @param size The size of a block.
 * @param blocks The number of blocks.
 * @param opt The option parameter (can be NULL), specifying the alignment of the blocks.
 *
 * @return The UID of the pool on success, < 0 on error.
 */
// Subroutine sceKernelCreateTlspl - Address 0x0000D574
SceUID sceKernelCreateTlspl(const char *name, int part, int attr, unsigned int size, unsigned int blocks,
    SceKernelTlsplOptParam *opt)
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
    SceThreadmanTlspl *tlspl;
    s32 slot;
    SceThreadmanWaitQ *waitQ;
    SceSysmemPartitionInfo partInfo;
    SceKernelTlsplOptParam *option;

    option = &g_threadman.statusBuf.tlsplOption;
    oldK1 = pspShiftK1();
    if (!pspK1PtrOk(name) || !pspK1StaBufOk(opt, sizeof(*opt))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((attr & ~SCE_THREADMAN_TLSPL_ATTR_MASK) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if ((s32)blocks <= 0 || size == 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_MEMSIZE;
    }

    alignment = 0;
    intr = sceKernelCpuSuspendIntr();

    /* Find a free slot. */
    for (slot = 0; slot < SCE_THREADMAN_NUM_TLSPL; slot++) {
        if (g_threadman.tlsplUids[slot] == 0)
            break;
    }
    if (slot == SCE_THREADMAN_NUM_TLSPL) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UTLS_FULL;
    }

    option->size = 0;
    option->alignment = 0;
    if (opt != NULL) {
        memcpy(option, opt, (opt->size > sizeof(*option)) ? sizeof(*option) : opt->size);
        alignment = option->alignment;
    }

    type = (attr >> 14) & 1; /* SCE_KERNEL_SMEM_High with SCE_KERNEL_TLSPA_HIGHMEM */
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
        type = (attr & SCE_KERNEL_TLSPA_HIGHMEM) ? SCE_KERNEL_SMEM_HIGHALIGNED : SCE_KERNEL_SMEM_LOWALIGNED;
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

    status = sceKernelCreateUID(g_threadman.tlsplType, name, (pspGetK1() >> 31) | attr, &uid);
    if (status != SCE_ERROR_OK) {
        if (links != NULL)
            sceKernelFreeHeapMemory(g_threadman.heapId, links);
        sceKernelFreePartitionMemory(blockId);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return status;
    }

    waitQ = UID_CB_TO_DATA(uid, g_threadman.waitQType, SceThreadmanWaitQ);
    tlspl = UID_CB_TO_DATA(uid, g_threadman.tlsplType, SceThreadmanTlspl);
    /* Store the slot in the UID. */
    uid->uid = (uid->uid & ~0x78) | ((slot & 0xF) << 3);
    status = uid->uid;
    tlspl->base = base;
    waitQ->attr = attr;
    if (links == NULL) {
        tlspl->inlineLinks[0].next = NULL;
        tlspl->inlineLinks[1].next = NULL;
        links = tlspl->inlineLinks;
    }
    tlspl->blockSize = blockSize;
    tlspl->links = links;
    tlspl->size = size;
    tlspl->option = opt;
    tlspl->blockId = blockId;
    tlspl->numBlocks = blocks;
    tlspl->lastFree = NULL;
    for (i = 0; i < (s32)blocks; i++)
        _sceKernelTlsplFreeBlock(tlspl, i);
    g_threadman.tlsplUids[slot] = status;
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_TLSPL, 1, status);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Delete a TLS pool. The threads waiting on it are released with SCE_ERROR_KERNEL_WAIT_DELETE.
 * The pool cannot be deleted while a block is allocated, except the block of the current thread.
 *
 * @param uid The UID of the pool.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteTlspl - Address 0x0000DA10
int sceKernelDeleteTlspl(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 slot;
    s32 index;
    void *block;
    SceSysmemUidCB *cb;
    SceThreadmanTlspl *tlspl;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    slot = THREADMAN_TLSPL_SLOT(uid);
    if (g_threadman.tlsplUids[slot] != uid
        || sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.tlsplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UNKNOWN_TLSPLID;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    tlspl = UID_CB_TO_DATA(cb, g_threadman.tlsplType, SceThreadmanTlspl);
    if (tlspl->numFreeBlocks < tlspl->numBlocks) {
        /* Only the block of the current thread can be in use. */
        block = NULL;
        if (tlspl->numFreeBlocks == tlspl->numBlocks - 1) {
            thread = g_threadman.curThread;
            block = thread->k0->tls[slot];
            if (block != NULL) {
                index = _sceKernelTlsplCheckBlock(tlspl, block);
                if (index < 0) {
                    sceKernelCpuResumeIntr(intr);
                    pspSetK1(oldK1);
                    return index;
                }
                thread->k0->tls[slot] = NULL;
                sceKernelMemset32(block, 0, tlspl->blockSize);
            }
        }
        if (block == NULL) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_UTLS_BUSY;
        }
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_TLSPL, 1, uid);
    g_threadman.tlsplUids[slot] = 0;
    sceKernelDeleteUID(uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/**
 * Get the block of the current thread in a TLS pool, allocating it (and waiting until one is available)
 * if needed. A newly allocated block is zeroed.
 *
 * @param uid The UID of the pool.
 * @param data Receives the address of the block.
 * @param timeout The timeout in microseconds (NULL for none), receiving the remaining time.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine _sceKernelAllocateTlspl - Address 0x0000DC10
int _sceKernelAllocateTlspl(SceUID uid, void **data, unsigned int *timeout)
{
    s32 oldK1;
    s32 intr;
    s32 status;
    s32 slot;
    s32 index;
    void *block;
    SceKernelSysClock timeoutClock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanTlspl *tlspl;
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

    slot = THREADMAN_TLSPL_SLOT(uid);
    if (g_threadman.tlsplUids[slot] != uid) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UNKNOWN_TLSPLID;
    }

    sub_00016FA4();

    if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.tlsplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UNKNOWN_TLSPLID;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    /* The thread already has a block. */
    block = thread->k0->tls[slot];
    if (block != NULL) {
        if (!pspK1PtrOk(block)) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
        }
        *data = block;
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_OK;
    }

    tlspl = UID_CB_TO_DATA(cb, g_threadman.tlsplType, SceThreadmanTlspl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    THREADMAN_LOG(SCE_THREADMAN_LOG_ALLOCATE_TLSPL, 1, uid);

    if (timeout != NULL)
        sub_00010D34(&timeoutClock, *timeout);

    if (tlspl->lastFree != NULL) {
        index = _sceKernelTlsplAllocBlock(tlspl);
        status = SCE_ERROR_OK;
        block = tlspl->base + tlspl->blockSize * index;
    } else {
        thread->waitWithCallbacks = 0;
        status = sub_00000908(cb, SCE_THREADMAN_WAIT_TLSPL, waitQ->attr, &timeoutClock, (SceUInt *)timeout);
        if (status != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return status;
        }
        block = (void *)thread->waitData[0];
    }
    thread->k0->tls[slot] = block;
    *data = block;
    sceKernelMemset32(block, 0, tlspl->blockSize);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return status;
}

/**
 * Free the block of the current thread in a TLS pool. The block is given to the first waiting thread, if
 * any.
 *
 * @param uid The UID of the pool.
 *
 * @return 0 on success (also if the thread has no block), < 0 on error.
 */
// Subroutine sceKernelFreeTlspl - Address 0x0000DEC8
int sceKernelFreeTlspl(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    s32 slot;
    s32 index;
    void *block;
    SceSysmemUidCB *cb;
    SceThreadmanTlspl *tlspl;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();

    slot = THREADMAN_TLSPL_SLOT(uid);
    if (g_threadman.tlsplUids[slot] != uid
        || sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.tlsplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UNKNOWN_TLSPLID;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = g_threadman.curThread;
    block = thread->k0->tls[slot];
    if (block == NULL) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_OK;
    }
    if (!pspK1PtrOk(block)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    tlspl = UID_CB_TO_DATA(cb, g_threadman.tlsplType, SceThreadmanTlspl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    index = _sceKernelTlsplCheckBlock(tlspl, block);
    if (index < 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return index;
    }
    thread->k0->tls[slot] = NULL;
    THREADMAN_LOG(SCE_THREADMAN_LOG_FREE_TLSPL, 2, uid, block);

    if (waitQ->numWaitThreads > 0) {
        /* Give the block to the first waiting thread. */
        thread = (SceThreadmanThread *)waitQ->waitList.next;
        thread->waitData[0] = (s32)block;
        sub_00000B08(cb, SCE_ERROR_OK, 0, intr);
    } else {
        sceKernelMemset32(block, 0, tlspl->blockSize);
        _sceKernelTlsplFreeBlock(tlspl, index);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * Free the TLS blocks of a thread (when it terminates). Each block is given to the first thread waiting on
 * its pool, if any.
 *
 * @return 0.
 */
// Subroutine sub_0000E0D4 - Address 0x0000E0D4
s32 _sceKernelReleaseThreadTls(SceThreadmanThread *thread)
{
    s32 intr;
    s32 slot;
    s32 index;
    SceUID uid;
    void *block;
    SceSysmemUidCB *cb;
    SceThreadmanTlspl *tlspl;
    SceThreadmanWaitQ *waitQ;
    SceThreadmanThread *waitThread;

    if (thread->k0 == NULL)
        return SCE_ERROR_OK;

    intr = sceKernelCpuSuspendIntr();
    for (slot = 0; slot < SCE_THREADMAN_NUM_TLSPL; slot++) {
        block = thread->k0->tls[slot];
        thread->k0->tls[slot] = NULL;
        uid = g_threadman.tlsplUids[slot];
        if (uid == 0 || THREADMAN_TLSPL_SLOT(uid) != slot || block == NULL)
            continue;
        if (sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.tlsplType, &cb) != SCE_ERROR_OK)
            continue;
        tlspl = UID_CB_TO_DATA(cb, g_threadman.tlsplType, SceThreadmanTlspl);
        waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
        index = _sceKernelTlsplCheckBlock(tlspl, block);
        if (index < 0)
            continue;
        if (waitQ->numWaitThreads > 0) {
            waitThread = (SceThreadmanThread *)waitQ->waitList.next;
            waitThread->waitData[0] = (s32)block;
            sub_00000B08(cb, SCE_ERROR_OK, 0, intr);
        } else {
            sceKernelMemset32(block, 0, tlspl->blockSize);
            _sceKernelTlsplFreeBlock(tlspl, index);
        }
    }
    sceKernelCpuResumeIntr(intr);
    return SCE_ERROR_OK;
}

/**
 * Get the status of a TLS pool.
 *
 * @param uid The UID of the pool.
 * @param info Receives the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferTlsplStatus - Address 0x0000E23C
int sceKernelReferTlsplStatus(SceUID uid, SceKernelTlsplInfo *info)
{
    s32 oldK1;
    s32 intr;
    s32 slot;
    u32 *ptr;
    SceSysmemUidCB *cb;
    SceThreadmanTlspl *tlspl;
    SceThreadmanWaitQ *waitQ;
    SceKernelTlsplInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    slot = THREADMAN_TLSPL_SLOT(uid);
    if (g_threadman.tlsplUids[slot] != uid
        || sceKernelGetUIDcontrolBlockWithType(uid, g_threadman.tlsplType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UNKNOWN_TLSPLID;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_TLSPL_STATUS, 2, uid, info);

    tlspl = UID_CB_TO_DATA(cb, g_threadman.tlsplType, SceThreadmanTlspl);
    waitQ = UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ);
    buf = &g_threadman.statusBuf.tlsplInfo;
    for (ptr = (u32 *)buf; ptr != (u32 *)(buf + 1); ptr++)
        *ptr = 0;
    buf->size = sizeof(*buf);
    if (cb->name != NULL)
        strncpy(buf->name, cb->name, SCE_UID_NAME_LEN);
    buf->index = slot;
    buf->attr = waitQ->attr;
    buf->blockSize = tlspl->size;
    buf->totalBlocks = tlspl->numBlocks;
    buf->freeBlocks = tlspl->numFreeBlocks;
    buf->numWaitThreads = waitQ->numWaitThreads;
    memcpy(info, buf, (buf->size < info->size) ? buf->size : info->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * Check that an address is the start of an allocated block of a TLS pool.
 *
 * @return The index of the block, or SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK.
 */
// Subroutine sub_0000E424 - Address 0x0000E424
static s32 _sceKernelTlsplCheckBlock(SceThreadmanTlspl *tlspl, void *data)
{
    u32 index;

    if ((u8 *)data < tlspl->base || (u8 *)data >= tlspl->base + tlspl->blockSize * tlspl->numBlocks)
        return SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK;
    index = (u32)((u8 *)data - tlspl->base) / tlspl->blockSize;
    if ((u8 *)data != tlspl->base + tlspl->blockSize * index)
        return SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK;
    if (tlspl->links[index].next != NULL)
        return SCE_ERROR_KERNEL_ILLEGAL_MEMBLOCK;
    return index;
}

/*
 * Find the TLS pool using a given partition memory block.
 *
 * @return The UID of the pool, or -1 if not found.
 */
// Subroutine sub_0000E4A4 - Address 0x0000E4A4
SceUID _sceKernelGetTlsplByBlockId(SceUID blockId)
{
    SceSysmemUidCB *cb;

    for (cb = g_threadman.tlsplType->PARENT0; cb != g_threadman.tlsplType; cb = cb->PARENT0) {
        if (UID_CB_TO_DATA(cb, g_threadman.tlsplType, SceThreadmanTlspl)->blockId == blockId)
            return cb->uid;
    }
    return -1;
}

/**
 * Register the "Tlspl" UID type.
 *
 * @return 0.
 */
// Subroutine sub_0000E4EC - Address 0x0000E4EC
s32 _sceKernelTlsplInit(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Tlspl", 0x30, g_TlsplFuncs, NULL, &g_threadman.tlsplType);
    return SCE_ERROR_OK;
}

// Subroutine sub_0000E530 - Address 0x0000E530
static s32 _sceKernelTlsplInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return uid->uid;
}

// Subroutine sub_0000E558 - Address 0x0000E558
static s32 _sceKernelTlsplDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanTlspl *tlspl;

    tlspl = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanTlspl);
    if (tlspl->links != tlspl->inlineLinks)
        sceKernelFreeHeapMemory(g_threadman.heapId, tlspl->links);
    sceKernelFreePartitionMemory(tlspl->blockId);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
