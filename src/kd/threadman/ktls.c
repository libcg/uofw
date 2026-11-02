/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/ktls.c
 *
 * Kernel thread local storage (original address range 0x00011A24 - 0x00011F68).
 *
 * A KTLS allocation reserves one of 16 slots; every thread can then get its own zeroed block of the
 * allocation's size, allocated on demand from the threadman heap and stored in the thread control block.
 * The blocks of a thread are released (through the allocation's destructor) when the thread is deleted.
 */

#include <common_imp.h>
#include <interruptman.h>
#include <loadcore.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

#include "threadman_int.h"

/**
 * Allocate a KTLS slot.
 *
 * @param size The size of the per-thread block.
 * @param destructor Called with the block of a thread (and arg) when the thread is deleted. Can be NULL.
 * @param arg The user argument passed to destructor.
 *
 * @return The allocation id (0 - 15) on success, < 0 on error.
 */
// Subroutine sceKernelAllocateKTLS - Address 0x00011A24
s32 sceKernelAllocateKTLS(s32 size, void (*destructor)(void *ktls, void *arg), void *arg)
{
    s32 oldK1;
    s32 intr;
    s32 id;
    SceThreadmanKtls *ktls;

    if (THREADMAN_IS_INTR_CONTEXT())
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;

    oldK1 = pspShiftK1();
    intr = sceKernelCpuSuspendIntr();

    for (id = 0; id < SCE_THREADMAN_NUM_KTLS; id++) {
        ktls = &g_threadman.ktls[id];
        if (ktls->used == 0) {
            ktls->numThreads = 0;
            ktls->size = size;
            ktls->destructor = destructor;
            ktls->arg = arg;
            ktls->gp = sceKernelGetModuleGPByAddressForKernel((u32)destructor);
            if (ktls->gp == 0)
                ktls->gp = threadmanGetGp();
            ktls->used = 1;

            THREADMAN_LOG(SCE_THREADMAN_LOG_ALLOCATE_KTLS, 4, size, destructor, arg, id);

            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return id;
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_KERNEL_KTLS_IS_FULL;
}

/**
 * Free a KTLS slot. No thread may have a block of it anymore.
 *
 * @param id The allocation id returned by sceKernelAllocateKTLS().
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelFreeKTLS - Address 0x00011B5C
s32 sceKernelFreeKTLS(s32 id)
{
    s32 oldK1;
    s32 intr;
    SceThreadmanKtls *ktls;

    if (THREADMAN_IS_INTR_CONTEXT())
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    if ((u32)id >= SCE_THREADMAN_NUM_KTLS)
        return SCE_ERROR_KERNEL_ILLEGAL_KTLS;

    oldK1 = pspShiftK1();
    intr = sceKernelCpuSuspendIntr();

    THREADMAN_LOG(SCE_THREADMAN_LOG_FREE_KTLS, 1, id);

    ktls = &g_threadman.ktls[id];
    if (ktls->used == 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_KTLS;
    }
    if (ktls->numThreads != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_KTLS_IS_BUSY;
    }
    ktls->used = 0;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q040: Separate ifs for the early checks (interrupt context, then the KTLS id): the original gives each check its
 * own branch.
 */
/*
 * PARITY-Q118: the current thread's uid is read through g_threadman.curThread before thread is assigned
 *     (thid = g_threadman.curThread->uid; thread = g_threadman.curThread;). CSE shares the load, and the
 *     original's copy (lw a1 / lw 0x8(a1) / move a0, a1) appears. csim +0.253, equiv -> regalloc. A local copy
 *     (cur = g_threadman.curThread; thid = cur->uid; thread = cur;) matches the same way; this form is kept
 *     because it is closer to the original statement order. Residue: thid/ktls get $s2/$s3 swapped. Declaration order
 *     and dropping the ktls variable do not fix it (the latter is down).
 */
/**
 * Get the KTLS block of a thread.
 *
 * @param id The allocation id returned by sceKernelAllocateKTLS().
 * @param thid The UID of the thread, or 0 for the current thread.
 * @param alloc When non-zero, the block is allocated (and zeroed) if the thread does not have one yet.
 *
 * @return The block, or NULL on error (or if the thread has no block and alloc is zero).
 */
/*
 * PARITY-Q188: The allocation path is predicted taken (__builtin_expect). The higher block frequency of its ktls uses
 *     gives ktls a higher global-allocation priority than thid, so ktls gets $s2 and thid $s3 as in the original.
 */
// Subroutine sceKernelGetThreadKTLS - Address 0x00011C68
void *sceKernelGetThreadKTLS(s32 id, SceUID thid, s32 alloc)
{
    s32 oldK1;
    s32 intr;
    void *block;
    SceSysmemUidCB *cb;
    SceThreadmanKtls *ktls;
    SceThreadmanThread *thread;

    if (THREADMAN_IS_INTR_CONTEXT())
        return NULL;
    if ((u32)id >= SCE_THREADMAN_NUM_KTLS)
        return NULL;

    oldK1 = pspShiftK1();
    intr = sceKernelCpuSuspendIntr();

    if (thid == 0) {
        thid = g_threadman.curThread->uid;
        thread = g_threadman.curThread;
    } else {
        if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return NULL;
        }
        thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    }

    ktls = &g_threadman.ktls[id];
    if (ktls->used == 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return NULL;
    }

    block = thread->ktls[id];
    if (block != NULL) {
        THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_KTLS, 4, id, thid, alloc, block);
    } else if (__builtin_expect(alloc != 0, 1)) { /* PARITY-Q188 */
        block = sceKernelAllocHeapMemory(g_threadman.heapId, ktls->size);
        THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_KTLS, 4, id, thid, alloc, block);
        if (block == NULL) {
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return NULL;
        }
        /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
        memset(block, 0, ktls->size);
        thread->ktls[id] = block;
        ktls->numThreads++;
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return block;
}

/*
 * PARITY-Q005 (3d248ac): Uses volatile $gp swap around handler calls.
 */
/**
 * Release the KTLS blocks of a thread being deleted, calling the destructor of their allocation.
 * A destructor can only be called in the context of the thread itself: the blocks which have a destructor
 * are kept when the thread is not the current one.
 *
 * @param thread The thread.
 *
 * @return 1 if some blocks were kept, 0 otherwise.
 */
// Subroutine sub_00011E3C - Address 0x00011E3C
s32 _sceKernelFreeThreadKTLS(SceThreadmanThread *thread)
{
    s32 kept;
    s32 id;
    u32 oldGp;
    s32 intr;
    void *block;
    SceThreadmanKtls *ktls;

    kept = 0;
    for (id = 0; id < SCE_THREADMAN_NUM_KTLS; id++) {
        block = thread->ktls[id];
        if (block == NULL)
            continue;

        ktls = &g_threadman.ktls[id];
        if (ktls->destructor != NULL) {
            if (thread != g_threadman.curThread) {
                kept = 1;
                continue;
            }
            {
                /*
                 * PARITY-Q196 (trick): register-variable pin. ktls->gp is held in $3 at local-alloc time;
                 * regrename then moves it to $v0 and the destructor to $v1 as in the original. Stock local-alloc
                 * gives the gp pseudo $2 (shorter lifetime, higher priority) and the destructor $3, which
                 * regrename swaps.
                 */
                register u32 gp asm("$3") = ktls->gp;
                oldGp = threadmanSwapGp(gp);
            }
            ktls->destructor(block, ktls->arg);
            pspSetGp(oldGp);
        }

        sceKernelFreeHeapMemory(g_threadman.heapId, block);
        intr = sceKernelCpuSuspendIntr();
        thread->ktls[id] = NULL;
        ktls->numThreads--;
        sceKernelCpuResumeIntr(intr);
    }
    return kept;
}

/**
 * Get the KTLS block of the current thread, allocating it if needed.
 *
 * @param id The allocation id returned by sceKernelAllocateKTLS().
 *
 * @return The block, or NULL on error.
 */
// Subroutine sceKernelGetKTLS - Address 0x00011F48
void *sceKernelGetKTLS(s32 id)
{
    return sceKernelGetThreadKTLS(id, 0, 1);
}
