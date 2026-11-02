/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/threadman.c
 *
 * sceThreadManager - the PSP kernel's thread manager.
 *
 * This file contains the module initialization, the threads and the scheduler. The other
 * subsystems live in their own files (e.g. semaphore.c), the context switch and exception
 * handling code which must be written in assembly in threadman_asm.S.
 */

#include <common_imp.h>
#include <exceptionman.h>
#include <interruptman.h>
#include <loadcore.h>
#include <sysmem_kdebug.h>
#include <sysmem_kernel.h>
#include <sysmem_sysclib.h>
#include <threadman_kernel.h>

/*
 * PARITY-Q016: only threadman.c sees g_threadman's 64-byte alignment (the original's 0x80/+8 offset splits appear
 * in threadman.c only)
 */
#define THREADMAN_C
#include "threadman_int.h"

SCE_MODULE_INFO(
        "sceThreadManager",
        SCE_MODULE_KERNEL |
        SCE_MODULE_ATTR_CANT_STOP | SCE_MODULE_ATTR_EXCLUSIVE_LOAD | SCE_MODULE_ATTR_EXCLUSIVE_START,
        1, 8);
SCE_MODULE_BOOTSTART("ThreadManInit");
SCE_MODULE_REBOOT_BEFORE("ThreadManRebootBefore");
SCE_SDK_VERSION(SDK_VERSION);


// 0x00019F40 (exported as ThreadManForKernel_921B7ACD)
/* Aligned on a data cache line (64 bytes), as in the original module (it follows g_sysTimerSavedCounter). */
SceThreadman g_threadman __attribute__((aligned(64)));

static s32 _sceKernelThreadInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
static s32 _sceKernelThreadDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

/* The FPU and VFPU context switch functions (0x00000000 - 0x00000680) are implemented in threadman_asm.S. */

/* Wait queues (0x00000680 - 0x00001140) are implemented in waitq.c. */

/*
 * Thread event handlers and callbacks (0x00001140 - 0x00002DF0) are implemented in callback.c and
 * callback_ret.S.
 */

/* Thread sleep, wakeup, suspend, resume, end wait and delay (0x00002DF0 - 0x00004248) are implemented in
 * threadwait.c. */

/* Semaphores (0x00004248 - 0x00005138) are implemented in semaphore.c. */


/* Event flags (0x00005138 - 0x00006284) are implemented in eventflag.c. */

/* Mutexes (0x00006284 - 0x000076D8) are implemented in mutex.c. */

/* Lightweight mutexes (0x000076D8 - 0x00008650) are implemented in lwmutex.c. */

/* Message boxes (0x00008650 - 0x00009468) are implemented in mbx.c. */

/* Message pipes (0x00009468 - 0x0000B44C) are implemented in msgpipe.c. */

/* Variable-length memory pools (0x0000B44C - 0x0000C408) are implemented in vpl.c. */

/* Fixed-length memory pools (0x0000C408 - 0x0000D574) are implemented in fpl.c. */

/* TLS pools (0x0000D574 - 0x0000E5EC) are implemented in tlspl.c. */

/* Timers and system time (0x0000E5EC - 0x00011A24) are implemented in timer.c, timer_syscall.S and timer_handler.c. */

/* KTLS (0x00011A24 - 0x00011F68) is implemented in ktls.c. */

/*
 * PARITY-Q039 (92af30a): List heads initialised with threadmanListInit (the original's store pattern).
 * PARITY-Q049 (4f79906): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q157: The idle thread context's type is set after gpr[31] and gpr[30] (store order).
 * PARITY-Q158: The init thread's stackLow is stored right after its status (store order).
 */
/**
 * Initialize the thread manager: the UID types and the data of all the subsystems, the idle and cleaner
 * system threads, and the "SceThreadmanInit" thread, which the caller of this function becomes.
 *
 * @param argc Unused.
 * @param argp Unused.
 *
 * @return 0.
 */
// Subroutine module_bootstart - Address 0x00011F68
s32 ThreadManInit(SceSize argc, const void *argp)
{
    s32 intr;
    s32 i;
    u32 gp;
    u32 sp;
    void *stack;
    SceUID blockId;
    SceUID partId;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceSysmemHeapAllocOption allocOpt;
    SceSysmemMemoryBlockInfo blockInfo;

    (void)argc;
    (void)argp;

    intr = sceKernelCpuSuspendIntr();
    gp = threadmanGetGp();
    sp = pspGetSp();

    sceKernelMemset32(&g_threadman, 0, sizeof(g_threadman));
    g_threadman.cop0State22 = pspCop0StateGet(COP0_STATE_CPUID);

    sub_00000C50(); // _sceKernelWaitQInit
    sub_00016B40();
    _sceKernelCallbackInit(); // sub_0000283C
    _sceKernelSemaInit(); // sub_00004FBC
    _sceKernelEventFlagInit(); // sub_00006120
    _sceKernelMutexInit(); // sub_0000743C
    _sceKernelLwMutexInit(); // sub_000084B4
    _sceKernelMbxInit(); // sub_000092F4
    _sceKernelMsgPipeInit(); // sub_0000B1B4
    _sceKernelVplInit(); // sub_0000C244
    _sceKernelFplInit(); // sub_0000D474
    _sceKernelTlsplInit(); // sub_0000E4EC
    _sceKernelTimerInit(); // sub_0001121C

    threadmanListInit(&g_threadman.sleepingThreads);
    threadmanListInit(&g_threadman.delayedThreads);
    threadmanListInit(&g_threadman.suspendedThreads);
    threadmanListInit(&g_threadman.deadThreadList);
    threadmanListInit(&g_threadman.dormantThreads);
    for (i = 0; i < SCE_THREADMAN_NUM_PRIORITIES; i++) {
        SceThreadmanListEntry *queue = &g_threadman.readyQueue[i];
        queue->next = queue;
        queue->prev = queue;
    }
    threadmanListInit(&g_threadman.threadEventHandlers);
    g_threadman.vfpuUserMode = -1;
    g_threadman.deadThreads.next = NULL;
    g_threadman.deadThreads.count = 0;
    g_threadman.vfpuOwner = NULL;
    g_threadman.deadThreadHandler = NULL;
    g_threadman.profilerMode = SCE_THREADMAN_PROFILER_MODE_NONE;

    /* Set up the CPU profiler. */
    if (sceKernelDipsw(23)) {
        HW(0xBC000030) |= 0x300;
        Kprintf("CPU Profiler enabled: ");
        if (sceKernelDipsw(22)) {
            g_threadman.profilerMode = SCE_THREADMAN_PROFILER_MODE_THREAD;
            Kprintf("Thread Mode\n");
        } else {
            g_threadman.profilerMode = SCE_THREADMAN_PROFILER_MODE_GLOBAL;
            Kprintf("Global Mode\n");
        }
    } else if (sceKernelIsDevelopmentToolMode() && !sceKernelDipsw(26)) {
        if (sceKernelDipsw(22))
            g_threadman.profilerMode = SCE_THREADMAN_PROFILER_MODE_THREAD_AND_GLOBAL;
        else
            g_threadman.profilerMode = SCE_THREADMAN_PROFILER_MODE_4;
    }

    g_threadman.heapId = sceKernelCreateHeap(1, 0x800, 1, "SceThreadman");

    /* Create the idle thread, running when no other thread is ready. */
    sceKernelCreateUID(g_threadman.threadType, "SceThreadmanIdle", 0, &cb);
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    thread->uid = cb->uid;
    thread->stackSize = 0x100;
    blockId = sceKernelAllocPartitionMemory(1, "stack:SceThreadmanIdle", 0, 0x100, 0);
    thread->stackBlockId = blockId;
    thread->kStackMpid = 1;
    stack = sceKernelGetBlockHeadAddr(blockId);
    *(SceUID *)stack = thread->uid;
    thread->status = SCE_THREADMAN_STATUS_READY;
    thread->mutexPriority = 0x7F;
    thread->currentPriority = 0x7F;
    thread->attr = 0;
    thread->initAttr = 0;
    thread->gp = gp;
    thread->basePriority = 0x7F;
    thread->entry = (SceKernelThreadEntry)sub_00000000;
    thread->stackLow = stack;
    thread->initPriority = 0x7F;
    if (g_threadman.profilerMode != SCE_THREADMAN_PROFILER_MODE_NONE)
        thread->entry = (SceKernelThreadEntry)sub_000172C8;
    allocOpt.size = sizeof(allocOpt);
    allocOpt.align = 0x40;
    thread->context = sceKernelAllocHeapMemoryWithOption(g_threadman.heapId, sizeof(SceThreadmanContext), &allocOpt);
    sub_000134A8(thread->context);
    thread->context->gpr[31] = (u32)sub_000172DC;
    thread->context->gpr[30] = (u32)thread->stackLow + thread->stackSize;
    thread->context->type = SCE_THREADMAN_CONTEXT_TYPE_FULL;
    thread->context->gpr[29] = thread->context->gpr[30];
    g_threadman.idleThread = thread;
    thread->context->status = SCE_THREADMAN_CONTEXT_STATUS_KERNEL;
    thread->context->epc = (u32)thread->entry;
    thread->context->gpr[28] = thread->gp;
    sub_000127D0(thread);

    /* Create the cleaner thread, deleting the dead threads (see _sceKernelCleanerThreadEntry()). */
    sceKernelCreateUID(g_threadman.threadType, "SceThreadmanCleaner", 0, &cb);
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    thread->stackSize = 0x800;
    thread->uid = cb->uid;
    blockId = sceKernelAllocPartitionMemory(1, "stack:SceThreadmanCleaner", 1, 0x800, 0);
    thread->kStackMpid = 1;
    thread->stackBlockId = blockId;
    stack = sceKernelGetBlockHeadAddr(blockId);
    *(SceUID *)stack = thread->uid;
    thread->mutexPriority = 0x7F;
    thread->currentPriority = 1;
    thread->gp = gp;
    thread->status = SCE_THREADMAN_STATUS_WAIT;
    thread->attr = 0;
    thread->initAttr = 0;
    thread->entry = _sceKernelCleanerThreadEntry;
    thread->initPriority = 1;
    thread->basePriority = 1;
    thread->stackLow = stack;
    allocOpt.size = sizeof(allocOpt);
    allocOpt.align = 0x40;
    thread->context = sceKernelAllocHeapMemoryWithOption(g_threadman.heapId, sizeof(SceThreadmanContext), &allocOpt);
    sub_000134A8(thread->context);
    thread->context->type = SCE_THREADMAN_CONTEXT_TYPE_FULL;
    thread->context->gpr[31] = (u32)sub_000172DC;
    thread->context->gpr[30] = (u32)thread->stackLow + thread->stackSize;
    thread->context->gpr[29] = thread->context->gpr[30];
    thread->context->epc = (u32)thread->entry;
    thread->context->gpr[28] = thread->gp;
    thread->context->status = SCE_THREADMAN_CONTEXT_STATUS_KERNEL;
    g_threadman.cleanerThread = thread;
    g_threadman.cleanupRequested = 0;

    /* The caller becomes the running "SceThreadmanInit" thread, on its current stack. */
    sceKernelCreateUID(g_threadman.threadType, "SceThreadmanInit", 0, &cb);
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    thread->uid = cb->uid;
    sceKernelQueryMemoryInfo(sp, &partId, &blockId);
    thread->kStackMpid = 1;
    thread->stackBlockId = blockId;
    blockInfo.size = sizeof(blockInfo);
    sceKernelQueryMemoryBlockInfo(blockId, &blockInfo);
    thread->stackSize = blockInfo.memSize;
    *(SceUID *)blockInfo.addr = thread->uid;
    thread->status = SCE_THREADMAN_STATUS_RUNNING;
    thread->stackLow = (void *)blockInfo.addr;
    thread->currentPriority = 1;
    thread->mutexPriority = 0x7F;
    thread->gp = gp;
    thread->attr = 0;
    thread->initAttr = 0;
    thread->basePriority = 0x20;
    thread->initPriority = 0x20;
    allocOpt.size = sizeof(allocOpt);
    allocOpt.align = 0x40;
    thread->context = sceKernelAllocHeapMemoryWithOption(g_threadman.heapId, sizeof(SceThreadmanContext), &allocOpt);
    sub_000134A8(thread->context);
    g_threadman.curThread = thread;
    g_threadman.nextThread = thread;
    threadmanCop0CtrlSet(COP0_CTRL_SP_KERNEL, (s32)thread->context);
    threadmanCop0CtrlSet(COP0_CTRL_SP_USER, 0);
    threadmanCop0CtrlSet(COP0_CTRL_TCB, (s32)&thread->syscallFrame);
    pspSetK1(0);

    InterruptManagerForKernel_E790EAED(sub_00001100, (void *)sub_00012828);
    _sceKernelSysTimerInit(); // sub_0000E5EC
    sceKernelRegisterIntrHandler(SCE_THREADMAN_THREAD_INTR, 0, _sceKernelThreadmanIntrHandler, NULL, NULL);
    sceKernelEnableIntr(SCE_THREADMAN_THREAD_INTR);
    sceKernelRegisterExceptionHandler(EXCEP_CPU, (void *)sub_00000338);
    threadmanCop0CtrlSet(THREADMAN_COP0_CTRL_FPU_OWNER, 0);

    g_threadman.exitThreadSyscall = sceKernelQuerySystemCall((void *)_sceKernelExitThread);
    g_threadman.returnFromChangeStackSyscall = sceKernelQuerySystemCall((void *)_sceKernelReturnFromChangeStack);
    g_threadman.returnFromCallbackSyscall = sceKernelQuerySystemCall((void *)_sceKernelReturnFromCallback);
    g_threadman.systemStatusFlag = sceKernelCreateEventFlag("SceThreadmanSysStat", SCE_KERNEL_EA_MULTI, 0, NULL);
    sceKernelSetBootCallbackLevel(sub_000172A8, 3, NULL);

    sceKernelCpuResumeIntr(intr);
    sceKernelCpuEnableIntr();
    return SCE_ERROR_OK;
}

/**
 * The handler of the thread manager software interrupt (it only triggers a dispatch).
 *
 * @return -1.
 */
// Subroutine sub_000125A0 - Address 0x000125A0
s32 _sceKernelThreadmanIntrHandler(void)
{
    return -1;
}

/*
 * PARITY-Q075: The dead-thread block is walked through a cur local: count cleared, then the next block read, then
 * next cleared (the original's store/load order).
 */
/**
 * The entry of the cleaner thread ("SceThreadmanCleaner"), deleting the dead threads queued by
 * sub_000126AC().
 *
 * @return Never returns.
 */
// Subroutine sub_000125A8 - Address 0x000125A8
s32 _sceKernelCleanerThreadEntry(SceSize args, void *argp)
{
    s32 intr;
    s32 i;
    SceSysmemUidCB *cb;
    SceThreadmanDeadThreads *deadThreads;
    SceThreadmanDeadThreads *cur;

    (void)args;
    (void)argp;

    for (;;) {
        intr = sceKernelCpuSuspendIntr();
        deadThreads = &g_threadman.deadThreads;
        do {
            for (i = 0; i < deadThreads->count; i++) {
                if (sceKernelGetUIDcontrolBlockWithType(deadThreads->uids[i], g_threadman.threadType, &cb)
                    == SCE_ERROR_OK)
                    sub_000155B8(UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread), intr);
                sceKernelDeleteUID(deadThreads->uids[i]);
            }
            cur = deadThreads;
            cur->count = 0;
            deadThreads = cur->next;
            cur->next = NULL;
            if (cur != &g_threadman.deadThreads)
                sceKernelFreeHeapMemory(g_threadman.heapId, cur);
        } while (deadThreads != NULL);

        g_threadman.curThread->status = SCE_THREADMAN_STATUS_WAIT;
        g_threadman.nextThread = NULL;
        sub_00000FE4(1, 1);
    }
}

/**
 * Queue a thread for deletion by the cleaner thread.
 *
 * @param uid The UID of the thread.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sub_000126AC - Address 0x000126AC
s32 sub_000126AC(SceUID uid)
{
    s32 i;
    SceThreadmanDeadThreads *deadThreads;

    deadThreads = &g_threadman.deadThreads;
    while (deadThreads->count >= SCE_THREADMAN_NUM_DEAD_THREADS) {
        for (i = 0; i < deadThreads->count; i++) {
            if (deadThreads->uids[i] == uid)
                return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
        }
        if (deadThreads->next == NULL) {
            deadThreads->next = sceKernelAllocHeapMemory(g_threadman.heapId, sizeof(SceThreadmanDeadThreads));
            if (deadThreads->next == NULL)
                return SCE_ERROR_KERNEL_NO_MEMORY;
            deadThreads->next->next = NULL;
            deadThreads->next->count = 0;
        }
        deadThreads = deadThreads->next;
    }
    for (i = 0; i < deadThreads->count; i++) {
        if (deadThreads->uids[i] == uid)
            return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    deadThreads->uids[deadThreads->count++] = uid;
    g_threadman.nextThread = NULL;
    g_threadman.cleanupRequested = 1;
    return SCE_ERROR_OK;
}

/**
 * Add a thread at the end of the ready queue of its priority.
 *
 * @param thread The thread.
 */
// Subroutine sub_000127D0 - Address 0x000127D0
void sub_000127D0(SceThreadmanThread *thread)
{
    threadmanReadyQueueAdd(thread);
}

/*
 * PARITY-Q098: The profiler registers are saved by an inline helper that jumps to the copy with a goto after the
 *     no-profiler return, as threadmanProfilerPause() (PARITY-Q089): the front end's goto prediction places the copy
 *     loop out of line after the dispatcher's return (the original's `bnez` to the end and `j` back). The plain
 *     `if (profiler != NULL) copy` (50%, no prediction) keeps the copy inline; an inline helper with an early
 *     `return` gives the same output as the plain form. Same layout with the copy after the return in
 *     sub_00012828 itself (goto there and back): wsim +0.075, csim lower. In sub_00012828 the user-mode arm of the
 *     stack checks comes first (kernel arm as the else: same layout, closer schedule). wsim +0.077, 1 up / 0 down;
 *     still equiv (the original keeps the user-mode arm as the fall-through, ours the kernel arm: the
 *     sign-test opcode heuristic predicts `attr < 0` false at 79%).
 * PARITY-Q106: the user-mode stack-check test written as `thread->attr >> 31` (as Q082): no opcode-positive
 *     prediction, so the user-mode arm becomes the fall-through as in the original. wsim +0.077, csim +0.093,
 *     1 up / 0 down; still equiv (the original moves the sub_000177C4 calls out of line). Same output: `!= 0`.
 * PARITY-Q107: the stack-bottom UID checks go through an inline helper that jumps to the sub_000177C4 call with a goto
 *     after the return (the Q089/Q098 pattern), and the context-status test is written `!= 0` with the user-mode
 *     interrupted arm first. The goto prediction (with the call heuristic, against opcode-nonequal: 69%) moves the
 *     UID-check calls out of line (the original's `bnel`/`bne` far), and the arm order gives the original's
 *     fall-through `lw 0xB4`/`lw 0x28`. wsim +0.090, csim +0.069, 1 up / 0 down; still equiv: bbro now also moves
 *     the two `sp < stackLow` calls out of line (the original keeps them inline; 70% call heuristic in both).
 *     Arms swapped only: +0.024; helper for the user arm only: +0.040 / with swap +0.064; helper in both arms
 *     without swap: +0.066.
 * PARITY-Q162: the profiler load at the end tests `thread->profiler == NULL` first (the clear as the then-arm). The
 *     front end then predicts the copy arm cold, so the copy goes out of line after the return and the clear stays
 *     inline, as in the original. wsim +0.061, csim +0.024, same size, 1 up / 0 down; still equiv. The original
 *     re-reads thread->profiler after the sync; ours reuses the register.
 * PARITY-Q169: the two user-mode `sp < stackLow` checks jump over the sub_000177C4 call with a goto when the stack
 *     pointer is in range. The goto prediction (skip not taken) outweighs the call heuristic, so bbro keeps the calls
 *     inline as in the original (the plain `if` puts them out of line). wsim +0.019, csim +0.022, 2 instructions
 *     closer, 1 up / 0 down; still equiv (ours `beql` over the call, the original `beqz`). Either check alone:
 *     +0.013/+0.029; the same form in the kernel-mode arm: no change.
 */
/** Report a stack overflow of a thread if the UID at the bottom of its stack was overwritten. */
static inline void threadmanCheckStackUid(SceThreadmanThread *thread, void *stackLow, s32 kernelStack, u32 sp)
{
    if (*(SceUID *)stackLow != thread->uid)
        goto overflow;
    return;

overflow:
    sub_000177C4(thread, kernelStack, sp);
}

/** Save the hardware profiler registers of a thread, if it has a profiler. */
static inline void threadmanProfilerSave(SceThreadmanThread *thread)
{
    if (thread->profiler != NULL)
        goto save;
    return;

save:
    thread->profiler->regs = *THREADMAN_HW_PROFILER_REGS;
}

/**
 * The thread dispatcher, called by the interrupt manager when an interrupt or a syscall returns. It checks
 * the stacks of the current thread, selects the next thread and switches to it.
 *
 * @param context The context of the current thread.
 * @param arg The reason of the dispatch (only reported to the debug log handler).
 *
 * @return The context to resume.
 */
/*
 * PARITY-Q195 (trick): arg is copied into a $s4 local register variable (the original's register for it). The
 *     dispatcher drops from 249 to 248 instructions (original 247); wsim +0.002, csim +0.006, still equiv.
 */
// Subroutine sub_00012828 - Address 0x00012828
SceThreadmanContext *sub_00012828(SceThreadmanContext *context, s32 arg_)
{
    register s32 arg asm("$20") = arg_; /* PARITY-Q195: arg in $s4 */
    u32 sp;
    u32 kSp;
    u64 now;
    SceThreadmanThread *thread;
    SceThreadmanThread *next;

    if (g_threadman.dispatchSuspended != 0)
        return context;

    thread = g_threadman.curThread;
    threadmanProfilerSave(thread);

    if (thread->status != SCE_THREADMAN_STATUS_DEAD) {
        if (thread->attr >> 31) {
            if ((thread->context->status & 0x18) != 0) {
                sp = thread->context->gpr[29];
                kSp = thread->userSp;
            } else {
                /* The thread was interrupted in kernel mode. */
                kSp = thread->context->gpr[29];
                sp = (thread->syscallFrame != NULL) ? ((SceThreadmanSyscallFrame *)thread->syscallFrame)->sp : (u32)-1;
            }
            threadmanCheckStackUid(thread, thread->stackLow, 0, sp);
            threadmanCheckStackUid(thread, thread->kStackLow, 1, kSp);
            if (sp >= (u32)thread->stackLow)
                goto userSpOk;
            sub_000177C4(thread, 0, sp);
userSpOk:
            if (kSp >= (u32)thread->kStackLow)
                goto userKSpOk;
            sub_000177C4(thread, 1, kSp);
userKSpOk:
            if ((u32)thread->k0 != thread->context->gpr[26])
                sub_00017844(thread);
        } else {
            kSp = thread->context->gpr[29];
            threadmanCheckStackUid(thread, thread->stackLow, 1, kSp);
            if (kSp < (u32)thread->stackLow)
                sub_000177C4(thread, 1, kSp);
        }
    }

    if (g_threadman.nextThread == NULL)
        sub_000172F8();

    if (thread == g_threadman.nextThread)
        g_threadman.noSwitchCount++;
    else {
        now = _sceKernelGetSystemTime64();
        threadmanSysClockSet(&thread->runClocks, threadmanSysClockGet(&thread->runClocks)
            + (now - threadmanSysClockGet(&g_threadman.lastSwitchClock)));
        threadmanSysClockSet(&g_threadman.lastSwitchClock, now);
        g_threadman.threadSwitchCount++;
        (*thread->switchCounter)++;

        next = g_threadman.nextThread;
        if ((next->context->status & 0x40000000) && g_threadman.vfpuOwner != next->vfpuContext)
            arg |= 0x100;
        if (g_threadman.logHandler != NULL)
            g_threadman.logHandler(SCE_THREADMAN_LOG_THREAD_SWITCH, NULL, 5, thread->uid, thread->currentPriority,
                next->uid, next->currentPriority, arg);

        if (THREADMAN_K0_HOOK != NULL)
            *THREADMAN_K0_HOOK = (u32)g_threadman.nextThread->k0;
        thread = g_threadman.nextThread;
        g_threadman.curThread = thread;
    }

    if (thread->profiler == NULL)
        threadmanCop0CtrlSet(COP0_CTRL_PROFILER_BASE, 0);
    else {
        *THREADMAN_HW_PROFILER_REGS = thread->profiler->regs;
        pspSync();
        threadmanCop0CtrlSet(COP0_CTRL_PROFILER_BASE, (s32)thread->profiler);
    }

    if (thread->context->status & 0x40000000)
        sub_00012C04(thread);

    threadmanCop0CtrlSet(COP0_CTRL_SP_KERNEL, (s32)thread->context);
    threadmanCop0CtrlSet(COP0_CTRL_SP_USER, thread->userSp);
    threadmanCop0CtrlSet(COP0_CTRL_TCB, (s32)&thread->syscallFrame);
    return thread->context;
}

/*
 * PARITY-Q054 (52d2bdc): Comparison operand order as the original's branch operands.
 */
/**
 * Switch the FPU and VFPU contexts to a thread.
 *
 * @param thread The thread.
 */
// Subroutine sub_00012C04 - Address 0x00012C04
void sub_00012C04(SceThreadmanThread *thread)
{
    u32 userMode;
    SceThreadmanContext *fpuOwner;

    fpuOwner = (SceThreadmanContext *)threadmanCop0CtrlGet(THREADMAN_COP0_CTRL_FPU_OWNER);
    if (fpuOwner != NULL && thread->context != fpuOwner)
        sub_000005A4(fpuOwner);

    if (g_threadman.vfpuOwner == thread->vfpuContext)
        return;

    userMode = thread->attr >> 31;
    if (g_threadman.vfpuUserMode != userMode) {
        sub_000002C8();
        if (userMode)
            HW(0xBC000044) &= ~0x20;
        else
            HW(0xBC000044) |= 0x20;
        pspSync();
        g_threadman.vfpuUserMode = userMode;
    }
    if (g_threadman.vfpuOwner != NULL)
        sub_00000080(g_threadman.vfpuOwner);
    g_threadman.vfpuOwner = thread->vfpuContext;
    sub_000001B8(thread->vfpuContext);
    g_threadman.vfpuSwitchCount++;
}

/**
 * Create a thread.
 *
 * @param name The name of the thread.
 * @param entry The entry point of the thread.
 * @param initPriority The initial priority of the thread (1 - 126, 8 - 119 in user mode).
 * @param stackSize The size of the stack of the thread (at least 512 bytes).
 * @param attr The thread attributes (SCE_KERNEL_TH_*).
 * @param option The options (the partition of the stack), or NULL.
 *
 * @return The UID of the created thread, or < 0 on error.
 */
/*
 * PARITY-Q096: Option-buffer pointer initialised at declaration, interrupt-context and thread-event-mask checks as
 *     two ifs, option fields cleared stackMpid first (MATCHING FORM for the store order, not believed to be the original's
 *     literal source). The early pointer gives the original's prologue `addiu $s1, G+0x6ec` with the curThread load
 *     relative to it (`lw -0x6EC($s1)`, CSE related value) and the split checks give the original's shared error
 *     exit (ours duplicated the second return). wsim +0.028, 1 up / 0 down; still differs (allocation, and the
 *     original keeps &g_threadman in $s1 across sceKernelCreateUID). size-first order: +0.022; optBuf in memcpy:
 *     +0.008..0.016; without the split: +0.012; split alone: +0.008; optBuf assigned after pspShiftK1: same output.
 * PARITY-Q099: Kernel-entry check as nested ifs. With `!user && (s32)entry >= 0`, fold turns the && into
 *     `(attr | entry) >= 0` (one `nor`/`bltz`, BRANCH_COST short-circuit removal) and the merged block lets CSE
 *     carry the &g_threadman pseudo from before sceKernelCreateUID to the snprintf, so it is not allocated and
 *     reload rematerialises `lui/addiu` after the call. Nested: the original's `bltz`/`bgez` pair and &g kept
 *     in a saved register across sceKernelCreateUID. wsim +0.021, 1 up / 0 down; still differs. Same output as
 *     before: `!((u32)entry & 0x80000000)`.
 * PARITY-Q100: kernel-stack partition stored in both arms of an if/else. With `if (user) stackMpid = 1;` ce2
 *     makes `slti/movn`; the original keeps the branch with the `addiu 1` block out of line and the two stores
 *     cross-jumped into one. wsim +0.029, 1 up / 0 down. Ternary: +0.003 (still movn); arms swapped: same output.
 * PARITY-Q101: stack partition permission check as nested ifs per mode, each arm with its own error return
 *     (cross-jumped into one out-of-line block), instead of a ternary condition. Gives the original's kernel arm
 *     `bnez` to the shared error block and the restored $s1 thread register. wsim +0.038, csim +0.146, 1 up /
 *     0 down. Shared label (goto from the user arm): +0.003.
 * PARITY-Q102: heap allocation option through a local pointer, assigned just before the context allocation and
 *     reused for the VFPU context. Gives the original's `addiu $s0, G+0x6e4` first with heapId loaded relative
 *     to it (`addiu -0x6E4`) and align stored as `0x4($s0)`. wsim +0.018, 1 up / 0 down. Pointer only for the
 *     context allocation: +0.014; pointer initialised at declaration: -0.024 (down); size stored first: same
 *     output or down.
 * PARITY-Q103: partition info through a local pointer, assigned just before the size store and used for the
 *     query and both attribute checks (same pattern as Q102). Gives the original's `addiu $s4, -0x20` (relative
 *     to the stack name), the stackMpid load as `lw -0x4($s4)` and the attribute loads as `0xC($s4)`; size now
 *     equal (367). wsim +0.030, csim +0.159, 1 up / 0 down. Pointer for the size store and query only: +0.005.
 * PARITY-Q104: thread profiler fields initialised savedEnable, enable, pauseCount (the original's stores come
 *     out pauseCount first, sched1 reverses the source order here). wsim +0.005, 1 up / 0 down. Other orders:
 *     savedEnable/pauseCount/enable +0.002, enable first: same output as before.
 * PARITY-Q105: the UID read from the control block right after the thread pointer, before the field stores.
 *     Gives the original's early `lw 0xC(cb)` and its store order. wsim +0.011, csim +0.092, 1 up / 0 down.
 *     Same output: before the thread pointer. After the option store: +0.008; after entry: same as before.
 * PARITY-Q108: VFPU-context allocation failure handled by its own inline error return instead of `goto no_memory`
 *     (cross-jumped into the shared block). The label after the error return ends CSE's skip-block path over the
 *     VFPU block, so the dormant-list address is no longer derived from the &g_threadman/allocOpt registers: the
 *     original's fresh `lui/addiu G+0x4a8` (also fixes the SuspendIntr delay slot). differs -> sched, wsim +0.011,
 *     1 up / 0 down. The same change for the context allocation: no change.
 * PARITY-Q109: both heap-option blocks store `size` before `align` (the struct's field order) instead of align
 *     first. Fixes the $s5 (&g_threadman) / $s6 (constant 8) register swap (pseudo numbering follows the first
 *     store). sched -> sched, csim +0.017, 1 up / 0 down. Only the first block matters; the second is changed for
 *     consistency (same output).
 * PARITY-Q110: profiler allocation failure also handled by its own inline error return (like Q108) instead of
 *     `goto no_memory`. The return block is cross-jumped into no_memory as before; the `sw v0,0x3C(s1)` store
 *     now stays before the `beqz` and the slot is filled from the fall-through (`addiu v1,v0,-0x64`) as in the
 *     original. sched -> sched, wsim/csim +0.003, 1 up / 0 down. Locals for the profiler pointer: no change.
 * PARITY-Q112: thread field stores ordered mutexPriority, attr, initAttr, entry, initPriority, basePriority,
 *     currentPriority, option. With all stores ready together, sched1 (rank_for_schedule) puts the stores that end a
 *     register's life (INSN_REG_WEIGHT) first, then source order. So the original's output order means the last
 *     uses of initPriority and option are currentPriority and option. Not type-based alias analysis: option as
 *     u32 / void * gives the same output. sched -> exact, 1 up / 0 down. Also exact: the same order with mutexPriority
 *     after basePriority.
 */
// Subroutine sceKernelCreateThread - Address 0x00012CF8
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, s32 initPriority,
                             SceSize stackSize, SceUInt attr, SceKernelThreadOptParam *option)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceUID uid;
    SceUID blockId;
    SceUID stackMpid;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanThreadProfiler *profiler;
    SceSysmemHeapAllocOption *allocOpt;
    SceSysmemPartitionInfo *partInfo;
    s32 i;
    SceKernelThreadOptParam *optBuf = &g_threadman.statusBuf.createThread.option;

    oldK1 = pspShiftK1();

    if (!pspK1PtrOk(name) || !pspK1PtrOk(entry) || !pspK1StaBufOk(option, sizeof(SceKernelThreadOptParam))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (g_threadman.curThread->threadEventMask != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (attr & SCE_THREADMAN_THREAD_ATTR_INVALID) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }
    if (pspK1IsUserMode()) {
        /* A user mode thread inherits the mode of its creator, unless SCE_KERNEL_TH_UNK_800000 is set. */
        attr &= ~0xF8000000;
        if (attr & SCE_KERNEL_TH_UNK_800000) {
            attr &= ~SCE_KERNEL_TH_UNK_800000;
            attr |= SCE_KERNEL_TH_USER_MODE;
        } else
            attr |= g_threadman.curThread->attr & 0xF0000000;
    }
    if ((u32)(initPriority - 1) >= 0x7E) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
    }
    if ((s32)attr < 0) {
        if ((u32)(initPriority - 8) >= 0x70) {
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
        }
        attr |= 0xFF;
    }
    if ((u32)entry & 3) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD_ENTRY_ADDR;
    }
    if (stackSize < 0x200) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_STACK_SIZE;
    }

    intr = sceKernelCpuSuspendIntr();

    optBuf->stackMpid = 0;
    optBuf->size = 0;
    if (option != NULL)
        memcpy(&g_threadman.statusBuf.createThread.option, option,
            (option->size > sizeof(SceKernelThreadOptParam)) ? sizeof(SceKernelThreadOptParam) : option->size);

    ret = sceKernelCreateUID(g_threadman.threadType, name, (attr | (pspGetK1() >> 31)) & 0xFF, &cb);
    if (ret != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return ret;
    }

    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    uid = cb->uid;
    thread->mutexPriority = 0x7F;
    thread->attr = attr;
    thread->initAttr = attr;
    thread->entry = entry;
    thread->initPriority = initPriority;
    thread->basePriority = initPriority;
    thread->currentPriority = initPriority;
    thread->option = option;
    thread->gp = sceKernelGetModuleGPByAddressForKernel((u32)entry);
    if (thread->gp == 0)
        thread->gp = threadmanGetGp();

    /* A kernel mode thread must have a kernel mode entry point. */
    if (!THREADMAN_IS_USER_THREAD(thread)) {
        if ((s32)entry >= 0) {
            sceKernelDeleteUID(uid);
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
        }
    }

    /* Allocate the stack. */
    snprintf(g_threadman.statusBuf.createThread.name, 31, "stack:%s", name);
    stackMpid = g_threadman.statusBuf.createThread.option.stackMpid;
    stackSize = (stackSize + 0xFF) & ~0xFF;
    if (stackMpid == 0)
        stackMpid = THREADMAN_IS_USER_THREAD(thread) ? 2 : 1;
    partInfo = &g_threadman.statusBuf.createThread.partInfo;
    partInfo->size = sizeof(SceSysmemPartitionInfo);
    ret = sceKernelQueryMemoryPartitionInfo(stackMpid, partInfo);
    if (ret != SCE_ERROR_OK) {
        sceKernelDeleteUID(uid);
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return ret;
    }
    /* The stack of a user mode thread must be in a user partition, the one of a kernel thread in a kernel one. */
    if (THREADMAN_IS_USER_THREAD(thread)) {
        if ((partInfo->attr & 3) != 3) {
            sceKernelDeleteUID(uid);
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
        }
    } else {
        if ((partInfo->attr & 3) != 0) {
            sceKernelDeleteUID(uid);
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
        }
    }
    blockId = sceKernelAllocPartitionMemory(stackMpid, g_threadman.statusBuf.createThread.name,
        ((attr >> 22) ^ 1) & 1, stackSize, 0);
    if (blockId <= 0)
        goto no_memory;
    thread->stackBlockId = blockId;
    thread->stackMpid = stackMpid;
    if (THREADMAN_IS_USER_THREAD(thread))
        thread->kStackMpid = 1;
    else
        thread->kStackMpid = stackMpid;
    thread->stackSize = stackSize;
    thread->stackLow = sceKernelGetBlockHeadAddr(blockId);

    if (THREADMAN_IS_USER_THREAD(thread)) {
        /* A user mode thread runs its syscalls on a separate kernel stack. */
        snprintf(g_threadman.statusBuf.createThread.name, 31, "kstack:%s", name);
        blockId = sceKernelAllocPartitionMemory(1, g_threadman.statusBuf.createThread.name, 1, 0x800, 0);
        if (blockId <= 0)
            goto no_memory;
        thread->kStackSize = 0x800;
        thread->kStackBlockId = blockId;
        thread->kStackLow = sceKernelGetBlockHeadAddr(blockId);
        if (g_threadman.profilerMode == SCE_THREADMAN_PROFILER_MODE_THREAD
            || g_threadman.profilerMode == SCE_THREADMAN_PROFILER_MODE_THREAD_AND_GLOBAL) {
            thread->profiler = sceKernelAllocHeapMemory(g_threadman.heapId, sizeof(SceThreadmanThreadProfiler));
            if (thread->profiler == NULL) {
                sceKernelDeleteUID(uid);
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_NO_MEMORY;
            }
            profiler = thread->profiler;
            /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
            memset(profiler, 0, sizeof(SceThreadmanThreadProfiler));
            thread->profiler->profiler.savedEnable = 1;
            thread->profiler->profiler.enable = (vu32 *)0xBC400000;
            thread->profiler->profiler.pauseCount = 1;
        }
    }

    allocOpt = &g_threadman.statusBuf.createThread.allocOpt;
    allocOpt->size = sizeof(SceSysmemHeapAllocOption);
    allocOpt->align = 0x40;
    thread->context = sceKernelAllocHeapMemoryWithOption(g_threadman.heapId, sizeof(SceThreadmanContext),
        allocOpt);
    if (thread->context == NULL)
        goto no_memory;
    if (attr & SCE_KERNEL_TH_USE_VFPU) {
        allocOpt->size = sizeof(SceSysmemHeapAllocOption);
        allocOpt->align = 0x40;
        thread->vfpuContext = sceKernelAllocHeapMemoryWithOption(g_threadman.heapId,
            sizeof(SceThreadmanVfpuContext), allocOpt);
        if (thread->vfpuContext == NULL) {
            sceKernelDeleteUID(uid);
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_NO_MEMORY;
        }
    }

    threadmanListAddTail(&g_threadman.dormantThreads, &thread->link);
    thread->exitStatus = SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
    thread->status = SCE_THREADMAN_STATUS_DORMANT;
    sceKernelCpuResumeIntr(intr);

    if ((attr & SCE_KERNEL_TH_NO_FILLSTACK) == 0) {
        sceKernelMemset32(thread->stackLow, -1, thread->stackSize);
        if (THREADMAN_IS_USER_THREAD(thread))
            sceKernelMemset32(thread->kStackLow, -1, thread->kStackSize);
    }

    intr = sceKernelCpuSuspendIntr();
    THREADMAN_LOG(SCE_THREADMAN_LOG_CREATE_THREAD, 2, entry, uid);
    sub_00001580(SCE_KERNEL_THREAD_EVENT_CREATE, thread, intr);
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return uid;

no_memory:
    sceKernelDeleteUID(uid);
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_KERNEL_NO_MEMORY;
}

/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 */
/**
 * Delete a dormant thread.
 *
 * @param thid The UID of the thread.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelDeleteThread - Address 0x000132B4
s32 sceKernelDeleteThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();

    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (g_threadman.curThread->threadEventMask != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (thid == SCE_KERNEL_THREAD_ID_SELF) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_IS_NOT_DORMANT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_DELETE)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    if (thread == g_threadman.idleThread || thread == g_threadman.cleanerThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (thread->status != SCE_THREADMAN_STATUS_DORMANT && thread->status != SCE_THREADMAN_STATUS_DEAD) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_IS_NOT_DORMANT;
    }
    if (thread->threadEventMask != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_DELETE_THREAD, 1, thid);
    sub_00001580(SCE_KERNEL_THREAD_EVENT_DELETE, thread, intr);
    _sceKernelEventFlagReleaseOwned(thread);
    _sceKernelMutexReleaseOwned(thread);
    threadmanListRemove(&thread->link);
    thread->status = 0;
    sub_000155B8(thread, intr);
    sceKernelDeleteUID(thread->uid);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q042 (46fbca9): Statement/store order taken from the original's store order.
 */
/**
 * Initialize a thread context, filling the registers with recognizable garbage.
 *
 * @param context The context.
 */
// Subroutine sub_000134A8 - Address 0x000134A8
void sub_000134A8(SceThreadmanContext *context)
{
    s32 i;

    for (i = 0; i < 32; i++)
        context->gpr[i] = 0xDEADBEEF;
    context->gpr[26] = 0; // $k0
    context->gpr[27] = 0; // $k1
    for (i = 0; i < 32; i++)
        context->fpr[i] = 0x7F800001; // NaN
    context->fcr31 = 0xE00;
    context->hi = 0xDEADBEEF;
    context->lo = 0xDEADBEEF;
    context->status = 0;
    context->epc = 0;
    context->gpr[0] = 0;
    context->cop0Ctrl23 = 0;
}

/*
 * PARITY-Q040 (dfa690b): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q048 (d9f1b73): Value read once into a local (the original keeps it in one register).
 * PARITY-Q055 (80149d7): Gpr[29] set in each branch / gpr[30] = gpr[29] chain (the original reloads it).
 * PARITY-Q130: State reset with the priorities last, `currentPriority = basePriority = prio` then mutexPriority:
 *   the base priority store is the one left for the sub_000134A8 delay slot, as in the original.
 * PARITY-Q150: The k0 unk248 = -1 store comes before the stackLow copy (store order).
 * PARITY-Q154: The start context's epc is set after gpr[28] and status (store order).
 * PARITY-Q155: The k0 unk248 = -1 store comes first in the k0 init (store order; extends Q150).
 */
/**
 * Start a dormant thread.
 *
 * @param thid The UID of the thread.
 * @param arglen The size of the argument block passed to the thread entry.
 * @param argp The argument block passed to the thread entry (copied to the stack of the thread).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelStartThread - Address 0x0001351C
s32 sceKernelStartThread(SceUID thid, SceSize arglen, void *argp)
{
    s32 prio;
    s32 oldK1;
    s32 intr;
    s32 i;
    u32 sp;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceThreadmanContext *context;

    oldK1 = pspShiftK1();

    if (!threadmanK1DynBufOk(argp, arglen)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (g_threadman.curThread->threadEventMask != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (thid == SCE_KERNEL_THREAD_ID_SELF) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    if (thread->status != SCE_THREADMAN_STATUS_DORMANT) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_IS_NOT_DORMANT;
    }
    sub_000156BC(thread);
    if (thread->threadEventMask != 0) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_START_THREAD, 3, thid, arglen, argp);

    sp = (u32)thread->stackLow + (thread->stackSize & ~3);
    if (THREADMAN_IS_USER_THREAD(thread)) {
        /* The $k0 area of a user mode thread is at the top of its stack. */
        sp -= sizeof(SceThreadmanK0);
        thread->k0 = (SceThreadmanK0 *)sp;
        /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
        memset(thread->k0, 0, sizeof(SceThreadmanK0));
        thread->k0->unk248 = -1;
        thread->k0->unk252 = -1;
        thread->k0->threadId = thid;
        thread->k0->stackLow = thread->stackLow;
    }
    /* Mark the bottom of the stacks, to detect stack overflows. */
    *(SceUID *)thread->stackLow = thread->uid;
    if (THREADMAN_IS_USER_THREAD(thread))
        *(SceUID *)thread->kStackLow = thread->uid;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(thread->ktls, 0, sizeof(thread->ktls));

    if (arglen == 0 || argp == NULL) {
        thread->argSize = 0;
        thread->argp = NULL;
    } else {
        sp -= (arglen + 0xF) & ~0xF;
        memcpy((void *)sp, argp, arglen);
        thread->argSize = arglen;
        thread->argp = (void *)sp;
    }

    if (THREADMAN_IS_USER_THREAD(thread)) {
        /* Build a "syscall _sceKernelExitThread" instruction on the stack, used as the return address of the entry. */
        sp -= 64;
        ((u32 *)sp)[0] = ((g_threadman.exitThreadSyscall << 6) & 0x03FFFFC0) | 0xC;
        ((u32 *)sp)[1] = 0xD; // break
        ((u32 *)sp)[2] = 0x1000FFFF; // b .
        ((u32 *)sp)[3] = 0;
        pspCache(0x1B, (void *)sp);
        pspCache(0x08, (void *)sp);
        pspSync();
    }

    thread->attr = thread->initAttr;
    thread->waitType = 0;
    thread->waitObj = NULL;
    thread->wakeupCount = 0;
    prio = thread->initPriority;
    thread->currentPriority = thread->basePriority = prio;
    thread->mutexPriority = 0x7F;
    sub_000134A8(thread->context);
    context = thread->context;
    thread->initSp = sp;
    context->type = SCE_THREADMAN_CONTEXT_TYPE_FULL;
    context->gpr[26] = (u32)thread->k0;
    if (THREADMAN_IS_USER_THREAD(thread)) {
        /* User threads start in kernel mode, on the top of their kernel stack. */
        sp = thread->kStackSize + (u32)thread->kStackLow;
        thread->userSp = sp;
        context->gpr[29] = sp;
    } else {
        thread->userSp = 0;
        context->gpr[29] = sp;
    }
    context->gpr[30] = context->gpr[29];
    threadmanListRemove(&thread->link);
    context->gpr[28] = thread->gp;
    context->status = SCE_THREADMAN_CONTEXT_STATUS_KERNEL;
    context->epc = (u32)sub_00013E80;
    thread->exitStatus = SCE_ERROR_KERNEL_THREAD_IS_NOT_DORMANT;

    if (g_threadman.skipUserThreads
        && (THREADMAN_IS_USER_THREAD(thread) || (thread->attr & SCE_THREADMAN_THREAD_ATTR_UNK_08000000))) {
        threadmanListAddTail(&g_threadman.suspendedThreads, &thread->link);
        thread->status = SCE_THREADMAN_STATUS_SUSPEND;
    } else
        threadmanReadyThread(thread, intr);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q040 (7f1d701): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q053 (8353439): Error/success paths share one return (the original's join).
 */
/**
 * Exit the current thread. Its exit status is set and it becomes dormant (or deleted if a thread
 * event handler asked for it).
 *
 * @param status The exit status of the thread (a negative status is replaced by
 * SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT).
 *
 * @return Does not return on success, < 0 on error.
 */
// Subroutine sceKernelExitThread - Address 0x000139C8
s32 sceKernelExitThread(s32 status)
{
    s32 oldK1;
    s32 intr;
    s32 ret;

    oldK1 = pspShiftK1();

    if (THREADMAN_IS_INTR_CONTEXT()) {
        ret = SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
        goto end;
    }
    if (g_threadman.curThread->threadEventMask != 0) {
        ret = SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
        goto end;
    }
    if ((pspMfic() == 0 || g_threadman.dispatchSuspended != 0) && threadmanSdkVersionAbove(0x0307FFFF)) {
        ret = SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
        goto end;
    }
    if (status < 0) {
        status = SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT;
    }

    intr = sceKernelCpuSuspendIntr();
    g_threadman.curThread->exitStatus = status;
    THREADMAN_LOG(SCE_THREADMAN_LOG_EXIT_THREAD, 1, status);
    sub_000178C0(0, intr, status);

end:
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q040 (7f1d701): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q042 (a747357): Statement/store order taken from the original's store order.
 * PARITY-Q053 (8353439): Error/success paths share one return (the original's join).
 */
/**
 * Exit and delete the current thread.
 *
 * @param status The exit status of the thread (only logged, a negative status is replaced by
 * SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT).
 *
 * @return Does not return on success, < 0 on error.
 */
// Subroutine sceKernelExitDeleteThread - Address 0x00013AC4
s32 sceKernelExitDeleteThread(s32 status)
{
    s32 oldK1;
    s32 intr;
    s32 ret;

    oldK1 = pspShiftK1();

    if (THREADMAN_IS_INTR_CONTEXT()) {
        ret = SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
        goto end;
    }
    if (g_threadman.curThread->threadEventMask != 0) {
        ret = SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
        goto end;
    }
    if ((pspMfic() == 0 || g_threadman.dispatchSuspended != 0) && threadmanSdkVersionAbove(0x0307FFFF)) {
        ret = SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
        goto end;
    }
    if (status < 0) {
        status = SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT;
    }

    intr = sceKernelCpuSuspendIntr();
    g_threadman.curThread->exitStatus = status;
    THREADMAN_LOG(SCE_THREADMAN_LOG_EXIT_DELETE_THREAD, 1, status);
    sub_000178C0(1, intr, g_threadman.curThread->exitStatus);

end:
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q067 (a82624b): The control block's uid logged (the original reads it from the CB).
 */
/**
 * Terminate a thread. Its exit status is set to SCE_ERROR_KERNEL_THREAD_IS_TERMINATED and it
 * becomes dormant.
 *
 * @param thid The UID of the thread to terminate.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelTerminateThread - Address 0x00013BC0
s32 sceKernelTerminateThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();

    if (thid == SCE_KERNEL_THREAD_ID_SELF) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (THREADMAN_IS_INTR_CONTEXT() && threadmanSdkVersionAbove(0x0307FFFF)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (g_threadman.dispatchSuspended != 0 && threadmanSdkVersionAbove(0x0307FFFF)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_TERMINATE_THREAD, 1, cb->uid);
    ret = sub_00017A14(cb, 0, intr);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q067 (1983710): The control block's uid logged (the original reads it from the CB).
 */
/**
 * Terminate and delete a thread.
 *
 * @param thid The UID of the thread to terminate and delete.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelTerminateDeleteThread - Address 0x00013D38
s32 sceKernelTerminateDeleteThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();

    if (thid == SCE_KERNEL_THREAD_ID_SELF) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (g_threadman.dispatchSuspended != 0 && threadmanSdkVersionAbove(0x0307FFFF)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_TERMINATE_DELETE_THREAD, 1, cb->uid);
    ret = sub_00017A14(cb, 1, intr);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q030: Placed after sceKernelTerminateDeleteThread: the original's function order.
 * PARITY-Q049 (36877a1): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q055 (36877a1): Gpr[29] set in each branch / gpr[30] = gpr[29] chain (the original reloads it).
 */
/**
 * The entry of a started thread (in kernel mode): call the thread start event handlers, then build the context
 * calling the entry of the thread and resume it.
 */
/*
 * PARITY-Q116: context->gpr[4] (argSize) is stored before context->gpr[5] (argp). With this order the
 * scheduler gives the original's load/store order for the context setup (lw 0x128, lw 0x124, lw 0xDC;
 * sw 0x54, 0x50, 0xA8). csim +0.037, 1 up / 0 down. 6 of the 24 orders of the four setup stores are up,
 * this one most. Residue: the context pointer stays in $t3 (original $a0, temporaries shifted by one), and
 * the VFPU ctrl stores are scheduled differently (the original keeps source order).
 * PARITY-Q124: SceThreadmanK0.dispatchSuspended is a one-member union (threadman_int.h, the Q070 form). The
 * `thread->k0->dispatchSuspended = 0` store is in alias set 0, so GCSE no longer removes the thread->context
 * reload in the USE_VFPU block. equiv -> sched, csim +0.374. Residue: the VFPU ctrl store order.
 * PARITY-Q128: The VFPU control registers are initialized in index order (ctrl[0] to ctrl[15]). sched1 puts
 *     the stores that free a register (the last use of the shared 0xE4 / 0x3F800000 constants, and the RCX0-3
 *     values) first, in source order, and the others after them. With the index order this gives the
 *     original's store order (0x204, 0x20C, 0x220-0x22C, 0x23C, then 0x200, 0x208, 0x210, 0x230-0x238).
 *     sched -> exact. A volatile ctrl[] also gives the store order, but it loses the beq delay-slot fill.
 */
// Subroutine sub_00013E80 - Address 0x00013E80
void sub_00013E80(void)
{
    s32 i;
    SceThreadmanThread *thread;
    SceThreadmanContext *context;
    SceThreadmanVfpuContext *vfpuContext;

    thread = g_threadman.curThread;
    sub_00001580(SCE_KERNEL_THREAD_EVENT_START, thread, sceKernelCpuSuspendIntr());
    sub_000134A8(thread->context);
    context = thread->context;
    context->type = SCE_THREADMAN_CONTEXT_TYPE_FULL;
    context->gpr[4] = thread->argSize; // $a0
    context->gpr[5] = (u32)thread->argp; // $a1
    context->gpr[26] = (u32)thread->k0;
    context->gpr[30] = context->gpr[29] = thread->initSp;
    /* The entry returns to the syscall built on the stack by sceKernelStartThread() in user mode. */
    if (THREADMAN_IS_USER_THREAD(thread))
        context->gpr[31] = thread->initSp;
    else
        context->gpr[31] = (u32)sub_000172DC;
    context->gpr[28] = thread->gp;
    if (THREADMAN_IS_USER_THREAD(thread))
        context->status = SCE_THREADMAN_CONTEXT_STATUS_USER;
    else
        context->status = SCE_THREADMAN_CONTEXT_STATUS_KERNEL;
    context->epc = (u32)thread->entry;
    if (THREADMAN_IS_USER_THREAD(thread))
        thread->k0->dispatchSuspended = 0;
    thread->attr = (thread->attr & ~SCE_KERNEL_TH_USE_VFPU) | (thread->initAttr & SCE_KERNEL_TH_USE_VFPU);

    vfpuContext = thread->vfpuContext;
    if (vfpuContext != NULL) {
        for (i = 0; i < 128; i++)
            vfpuContext->vr[i] = 0x7F800001; // NaN
        vfpuContext->ctrl[0] = 0xE4; // PFXS
        vfpuContext->ctrl[1] = 0xE4; // PFXT
        vfpuContext->ctrl[2] = 0; // PFXD
        vfpuContext->ctrl[3] = 0x3F; // CC
        vfpuContext->ctrl[4] = 0; // INF4
        vfpuContext->ctrl[8] = 0x3F800001; // RCX0
        vfpuContext->ctrl[9] = 0x3F800002; // RCX1
        vfpuContext->ctrl[10] = 0x3F800004; // RCX2
        vfpuContext->ctrl[11] = 0x3F800008; // RCX3
        vfpuContext->ctrl[12] = 0x3F800000; // RCX4
        vfpuContext->ctrl[13] = 0x3F800000; // RCX5
        vfpuContext->ctrl[14] = 0x3F800000; // RCX6
        vfpuContext->ctrl[15] = 0x3F800000; // RCX7
        if (g_threadman.vfpuOwner == vfpuContext)
            g_threadman.vfpuOwner = NULL;
    }
    if (thread->attr & SCE_KERNEL_TH_USE_VFPU) {
        thread->context->status |= SCE_THREADMAN_CONTEXT_STATUS_CU2;
        sub_00012C04(thread);
    }
    InterruptManagerForKernel_551097F2(thread->context);
}

/**
 * Suspend the thread dispatch: the current thread keeps running until the dispatch is resumed.
 *
 * @return The previous state of the dispatch (1 if it was enabled, 0 if it was already suspended), to
 * be given to sceKernelResumeDispatchThread(), < 0 on error.
 */
// Subroutine sceKernelSuspendDispatchThread - Address 0x00014034
s32 sceKernelSuspendDispatchThread(void)
{
    s32 oldK1;
    s32 intr;
    s32 state;

    oldK1 = pspShiftK1();

    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0) {
        sceKernelCpuResumeIntr(0);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_INTERRUPTS_ALREADY_DISABLED;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_SUSPEND_DISPATCH_THREAD, 0);
    state = 0;
    if (g_threadman.dispatchSuspended == 0) {
        g_threadman.dispatchSuspended = 1;
        state = 1;
    }
    if (THREADMAN_IS_USER_THREAD(g_threadman.curThread))
        g_threadman.curThread->k0->dispatchSuspended = 1;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return state;
}

/**
 * Suspend or resume the thread dispatch. When it is resumed, the operation postponed while it
 * was suspended (g_threadman.dispatchSuspended: 1 nothing, 2 suspend the current thread, 3
 * terminate it) is done.
 *
 * @param resume Set to resume the dispatch, 0 to suspend it.
 * @param isIntr Set if called from an interrupt context.
 * @param intr The interrupt state.
 *
 * @return 0.
 */
/*
 * PARITY-Q115: case 3 written before case 2, with the !isIntr call first. -frename-registers follows insn order
 * (see Q113), so this gives the original's temporaries in both arms ($t8/$t7/$t0 for the threadType CB, $t3/$t9
 * for the suspended-list add). Case order alone: csim +0.112; !isIntr alone: +0.056. Residue: the list-add
 * entry->next store and the status store are scheduled in a different order. All 6 orders of the case-2
 * statements give the same output, and so does reordering the shared threadmanListAddTail() (or it goes down).
 * PARITY-Q165: the suspended-list add goes through the SceThreadmanListHead view (see threadman_int.h, user-
 *   approved, this function only): the head's stores are then in their own alias set and sched2 orders the
 *   entry->next and status stores as the original.
 */
// Subroutine sub_00014124 - Address 0x00014124
s32 sub_00014124(s32 resume, s32 isIntr, s32 intr)
{
    s32 state;
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    if (!resume) {
        if (g_threadman.dispatchSuspended == 0)
            g_threadman.dispatchSuspended = 1;
        if (THREADMAN_IS_USER_THREAD(thread))
            thread->k0->dispatchSuspended = 1;
        return SCE_ERROR_OK;
    }

    state = g_threadman.dispatchSuspended;
    g_threadman.dispatchSuspended = 0;
    if (THREADMAN_IS_USER_THREAD(thread))
        thread->k0->dispatchSuspended = 0;
    switch (state) {
    case 1:
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
        break;
    case 3:
        /* The current thread was terminated while the dispatch was suspended. */
        if (!isIntr)
            sub_000178C0(0, isIntr, SCE_ERROR_KERNEL_THREAD_IS_TERMINATED);
        else
            sub_00017A14(THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType), 0, intr);
        break;
    case 2:
        /* The current thread was suspended while the dispatch was suspended. */
        threadmanListHeadAddTail((SceThreadmanListHead *)&g_threadman.suspendedThreads, &thread->link);
        thread->status = SCE_THREADMAN_STATUS_SUSPEND;
        g_threadman.nextThread = NULL;
        sub_00000FE4(1, 1);
        break;
    }
    return SCE_ERROR_OK;
}

/**
 * Resume the thread dispatch.
 *
 * @param state The state returned by sceKernelSuspendDispatchThread() (0 keeps the dispatch suspended).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelResumeDispatchThread - Address 0x00014240
s32 sceKernelResumeDispatchThread(s32 state)
{
    s32 oldK1;
    s32 intr;
    s32 ret;

    oldK1 = pspShiftK1();

    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (intr == 0) {
        sceKernelCpuResumeIntr(0);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_INTERRUPTS_ALREADY_DISABLED;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_RESUME_DISPATCH_THREAD, 1, state);
    ret = sub_00014124(state, 0, intr);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * Read-modify-write of the COP0 Status register with a 32-bit immediate, as hand-written inline asm in the
 * original: the module uses "or/and reg, reg, imm32", which the assembler expands through $at (lui $at; [ori $at;]
 * or/and). The compiler never emits that sequence (it loads such constants into an allocatable register or uses
 * ins), so this is genuine asm, see threadman-deviations.md.
 */
/*
 * PARITY-Q008: mfc0/or|and/mtc0 with a 32-bit immediate expanded through $at by gas, as the original (C cannot use
 * $at)
 */
#define THREADMAN_STATUS_OR(imm) do { \
    asm volatile (".set noreorder\n\tmfc0 $v1, $12\n\tor $v1, $v1, %0\n\tmtc0 $v1, $12\n\t.set reorder" : : "i" (imm) : "$3"); \
} while (0)
#define THREADMAN_STATUS_AND(imm) do { \
    asm volatile (".set noreorder\n\tmfc0 $v1, $12\n\tand $v1, $v1, %0\n\tmtc0 $v1, $12\n\t.set reorder" : : "i" (imm) : "$3"); \
} while (0)

/*
 * PARITY-Q008 (c5aeee2): Uses Status.CU2 set/clear asm in sceKernelChangeCurrentThreadAttr.
 * PARITY-Q040 (19301c6): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q042: curThread read before the k1 shift (the original loads it ahead of the shift).
 * PARITY-Q128: The VFPU control registers are initialized in index order (ctrl[0] to ctrl[15]), as in
 *     sub_00013E80. The scheduler then emits the stores in the original's order (sw 0x23C, 0x204, 0x20C, ...).
 *     csim +0.018, still equiv. Rejected: the order 0, 2, 4, 12-15, 1, 3, 8-11 (csim -0.14).
 * PARITY-Q151: The VFPU context alloc option's align is set before its size (store order).
 */
/**
 * Change the attributes of the current thread. Only SCE_KERNEL_TH_USE_VFPU can be changed.
 *
 * @param clearAttr The attributes to clear.
 * @param setAttr The attributes to set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelChangeCurrentThreadAttr - Address 0x00014308
s32 sceKernelChangeCurrentThreadAttr(u32 clearAttr, u32 setAttr)
{
    s32 oldK1;
    s32 intr;
    u32 status;
    SceThreadmanThread *thread;
    SceThreadmanVfpuContext *vfpuContext;
    SceSysmemHeapAllocOption allocOpt;
    s32 i;
    u32 nan;

    thread = g_threadman.curThread;
    oldK1 = pspShiftK1();

    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (thread->threadEventMask != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if ((clearAttr & ~SCE_KERNEL_TH_USE_VFPU) != 0 || (setAttr & ~SCE_KERNEL_TH_USE_VFPU) != 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ATTR;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CHANGE_CURRENT_THREAD_ATTR, 2, clearAttr, setAttr);
    intr = sceKernelCpuSuspendIntr();
    thread->attr = (thread->attr & ~clearAttr) | setAttr;
    status = threadmanCop0StateGet(COP0_STATE_STATUS); /* PARITY-Q185 */
    if ((thread->attr & SCE_KERNEL_TH_USE_VFPU) && !(status & SCE_THREADMAN_CONTEXT_STATUS_CU2)) {
        /* Enable the VFPU. */
        if (thread->vfpuContext == NULL) {
            allocOpt.size = sizeof(SceSysmemHeapAllocOption);
            allocOpt.align = 0x40;
            vfpuContext = sceKernelAllocHeapMemoryWithOption(g_threadman.heapId, sizeof(SceThreadmanVfpuContext),
                &allocOpt);
            thread->vfpuContext = vfpuContext;
            /*
             * PARITY-Q202 (source form): the NaN fill value is held in a local set
             * before the NULL check, and the option size is stored before the
             * alignment; this keeps the constant's high part live across the
             * allocation call as in the original code.
             */
            nan = 0x7F800001;
            if (vfpuContext == NULL) {
                thread->attr &= ~SCE_KERNEL_TH_USE_VFPU;
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_NO_MEMORY;
            }
            for (i = 0; i < 128; i++)
                vfpuContext->vr[i] = nan; // NaN
            vfpuContext->ctrl[0] = 0xE4; // PFXS
            vfpuContext->ctrl[1] = 0xE4; // PFXT
            vfpuContext->ctrl[2] = 0; // PFXD
            vfpuContext->ctrl[3] = 0x3F; // CC
            vfpuContext->ctrl[4] = 0; // INF4
            vfpuContext->ctrl[8] = 0x3F800001; // RCX0
            vfpuContext->ctrl[9] = 0x3F800002; // RCX1
            vfpuContext->ctrl[10] = 0x3F800004; // RCX2
            vfpuContext->ctrl[11] = 0x3F800008; // RCX3
            vfpuContext->ctrl[12] = 0x3F800000; // RCX4
            vfpuContext->ctrl[13] = 0x3F800000; // RCX5
            vfpuContext->ctrl[14] = 0x3F800000; // RCX6
            vfpuContext->ctrl[15] = 0x3F800000; // RCX7
        }
        thread->context->status |= SCE_THREADMAN_CONTEXT_STATUS_CU2;
        if (thread->syscallFrame != NULL)
            ((SceThreadmanSyscallFrame *)thread->syscallFrame)->status |= SCE_THREADMAN_CONTEXT_STATUS_CU2;
        THREADMAN_STATUS_OR(SCE_THREADMAN_CONTEXT_STATUS_CU2);
        sub_00012C04(thread);
    } else if (!(thread->attr & SCE_KERNEL_TH_USE_VFPU) && (status & SCE_THREADMAN_CONTEXT_STATUS_CU2)) {
        /* Disable the VFPU. */
        thread->context->status &= ~SCE_THREADMAN_CONTEXT_STATUS_CU2;
        if (thread->syscallFrame != NULL)
            ((SceThreadmanSyscallFrame *)thread->syscallFrame)->status &= ~SCE_THREADMAN_CONTEXT_STATUS_CU2;
        THREADMAN_STATUS_AND(~SCE_THREADMAN_CONTEXT_STATUS_CU2);
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q041 (b8da06e): Checks combined in one condition / merged identical error checks: the original shares one
 *     branch.
 * PARITY-Q047 (b8da06e): Interrupt state read raw / at the original's position.
 * PARITY-Q065 (0cfff40): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
/**
 * Change the current priority of a thread, moving it in its ready queue or its wait queue and
 * rescheduling if needed.
 *
 * @param thread The thread.
 * @param priority The new priority.
 * @param intr The interrupt state.
 *
 * @return 0.
 */
// Subroutine sub_00014598 - Address 0x00014598
s32 sub_00014598(SceThreadmanThread *thread, s32 priority, s32 intr)
{
    s32 isIntr;
    SceThreadmanThread *curThread;
    SceThreadmanWaitQ *waitQ;

    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (thread == g_threadman.curThread) {
        if (g_threadman.dispatchSuspended != 0) {
            thread->currentPriority = priority;
            return SCE_ERROR_OK;
        }
        if (isIntr) {
            thread->currentPriority = priority;
            thread->status = SCE_THREADMAN_STATUS_READY;
            sub_000127D0(thread);
            g_threadman.nextThread = NULL;
            return SCE_ERROR_OK;
        }
        if (priority < threadmanReadyQueueGetHighestPriority()) {
            /* The thread still has the highest priority. */
            thread->currentPriority = priority;
            return SCE_ERROR_OK;
        }
        thread->currentPriority = priority;
        if (intr) {
            thread->status = SCE_THREADMAN_STATUS_READY;
            sub_000127D0(thread);
        }
        g_threadman.nextThread = NULL;
        sub_00000FE4(0, intr);
        return SCE_ERROR_OK;
    }

    if (thread->status == SCE_THREADMAN_STATUS_READY) {
        threadmanReadyQueueRemove(thread, thread->currentPriority);
        thread->currentPriority = priority;
        if (!isIntr && g_threadman.dispatchSuspended == 0) {
            curThread = g_threadman.curThread;
            if (priority >= curThread->currentPriority) {
                thread->status = SCE_THREADMAN_STATUS_READY;
                sub_000127D0(thread);
                return SCE_ERROR_OK;
            }
            if (intr == 0) {
                thread->status = SCE_THREADMAN_STATUS_READY;
                sub_000127D0(thread);
                g_threadman.nextThread = NULL;
            } else {
                /* Switch directly to the thread, the current thread going first in its ready queue. */
                threadmanListAddTail(g_threadman.readyQueue[curThread->currentPriority].next, &curThread->link);
                g_threadman.readyBitmap[THREADMAN_READY_INDEX(curThread->currentPriority)] |=
                    THREADMAN_READY_BIT(curThread->currentPriority);
                curThread->status = SCE_THREADMAN_STATUS_READY;
                g_threadman.nextThread = thread;
                thread->status = SCE_THREADMAN_STATUS_RUNNING;
            }
            sub_00000FE4(0, intr);
            return SCE_ERROR_OK;
        }
        sub_000127D0(thread);
        g_threadman.nextThread = NULL;
        return SCE_ERROR_OK;
    }

    thread->currentPriority = priority;
    if ((thread->status == SCE_THREADMAN_STATUS_WAIT || thread->status == SCE_THREADMAN_STATUS_WAIT_SUSPEND)
        && thread->waitObj != NULL) {
        waitQ = UID_CB_TO_DATA(thread->waitObj, g_threadman.waitQType, SceThreadmanWaitQ);
        if (waitQ->attr & SCE_THREADMAN_WAITQ_ATTR_PRIORITY) {
            /* Move the thread in its wait queue, sorted by priority. */
            threadmanListRemove(&thread->link);
            sub_00000890(thread, thread->waitObj);
            if (sub_00000C30(thread->waitObj) != 0 && g_threadman.dispatchSuspended == 0) {
                g_threadman.nextThread = NULL;
                if (!isIntr)
                    sub_00000FE4(0, intr);
            }
        }
    }
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q047 (09d9be8): Interrupt state read raw / at the original's position.
 * PARITY-Q082 (b98d90a): user-mode test as attr >> 31 (no opcode-positive branch prediction; the original's layout).
 * PARITY-Q094: Priority range checked in each arm of the user/kernel test, each failing arm with its own
 *     resume-intr/k1-restore/return. With a shared `valid` flag set in both arms, ce2 if-conversion (IF-CASE-1)
 *     speculates the 2-insn user arm into the test block, which swaps two registers ($v1/$a0) against the original
 *     (regalloc -> exact). The calls in the failing arms keep the arms out of if-conversion. A goto to one shared
 *     error block gives the same output; the ternary form is identical to the flag form.
 */
/**
 * Change the priority of a thread.
 *
 * @param thid The UID of the thread (0 for the current thread).
 * @param priority The new priority (0 for the priority of the current thread).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelChangeThreadPriority - Address 0x00014854
s32 sceKernelChangeThreadPriority(SceUID thid, s32 priority)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 ret;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);

    if (thid == SCE_KERNEL_THREAD_ID_SELF) {
        if (isIntr) {
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
        }
        thid = g_threadman.curThread->uid;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_SIGNAL)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    if (thread->status == SCE_THREADMAN_STATUS_DORMANT || thread->status == SCE_THREADMAN_STATUS_DEAD) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
    }
    if (priority == 0)
        priority = g_threadman.curThread->currentPriority;
    if (thread->attr >> 31) {
        if ((u32)(priority - 8) >= 0x70) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
        }
    } else if ((u32)(priority - 1) >= 0x7E) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CHANGE_THREAD_PRIORITY, 2, thid, priority);
    thread->basePriority = priority;
    if (thread->mutexPriority < priority) {
        /* The priority stays raised by the owned mutexes. */
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_OK;
    }
    ret = sub_00014598(thread, priority, intr);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q078: the ready-bitmap scan (threadmanReadyQueueGetHighestPriority) written out at the site, its last
 *     clz going straight into priority (the original's movz writes the result register; equiv -> regalloc).
 */
/*
 * PARITY-Q047 (d9d18b9): Interrupt state read raw / at the original's position.
 */
/**
 * Rotate a ready queue: its first thread goes to its end. For the queue of the current thread, the
 * current thread yields to the other threads of the same priority.
 *
 * @param priority The priority of the queue (0 for the priority of the current thread, or for the
 * highest ready priority from an interrupt).
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelRotateThreadReadyQueue - Address 0x00014A3C
s32 sceKernelRotateThreadReadyQueue(s32 priority)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    SceThreadmanThread *thread;
    SceThreadmanListEntry *queue;
    SceThreadmanListEntry *first;

    oldK1 = pspShiftK1();
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    thread = g_threadman.curThread;

    if (isIntr && priority == 0) {
        s32 prio;
        s32 clz;

        prio = pspClz(g_threadman.readyBitmap[3]) + 32;
        clz = pspClz(g_threadman.readyBitmap[2]);
        if (g_threadman.readyBitmap[2] == 0)
            clz = prio;
        prio = clz + 32;
        clz = pspClz(g_threadman.readyBitmap[1]);
        if (g_threadman.readyBitmap[1] == 0)
            clz = prio;
        prio = clz + 32;
        priority = pspClz(g_threadman.readyBitmap[0]);
        if (g_threadman.readyBitmap[0] == 0)
            priority = prio;
    }
    if (pspK1IsUserMode()) {
        if (priority != 0 && (u32)(priority - 8) >= 0x70) {
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
        }
    } else if ((u32)priority >= 0x7F) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PRIORITY;
    }

    intr = sceKernelCpuSuspendIntr();
    THREADMAN_LOG(SCE_THREADMAN_LOG_ROTATE_THREAD_READY_QUEUE, 1, priority);
    if (!isIntr && priority == 0)
        priority = thread->currentPriority;

    queue = &g_threadman.readyQueue[priority];
    if (queue->next != queue) {
        if (priority != thread->currentPriority) {
            first = g_threadman.readyQueue[priority].next;
            threadmanListRemove(first);
            threadmanListAddTail(queue, first);
        } else if (g_threadman.dispatchSuspended == 0) {
            if (isIntr) {
                thread->status = SCE_THREADMAN_STATUS_READY;
                sub_000127D0(thread);
                g_threadman.nextThread = NULL;
            } else {
                if (intr) {
                    thread->status = SCE_THREADMAN_STATUS_READY;
                    sub_000127D0(thread);
                }
                g_threadman.nextThread = NULL;
                sub_00000FE4(0, intr);
            }
        }
    }

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q076: caller mode read once into a userMode local after the UID lookup and shared by both permission
 *     checks (the original computes k1 >> 31 once and reuses it).
 */
/*
 * PARITY-Q047 (d9d18b9): Interrupt state read raw / at the original's position.
 */
/**
 * Release a thread from its wait: its wait ends with SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED.
 *
 * @param thid The UID of the waiting thread.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReleaseWaitThread - Address 0x00014C20
s32 sceKernelReleaseWaitThread(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    s32 numWokenUp;
    u32 userMode;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();

    if (thid == SCE_KERNEL_THREAD_ID_SELF) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }

    intr = sceKernelCpuSuspendIntr();

    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    userMode = pspK1IsUserMode();
    if (!threadmanUidPermOkMode(cb, SCE_THREADMAN_UID_ATTR_SIGNAL, userMode)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    if (!isIntr && thread == g_threadman.curThread) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    }
    if (thread->status != SCE_THREADMAN_STATUS_WAIT && thread->status != SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_THREAD_IS_NOT_WAIT;
    }
    if (thread->waitObj != NULL && !threadmanUidPermOkMode((SceSysmemUidCB *)thread->waitObj, SCE_THREADMAN_UID_ATTR_CANCEL, userMode)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_RELEASE_WAIT_THREAD, 1, thid);
    numWokenUp = sub_00000924(thread);
    thread->waitStatus = SCE_ERROR_KERNEL_WAIT_STATUS_RELEASED;
    if (thread->waitObj != NULL)
        numWokenUp += sub_00000C30(thread->waitObj);
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
 * Get the exit status of a thread.
 *
 * @param thid The UID of the thread.
 *
 * @return The exit status of the thread (SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT if it did not
 * exit yet), < 0 on error.
 */
// Subroutine sceKernelGetThreadExitStatus - Address 0x00014E10
s32 sceKernelGetThreadExitStatus(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    s32 exitStatus;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    exitStatus = thread->exitStatus;
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_EXIT_STATUS, 2, thid, exitStatus);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return exitStatus;
}

/*
 * PARITY-Q040 (ab321a9): Separate ifs for the early error checks: the original gives each check its own branch.
 * PARITY-Q052 (ab321a9): Fatal break loop placed at the end of the function, as the original.
 */
/**
 * Return from a function run on a user stack extension (see sub_00017EB4()): restore the previous user
 * stack of the current thread and free the extension stack. Called by the syscall instruction built at the
 * top of the extension stack.
 *
 * @return The result of the function run on the extension stack.
 */
// Subroutine _sceKernelReturnFromChangeStack - Address 0x00014F10
s32 _sceKernelReturnFromChangeStack(void)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    s32 stackSize;
    SceUID blockId;
    void *stackLow;
    SceThreadmanThread *thread;
    SceThreadmanSyscallFrame *frame;
    SceThreadmanUserStackExt *ext;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    if (THREADMAN_IS_INTR_CONTEXT())
        goto fatal;
    if (thread->stackExts == NULL)
        goto fatal;

    intr = sceKernelCpuSuspendIntr();
    ext = (SceThreadmanUserStackExt *)thread->stackExts;
    frame = thread->syscallFrame;
    stackSize = thread->stackSize;
    blockId = thread->stackBlockId;
    ret = frame->v0;
    frame->epc = ext->epc;
    frame->ra = ext->ra;
    frame->sp = ext->sp;
    stackLow = thread->stackLow;
    thread->stackExts = ext->ext.next;
    thread->stackLow = ext->ext.low;
    thread->stackBlockId = ext->ext.blockId;
    thread->stackSize = ext->ext.size;
    sceKernelCpuResumeIntr(intr);

    if (thread->attr & SCE_KERNEL_TH_CLEAR_STACK)
        sceKernelMemset32(stackLow, 0, stackSize);
    if (blockId != 0)
        sceKernelFreePartitionMemory(blockId);

    pspSetK1(oldK1);
    return ret;

fatal:
    for (;;)
        pspBreak(THREADMAN_BREAKCODE_FATAL);
}

/**
 * Run a function on another user stack, in an existing memory block, on return of the syscall. The
 * previous stack is restored when the function returns (see _sceKernelReturnFromChangeStack()).
 *
 * @param blockId The memory block holding the new stack (the block is not freed with the stack).
 * @param func The function to run.
 * @param argp The argument of the function.
 *
 * @return 0xDEADBEEF (the caller receives the result of the function instead), < 0 on error.
 */
// Subroutine ThreadManForUser_28BFD974 - Address 0x0001502C
s32 ThreadManForUser_28BFD974(SceUID blockId, s32 (*func)(void *), void *argp)
{
    s32 oldK1;
    s32 ret;
    SceSysmemMemoryBlockInfo info;

    oldK1 = pspShiftK1();

    info.size = sizeof info;
    ret = sceKernelQueryMemoryBlockInfo(blockId, &info);
    if (ret < 0) {
        pspSetK1(oldK1);
        return ret;
    }
    if (!threadmanK1DynBufOk((void *)info.addr, info.memSize)) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (info.memSize < 0x200) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_STACK_SIZE;
    }

    ret = sub_00017EB4(0, (void *)info.addr, info.memSize, func, argp);
    if (ret < 0) {
        pspSetK1(oldK1);
        return ret;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CHANGE_THREAD_STACK, 3, blockId, func, argp);
    pspSetK1(oldK1);
    {
        /*
         * The original also loads 0xDEADBEEF into $v1, the second return register of the syscall, which this
         * function never reads: a dead register write C cannot express, so the original source must have set it with a
         * register variable / asm. Hand-written register effect, see threadman-deviations.md section 4.
         */
        /*
         * PARITY-Q002: $v1 = 0xDEADBEEF built in $v1 itself, as the original (no C variable lives in $v1
         * here)
         */
        asm volatile("li $v1, 0xDEADBEEF" : : : "$3");
    }
    return 0xDEADBEEF;
}

/**
 * Run a function on a new user stack on return of the syscall. The previous stack is restored and the
 * new one freed when the function returns (see _sceKernelReturnFromChangeStack()).
 *
 * @param size The size of the new stack.
 * @param func The function to run.
 * @param argp The argument of the function.
 *
 * @return 0xDEADBEEF (the caller receives the result of the function instead), < 0 on error.
 */
// Subroutine sceKernelExtendThreadStack - Address 0x0001512C
s32 sceKernelExtendThreadStack(SceSize size, s32 (*func)(void *), void *argp)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    SceUID blockId;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    if (size < 0x200) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_STACK_SIZE;
    }
    size = (size + 0xFF) & ~0xFF;

    intr = sceKernelCpuSuspendIntr();
    snprintf(g_threadman.statusBuf.stackExtName, 31, "exstack:%s",
        THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType)->name);
    blockId = sceKernelAllocPartitionMemory(thread->stackMpid, g_threadman.statusBuf.stackExtName,
        ((thread->attr >> 22) ^ 1) & 1, size, 0);
    sceKernelCpuResumeIntr(intr);
    if (blockId <= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }

    ret = sub_00017EB4(blockId, sceKernelGetBlockHeadAddr(blockId), size, func, argp);
    if (ret < 0) {
        sceKernelFreePartitionMemory(blockId);
        pspSetK1(oldK1);
        return ret;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_EXTEND_THREAD_STACK, 3, size, func, argp);
    pspSetK1(oldK1);
    {
        /*
         * The original also loads 0xDEADBEEF into $v1, the second return register of the syscall, which this
         * function never reads: a dead register write C cannot express, so the original source must have set it with a
         * register variable / asm. Hand-written register effect, see threadman-deviations.md section 4.
         */
        /*
         * PARITY-Q002: $v1 = 0xDEADBEEF built in $v1 itself, as the original (no C variable lives in $v1
         * here)
         */
        asm volatile("li $v1, 0xDEADBEEF" : : : "$3");
    }
    return 0xDEADBEEF;
}

/*
 * PARITY-Q013 (e48c6d9): Uses mtie + 8-nop interrupt switch.
 * PARITY-Q051 (c944f20): Access through a local/named pointer (the original's addressing).
 */
/**
 * Run a kernel function on a new stack (the kernel stack of a user mode thread). The stack is freed when
 * the function returns.
 *
 * @param size The size of the new stack.
 * @param func The function to run.
 * @param arg The argument of the function.
 *
 * @return The result of the function, < 0 on error.
 */
// Subroutine sceKernelExtendKernelStack - Address 0x000152C0
s32 sceKernelExtendKernelStack(SceSize size, s32 (*func)(void *), void *arg)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    s32 sp;
    SceUID blockId;
    void *stackLow;
    SceThreadmanThread *thread;
    SceThreadmanStackExt ext;
    SceThreadmanStackExt *curExt;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    if (pspK1IsUserMode()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    if ((s32)func >= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (size < 0x200) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_STACK_SIZE;
    }

    intr = sceKernelCpuSuspendIntr();
    snprintf(g_threadman.statusBuf.stackExtName, 31, "exkstack:%s",
        THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType)->name);
    size = (size + 0xFF) & ~0xFF;
    blockId = sceKernelAllocPartitionMemory(thread->kStackMpid, g_threadman.statusBuf.stackExtName,
        SCE_KERNEL_SMEM_High, size, 0);
    sceKernelCpuResumeIntr(intr);
    if (blockId <= 0) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NO_MEMORY;
    }

    stackLow = sceKernelGetBlockHeadAddr(blockId);
    if (!THREADMAN_IS_USER_THREAD(thread) && (thread->attr & SCE_KERNEL_TH_NO_FILLSTACK) == 0)
        sceKernelMemset32(stackLow, -1, size);
    *(SceUID *)stackLow = thread->uid;
    THREADMAN_LOG(SCE_THREADMAN_LOG_EXTEND_KERNEL_STACK, 2, func, arg);

    intr = sceKernelCpuSuspendIntr();
    ext.next = thread->kStackExts;
    thread->kStackExts = &ext;
    if (THREADMAN_IS_USER_THREAD(thread)) {
        ext.low = thread->kStackLow;
        ext.blockId = thread->kStackBlockId;
        ext.size = thread->kStackSize;
        thread->kStackLow = stackLow;
        thread->kStackSize = size;
        thread->kStackBlockId = blockId;
    } else {
        ext.low = thread->stackLow;
        ext.blockId = thread->stackBlockId;
        ext.size = thread->stackSize;
        thread->stackLow = stackLow;
        thread->stackBlockId = blockId;
        thread->stackSize = size;
    }

    /* Run the function on the new stack. */
    sp = pspGetSp();
    pspSetSp((s32)stackLow + size);
    /* The interrupts are switched inline around the call (mtie/mfie), not through the interrupt manager. */
    pspMtic(intr);
    pspSetK1(oldK1);
    ret = func(arg);
    oldK1 = pspGetK1();
    pspSetK1((u32)oldK1 >> 16);
    intr = pspMfic();
    pspMtic(0);
    pspSetSp(sp);

    curExt = thread->kStackExts;
    if (THREADMAN_IS_USER_THREAD(thread)) {
        thread->kStackBlockId = curExt->blockId;
        thread->kStackLow = curExt->low;
        thread->kStackSize = curExt->size;
    } else {
        thread->stackBlockId = curExt->blockId;
        thread->stackLow = curExt->low;
        thread->stackSize = curExt->size;
    }
    thread->kStackExts = curExt->next;
    sceKernelCpuResumeIntr(intr);

    if (!THREADMAN_IS_USER_THREAD(thread) && (thread->attr & SCE_KERNEL_TH_CLEAR_STACK))
        sceKernelMemset32(stackLow, 0, size);
    sceKernelFreePartitionMemory(blockId);

    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q082: user-mode test as attr >> 31 and the $k0 area in a pointer local: GCC 3.3's branch prediction then
 *     sees no sign test (opcode heuristic) and a pointer NULL test (pointer heuristic), giving the original's
 *     inline layout of the TLS release block
 */
/**
 * Clear the $k0 area and the TLS blocks of a dead thread and, with SCE_KERNEL_TH_CLEAR_STACK, its stacks.
 *
 * @param thread The thread.
 * @param intr The interrupt state of the caller (the interrupts are enabled while the stacks are cleared).
 */
// Subroutine sub_000155B8 - Address 0x000155B8
void sub_000155B8(SceThreadmanThread *thread, s32 intr)
{
    s32 i;
    SceThreadmanStackExt *ext;
    SceThreadmanK0 *k0;

    if (thread->attr >> 31) {
        k0 = thread->k0;
        if (k0 != NULL) {
        _sceKernelReleaseThreadTls(thread);
        /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
        memset(thread->k0, 0, sizeof(SceThreadmanK0));
        }
    }

    if ((thread->attr & SCE_KERNEL_TH_CLEAR_STACK) == 0)
        return;

    sceKernelCpuResumeIntr(intr);
    sceKernelMemset32(thread->stackLow, 0, thread->stackSize);
    if (THREADMAN_IS_USER_THREAD(thread)) {
        for (ext = thread->stackExts; ext != NULL; ext = ext->next)
            sceKernelMemset32(ext->low, 0, ext->size);
        sceKernelMemset32(thread->kStackLow, 0, thread->kStackSize);
    }
    for (ext = thread->kStackExts; ext != NULL; ext = ext->next)
        sceKernelMemset32(ext->low, 0, ext->size);
    sceKernelCpuSuspendIntr();
}

/*
 * PARITY-Q074: The first loop keeps the freed block in its own local (kBlockId), separate from the second loop's
 * blockId (the original's register use: a temporary, not a callee-saved register, in the first loop).
 * PARITY-Q050: Both loops test the list head, then load it into ext in the body (the original's loop-closing
 * load and register copy).
 */
/**
 * Free the stack extensions of a thread, restoring its original stacks.
 *
 * @param thread The thread.
 */
// Subroutine sub_000156BC - Address 0x000156BC
void sub_000156BC(SceThreadmanThread *thread)
{
    SceUID blockId;
    SceUID kBlockId;
    SceThreadmanStackExt *ext;

    while (thread->kStackExts != NULL) {
        ext = thread->kStackExts;
        if (THREADMAN_IS_USER_THREAD(thread)) {
            kBlockId = thread->kStackBlockId;
            thread->kStackBlockId = ext->blockId;
            thread->kStackLow = ext->low;
            thread->kStackSize = ext->size;
        } else {
            kBlockId = thread->stackBlockId;
            thread->stackBlockId = ext->blockId;
            thread->stackLow = ext->low;
            thread->stackSize = ext->size;
        }
        thread->kStackExts = ext->next;
        sceKernelFreePartitionMemory(kBlockId);
    }

    if (!THREADMAN_IS_USER_THREAD(thread))
        return;

    while (thread->stackExts != NULL) {
        ext = thread->stackExts;
        blockId = thread->stackBlockId;
        thread->stackSize = ext->size;
        thread->stackLow = ext->low;
        thread->stackBlockId = ext->blockId;
        thread->stackExts = ext->next;
        sceKernelFreeHeapMemory(g_threadman.heapId, ext);
        if (blockId != 0)
            sceKernelFreePartitionMemory(blockId);
    }
}

/*
 * PARITY-Q048 (6fd4087): Value read once into a local (the original keeps it in one register).
 * PARITY-Q083: kernel-stack fast path as `if (freeSize >= 64) goto end;` (goto prediction cancels the call heuristic;
 * the original's overflow fall-through layout).
 */
/**
 * Check the free space of the user stack of the current thread (its stack for a kernel mode thread).
 * The thread is deleted if less than 64 bytes are left.
 *
 * @return The free stack space, 0 if the thread has no stack or when called from an interrupt handler.
 */
// Subroutine sceKernelCheckThreadStack - Address 0x000157C8
/*
 * PARITY-Q113: the kernel-thread path is written first (inverted user-thread test) and the user-thread path
 * comes after it. -frename-registers walks the blocks in insn order and picks the least recently used
 * register, so the block order decides the temporaries ($a0/$a3/$t0.. in the kernel path, $t2-$t4 in the
 * user path) the original binary uses. Same output with an explicit else around the user path.
 */
s32 sceKernelCheckThreadStack(void)
{
    s32 oldK1;
    s32 freeSize;
    s32 userFreeSize;
    u32 sp;
    SceThreadmanThread *thread;
    SceThreadmanThread *cur;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return 0;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CHECK_THREAD_STACK, 0);
    if (!THREADMAN_IS_USER_THREAD(thread)) {
        cur = g_threadman.curThread;
        pspSetK1(oldK1);
        if (THREADMAN_IS_USER_THREAD(cur))
            freeSize = (u32)&freeSize - (u32)cur->kStackLow;
        else {
            if (cur->stackLow == NULL)
                return 0;
            freeSize = (u32)&freeSize - (u32)cur->stackLow;
        }

        if (freeSize >= SCE_THREADMAN_MIN_FREE_STACK)
            goto end;
        sceKernelCpuSuspendIntr();
        SaveThreadContext(cur->context);
        sub_000177C4(cur, cur->attr >> 31, cur->context->gpr[29] /* $sp */);
        sub_00000FE4(1, 1);

end:
        return freeSize;
    }

    /* The user stack pointer is the one saved by the syscall. */
    sp = ((SceThreadmanSyscallFrame *)thread->syscallFrame)->sp;
    userFreeSize = sp - (u32)thread->stackLow;
    if (userFreeSize < SCE_THREADMAN_MIN_FREE_STACK) {
        sceKernelCpuSuspendIntr();
        SaveThreadContext(thread->context);
        sub_000177C4(thread, 0, sp);
        sub_00000FE4(1, 1);
    }
    pspSetK1(oldK1);
    return userFreeSize;
}

/**
 * Get the never used space of the stack of a thread (its user stack for a user mode thread), measured
 * from the stack fill pattern.
 *
 * @param thid The UID of the thread, 0 for the current thread.
 *
 * @return The free stack space, < 0 on error.
 */
// Subroutine sceKernelGetThreadStackFreeSize - Address 0x00015928
s32 sceKernelGetThreadStackFreeSize(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    u32 i;
    u32 count;
    u32 stackSize;
    u32 *stack;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (thid == 0)
        thid = g_threadman.curThread->uid;
    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_STACK_FREE_SIZE, 1, thid);
    stackSize = thread->stackSize;
    stack = thread->stackLow;
    sceKernelCpuResumeIntr(intr);

    /* Skip the first 16 bytes of the stack (holding the UID of the thread). */
    count = stackSize / 4 - 4;
    stack += 4;
    for (i = 0; i < count; i++) {
        if (stack[i] != 0xFFFFFFFF)
            break;
    }

    pspSetK1(oldK1);
    return i * 4;
}

/*
 * PARITY-Q042 (684132a): Statement/store order taken from the original's store order.
 */
/**
 * Get the never used space of the kernel stack of a thread (its stack for a kernel mode thread),
 * measured from the stack fill pattern.
 *
 * @param thid The UID of the thread, 0 for the current thread.
 *
 * @return The free stack space, < 0 on error.
 */
// Subroutine sceKernelGetThreadKernelStackFreeSize - Address 0x00015A8C
s32 sceKernelGetThreadKernelStackFreeSize(SceUID thid)
{
    s32 oldK1;
    s32 intr;
    u32 i;
    u32 count;
    u32 stackSize;
    u32 *stack;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (thid == 0)
        thid = g_threadman.curThread->uid;
    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }
    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_KERNEL_STACK_FREE_SIZE, 1, thid);
    if (THREADMAN_IS_USER_THREAD(thread)) {
        stackSize = thread->kStackSize;
        stack = thread->kStackLow;
    } else {
        stackSize = thread->stackSize;
        stack = thread->stackLow;
    }
    sceKernelCpuResumeIntr(intr);

    /* Skip the first 16 bytes of the stack (holding the UID of the thread). */
    count = stackSize / 4 - 4;
    stack += 4;
    for (i = 0; i < count; i++) {
        if (stack[i] != 0xFFFFFFFF)
            break;
    }

    pspSetK1(oldK1);
    return i * 4;
}

/*
 * PARITY-Q042 (4d63843, b89af15): Statement/store order taken from the original's store order.
 * PARITY-Q047 (09d9be8): Interrupt state read raw / at the original's position.
 * PARITY-Q048 (b89af15): Value read once into a local (the original keeps it in one register).
 * PARITY-Q049 (b89af15): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q054 (b0f43f4): Comparison operand order as the original's branch operands.
 * PARITY-Q061 (7bc6a6b, 4d63843): Status copy-out from buf / size store through g_threadman / name read before the
 *     clear, as the original.
 * PARITY-Q139: gpReg copied after currentPriority, as the original's last store of the field-copy run.
 * PARITY-Q137: &buf->runClocks taken into a local before the run time is computed (as Q136): the original computes
 *     the address before the current-thread test and keeps it in a callee-saved register across the call.
 */
/**
 * Get the status of a thread.
 *
 * @param thid The UID of the thread, 0 for the current thread.
 * @param info Receives the status. Its size field must be set by the caller.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferThreadStatus - Address 0x00015C04
s32 sceKernelReferThreadStatus(SceUID thid, SceKernelThreadInfo *info)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    u64 runClocks;
    SceKernelSysClock *clock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceKernelThreadInfo *buf;
    s32 status;
    const char *name;

    oldK1 = pspShiftK1();
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (thid == 0) {
        if (isIntr) {
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
        }
        thid = g_threadman.curThread->uid;
    }

    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_THREAD_STATUS, 2, thid, info);

    name = cb->name;
    buf = &g_threadman.statusBuf.threadInfo;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    if (name != NULL)
        strncpy(buf->name, name, SCE_UID_NAME_LEN);
    buf->attr = thread->attr;
    status = thread->status;
    buf->status = status;
    buf->entry = thread->entry;
    buf->stack = thread->stackLow;
    buf->stackSize = thread->stackSize;
    buf->initPriority = thread->initPriority;
    buf->currentPriority = thread->currentPriority;
    buf->gpReg = (void *)thread->gp;
    if (status == SCE_THREADMAN_STATUS_WAIT || status == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
        if (thread->waitWithCallbacks != 0)
            buf->waitType = thread->waitType + 0x100;
        else
            buf->waitType = thread->waitType;
        if (thread->waitObj != NULL)
            buf->waitId = thread->waitObj->uid;
        else
            buf->waitId = 0;
    }
    buf->wakeupCount = thread->wakeupCount;
    buf->exitStatus = thread->exitStatus;
    clock = &buf->runClocks;
    runClocks = threadmanSysClockGet(&thread->runClocks);
    if (g_threadman.curThread == thread)
        runClocks += _sceKernelGetSystemTime64() - threadmanSysClockGet(&g_threadman.lastSwitchClock);
    threadmanSysClockSet(clock, runClocks);
    buf->intrPreemptCount = thread->intrPreemptCount;
    buf->threadPreemptCount = thread->threadPreemptCount;
    buf->releaseCount = thread->releaseCount;
    buf->notifyCallback = thread->callbackNotified;

    if (!threadmanSdkVersionAbove(0x02060010)) {
        /* Older applications use the smaller structure, with the wait type only. */
        g_threadman.statusBuf.threadInfo.size = 0x68;
        buf->waitType &= 0xF;
    } else if (info->size > sizeof(*buf)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_SIZE;
    }
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.threadInfo);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q048 (fcf209a): Value read once into a local (the original keeps it in one register).
 * PARITY-Q049 (fcf209a): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q054 (b0f43f4): Comparison operand order as the original's branch operands.
 * PARITY-Q061 (1737d25): Status copy-out from buf / size store through g_threadman / name read before the clear, as
 *     the original.
 * PARITY-Q137: &buf->runClocks taken into a local before the run time is computed (as Q136): the original computes
 *     the address before the current-thread test and keeps it in a callee-saved register across the call.
 * PARITY-Q138: The copy-out names &g_threadman.statusBuf.threadRunStatus as its source (as sceKernelReferThreadStatus
 *     and sceKernelReferThreadDebugStatus do): the original reads the size through the g_threadman base.
 */
/**
 * Get the runtime status of a thread.
 *
 * @param thid The UID of the thread, 0 for the current thread.
 * @param status Receives the status. Its size field must be set by the caller.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferThreadRunStatus - Address 0x00015F50
s32 sceKernelReferThreadRunStatus(SceUID thid, SceKernelThreadRunStatus *status)
{
    s32 threadStatus;
    s32 oldK1;
    s32 intr;
    u64 runClocks;
    SceKernelSysClock *clock;
    SceSysmemUidCB *cb;
    SceThreadmanThread *thread;
    SceKernelThreadRunStatus *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(status, sizeof(*status))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }

    intr = sceKernelCpuSuspendIntr();
    if (thid == 0)
        thid = g_threadman.curThread->uid;

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_THREAD_RUN_STATUS, 2, thid, status);

    buf = &g_threadman.statusBuf.threadRunStatus;
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(buf, 0, sizeof(*buf));
    buf->size = sizeof(*buf);
    threadStatus = thread->status;
    buf->status = threadStatus;
    buf->currentPriority = thread->currentPriority;
    if (threadStatus == SCE_THREADMAN_STATUS_WAIT || threadStatus == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
        buf->waitType = thread->waitType;
        if (thread->waitObj != NULL)
            buf->waitId = thread->waitObj->uid;
        else
            buf->waitId = 0;
    }
    buf->wakeupCount = thread->wakeupCount;
    clock = &buf->runClocks;
    runClocks = threadmanSysClockGet(&thread->runClocks);
    if (g_threadman.curThread == thread)
        runClocks += _sceKernelGetSystemTime64() - threadmanSysClockGet(&g_threadman.lastSwitchClock);
    threadmanSysClockSet(clock, runClocks);
    buf->intrPreemptCount = thread->intrPreemptCount;
    buf->threadPreemptCount = thread->threadPreemptCount;
    buf->releaseCount = thread->releaseCount;
    buf->notifyCallback = thread->callbackNotified;
    THREADMAN_COPY_STATUS(status, &g_threadman.statusBuf.threadRunStatus);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q013 (e48c6d9, 7792c36): Uses mtie + 8-nop interrupt switch.
 * PARITY-Q042 (ed256d9): Statement/store order taken from the original's store order.
 */
/**
 * Get the debug status of a thread (with its contexts, stacks and wait data).
 *
 * @param thid The UID of the thread, 0 for the current thread.
 * @param info Receives the status. Its size field must be set by the caller.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferThreadDebugStatus - Address 0x000161EC
s32 sceKernelReferThreadDebugStatus(SceUID thid, SceKernelThreadKInfo *info)
{
    s32 oldK1;
    s32 intr;
    s32 isIntr;
    SceSysmemUidCB *cb;
    SceKernelThreadKInfo *buf;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(info, sizeof(*info))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }
    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    if (thid == 0) {
        if (isIntr) {
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
        }
        thid = g_threadman.curThread->uid;
    }

    /* The interrupts are disabled inline (mfie/mtie), not through the interrupt manager. */
    intr = threadmanSuspendIntrSync();

    if (sceKernelGetUIDcontrolBlockWithType(thid, g_threadman.threadType, &cb) != SCE_ERROR_OK) {
        threadmanResumeIntrSync(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;
    }
    if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
        threadmanResumeIntrSync(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_THREAD_DEBUG_STATUS, 2, thid, info);
    buf = &g_threadman.statusBuf.threadKInfo;
    sub_000180AC(cb, buf, isIntr);
    THREADMAN_COPY_STATUS(info, &g_threadman.statusBuf.threadKInfo);

    threadmanResumeIntrSync(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q050: Both list walks skip a UID without the refer permission with `continue` (the original's layout: the
 *     counting block out of line, the loop step as the fall-through).
 * PARITY-Q090: The thread-list switch is on (s32)type: the original's case tree compares signed (slti) and splits at
 *     SCE_KERNEL_TMID_SuspendThread.
 * PARITY-Q180: The UID-type walk takes the address of the link first (`link = &cb->PARENT0`) and steps with a plain
 *     dereference (`cb = *link`). cb is address-taken (&cb for sceKernelGetUIDcontrolBlock), so it has a stack slot.
 *     A member read (cb->PARENT0) is marked as a struct access, which the compiler assumes cannot alias a scalar
 *     local (alias.c fixed_scalar_and_varying_struct_p), so loop.c load_mems keeps cb in a register and sinks its
 *     store out of the loop. A plain *link read may alias the slot, so cb is stored on every step as in the original
 *     (lw $t5,0($a0); sw $t5,0($sp)).
 */
/**
 * Get a list of UIDs from threadman. Allows you to enumerate resources such as threads or semaphores.
 *
 * @param type The type of resource to list, one of ::SceKernelIdListType, or the UID of a synchronization
 * object to list the threads waiting on it.
 * @param readbuf A pointer to a buffer to store the list.
 * @param readbufsize The size of the buffer in SceUID units.
 * @param idcount Pointer to an integer in which to return the total number of ids (can be NULL).
 *
 * @return The number of ids written to readbuf on success, < 0 on error.
 */
// Subroutine sceKernelGetThreadmanIdList - Address 0x000163E0
s32 sceKernelGetThreadmanIdList(SceKernelIdListType type, SceUID *readbuf, s32 readbufsize, s32 *idcount)
{
    s32 oldK1;
    s32 intr;
    s32 numWritten;
    s32 numIds;
    SceSysmemUidCB *cb;
    SceSysmemUidCB *uidType;
    SceSysmemUidCB **link; /* PARITY-Q180 */
    SceThreadmanListEntry *list;
    SceThreadmanListEntry *entry;

    oldK1 = pspShiftK1();
    if (!threadmanK1DynBufOk(readbuf, readbufsize * sizeof(SceUID)) || !pspK1StaBufOk(idcount, sizeof(*idcount))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREADMAN_ID_LIST, 4, type, readbuf, readbufsize, idcount);

    if ((u32)(type - 1) < 63) {
        /* List all the objects of a UID type. */
        switch (type) {
        case SCE_KERNEL_TMID_Thread:
            uidType = g_threadman.threadType;
            break;
        case SCE_KERNEL_TMID_Semaphore:
            uidType = g_threadman.semaType;
            break;
        case SCE_KERNEL_TMID_EventFlag:
            uidType = g_threadman.eventFlagType;
            break;
        case SCE_KERNEL_TMID_Mbox:
            uidType = g_threadman.mbxType;
            break;
        case SCE_KERNEL_TMID_Vpl:
            uidType = g_threadman.vplType;
            break;
        case SCE_KERNEL_TMID_Fpl:
            uidType = g_threadman.fplType;
            break;
        case SCE_KERNEL_TMID_Mpipe:
            uidType = g_threadman.msgPipeType;
            break;
        case SCE_KERNEL_TMID_Callback:
            uidType = g_threadman.callbackType;
            break;
        case SCE_KERNEL_TMID_ThreadEventHandler:
            uidType = g_threadman.threadEventHandlerType;
            break;
        case SCE_KERNEL_TMID_Alarm:
            uidType = g_threadman.alarmType;
            break;
        case SCE_KERNEL_TMID_VTimer:
            uidType = g_threadman.vtimerType;
            break;
        case SCE_KERNEL_TMID_Mutex:
            uidType = g_threadman.mutexType;
            break;
        case SCE_KERNEL_TMID_LwMutex:
            uidType = g_threadman.lwMutexType;
            break;
        case SCE_KERNEL_TMID_Tlspl:
            uidType = g_threadman.tlsplType;
            break;
        default:
            sceKernelCpuResumeIntr(intr);
            pspSetK1(oldK1);
            return SCE_ERROR_KERNEL_ILLEGAL_TYPE;
        }

        numWritten = 0;
        numIds = 0;
        for (cb = uidType->PARENT0; cb != uidType; cb = *link) { /* PARITY-Q180 */
            link = &cb->PARENT0;
            if (uidType == g_threadman.threadType) {
                SceThreadmanThread *thread = UID_CB_TO_DATA(cb, uidType, SceThreadmanThread);
                /* The system threads are hidden. */
                if (thread == g_threadman.idleThread || thread == g_threadman.cleanerThread)
                    continue;
            }
            if (!THREADMAN_UID_PERM_OK(cb, 0xFF))
                continue;
            numIds++;
            if (numWritten < readbufsize) {
                *(readbuf++) = cb->uid;
                numWritten++;
            }
        }
    } else {
        /* List threads. */
        switch ((s32)type) {
        case SCE_KERNEL_TMID_SleepThread:
            list = &g_threadman.sleepingThreads;
            break;
        case SCE_KERNEL_TMID_DelayThread:
            list = &g_threadman.delayedThreads;
            break;
        case SCE_KERNEL_TMID_SuspendThread:
            list = &g_threadman.suspendedThreads;
            break;
        case SCE_KERNEL_TMID_DormantThread:
            list = &g_threadman.dormantThreads;
            break;
        default:
            /* The threads waiting on a synchronization object. */
            if (sceKernelGetUIDcontrolBlock(type, &cb) != SCE_ERROR_OK
             || !sceKernelIsKindOf(cb, g_threadman.waitQType)) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_ILLEGAL_TYPE;
            }
            if (!THREADMAN_UID_PERM_OK(cb, SCE_THREADMAN_UID_ATTR_REFER)) {
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return SCE_ERROR_KERNEL_ILLEGAL_PERMISSION;
            }
            list = &UID_CB_TO_DATA(cb, g_threadman.waitQType, SceThreadmanWaitQ)->waitList;
            break;
        }

        numWritten = 0;
        numIds = 0;
        for (entry = list->next; entry != list; entry = entry->next) {
            SceThreadmanThread *thread = (SceThreadmanThread *)entry;
            if (!THREADMAN_UID_PERM_OK(THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType), 0xFF))
                continue;
            numIds++;
            if (numWritten < readbufsize) {
                *(readbuf++) = thread->uid;
                numWritten++;
            }
        }
    }

    sceKernelCpuResumeIntr(intr);
    if (idcount != NULL)
        *idcount = numIds;
    pspSetK1(oldK1);
    return numWritten;
}

/*
 * PARITY-Q040 (a142225): Separate ifs for the early error checks: the original gives each check its own branch.
 */
/**
 * Get the type of a threadman uid
 *
 * @param uid The uid to get the type from
 *
 * @return The type (one of ::SceKernelIdListType), < 0 on error
 */
// Subroutine sceKernelGetThreadmanIdType - Address 0x00016784
SceKernelIdListType sceKernelGetThreadmanIdType(SceUID uid)
{
    s32 oldK1;
    s32 intr;
    SceKernelIdListType type;
    SceSysmemUidCB *cb;

    oldK1 = pspShiftK1();
    intr = sceKernelCpuSuspendIntr();

    if (sceKernelGetUIDcontrolBlock(uid, &cb) != SCE_ERROR_OK) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT;
    }
    if (!THREADMAN_UID_PERM_OK(cb, 0xFF)) {
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT;
    }
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREADMAN_ID_TYPE, 1, uid);

    if (sceKernelIsKindOf(cb, g_threadman.threadType))
        type = SCE_KERNEL_TMID_Thread;
    else if (sceKernelIsKindOf(cb, g_threadman.semaType))
        type = SCE_KERNEL_TMID_Semaphore;
    else if (sceKernelIsKindOf(cb, g_threadman.eventFlagType))
        type = SCE_KERNEL_TMID_EventFlag;
    else if (sceKernelIsKindOf(cb, g_threadman.mbxType))
        type = SCE_KERNEL_TMID_Mbox;
    else if (sceKernelIsKindOf(cb, g_threadman.vplType))
        type = SCE_KERNEL_TMID_Vpl;
    else if (sceKernelIsKindOf(cb, g_threadman.fplType))
        type = SCE_KERNEL_TMID_Fpl;
    else if (sceKernelIsKindOf(cb, g_threadman.msgPipeType))
        type = SCE_KERNEL_TMID_Mpipe;
    else if (sceKernelIsKindOf(cb, g_threadman.callbackType))
        type = SCE_KERNEL_TMID_Callback;
    else if (sceKernelIsKindOf(cb, g_threadman.threadEventHandlerType))
        type = SCE_KERNEL_TMID_ThreadEventHandler;
    else if (sceKernelIsKindOf(cb, g_threadman.alarmType))
        type = SCE_KERNEL_TMID_Alarm;
    else if (sceKernelIsKindOf(cb, g_threadman.vtimerType))
        type = SCE_KERNEL_TMID_VTimer;
    else if (sceKernelIsKindOf(cb, g_threadman.mutexType))
        type = SCE_KERNEL_TMID_Mutex;
    else if (sceKernelIsKindOf(cb, g_threadman.lwMutexType))
        type = SCE_KERNEL_TMID_LwMutex;
    else if (sceKernelIsKindOf(cb, g_threadman.tlsplType))
        type = SCE_KERNEL_TMID_Tlspl;
    else
        type = SCE_ERROR_KERNEL_ILLEGAL_ARGUMENT;

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return type;
}

/*
 * PARITY-Q048 (0e15864): Value read once into a local (the original keeps it in one register).
 * PARITY-Q064 (9568ff2): Switch result kept in its own variable / lookup result and owner type in one variable (the
 *     original's register use).
 * PARITY-Q192 (trick): owner is a local register variable ($s1; blockId then gets $s2 as in the original); the
 *     thread-found type store builds SCE_KERNEL_TMID_Thread in a $v0 register variable held by an empty asm input,
 *     and the final type store has an empty volatile asm. Without them, cross-jumping merges the two stores (the
 *     thread block branches into the main tail, 5 instructions short) and regrename picks $a3/$t5. equiv -> exact.
 *     Each of the three alone or in pairs: still equiv.
 */
/**
 * Find the threadman object owning a memory block (a memory pool or a thread stack).
 *
 * @param blockId The UID of the memory block.
 * @param type Receives the type of the owner (one of ::SceKernelIdListType), can be NULL.
 *
 * @return The UID of the owner on success, < 0 on error.
 */
// Subroutine sceKernelGetThreadmanIdOwner - Address 0x0001696C
SceUID sceKernelGetThreadmanIdOwner(SceUID blockId, SceKernelIdListType *type)
{
    s32 oldK1;
    s32 intr;
    s32 ret;
    register SceUID owner asm("$17"); /* PARITY-Q192: owner in $s1, blockId in $s2 */
    SceSysmemUidCB *cb;
    SceSysmemUidCB *threadType;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(type, sizeof(*type))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    /* ret first receives the UID found by the lookup, then the type of its owner. */
    if ((ret = _sceKernelGetVplByHeapId(blockId)) > 0) {
        owner = ret;
        ret = SCE_KERNEL_TMID_Vpl;
    } else if ((ret = _sceKernelGetFplByBlockId(blockId)) > 0) {
        owner = ret;
        ret = SCE_KERNEL_TMID_Fpl;
    } else if ((ret = _sceKernelGetMsgPipeByBlockId(blockId)) > 0) {
        owner = ret;
        ret = SCE_KERNEL_TMID_Mpipe;
    } else if ((ret = _sceKernelGetTlsplByBlockId(blockId)) > 0) {
        owner = ret;
        ret = SCE_KERNEL_TMID_Tlspl;
    } else {
        /* Look for a thread stack, or a saved stack of a thread stack extension. */
        owner = 0;
        threadType = g_threadman.threadType;
        for (cb = threadType->PARENT0; cb != threadType; cb = cb->PARENT0) {
            SceThreadmanThread *thread = UID_CB_TO_DATA(cb, threadType, SceThreadmanThread);
            SceThreadmanStackExt *ext;

            if (thread->stackBlockId == blockId || thread->kStackBlockId == blockId)
                owner = thread->uid;
            for (ext = thread->kStackExts; ext != NULL; ext = ext->next) {
                if (ext->blockId == blockId) {
                    owner = thread->uid;
                    break;
                }
            }
            for (ext = thread->stackExts; ext != NULL; ext = ext->next) {
                if (ext->blockId == blockId) {
                    owner = thread->uid;
                    break;
                }
            }
            if (owner > 0) {
                if (type != NULL) {
                    /* PARITY-Q192: type built in $v0; keeps this store out of the cross-jump with the one below */
                    register s32 tmid asm("$2") = SCE_KERNEL_TMID_Thread;
                    asm("" : : "r"(tmid));
                    *type = tmid;
                }
                sceKernelCpuResumeIntr(intr);
                pspSetK1(oldK1);
                return owner;
            }
        }
        sceKernelCpuResumeIntr(intr);
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_UNKNOWN_UID;
    }

    if (type != NULL) {
        asm volatile(""); /* PARITY-Q192: the store keeps its own beqz/nop block */
        *type = ret;
    }
    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return owner;
}

/**
 * Disable the check of the $k0 register of the threads (sub_00017844()), by patching its code into an
 * immediate return.
 *
 * @param disable Non-zero to disable the check.
 *
 * @return 0.
 */
// Subroutine ThreadManForKernel_D366D35A - Address 0x00016B1C
s32 ThreadManForKernel_D366D35A(s32 disable)
{
    if (disable) {
        u32 *code = (u32 *)sub_00017844;

        code[0] = 0x03E00008; // jr $ra
        code[1] = 0; // nop
    }
    return SCE_ERROR_OK;
}

/* The functions of the "Thread" UID type (defined here: the original has it after the jump table of
 * sceKernelGetThreadmanIdList() in .data). */
// 0x00019E70
SceSysmemUidLookupFunc g_ThreadFuncs[] = {
    { 0xD310D2D9, _sceKernelThreadInitialize },
    { 0x87089863, _sceKernelThreadDelete },
    { 0, NULL }
};

/**
 * Create the "Thread" UID type.
 *
 * @return 0.
 */
// Subroutine sub_00016B40 - Address 0x00016B40
s32 sub_00016B40(void)
{
    sceKernelCreateUIDtypeInherit("WaitQ", "Thread", sizeof(SceThreadmanThread), g_ThreadFuncs, NULL,
        &g_threadman.threadType);
    return SCE_ERROR_OK;
}

/**
 * Release the exception and interrupt handlers of the thread manager before a reboot.
 *
 * @return 0.
 */
// Subroutine module_reboot_before - Address 0x00016B84
s32 ThreadManRebootBefore(void *arg0, s32 arg1, s32 arg2, s32 arg3)
{
    s32 intr;

    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;

    intr = sceKernelCpuSuspendIntr();
    sceKernelReleaseExceptionHandler(SCE_THREADMAN_EXCEPTION_COP_UNUSABLE, sub_00000338);
    sceKernelReleaseIntrHandler(g_threadman.sysTimerIntr);
    sceKernelReleaseIntrHandler(SCE_THREADMAN_THREAD_INTR);
    sceKernelCpuResumeIntr(intr);
    return SCE_ERROR_OK;
}

/**
 * Set the debug log handler.
 *
 * @param handler The handler (NULL to disable logging).
 * @param threadman If not NULL, receives the address of the global state of the thread manager.
 *
 * @return 0.
 */
// Subroutine ThreadManForKernel_B50F4E46 - Address 0x00016BD8
s32 sceKernelSetThreadmanLogHandler(SceThreadmanLogHandler handler, SceThreadman **threadman)
{
    g_threadman.logHandler = handler;
    if (threadman != NULL)
        *threadman = &g_threadman;
    return SCE_ERROR_OK;
}

/**
 * Remove a thread from the ready queue of a priority, updating the ready bitmap.
 *
 * @param thread The thread.
 * @param priority The priority of the ready queue the thread is in.
 */
// Subroutine sub_00016BF4 - Address 0x00016BF4
void sub_00016BF4(SceThreadmanThread *thread, s32 priority)
{
    threadmanReadyQueueRemove(thread, priority);
}

/**
 * Make a thread ready, switching to it if it has a higher priority than the current thread.
 *
 * @param thread The thread.
 * @param intr The interrupt state (passed to sub_00000FE4(); the current thread is put at the head of
 * its ready queue when non-zero).
 */
// Subroutine sub_00016C48 - Address 0x00016C48
void sub_00016C48(SceThreadmanThread *thread, s32 intr)
{
    threadmanReadyThread(thread, intr);
}

/**
 * Exit the current thread with the result of its entry point, then delete it. This is the syscall the
 * threads return to from their entry point.
 *
 * @return The result of sceKernelExitThread().
 */
// Subroutine _sceKernelExitThread - Address 0x00016D34
s32 _sceKernelExitThread(void)
{
    s32 oldK1;
    s32 ret;

    ret = sceKernelExitThread(((SceThreadmanSyscallFrame *)g_threadman.curThread->syscallFrame)->v0);
    oldK1 = pspShiftK1();
    sub_0001752C(THREADMAN_UID_DATA_TO_CB(g_threadman.curThread, g_threadman.threadType));
    pspSetK1(oldK1);
    return ret;
}

/*
 * PARITY-Q093: pspSetK1(oldK1) in each return path instead of once before the interrupt-context check. With a single
 *     restore, the error return is a one-insn THEN block that if-conversion (IF-CASE-1) speculates into the test
 *     block, and the error constant is then scheduled after the k1 restore. The per-arm restore (a volatile asm)
 *     cannot be speculated, so the error path keeps its own block as in the original (sched -> exact). MATCHING FORM,
 *     not believed to be the original's literal source: the sibling getters (GetThreadCurrentPriority, GetUserLevel,
 *     IsUserModeThread) match with the single restore. Alternatives tried with the single restore (ret local,
 *     ternary, inverted if, goto, switch, do/while): identical output.
 */
/**
 * Get the UID of the current thread.
 *
 * @return The UID of the current thread, < 0 on error.
 */
// Subroutine sceKernelGetThreadId - Address 0x00016DAC
s32 sceKernelGetThreadId(void)
{
    s32 oldK1;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_ID, 0);

    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    }
    pspSetK1(oldK1);
    return thread->uid;
}

/**
 * Get the current priority of the current thread.
 *
 * @return The current priority, < 0 on error.
 */
// Subroutine sceKernelGetThreadCurrentPriority - Address 0x00016E24
s32 sceKernelGetThreadCurrentPriority(void)
{
    s32 oldK1;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_THREAD_CURRENT_PRIORITY, 1, thread->currentPriority);
    pspSetK1(oldK1);

    if (THREADMAN_IS_INTR_CONTEXT())
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    return thread->currentPriority;
}

/**
 * Get the user level of the current thread.
 *
 * @return The user level (one of SCE_USER_LEVEL_*, 8 for a kernel thread), < 0 on error.
 */
// Subroutine sceKernelGetUserLevel - Address 0x00016E9C
s32 sceKernelGetUserLevel(void)
{
    s32 oldK1;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    THREADMAN_LOG(SCE_THREADMAN_LOG_GET_USER_LEVEL, 1, (thread->attr >> 28) ^ 8);
    pspSetK1(oldK1);

    if (THREADMAN_IS_INTR_CONTEXT())
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    return (thread->attr >> 28) ^ 8;
}

/**
 * Check whether the current thread is a user mode thread.
 *
 * @return 1 for a user mode thread, 0 for a kernel mode thread, < 0 on error.
 */
// Subroutine sceKernelIsUserModeThread - Address 0x00016F24
s32 sceKernelIsUserModeThread(void)
{
    s32 oldK1;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    thread = g_threadman.curThread;
    THREADMAN_LOG(SCE_THREADMAN_LOG_IS_USER_MODE_THREAD, 1, thread->attr >> 31);
    pspSetK1(oldK1);

    if (THREADMAN_IS_INTR_CONTEXT())
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    return thread->attr >> 31;
}

/*
 * PARITY-Q083: fast path as `if (freeSize >= 64) goto end;`: the C front end's goto prediction (70% not taken) cancels
 * GCC 3.3's call heuristic on the overflow block (50%), so block reordering keeps the overflow path as the fall-through
 * and the shared return after it (the original's layout and beql/delay-slot shape).
 */
/**
 * Check the free space of the stack the current thread runs on (its kernel stack for a user mode thread).
 * The thread is deleted if less than 64 bytes are left.
 *
 * @return The free stack space, 0 if the thread has no stack.
 */
// Subroutine sub_00016FA4 - Address 0x00016FA4
s32 sub_00016FA4(void)
{
    s32 freeSize;
    SceThreadmanThread *thread;

    thread = g_threadman.curThread;
    if (THREADMAN_IS_USER_THREAD(thread))
        freeSize = (u32)&freeSize - (u32)thread->kStackLow;
    else {
        if (thread->stackLow == NULL)
            return 0;
        /*
         * PARITY-Q199 (trick): empty volatile asm barrier. It keeps the kernel-stack arm's
         * `freeSize = &freeSize - stackLow` from being merged with the user arm at the join
         * (cross-jumping) and changes the register choice for freeSize, which gives the
         * original's separate subu/sw per arm and its $v0/$a2/$a3/$t0 registers (barrier/pin search,
         * /workspace/scratch-alloc/ab; -fno-crossjumping alone: equiv 0.725).
         */
        __asm__ __volatile__("");
        freeSize = (u32)&freeSize - (u32)thread->stackLow;
    }

    if (freeSize >= SCE_THREADMAN_MIN_FREE_STACK)
        goto end;
    sceKernelCpuSuspendIntr();
    SaveThreadContext(thread->context);
    sub_000177C4(thread, thread->attr >> 31, thread->context->gpr[29] /* $sp */);
    sub_00000FE4(1, 1);

end:
    return freeSize;
}

/*
 * PARITY-Q083: fast path as `if (freeSize >= 64) goto end;`: the C front end's goto prediction (70% not taken) cancels
 * GCC 3.3's call heuristic on the overflow block (50%), so block reordering keeps the overflow path as the fall-through
 * and the shared return after it (the original's layout and beql/delay-slot shape).
 */
/**
 * Check the free space of the stack the current thread runs on (its kernel stack for a user mode thread).
 * The thread is deleted if less than 64 bytes are left.
 *
 * @return The free stack space, 0 if the thread has no stack or when called from an interrupt handler.
 */
// Subroutine sceKernelCheckThreadKernelStack - Address 0x00017044
s32 sceKernelCheckThreadKernelStack(void)
{
    s32 oldK1;
    s32 freeSize;
    SceThreadmanThread *thread;

    oldK1 = pspShiftK1();
    if (THREADMAN_IS_INTR_CONTEXT()) {
        pspSetK1(oldK1);
        return 0;
    }

    THREADMAN_LOG(SCE_THREADMAN_LOG_CHECK_THREAD_KERNEL_STACK, 0);
    thread = g_threadman.curThread;
    pspSetK1(oldK1);

    if (THREADMAN_IS_USER_THREAD(thread))
        freeSize = (u32)&freeSize - (u32)thread->kStackLow;
    else {
        if (thread->stackLow == NULL)
            return 0;
        freeSize = (u32)&freeSize - (u32)thread->stackLow;
    }

    if (freeSize >= SCE_THREADMAN_MIN_FREE_STACK)
        goto end;
    sceKernelCpuSuspendIntr();
    SaveThreadContext(thread->context);
    sub_000177C4(thread, thread->attr >> 31, thread->context->gpr[29] /* $sp */);
    sub_00000FE4(1, 1);

end:
    return freeSize;
}

/**
 * Get the system status event flag.
 *
 * @return The UID of the system status event flag.
 */
// Subroutine sceKernelGetSystemStatusFlag - Address 0x0001712C
SceUID sceKernelGetSystemStatusFlag(void)
{
    return g_threadman.systemStatusFlag;
}

/**
 * Register the dead thread handler.
 *
 * @param handler The handler.
 *
 * @return 0.
 */
// Subroutine ThreadManForKernel_80CA77BA - Address 0x00017138
s32 sceKernelRegisterDeadThreadHandler(void *handler)
{
    g_threadman.deadThreadHandler = (SceThreadmanDeadThreadHandler)handler;
    return SCE_ERROR_OK;
}

/**
 * Get the current system status.
 *
 * @param status Receives the status. Its size field must be set.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sceKernelReferSystemStatus - Address 0x00017148
s32 sceKernelReferSystemStatus(SceKernelSystemStatus *status)
{
    s32 oldK1;
    s32 intr;
    SceKernelSystemStatus *info;

    oldK1 = pspShiftK1();
    if (!pspK1StaBufOk(status, sizeof(*status))) {
        pspSetK1(oldK1);
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;
    }

    intr = sceKernelCpuSuspendIntr();

    THREADMAN_LOG(SCE_THREADMAN_LOG_REFER_SYSTEM_STATUS, 1, status);

    info = &g_threadman.statusBuf.systemStatus;
    sub_000183EC(info, intr);
    memcpy(status, info, (info->size < status->size) ? info->size : status->size);

    sceKernelCpuResumeIntr(intr);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

/*
 * PARITY-Q006 (a257619): Uses SceThreadmanSegAddr union for the profiler register address.
 * PARITY-Q095: Result preset to NULL and set inside the mode test, with the segment field written before the
 *     profiler test and the offset only when the thread has a profiler: the original's preset `move $a0, $zero`, entry
 *     block and in-block `lui 0x4000` (wsim +0.054, still equiv: the original keeps the thread-mode path as the
 *     fall-through, ours moves it out of line on the opcode-nonequal prediction). Forms with the same output: goto end,
 *     nested ifs, switch. Folded/other forms tried (segment+offset together -> constant folded to 0x5C40, early
 *     `return NULL`, union NULL preset): differs or down.
 * PARITY-Q170: the mode test is a switch whose THREAD case returns ret and whose default returns NULL. The
 *     default's NULL return note (90%) outweighs the opcode-nonequal prediction (71%) on the mode test (21.4% skip in
 *     the .bp dump), so the thread-mode path stays the fall-through and the other modes branch to the end, as in the
 *     original. wsim +0.194, csim +0.155, 19 = orig size, 1 up / 0 down; still equiv (ours copies ret through a
 *     second register; the original's NULL reset is a beql). Same layout with `break` in the case (+0.123/+0.097,
 *     20 instructions); plain-if forms with a separate `return NULL` for the mode test: differs.
 */
/**
 * Get the profiler registers of the current thread (in profiler thread mode).
 *
 * @return The profiler registers, NULL on error.
 */
// Subroutine sceKernelReferThreadProfiler - Address 0x00017230
PspDebugProfilerRegs *sceKernelReferThreadProfiler(void)
{
    SceThreadmanSegAddr regs;
    PspDebugProfilerRegs *ret = NULL;

    if (!THREADMAN_IS_INTR_CONTEXT()) {
        switch (g_threadman.profilerMode) {
        case SCE_THREADMAN_PROFILER_MODE_THREAD:
            regs.s.segment = THREADMAN_PROFILER_REGS_SEGMENT;
            if (g_threadman.curThread->profiler != NULL) {
                regs.s.offset = (u32)THREADMAN_HW_PROFILER_REGS;
                ret = (PspDebugProfilerRegs *)regs.addr;
            }
            return ret;
        default:
            return NULL;
        }
    }
    return ret;
}

/*
 * PARITY-Q006 (a257619): Uses SceThreadmanSegAddr union for the profiler register address.
 * PARITY-Q084: NULL case stored in the union (one return path): the original's NULL block sets the register
 *     holding the address, not a separate return value.
 * PARITY-Q083: The NULL case is reached with a goto: the front end's goto prediction moves it after the return
 *     (the original's bne to an out-of-line NULL block that jumps back).
 * PARITY-Q189 (trick): two local register variables anchored with empty asm statements: the profiler register
 *     offset (0xBC400000) is built first, in $a1, and the profiler mode is compared from $a2, as in the original
 *     (GCC 3.3 schedules the mode constant before the offset and register-renames all three temporaries).
 *     They force the registers and the order only; no code is added.
 */
/**
 * Get the global profiler registers (in profiler global mode).
 *
 * @return The profiler registers, NULL on error.
 */
// Subroutine sceKernelReferGlobalProfiler - Address 0x0001727C
PspDebugProfilerRegs *sceKernelReferGlobalProfiler(void)
{
    SceThreadmanSegAddr regs;
    register u32 offset asm("$5") = (u32)THREADMAN_HW_PROFILER_REGS; /* PARITY-Q189: $a1, built first */

    asm("" : "+r"(offset));
    regs.s.segment = THREADMAN_PROFILER_REGS_SEGMENT;
    {
        register u32 mode asm("$6") = g_threadman.profilerMode; /* PARITY-Q189: compared from $a2 */

        asm("" : : "r"(mode));
        if (mode != SCE_THREADMAN_PROFILER_MODE_GLOBAL)
            goto fail;
    }
    regs.s.offset = offset;
end:
    return (PspDebugProfilerRegs *)regs.addr;
fail:
    regs.addr = (u32)NULL;
    goto end;
}

/**
 * The boot callback registered by ThreadManInit(), called once the system is booted: exit and delete the
 * initial thread ("SceThreadmanInit").
 *
 * @return 0.
 */
// Subroutine sub_000172A8 - Address 0x000172A8
s32 sub_000172A8(void *data, s32 arg, void *opt)
{
    (void)data;
    (void)arg;
    (void)opt;

    sceKernelExitDeleteThread(0);
    return 0;
}

/**
 * The entry of the idle thread when the CPU profiler is enabled: put the CPU to sleep forever, writing back
 * the data cache line of the current thread context (COP0 state register 25) before each sleep.
 */
// Subroutine sub_000172C8 - Address 0x000172C8
void sub_000172C8(void)
{
    void *context = (void *)pspCop0StateGet(25);

    for (;;) {
        pspCache(0x0A, context);
        pspHalt();
    }
}

/*
 * The following function is hand-written assembly placed between the C functions of this file (at 0x172DC in
 * the original module, between sub_000172C8() and sub_000172F8()).
 */

/**
 * The return address of the entry of the kernel mode threads (see sub_00013E80()): exit the thread with the
 * result of its entry, in $v0 on entry. This cannot be written in C as $v0 is read on entry.
 */
/* PARITY-Q186: in its own .text.sub_000172DC section so that -ffunction-sections links it between sub_000172C8() and
 * sub_000172F8() (see THREADMAN_WITH_CALLBACKS_THUNK) */
// Subroutine sub_000172DC - Address 0x000172DC
asm(".pushsection .text.sub_000172DC, \"ax\", @progbits\n"
    ".align 2\n"
    ".globl sub_000172DC\n"
    ".type sub_000172DC, @function\n"
    ".ent sub_000172DC\n"
    "sub_000172DC:\n"
    ".set push\n"
    ".set noreorder\n"
    "addiu $sp, $sp, -16\n"
    "sw $ra, 0($sp)\n"
    "jal sceKernelExitThread\n"
    "addu $a0, $v0, $zero\n"
    "lw $ra, 0($sp)\n"
    "jr $ra\n"
    "addiu $sp, $sp, 16\n"
    ".set pop\n"
    ".end sub_000172DC\n"
    ".size sub_000172DC, . - sub_000172DC\n"
    ".popsection\n");

/*
 * PARITY-Q042 (7a75223): Statement/store order taken from the original's store order.
 * PARITY-Q135: The cleanup path reads g_threadman.cleanerThread once into the `thread` local (before clearing
 * cleanupRequested) and uses it for both the status store and nextThread; this gives the original's $v0/$a0
 * assignment in that path.
 */
/**
 * Select the next thread to run (g_threadman.nextThread): the cleaner thread if dead threads must be deleted,
 * otherwise the first thread of the highest priority ready queue if it has a higher priority than the
 * current thread (or if the current thread does not run anymore).
 */
// Subroutine sub_000172F8 - Address 0x000172F8
void sub_000172F8(void)
{
    s32 prio;
    SceThreadmanThread *curThread;
    SceThreadmanThread *thread;

    curThread = g_threadman.curThread;
    g_threadman.nextThread = curThread;
    if (g_threadman.cleanupRequested != 0) {
        thread = g_threadman.cleanerThread;
        g_threadman.cleanupRequested = 0;
        thread->status = SCE_THREADMAN_STATUS_RUNNING;
        g_threadman.nextThread = thread;
        if (curThread->status == SCE_THREADMAN_STATUS_RUNNING) {
            threadmanReadyQueueAddHead(curThread);
            curThread->status = SCE_THREADMAN_STATUS_READY;
        }
        return;
    }

    prio = threadmanReadyQueueGetHighestPriority();

    if (prio >= SCE_THREADMAN_NUM_PRIORITIES) {
        Kprintf("Panic: not found executable Thread\n");
        return;
    }

    if (curThread->status != SCE_THREADMAN_STATUS_RUNNING) {
        thread = (SceThreadmanThread *)g_threadman.readyQueue[prio].next;
        threadmanReadyQueueRemove(thread, prio);
        thread->status = SCE_THREADMAN_STATUS_RUNNING;
        g_threadman.nextThread = thread;
    } else if (prio < curThread->currentPriority) {
        thread = (SceThreadmanThread *)g_threadman.readyQueue[prio].next;
        threadmanReadyQueueRemove(thread, prio);
        threadmanReadyQueueAddHead(curThread);
        thread->status = SCE_THREADMAN_STATUS_RUNNING;
        g_threadman.nextThread = thread;
        curThread->status = SCE_THREADMAN_STATUS_READY;
    }
}

/**
 * Kill a thread which crashed (stack overflow, corrupted $k0, return from its entry point...): it
 * releases its resources, dumps its saved registers, then becomes DEAD and the dead thread handler
 * (or a break) is called.
 *
 * @param uid The control block of the thread.
 */
// Subroutine sub_0001752C - Address 0x0001752C
void sub_0001752C(SceSysmemUidCB *uid)
{
    SceThreadmanThread *thread;
    SceThreadmanContext *context;

    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (g_threadman.dispatchSuspended != 0) {
        g_threadman.dispatchSuspended = 0;
        Kprintf("thread_is_dead: Thread dispatch is disabled!\n");
    }

    threadmanReadyQueueRemove(thread, thread->currentPriority);
    sub_00000B48(uid, SCE_ERROR_KERNEL_THREAD_IS_TERMINATED);
    sub_00002570(thread);
    sub_000027B4(thread);
    _sceKernelFreeThreadKTLS(thread);
    _sceKernelEventFlagReleaseOwned(thread);
    _sceKernelMutexReleaseOwned(thread);
    sub_000026CC(thread);
    _sceKernelReleaseThreadTls(thread);

    Kprintf("Thread \"%s\" (thid=0x%08x) is DEAD\n", (uid->name != NULL) ? uid->name : "", thread->uid);
    context = thread->context;
    if (context->type == SCE_THREADMAN_CONTEXT_TYPE_FULL) {
        Kprintf(" Saved registers:\n");
        Kprintf(" at=%08x  v0-1=%08x,%08x  a0-3=%08x,%08x,%08x,%08x\n",
            context->gpr[1], context->gpr[2], context->gpr[3], context->gpr[4], context->gpr[5], context->gpr[6],
            context->gpr[7]);
        Kprintf(" t0-7=%08x,%08x,%08x,%08x, %08x,%08x,%08x,%08x\n",
            context->gpr[8], context->gpr[9], context->gpr[10], context->gpr[11], context->gpr[12], context->gpr[13],
            context->gpr[14], context->gpr[15]);
        Kprintf(" s0-7=%08x,%08x,%08x,%08x, %08x,%08x,%08x,%08x\n",
            context->gpr[16], context->gpr[17], context->gpr[18], context->gpr[19], context->gpr[20], context->gpr[21],
            context->gpr[22], context->gpr[23]);
        Kprintf(" t8=%08x t9=%08x\tk0=%08x k1=%08x\t  gp=%08x sp=%08x\n",
            context->gpr[24], context->gpr[25], context->gpr[26], context->gpr[27], context->gpr[28], context->gpr[29]);
        Kprintf(" fp=%08x ra=%08x\tlo=%08x hi=%08x\t  PC=%08x\n",
            context->gpr[30], context->gpr[31], context->lo, context->hi, context->epc);
    }
    if (thread->syscallFrame != NULL)
        Kprintf(" Syscall return address = 0x%08x, sp = 0x%08x\n", ((SceThreadmanSyscallFrame *)thread->syscallFrame)->ra, ((SceThreadmanSyscallFrame *)thread->syscallFrame)->sp);
    Kprintf("\n");

    threadmanListAddTail(&g_threadman.deadThreadList, &thread->link);
    thread->exitStatus = SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
    thread->status = SCE_THREADMAN_STATUS_DEAD;
    g_threadman.nextThread = NULL;
    if (g_threadman.deadThreadHandler == NULL) {
        pspBreak(0);
        return;
    }
    g_threadman.deadThreadHandler(thread->uid, thread->context);
}

/*
 * PARITY-Q068 (3a8ed2d): The stack overflow reported with a single Kprintf (the original's one call).
 */
/**
 * Report a stack overflow of a thread and delete it.
 *
 * @param thread The thread.
 * @param kernelStack Non-zero if the overflow is on the kernel stack of a user mode thread.
 * @param sp The stack pointer of the thread.
 */
// Subroutine sub_000177C4 - Address 0x000177C4
void sub_000177C4(SceThreadmanThread *thread, s32 kernelStack, u32 sp)
{
    const char *stackType;
    void *stackLow;
    s32 stackSize;

    if (thread->status == SCE_THREADMAN_STATUS_DEAD)
        return;

    if (kernelStack) {
        stackType = "kernel ";
        stackLow = thread->kStackLow;
        stackSize = thread->kStackSize;
    } else {
        stackType = "";
        stackLow = thread->stackLow;
        stackSize = thread->stackSize;
    }
    Kprintf("\nThread (thid=0x%08x) %sstack overflow\n Stack = 0x%08x, Stack size = 0x%08x, SP = 0x%08x\n",
        thread->uid, stackType, stackLow, stackSize, sp);
    sub_0001752C(THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType));
}

/**
 * Report a corruption of the $k0 register of a thread and delete it. For modules compiled with an SDK
 * older than 6.00, the check is disabled instead (see ThreadManForKernel_D366D35A()).
 *
 * @param thread The thread.
 */
// Subroutine sub_00017844 - Address 0x00017844
void sub_00017844(SceThreadmanThread *thread)
{
    if (!threadmanSdkVersionAbove(0x05FFFFFF)) {
        ThreadManForKernel_D366D35A(1);
        return;
    }
    if (thread->status == SCE_THREADMAN_STATUS_DEAD)
        return;

    Kprintf("\nThread (thid=0x%08x) k0 reg corrupted\n", thread->uid);
    sub_0001752C(THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType));
}

/*
 * PARITY-Q081: Final reschedule/panic loop written with a label and goto (not for (;;)): the original does not
 *     hoist the &g_threadman address out of it (no loop-invariant motion).
 */
/**
 * Exit the current thread (the end of sceKernelExitThread() and sceKernelExitDeleteThread(), also
 * run by a thread terminated by sub_00017A14()): run its exit event handlers, release its
 * resources and make it dormant, or delete it.
 *
 * @param deleteThread Set to delete the thread.
 * @param intr The interrupt state.
 * @param exitStatus The exit status of the thread, returned to the threads waiting for its end.
 */
// Subroutine sub_000178C0 - Address 0x000178C0
void sub_000178C0(s32 deleteThread, s32 intr, s32 exitStatus)
{
    SceThreadmanThread *thread;
    SceSysmemUidCB *uid;

    sceKernelCpuSuspendIntr();
    thread = g_threadman.curThread;
    uid = THREADMAN_UID_DATA_TO_CB(thread, g_threadman.threadType);
    if (g_threadman.dispatchSuspended != 0) {
        g_threadman.dispatchSuspended = 0;
        Kprintf("doExitDeleteThread: Thread dispatch is disabled!\n");
    }
    if (deleteThread)
        thread->threadEventMask |= SCE_KERNEL_THREAD_EVENT_DELETE;

    sub_00001580(SCE_KERNEL_THREAD_EVENT_EXIT, thread, intr);
    _sceKernelFreeThreadKTLS(thread);
    sub_00000B48(uid, exitStatus);
    _sceKernelEventFlagReleaseOwned(thread);
    _sceKernelMutexReleaseOwned(thread);
    sub_000026CC(thread);
    _sceKernelReleaseThreadTls(thread);

    if (thread->threadEventMask & SCE_KERNEL_THREAD_EVENT_DELETE) {
        deleteThread = 1;
    }
    if (!deleteThread) {
        threadmanListAddTail(&g_threadman.dormantThreads, &thread->link);
        thread->status = SCE_THREADMAN_STATUS_DORMANT;
    } else {
        sub_00001580(SCE_KERNEL_THREAD_EVENT_DELETE, thread, intr);
        thread->status = 0;
        sub_000126AC(thread->uid);
    }

    /* Switch to another thread; the current one must never be scheduled again. */
loop:
    g_threadman.nextThread = NULL;
    sub_00000FE4(1, 1);
    Kprintf("panic ! Thread DORMANT !\n");
    pspBreak(1);
    goto loop;
}

/*
 * PARITY-Q047 (d9d18b9): Interrupt state read raw / at the original's position.
 * PARITY-Q048 (a670bf7): Value read once into a local (the original keeps it in one register).
 * PARITY-Q055 (7197c1b): Gpr[29] set in each branch / gpr[30] = gpr[29] chain (the original reloads it).
 * PARITY-Q042: curThread read right after the interrupt state (the original loads it before the UID data pointer).
 * PARITY-Q111: `ret` declared volatile. MATCHING FORM, not believed to be the original's literal source. The original
 *     keeps ret in its stack slot all through (`sw zero,0(sp)` in the prologue, `sw v0` / `lw` around the
 *     wait) and does not if-convert `if (ret == THREAD_IS_TERMINATED) ret = 0` (beql + out-of-line store),
 *     whereas a plain local is converted to xor/movz. A volatile local is the only form found that keeps it in
 *     memory. equiv -> sched, wsim +0.008, 1 up / 0 down. Tried with no gain: goto/else-goto skip, ternary,
 *     separate status local; per-arm early return (+0.002, duplicated tail); nested `ret != 0` test (down).
 * PARITY-Q152: The thread context's gpr[28] (gp) is set before its epc (store order).
 * PARITY-Q153: The thread context's gpr[30] copy is set before its gpr[31] return address (store order).
 */
/**
 * Terminate a thread (sceKernelTerminateThread() and sceKernelTerminateDeleteThread()). If the
 * thread has exit event handlers or KTLS destructors to run, it is made to run sub_000178C0() itself
 * (and the caller waits for its end); otherwise its resources are released immediately.
 *
 * @param uid The control block of the thread.
 * @param deleteThread Set to delete the thread too.
 * @param intr The interrupt state.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sub_00017A14 - Address 0x00017A14
s32 sub_00017A14(SceSysmemUidCB *uid, s32 deleteThread, s32 intr)
{
    s32 isIntr;
    s32 status;
    volatile s32 ret;
    SceThreadmanThread *thread;
    SceThreadmanThread *curThread;
    SceThreadmanContext *context;

    isIntr = pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT);
    curThread = g_threadman.curThread;
    ret = SCE_ERROR_OK;
    thread = UID_CB_TO_DATA(uid, g_threadman.threadType, SceThreadmanThread);
    if (deleteThread && isIntr)
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    if (!isIntr && curThread->threadEventMask != 0)
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    if (!isIntr && thread == curThread)
        return SCE_ERROR_KERNEL_ILLEGAL_THREAD;
    if (thread->status == 0)
        return SCE_ERROR_KERNEL_NOT_FOUND_THREAD;

    if (thread->status == SCE_THREADMAN_STATUS_DORMANT || thread->status == SCE_THREADMAN_STATUS_DEAD) {
        if (!deleteThread)
            return SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
        sub_00001580(SCE_KERNEL_THREAD_EVENT_DELETE, thread, intr);
        threadmanListRemove(&thread->link);
        thread->status = 0;
        sub_000155B8(thread, intr);
        sceKernelDeleteUID(thread->uid);
        return SCE_ERROR_OK;
    }
    if (isIntr && thread == curThread && g_threadman.dispatchSuspended != 0) {
        /* The thread will be terminated when the dispatch is resumed. */
        g_threadman.dispatchSuspended = 3;
        return SCE_ERROR_OK;
    }
    if (thread->threadEventMask & (SCE_KERNEL_THREAD_EVENT_EXIT | SCE_KERNEL_THREAD_EVENT_DELETE)) {
        /* The thread is already exiting. */
        if (!deleteThread)
            return SCE_ERROR_KERNEL_THREAD_ALREADY_DORMANT;
        thread->threadEventMask |= SCE_KERNEL_THREAD_EVENT_DELETE;
        return SCE_ERROR_OK;
    }

    thread->exitStatus = SCE_ERROR_KERNEL_THREAD_IS_TERMINATED;
    if (g_threadman.dispatchSuspended != 0) {
        g_threadman.dispatchSuspended = 0;
        Kprintf("doTerminateDeleteThread: Thread dispatch is disabled!\n");
    }
    sub_00002570(thread);
    sub_000027B4(thread);
    if (_sceKernelFreeThreadKTLS(thread) == 0 && sub_000025FC(SCE_KERNEL_THREAD_EVENT_EXIT, thread) == 0) {
        /* Nothing to run in the thread: release its resources now. */
        sub_00000B48(uid, SCE_ERROR_KERNEL_THREAD_IS_TERMINATED);
        _sceKernelEventFlagReleaseOwned(thread);
        _sceKernelMutexReleaseOwned(thread);
        sub_000026CC(thread);
        _sceKernelReleaseThreadTls(thread);
        if (isIntr && thread == curThread)
            g_threadman.nextThread = NULL;
        else {
            status = thread->status;
            if (status == SCE_THREADMAN_STATUS_READY)
                threadmanReadyQueueRemove(thread, thread->currentPriority);
            else {
                threadmanListRemove(&thread->link);
                if (status == SCE_THREADMAN_STATUS_WAIT || status == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
                    sub_00000BC4(thread);
                    if (thread->waitObj != NULL)
                        sub_00000C30(thread->waitObj);
                }
            }
        }
        threadmanListAddTail(&g_threadman.dormantThreads, &thread->link);
        thread->status = SCE_THREADMAN_STATUS_DORMANT;
    } else {
        /* Make the thread run sub_000178C0() to exit by itself. */
        status = thread->status;
        if (status != SCE_THREADMAN_STATUS_RUNNING && status != SCE_THREADMAN_STATUS_READY) {
            threadmanListRemove(&thread->link);
            if (status == SCE_THREADMAN_STATUS_WAIT || status == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
                sub_00000BC4(thread);
                if (thread->waitObj != NULL)
                    sub_00000C30(thread->waitObj);
            }
        }
        sub_000134A8(thread->context);
        context = thread->context;
        context->type = SCE_THREADMAN_CONTEXT_TYPE_FULL;
        context->gpr[5] = 1; // $a1: intr
        context->gpr[6] = SCE_ERROR_KERNEL_THREAD_IS_TERMINATED; // $a2: exitStatus
        context->gpr[26] = (u32)thread->k0;
        context->gpr[4] = 0; // $a0: deleteThread
        if (THREADMAN_IS_USER_THREAD(thread))
            context->gpr[29] = thread->userSp;
        else
            context->gpr[29] = thread->initSp;
        context->gpr[30] = context->gpr[29];
        context->gpr[31] = (u32)sub_000172DC;
        context->gpr[28] = thread->gp;
        context->epc = (u32)sub_000178C0;
        context->status = SCE_THREADMAN_CONTEXT_STATUS_KERNEL;
        if (thread->status != SCE_THREADMAN_STATUS_READY) {
            thread->status = SCE_THREADMAN_STATUS_READY;
            sub_000127D0(thread);
            g_threadman.nextThread = NULL;
        }
        if (!isIntr) {
            /* Wait for the end of the thread. */
            ret = sub_00000908(uid, SCE_THREADMAN_WAIT_THREAD_END, 0, NULL, NULL);
            if (ret == (s32)SCE_ERROR_KERNEL_THREAD_IS_TERMINATED)
                ret = SCE_ERROR_OK;
        }
    }

    if (!deleteThread)
        return ret;
    sub_00001580(SCE_KERNEL_THREAD_EVENT_DELETE, thread, intr);
    threadmanListRemove(&thread->link);
    thread->status = 0;
    sub_000155B8(thread, intr);
    sceKernelDeleteUID(thread->uid);
    return ret;
}

/*
 * PARITY-Q091: The stub address is written stackSize + stackLow (the original's addu operand order).
 * PARITY-Q134: The syscall frame is set ra, epc, then sp (sp, ra, epc before).
 */
/**
 * Make the current (user mode) thread run a function on a new user stack on return of the syscall.
 * A stub at the top of the new stack calls the function, then returns to the caller of the syscall
 * through _sceKernelReturnFromChangeStack().
 *
 * @param blockId The memory block of the new stack, freed with the stack (0 if it must not be freed).
 * @param stackLow The lowest address of the new stack.
 * @param stackSize The size of the new stack.
 * @param func The user function to run.
 * @param argp The argument of the function.
 *
 * @return 0 on success, < 0 on error.
 */
// Subroutine sub_00017EB4 - Address 0x00017EB4
s32 sub_00017EB4(SceUID blockId, void *stackLow, SceSize stackSize, s32 (*func)(void *), void *argp)
{
    s32 intr;
    u32 *stub;
    SceThreadmanThread *thread;
    SceThreadmanSyscallFrame *frame;
    SceThreadmanUserStackExt *ext;

    thread = g_threadman.curThread;
    if (THREADMAN_IS_INTR_CONTEXT())
        return SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT;
    if ((s32)func < 0)
        return SCE_ERROR_KERNEL_ILLEGAL_ADDR;

    ext = sceKernelAllocHeapMemory(g_threadman.heapId, sizeof *ext);
    if (ext == NULL)
        return SCE_ERROR_KERNEL_NO_MEMORY;

    if ((thread->attr & SCE_KERNEL_TH_NO_FILLSTACK) == 0)
        sceKernelMemset32(stackLow, -1, stackSize);
    *(SceUID *)stackLow = thread->uid;

    intr = sceKernelCpuSuspendIntr();
    frame = thread->syscallFrame;
    ext->ext.blockId = thread->stackBlockId;
    ext->ext.size = thread->stackSize;
    ext->epc = frame->epc;
    ext->ra = frame->ra;
    ext->sp = frame->sp;
    thread->stackBlockId = blockId;
    thread->stackSize = stackSize;

    stub = (u32 *)(stackSize + (u32)stackLow - 64);
    stub[0] = 0x3C040000 | ((u32)argp >> 16); // lui $a0, %hi(argp)
    stub[2] = 0x34840000 | ((u32)argp & 0xFFFF); // ori $a0, $a0, %lo(argp)
    stub[3] = 0x8FBF0038; // lw $ra, 56($sp)
    stub[1] = 0x0C000000 | (((u32)func >> 2) & 0x03FFFFFF); // jal func
    stub[4] = 0x8FBD003C; // lw $sp, 60($sp)
    stub[5] = ((g_threadman.returnFromChangeStackSyscall << 6) & 0x03FFFFC0) | 0xC; // syscall _sceKernelReturnFromChangeStack
    stub[6] = 0x03E00008; // jr $ra
    stub[7] = 0;
    stub[8] = 0;
    stub[9] = 0;
    stub[10] = 0;
    stub[11] = 0;
    stub[12] = 0;
    ext->ext.next = thread->stackExts;
    ext->ext.low = thread->stackLow;
    thread->stackExts = &ext->ext;
    thread->stackLow = stackLow;
    stub[13] = 0;
    stub[14] = ext->ra; // the saved $ra
    stub[15] = ext->sp; // the saved $sp

    /* Return from the syscall to the stub, running on the new stack. */
    frame->ra = 0;
    frame->epc = (u32)stub;
    frame->sp = (u32)stub;
    pspCache(0x08, stub);
    pspCache(0x1B, stub);
    sceKernelCpuResumeIntrWithSync(intr);
    return 0;
}

/*
 * PARITY-Q048 (6ff02fa, b89af15): Value read once into a local (the original keeps it in one register).
 * PARITY-Q049 (b89af15, 0a815ff): If/else statements instead of ternaries (the original branches, no movz/movn).
 * PARITY-Q054 (b0f43f4, 52d2bdc): Comparison operand order as the original's branch operands.
 * PARITY-Q062 (52d2bdc, 0a815ff): FPU save skip via goto / syscallRa NULL-first if/else (the original's layout).
 * PARITY-Q136: &info->runClocks taken into a local before the run time is computed: the original computes the
 *     address before the current-thread test and keeps it in a callee-saved register across the
 *     _sceKernelGetSystemTime64() call.
 * PARITY-Q140: SceKernelThreadKInfo.entry is a `void *` like the other address fields: the store then shares their
 *     alias set, which gives the original's load order in the field-copy run (entry read right after attr).
 */
/**
 * Build the debug status of a thread (see sceKernelReferThreadDebugStatus()).
 *
 * @param cb The UID control block of the thread.
 * @param info Receives the status.
 * @param isIntr The interrupt context flag of the caller (unused, but passed by the original caller).
 */
// Subroutine sub_000180AC - Address 0x000180AC
void sub_000180AC(SceSysmemUidCB *cb, SceKernelThreadKInfo *info, s32 isIntr __attribute__((unused)))
{
    u64 runClocks;
    SceThreadmanContext *context;
    SceKernelSysClock *clock;
    SceThreadmanContext *fpuOwner;
    SceThreadmanThread *thread;
    s32 status;

    thread = UID_CB_TO_DATA(cb, g_threadman.threadType, SceThreadmanThread);
    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    if (cb->name != NULL)
        strncpy(info->name, cb->name, SCE_UID_NAME_LEN);
    info->attr = thread->attr;
    status = thread->status;
    info->status = status;
    info->entry = (void *)thread->entry;
    info->stack = thread->stackLow;
    info->stackSize = thread->stackSize;
    info->kStack = thread->kStackLow;
    info->kStackSize = thread->kStackSize;
    info->gpReg = (void *)thread->gp;
    info->args = thread->argSize;
    info->argp = thread->argp;
    info->initPriority = thread->initPriority;
    info->currentPriority = thread->currentPriority;
    if (status == SCE_THREADMAN_STATUS_WAIT || status == SCE_THREADMAN_STATUS_WAIT_SUSPEND) {
        if (thread->waitWithCallbacks != 0)
            info->waitType = thread->waitType + 0x100;
        else
            info->waitType = thread->waitType;
        if (thread->waitObj != NULL)
            info->waitId = thread->waitObj->uid;
        else
            info->waitId = 0;
    }
    info->wakeupCount = thread->wakeupCount;
    clock = &info->runClocks;
    runClocks = threadmanSysClockGet(&thread->runClocks);
    if (g_threadman.curThread == thread)
        runClocks += _sceKernelGetSystemTime64() - threadmanSysClockGet(&g_threadman.lastSwitchClock);
    threadmanSysClockSet(clock, runClocks);
    info->intrPreemptCount = thread->intrPreemptCount;
    info->threadPreemptCount = thread->threadPreemptCount;
    info->releaseCount = thread->releaseCount;
    info->notifyCallback = thread->callbackNotified;
    info->context = thread->context;

    /* Save the FPU registers still in the FPU, keeping their owner. */
    fpuOwner = (SceThreadmanContext *)threadmanCop0CtrlGet(THREADMAN_COP0_CTRL_FPU_OWNER);
    if (fpuOwner == NULL)
        goto fpu_saved;
    sub_000005A4(fpuOwner);
    threadmanCop0CtrlSet(THREADMAN_COP0_CTRL_FPU_OWNER, (s32)fpuOwner);
fpu_saved:
    context = info->context;
    context->type = (u16)context->type;
    if (thread->fpuUsed != 0 || (context->status & 0x20000000)) /* CU1: the FPU is enabled */
        context->type |= 0x1000000;

    info->vfpuContext = thread->vfpuContext;
    if (thread->vfpuContext != NULL && g_threadman.vfpuOwner == thread->vfpuContext)
        info->vfpuContext = NULL;
    if (thread->syscallFrame == NULL)
        info->syscallRa = 0;
    else
        info->syscallRa = ((SceThreadmanSyscallFrame *)thread->syscallFrame)->ra;
    info->syscallFrame = thread->syscallFrame;
    if (thread->timeoutAlarmId > 0 && thread->timeout != NULL)
        info->timeout = *thread->timeout;

    switch ((u8)thread->waitType) {
    case SCE_THREADMAN_WAIT_DELAY:
    case SCE_THREADMAN_WAIT_EVENT_FLAG:
        info->waitData[0] = thread->waitData[0];
        info->waitData[1] = thread->waitData[1];
        break;
    case SCE_THREADMAN_WAIT_SEMA:
    case SCE_THREADMAN_WAIT_MUTEX:
    case SCE_THREADMAN_WAIT_LW_MUTEX:
        info->waitData[0] = thread->waitData[0];
        break;
    case SCE_THREADMAN_WAIT_VPL:
        info->waitData[0] = thread->waitData[1];
        break;
    case SCE_THREADMAN_WAIT_MSG_PIPE:
        info->waitData[1] = thread->waitData[0];
        info->waitData[2] = thread->waitData[2];
        info->waitData[3] = thread->waitData[4];
        info->waitData[4] = thread->waitData[1];
        info->waitData[0] = thread->waitData[5];
        break;
    case SCE_THREADMAN_WAIT_TLSPL:
    default:
        break;
    }

    if (thread->profiler != NULL)
        info->profilerRegs = (u8 *)thread->profiler + 16;
    else
        info->profilerRegs = NULL;
    info->k0 = thread->k0;
    memcpy(info->ktls, thread->ktls, sizeof(info->ktls));
}

/**
 * Build the system status.
 *
 * @param status Receives the status.
 * @param intr The interrupt state of the caller.
 */
// Subroutine sub_000183EC - Address 0x000183EC
void sub_000183EC(SceKernelSystemStatus *status, s32 intr)
{
    u32 *p;
    SceThreadmanThread *idleThread;

    /* PARITY-Q019: constant-size clear as memset: 3.3's inline store loop is the original's */
    memset(status, 0, sizeof(*status));
    status->size = sizeof(*status);

    if (THREADMAN_IS_INTR_CONTEXT())
        status->status = 4;
    else if (intr == 0)
        status->status = 3;
    else if (g_threadman.dispatchSuspended != 0)
        status->status = 1;
    else
        status->status = 0;

    idleThread = g_threadman.idleThread;
    status->idleClocks = idleThread->runClocks;
    status->threadSwitchCount = g_threadman.threadSwitchCount;
    status->vfpuSwitchCount = g_threadman.vfpuSwitchCount;
    status->comesOutOfIdleCount = idleThread->intrPreemptCount;
}

/*
 * PARITY-Q039 (99969ad): List heads initialised with threadmanListInit (the original's store pattern).
 * PARITY-Q042 (99969ad): Statement/store order taken from the original's store order.
 * PARITY-Q131: Status cleared after the list heads (the status zero store is the last use of thread, so sched1
 *   still places it first; the list-head stores then follow the original's order).
 */
/** Initialize a "Thread" UID object. */
// Subroutine sub_00018474 - Address 0x00018474
static s32 _sceKernelThreadInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    SceThreadmanThread *thread;

    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    thread = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanThread);
    sceKernelMemset32(thread, 0, sizeof(*thread));
    thread->uid = uid->uid;
    threadmanListInit(&thread->link);
    threadmanListInit(&thread->ownedMutexes);
    threadmanListInit(&thread->ownedEventFlags);
    threadmanListInit(&thread->callbacks);
    thread->status = 0;
    return uid->uid;
}
/**
 * Delete a "Thread" UID object, releasing its stacks and contexts. A thread which is not dormant is
 * terminated first.
 */
// Subroutine sub_00018504 - Address 0x00018504
static s32 _sceKernelThreadDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap)
{
    s32 intr;
    s32 ret;
    SceThreadmanThread *thread;

    intr = sceKernelCpuSuspendIntr();
    thread = UID_CB_TO_DATA(uid, uidWithFunc, SceThreadmanThread);
    if (thread->status != 0) {
        ret = sub_00017A14(uid, 1, intr);
        if (ret != SCE_ERROR_OK) {
            sceKernelCpuResumeIntr(intr);
            return ret;
        }
    }
    sub_000156BC(thread);

    if (thread->stackLow != NULL)
        sceKernelFreePartitionMemory(thread->stackBlockId);
    if (THREADMAN_IS_USER_THREAD(thread)) {
        if (thread->kStackLow != NULL)
            sceKernelFreePartitionMemory(thread->kStackBlockId);
        if (thread->profiler != NULL)
            sceKernelFreeHeapMemory(g_threadman.heapId, thread->profiler);
    }
    if ((s32)thread->context == threadmanCop0CtrlGet(THREADMAN_COP0_CTRL_FPU_OWNER))
        threadmanCop0CtrlSet(THREADMAN_COP0_CTRL_FPU_OWNER, 0);
    if (thread->context != NULL)
        sceKernelFreeHeapMemory(g_threadman.heapId, thread->context);
    if (thread->vfpuContext != NULL) {
        if (g_threadman.vfpuOwner == thread->vfpuContext)
            g_threadman.vfpuOwner = NULL;
        sub_000002C8();
        sceKernelFreeHeapMemory(g_threadman.heapId, thread->vfpuContext);
    }

    sceKernelCpuResumeIntr(intr);
    sceKernelCallUIDObjCommonFunction(uid, uidWithFunc, funcId, ap);
    return SCE_ERROR_OK;
}
