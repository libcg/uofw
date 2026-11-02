/* Copyright (C) 2016 - 2026 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/kd/threadman/threadman_int.h
 *
 * Internal data structures and helper functions of the thread manager, shared by all of its source files.
 * Layouts were reversed from the 6.60 threadman.prx; unknown fields are named after their offset.
 */

#ifndef THREADMAN_INT_H
#define THREADMAN_INT_H

#include <common_header.h>
#include <sysmem_kernel.h>
#include <threadman_kernel.h>

/*
 * The registers of the CPU profiler (at 0xBC400000). The thread manager only manipulates
 * pointers to them, so their layout is not declared here.
 */
typedef struct PspDebugProfilerRegs PspDebugProfilerRegs;

/** A node of (or the head of) a circular doubly linked list. */
typedef struct SceThreadmanListEntry {
    /** The next entry (the first entry for a list head). */
    struct SceThreadmanListEntry *next; // 0
    /** The previous entry (the last entry for a list head). */
    struct SceThreadmanListEntry *prev; // 4
} SceThreadmanListEntry; // size: 8

/** Thread attribute set for threads running in user mode. */
#define SCE_THREADMAN_THREAD_ATTR_USER          (0x80000000)
/** Thread attribute: skip this thread when waking up waiters while the "skip suspended" mode is active. */
#define SCE_THREADMAN_THREAD_ATTR_UNK_08000000  (0x08000000)

/** Check whether a thread runs in user mode (SCE_THREADMAN_THREAD_ATTR_USER is the sign bit). */
#define THREADMAN_IS_USER_THREAD(thread) ((s32)(thread)->attr < 0)

/* CPU profiler modes (g_threadman.profilerMode). */
#define SCE_THREADMAN_PROFILER_MODE_NONE    (0)
#define SCE_THREADMAN_PROFILER_MODE_THREAD  (1)
#define SCE_THREADMAN_PROFILER_MODE_GLOBAL  (2)
/** Both the per-thread and the global profiler (presumably). */
#define SCE_THREADMAN_PROFILER_MODE_THREAD_AND_GLOBAL   (3)
/** Set on development tools when no profiler is enabled through the DIP switches (meaning unknown). */
#define SCE_THREADMAN_PROFILER_MODE_4   (4)

/** The thread attributes which cannot be passed to sceKernelCreateThread(). */
#define SCE_THREADMAN_THREAD_ATTR_INVALID   (0x070F9F00)

/** The COP0 control register holding the thread context whose FPU registers are currently loaded. */
#define THREADMAN_COP0_CTRL_FPU_OWNER       (8)

/*
 * COP0 control register write that can take $zero directly: a constant 0 is written as "ctc0 $zero, $n" (operand
 * constraint "dJ" + %z), as everywhere in the original module (7 sites) and in other kernel modules (sysmem,
 * interruptman, exceptionman; survey33/ctc0zero.py). common/cop0.h's pspCop0CtrlSet() only takes a register ("r"),
 * which makes the compiler materialize 0 in a register (often a callee-saved one) first.
 */
/* PARITY-Q009: ctc0 that takes $zero directly ("dJ" + %z), as all 7 original sites; uses grep threadmanCop0CtrlSet */
static inline void threadmanCop0CtrlSet(int reg, int val)
{
    asm("ctc0 %z0, $%1" : : "dJ" (val), "ri" (reg));
}

/*
 * COP0 control register read in program order (a volatile asm). common/cop0.h's pspCop0CtrlGet() is non-volatile,
 * so GCC schedules the cfc0 above earlier stores/branches; in the original the reads stay where the source has them
 * (e.g. sub_00002944: the context stores, then `cfc0 BadVaddr`).
 */
/*
 * PARITY-Q011: volatile cfc0 for the non-interrupt COP0 reads (EXC_TABLE, FPU owner, profiler base): the original
 * keeps them in source order
 */
static inline s32 threadmanCop0CtrlGet(int reg)
{
    s32 ret;
    asm volatile ("cfc0 %0, $%1" : "=r" (ret) : "ri" (reg));
    return ret;
}

/*
 * COP0 state register read in program order (a volatile asm).
 * Reproduces (6.60 sceKernelChangeCurrentThreadAttr, 0x143C4-0x143CC):
 *     sw    $t2, 0x14($s1)        thread->attr = ...
 *     mfc0  $v1, Status           read right after the attr store,
 *     andi  $t1, $t2, 0x4000      before the SCE_KERNEL_TH_USE_VFPU test.
 * Why not C: C has no way to read a COP0 register, so an asm is unavoidable; and common/cop0.h's pspCop0StateGet()
 * is a non-volatile asm, which GCC is free to sink below the attr test to the first use of the value. Only a
 * volatile asm stays where the source puts it.
 */
/*
 * PARITY-Q185 (user-approved inline asm, 19:03 PT 2026-10-27): volatile mfc0 for the Status read in
 * sceKernelChangeCurrentThreadAttr; uses grep threadmanCop0StateGet
 */
static inline s32 threadmanCop0StateGet(int reg)
{
    s32 ret;
    asm volatile ("mfc0 %0, $%1" : "=r" (ret) : "ri" (reg));
    return ret;
}

/*
 * Set bits in a COP0 state register (mfc0/ori/mtc0 in one asm, one register for the whole read-modify-write).
 * Reproduces (6.60 sub_00000FE4, 0x010D0-0x010D8, Cause |= 0x200 before sceKernelEnableIntr):
 *     mfc0  $v0, Cause
 *     ori   $v0, $v0, 0x200
 *     mtc0  $v0, Cause
 * Why not C: C has no way to access COP0, so the mfc0 and mtc0 are asm in any case. Written as
 * pspCop0StateSet(reg, pspCop0StateGet(reg) | imm) the ori is a C operation between two asms and regrename gives
 * its result a second register ($t6 read, $t5 or-result/write); the original uses $v0 for all three instructions.
 * Keeping the three in one asm with one early-clobber operand is the only way to tie them to one register.
 */
/*
 * PARITY-Q184 (user-approved inline asm, 19:03 PT 2026-10-27): single-asm mfc0/ori/mtc0 Cause read-modify-write in
 * sub_00000FE4; uses grep threadmanCop0StateOr
 */
#define threadmanCop0StateOr(reg, imm) do { \
    s32 _v; \
    asm volatile ("mfc0 %0, $%1\n\tori %0, %0, %2\n\tmtc0 %0, $%1" : "=&r" (_v) : "i" (reg), "i" (imm)); \
} while (0)

/* Read $gp in program order (a volatile asm, like common/registers.h's pspGetSp()). */
/* PARITY-Q010: volatile $gp read: the original reads $gp in program order; uses grep threadmanGetGp */
static inline u32 threadmanGetGp(void)
{
    u32 gp;
    asm volatile ("move %0, $gp" : "=r" (gp));
    return gp;
}

/**
 * An address as a segment (top 3 bits) and an offset in it. The profiler getters build the address of the profiler
 * registers in segment 2 (0x5C400000) with it: 6.60 has `lui rX, 0x4000` before the mode check and
 * `ins rX, rY(0xBC400000), 0, 29` after it, a bit-field insertion that GCC does not fold across blocks.
 */
/*
 * PARITY-Q006: segment/offset bit-field union: reproduces the original's `lui 0x4000` + `ins ...,0,29` profiler
 * address build
 */
typedef union {
    u32 addr;
    struct {
        u32 offset : 29;
        u32 segment : 3;
    } s;
} SceThreadmanSegAddr;

/** The segment of the profiler registers address returned to the callers. */
#define THREADMAN_PROFILER_REGS_SEGMENT     (2)

/** The index of the word of g_threadman.readyBitmap holding the bit of a priority. */
#define THREADMAN_READY_INDEX(prio)         ((u32)(prio) >> 5)
/*
 * Disable the interrupts inline (mfie/mtie) and wait 8 cycles for the change to take effect; returns the
 * previous interrupt enable state. The original's sceKernelReferThreadDebugStatus has these as asm: each
 * mtie there is followed by exactly 8 nops, which the compiler never emits (sysmem's 46 compiled mtie have
 * at most 1 nop); the same mtie + nop runs are in hand-written code (interruptman, usersystemlib).
 */
/* PARITY-Q013: mfic/mtic followed by exactly 8 nops, as the original (the compiler never emits the nop runs) */
static inline s32 threadmanSuspendIntrSync(void)
{
    s32 intr;

    asm __volatile__ ("mfic %0, $0\n\tmtic $0, $0\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                      : "=r" (intr));
    return intr;
}

/* Restore the interrupt enable state intr and wait 8 cycles (see threadmanSuspendIntrSync()). */
static inline void threadmanResumeIntrSync(s32 intr)
{
    asm __volatile__ ("mtic %0, $0\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" : : "r" (intr));
}

/** The bit of a priority in g_threadman.readyBitmap[THREADMAN_READY_INDEX(priority)] (a rotation of 0x80000000). */
/* PARITY-Q034: ready bit as a rotation with % 32, the form GCC turns into the original's rotr */
#define THREADMAN_READY_BIT(prio)           ((0x80000000 >> ((prio) % 32)) | (0x80000000 << (32 - (prio) % 32)))

/** A thread is deleted when less than this many bytes are left on its stack. */
#define SCE_THREADMAN_MIN_FREE_STACK        (0x40)

/** The exception the threadman handles (coprocessor unusable, for the lazy FPU/VFPU context switch). */
#define SCE_THREADMAN_EXCEPTION_COP_UNUSABLE    (11)
/** The software interrupt used by the threadman to dispatch threads. */
#define SCE_THREADMAN_THREAD_INTR           (0x41)

/* Thread status values (thread->status). */
#define SCE_THREADMAN_STATUS_RUNNING    (0x01)
#define SCE_THREADMAN_STATUS_READY      (0x02)
#define SCE_THREADMAN_STATUS_WAIT       (0x04)
#define SCE_THREADMAN_STATUS_SUSPEND    (0x08)
#define SCE_THREADMAN_STATUS_DORMANT    (0x10)
#define SCE_THREADMAN_STATUS_DEAD       (0x20)

/* Wait types passed to _sceKernelWaitThread() */
#define SCE_THREADMAN_WAIT_SLEEP        (1)
#define SCE_THREADMAN_WAIT_DELAY        (2)
#define SCE_THREADMAN_WAIT_SEMA         (3)
#define SCE_THREADMAN_WAIT_EVENT_FLAG   (4)
#define SCE_THREADMAN_WAIT_MBX          (5)
#define SCE_THREADMAN_WAIT_VPL          (6)
#define SCE_THREADMAN_WAIT_FPL          (7)
#define SCE_THREADMAN_WAIT_MSG_PIPE     (8)
#define SCE_THREADMAN_WAIT_THREAD_END   (9)
#define SCE_THREADMAN_WAIT_THREAD_EVENT_HANDLER (10)
#define SCE_THREADMAN_WAIT_CALLBACK     (11)
#define SCE_THREADMAN_WAIT_MUTEX        (12)
#define SCE_THREADMAN_WAIT_TLSPL        (14)
#define SCE_THREADMAN_WAIT_LW_MUTEX     (13)

/** Get the UID control block of the data part data of a UID object of type type. */
#define THREADMAN_UID_DATA_TO_CB(data, type) ((SceSysmemUidCB *)((u8 *)(data) - (type)->size * 4))

/** The mode of a thread: the top 4 bits of its attributes, with SCE_THREADMAN_THREAD_ATTR_USER inverted. */
#define THREADMAN_THREAD_MODE(thread) (((thread)->attr ^ SCE_THREADMAN_THREAD_ATTR_USER) >> 28)

/**
 * Initialize the circular list head as empty.
 *
 * PARITY-Q201 (source form): next is stored before prev (see _sceKernelSysTimerInit).
 */
static inline void threadmanListInit(SceThreadmanListEntry *head)
{
    head->next = head;
    head->prev = head;
}

/** Insert entry at the end of the circular list head. */
static inline void threadmanListAddTail(SceThreadmanListEntry *head, SceThreadmanListEntry *entry)
{
    entry->next = head;
    entry->prev = head->prev;
    head->prev = entry;
    entry->prev->next = entry;
}

/*
 * PARITY-Q165: A list head seen as its own record type, used only for the suspended-thread add in
 * sub_00014124 (user-approved). Its accesses are in a different alias set from SceThreadmanListEntry's,
 * which reproduces the original's store order there (head->prev and entry->next scheduled as in the original
 * build). Applied to the suspendedThreads field itself, it regresses the four other adds of that list
 * (they keep the entry type), so the type is used through a cast in that one function only.
 */
/** A circular list head (its own type: not a list entry, but linked to like one). */
typedef struct {
    /** The first entry (the head itself if the list is empty). */
    SceThreadmanListEntry *next; // 0
    /** The last entry (the head itself if the list is empty). */
    SceThreadmanListEntry *prev; // 4
} SceThreadmanListHead; // size: 8

/** Insert entry at the end of the circular list headed by head. */
static inline void threadmanListHeadAddTail(SceThreadmanListHead *head, SceThreadmanListEntry *entry)
{
    entry->prev = head->prev;
    head->prev = entry;
    entry->next = (SceThreadmanListEntry *)head;
    entry->prev->next = entry;
}

/** Remove entry from the circular list it is linked in. */
static inline void threadmanListRemove(SceThreadmanListEntry *entry)
{
    SceThreadmanListEntry *next;

    next = entry->next;
    next->prev = entry->prev;
    entry->prev->next = next;
}

/**
 * A saved thread context, as built by SaveThreadContext() of the interrupt manager (which also
 * resumes it with InterruptManagerForKernel_551097F2()).
 */
typedef struct {
    u32 unk0[9]; // 0
    /** The context type (1: saved by SaveThreadContext(), 3: full context, e.g. built by the threadman to enter a callback). */
    u32 type; // 36
    /** The FPU control/status register. */
    u32 fcr31; // 40
    /** The HI register. */
    u32 hi; // 44
    /** The LO register. */
    u32 lo; // 48
    /** The COP0 Status register. */
    u32 status; // 52
    /** The address the execution resumes at. */
    u32 epc; // 56
    /**
     * The COP0 control register 23 (saved by SaveThreadContext() with "cfc0 $t4, $23; sw $t4, 60($a0)" and
     * restored by InterruptManagerForKernel_551097F2() with "lw $k0, 60($a0); ctc0 $k0, $23", see interruptman start.S).
     */
    u32 cop0Ctrl23; // 60
    /** The general purpose registers. */
    u32 gpr[32]; // 64
    /** The FPU registers. */
    u32 fpr[32]; // 192
} SceThreadmanContext; // size: 320

/** The handler called when a thread is killed (see sceKernelRegisterDeadThreadHandler()). */
typedef void (*SceThreadmanDeadThreadHandler)(SceUID thid, SceThreadmanContext *context);

/* COP0 Status values of a callback entry context */
/** Kernel mode, exception level, interrupts enabled. */
#define SCE_THREADMAN_CONTEXT_STATUS_KERNEL (0x03)
/** User mode, exception level, interrupts enabled. */
#define SCE_THREADMAN_CONTEXT_STATUS_USER   (0x13)
/** COP0 Status bit set when COP2 (the VFPU) is usable. */
#define SCE_THREADMAN_CONTEXT_STATUS_CU2    (0x40000000)

/** Context type of a full context (all the registers are valid), as built to enter a callback or to terminate a thread. */
#define SCE_THREADMAN_CONTEXT_TYPE_FULL (3)

/** The area at the top of a thread stack pointed to by its $k0 register (only the used fields are known). */
typedef struct {
    u32 unk0[16]; // 0
    /** The TLS pool blocks of the thread, indexed by the slot of their pool. */
    /*
     * PARITY-Q026: u32 words (alias set of SceSize): the blockSize load stays after the tls store as in the
     * original
     */
    u32 tls[16]; // 64
    u32 unk128[16]; // 128
    /** The UID of the thread. */
    SceUID threadId; // 192
    /** Set while the thread dispatch is suspended (sceKernelSuspendDispatchThread()), for user threads. */
    /*
     * PARITY-Q124: wrapped in a union (as Q070): the union access gives alias set 0, so the store conflicts with
     * the thread->context load and sub_00013E80 reloads the context as the original does
     */
    union {
        u32 dispatchSuspended;
    }; // 196
    /*
     * PARITY-Q141: k0 stackLow typed u8 * (not void *): the store in sceKernelStartThread then has its own alias
     *   set instead of sharing the void * one with the thread's stack fields.
     */
    /** The lowest address of the stack of the thread. */
    u8 *stackLow; // 200
    u32 unk204[11]; // 204
    s32 unk248; // 248
    s32 unk252; // 252
} SceThreadmanK0; // size: 256

/** A saved VFPU context. */
typedef struct {
    /** The VFPU registers. */
    u32 vr[128]; // 0
    /** The VFPU control registers (PFXS, PFXT, PFXD, CC, INF4, RSV5, RSV6, REV, RCX0 - RCX7). */
    u32 ctrl[16]; // 512
} SceThreadmanVfpuContext; // size: 576

/** A register frame saved on a syscall (only the used fields are known). */
typedef struct {
    /** The COP0 Status register of the caller. */
    u32 status; // 0
    /** The address the syscall returns to (the EPC). */
    u32 epc; // 4
    /** The stack pointer of the caller (presumably). */
    u32 sp; // 8
    /** The return address of the syscall. */
    u32 ra; // 12
    u32 unk16[2]; // 16
    /** The $v0 register of the caller (presumably; holds the callback result in _sceKernelReturnFromCallback()). */
    u32 v0; // 24
} SceThreadmanSyscallFrame;

/** The maximum number of kernel thread local storage (KTLS) allocations. */
#define SCE_THREADMAN_NUM_KTLS          (16)

/** The profiler state pointed to by the COP0 control register COP0_CTRL_PROFILER_BASE (NULL when unused). */
typedef struct {
    /** The profiler enable register. */
    vu32 *enable; // 0
    /** The value of *enable saved while the profiler is paused. */
    u32 savedEnable; // 4
    /** The pause nesting count. */
    s32 pauseCount; // 8
} SceThreadmanProfiler; // size: 12

/** The hardware profiler registers. */
#define THREADMAN_HW_PROFILER_REGS          ((SceThreadmanProfilerRegs *)0xBC400000)

/** A copy of the hardware profiler registers. */
typedef struct {
    u32 regs[21]; // 0
} SceThreadmanProfilerRegs; // size: 84

/** The profiler state of a thread (in profiler thread mode). */
typedef struct {
    /** The profiler state (pointed to by COP0_CTRL_PROFILER_BASE while the thread runs). */
    SceThreadmanProfiler profiler; // 0
    u32 unk12; // 12
    /** The profiler registers of the thread, saved when it is switched out. */
    SceThreadmanProfilerRegs regs; // 16
} SceThreadmanThreadProfiler; // size: 100

/** A stack replaced by a stack extension (sceKernelExtendKernelStack(), sceKernelExtendThreadStack()). */
typedef struct SceThreadmanStackExt {
    /** The previous saved stack. */
    struct SceThreadmanStackExt *next; // 0
    /** The lowest address of the saved stack. */
    void *low; // 4
    /** The memory block of the saved stack. */
    SceUID blockId; // 8
    /** The size of the saved stack. */
    s32 size; // 12
} SceThreadmanStackExt; // size: 16

/**
 * A user stack replaced by a user stack extension (sceKernelExtendThreadStack()), allocated from the
 * threadman heap. It also keeps the syscall frame fields replaced to run on the new stack.
 */
typedef struct {
    /** The replaced stack. */
    SceThreadmanStackExt ext; // 0
    /** The saved syscall return address (EPC). */
    u32 epc; // 16
    /** The saved return address of the caller. */
    u32 ra; // 20
    /** The saved stack pointer of the caller. */
    u32 sp; // 24
} SceThreadmanUserStackExt; // size: 28

/**
 * The thread control block (data part of a "Thread" UID object, size 0x140).
 * Only the fields used by the reversed code are named so far.
 */
typedef struct SceThreadmanThread {
    /** Link in the ready queue or in a wait queue. */
    SceThreadmanListEntry link; // 0
    /** The UID of the thread. */
    SceUID uid; // 8
    /** The thread status (one of SCE_THREADMAN_STATUS_*). */
    s32 status; // 12
    /** The current priority (lower value means higher priority). */
    s32 currentPriority; // 16
    /** The thread attributes (SCE_THREADMAN_THREAD_ATTR_*). */
    u32 attr; // 20
    /**
     * Set when the thread uses the FPU: set to 1 by the COP1 unusable exception handler sub_00000338() when
     * it enables COP1 for the thread ("sw $v1, 0x18($v0)"), saved around the callbacks.
     */
    u32 fpuUsed; // 24
    /** The saved thread context. */
    SceThreadmanContext *context; // 28
    /** The VFPU context of the thread (allocated from the threadman heap, NULL if the thread does not use the VFPU). */
    /* PARITY-Q027: void * (vfpuContext, syscallFrame): the alias-set reloads of the original in sub_000180AC */
    void *vfpuContext; // 32
    /** The register frame of the current syscall (NULL when none). */
    void *syscallFrame; // 36
    /** The user mode stack pointer (written to COP0_CTRL_SP_USER). */
    u32 userSp; // 40
    void *stackLow; // 44
    void *kStackLow; // 48
    /** The time the thread has been running. */
    SceKernelSysClock runClocks; // 52
    /** The profiler state of the thread (allocated from the threadman heap in profiler thread mode). */
    SceThreadmanThreadProfiler *profiler; // 60
    /** The run status counter incremented when the thread is switched out next (one of the three below). */
    s32 *switchCounter; // 64
    /** The number of times the thread was preempted by an interrupt. */
    s32 intrPreemptCount; // 68
    /** The number of times the thread was preempted by another thread. */
    s32 threadPreemptCount; // 72
    /** The number of times the thread released the CPU (by waiting). */
    s32 releaseCount; // 76
    /** The number of pending wakeups (sceKernelWakeupThread() calls on a thread which was not sleeping). */
    s32 wakeupCount; // 80
    /** The exit status of the thread. */
    s32 exitStatus; // 84
    /** The type of the current wait (SCE_THREADMAN_WAIT_*). */
    s32 waitType; // 88
    /** The UID object (wait queue) the thread is currently waiting on. */
    SceSysmemUidCB *waitObj; // 92
    /**
     * Wait type specific data (for semaphores: the requested count; for event flags: the bit pattern,
     * the wait mode and the address receiving the matched pattern).
     */
    /*
     * PARITY-Q070: wrapped in a union (the original reuses one field per wait type); the union access gives alias set 0,
     * which the original's scheduling of the wait-data stores/loads needs
     */
    union {
        s32 waitData[6];
    }; // 96
    /** The alarm ending the current wait on timeout (0 when the wait has no timeout). */
    SceUID timeoutAlarmId; // 120
    /** The timeout of the current wait (receives the remaining time when the wait ends), if any. */
    SceUInt *timeout; // 124
    /**
     * The result of the last wait (SCE_ERROR_OK or a SCE_ERROR_KERNEL_WAIT_* error), also receiving
     * the result of a callback.
     */
    s32 waitStatus; // 128
    /** The context the current callback (or thread event handler) returns to. */
    SceThreadmanContext *callbackContext; // 132
    /** The thread events whose handlers the thread is currently running (SCE_KERNEL_THREAD_EVENT_*). */
    u32 threadEventMask; // 136
    /** The thread event handler the thread is currently running (NULL if none). */
    SceSysmemUidCB *threadEventHandler; // 140
    /** The callbacks of the thread (SceThreadmanCallback.link). */
    SceThreadmanListEntry callbacks; // 144
    /** Set when the thread waits and processes callbacks at the same time. */
    s32 waitWithCallbacks; // 152
    /** Number of callbacks notified for this thread. */
    s32 callbackNotified; // 156
    /** The number of nested running callbacks. */
    s32 callbackDepth; // 160
    /** The callback currently running (NULL if none). */
    SceSysmemUidCB *curCallback; // 164
    /** The mutexes this thread currently owns (SceThreadmanMutex.ownerLink), by ceiling priority if any. */
    SceThreadmanListEntry ownedMutexes; // 168
    /** The single-waiter event flags this thread currently owns (SceThreadmanEventFlag.ownerLink). */
    SceThreadmanListEntry ownedEventFlags; // 176
    /** The initial priority of the thread. */
    s16 initPriority; // 184
    /** The priority of the thread when it is not raised by the ceiling priority of the mutexes it owns. */
    s16 basePriority; // 186
    /** The highest ceiling priority of the mutexes the thread owns (0x7F when none). */
    s32 mutexPriority; // 188
    /*
     * PARITY-Q142: entry typed void * (threadman never calls it, only loads it into EPC): the load then shares
     *   the void * alias set, so sceKernelReferThreadStatus reads it at the original's position.
     */
    /** The entry point of the thread. */
    void *entry; // 192
    /** The size of the (user or kernel) stack of the thread. */
    s32 stackSize; // 196
    /**
     * The initial stack pointer of the thread (the top of its stack, below the $k0 area of a user mode thread),
     * set by sceKernelStartThread() and used as the initial $sp and $fp, and as the stack a user mode callback
     * runs on when the thread is not in a syscall.
     */
    u32 initSp; // 200
    /** The size of the kernel stack of a user mode thread. */
    s32 kStackSize; // 204
    /**
     * The stacks replaced by extensions of the stack the thread runs on in kernel mode (its kernel stack
     * for a user mode thread, its stack otherwise).
     */
    SceThreadmanStackExt *kStackExts; // 208
    /** The stacks replaced by extensions of the stack of a user mode thread. */
    SceThreadmanStackExt *stackExts; // 212
    /** The $gp register value of the thread. */
    u32 gp; // 216
    /** The $k0 register value of the thread (an area at the top of its stack). */
    SceThreadmanK0 *k0; // 220
    /** The KTLS blocks of the thread, indexed by KTLS allocation id. */
    void *ktls[SCE_THREADMAN_NUM_KTLS]; // 224
    /** The initial attributes of the thread. */
    u32 initAttr; // 288
    /** The size of the argument block passed to the thread entry. */
    SceSize argSize; // 292
    /** The argument block passed to the thread entry (a copy on the thread stack). */
    void *argp; // 296
    /** The options of sceKernelCreateThread() (caller's pointer). */
    SceKernelThreadOptParam *option; // 300
    /** The memory block of the stack. */
    SceUID stackBlockId; // 304
    /** The memory block of the kernel stack of a user mode thread. */
    SceUID kStackBlockId; // 308
    /** The memory partition of the stack. */
    SceUID stackMpid; // 312
    /** The memory partition of the stack the thread runs on in kernel mode. */
    SceUID kStackMpid; // 316
} SceThreadmanThread; // size: 320

/** Thread status of a thread which is both waiting and suspended. */
#define SCE_THREADMAN_STATUS_WAIT_SUSPEND   (SCE_THREADMAN_STATUS_WAIT | SCE_THREADMAN_STATUS_SUSPEND)

/** Wait queue attribute: waiting threads are woken up in priority order (FIFO otherwise). */
#define SCE_THREADMAN_WAITQ_ATTR_PRIORITY   (0x100)

/** The data part of a "WaitQ" UID object, which all the synchronization objects inherit. */
typedef struct {
    /** The attributes of the object (SCE_THREADMAN_WAITQ_ATTR_*). */
    u32 attr; // 0
    /**
     * When set, the first waiting thread is being served (a message pipe transfer of it is partially done,
     * see msgpipe.c: set to "origSize != size"): priority ordered insertions keep it first.
     */
    u32 firstServing; // 4
    /** The number of threads waiting on the object. */
    s32 numWaitThreads; // 8
    /** The list of waiting threads. */
    SceThreadmanListEntry waitList; // 12
} SceThreadmanWaitQ; // size: 20

/** The data part of a "Semaphore" UID object. */
typedef struct {
    /** The option parameter passed to sceKernelCreateSema(). */
    SceKernelSemaOptParam *option; // 0
    /** The current semaphore count. */
    s32 curCount; // 4
    /** The maximum semaphore count. */
    s32 maxCount; // 8
    /** The initial semaphore count. */
    s32 initCount; // 12
} SceThreadmanSema; // size: 16

/** The data part of an "EventFlag" UID object. */
typedef struct {
    /** The option parameter passed to sceKernelCreateEventFlag(). */
    SceKernelEventFlagOptParam *option; // 0
    /** The current bit pattern. */
    u32 curPattern; // 4
    /** The initial bit pattern. */
    u32 initPattern; // 8
    /**
     * For an event flag without SCE_KERNEL_EA_MULTI: the only thread allowed to wait on it,
     * set by the first waiting thread (NULL when unowned).
     */
    SceThreadmanThread *owner; // 12
    /** The number of pending waits of the owner. */
    s32 ownerWaitCount; // 16
    /** Link in owner->ownedEventFlags. */
    SceThreadmanListEntry ownerLink; // 20
} SceThreadmanEventFlag; // size: 28

/** The data part of a "Mutex" UID object. */
typedef struct {
    /** Link in owner->ownedMutexes. */
    SceThreadmanListEntry ownerLink; // 0
    /** The UID of the mutex. */
    SceUID uid; // 8
    /** The option parameter passed to sceKernelCreateMutex(). */
    const SceKernelMutexOptParam *option; // 12
    /** The initial lock count. */
    s32 initCount; // 16
    /** The current lock count. */
    s32 lockCount; // 20
    /**
     * The thread owning the mutex: NULL when unlocked, SCE_THREADMAN_MUTEX_OWNER_EXITED when its owner
     * exited without unlocking it.
     */
    SceThreadmanThread *owner; // 24
    /** The ceiling priority of the mutex (only meaningful with SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING). */
    s32 ceilingPriority; // 28
} SceThreadmanMutex; // size: 32

/** The data part of a "LwMutex" UID object (the state of a lightweight mutex is in its user work area). */
typedef struct {
    u32 unk0[2]; // 0
    /** The UID of the lightweight mutex. */
    SceUID uid; // 8
    /** The work area holding the lock count, the owner and the number of waiting threads. */
    SceLwMutex *workarea; // 12
    /** The option parameter passed to sceKernelCreateLwMutex(). */
    SceKernelLwMutexOptParam *option; // 16
    /** The initial lock count. */
    s32 initCount; // 20
} SceThreadmanLwMutex; // size: 24

/** The data part of a "Mbx" UID object. */
typedef struct {
    /** The option parameter given at creation. */
    SceKernelMbxOptParam *option; // 0
    /** The number of messages in the message box. */
    s32 numMessages; // 4
    /** The last message of the circular message list (its next member is the first message). */
    SceKernelMsgPacket *lastMessage; // 8
} SceThreadmanMbx; // size: 12

/* Message pipe states: the threads waiting on a message pipe are either all senders or all receivers. */
#define SCE_THREADMAN_MSG_PIPE_STATE_EMPTY              (0)
#define SCE_THREADMAN_MSG_PIPE_STATE_SENDERS_WAITING    (1)
#define SCE_THREADMAN_MSG_PIPE_STATE_RECEIVERS_WAITING  (2)

/** The data part of a "MsgPipe" UID object. */
typedef struct {
    /** The option parameter given at creation. */
    SceKernelMppOptParam *option; // 0
    /** The kind of the waiting threads (SCE_THREADMAN_MSG_PIPE_STATE_*). */
    s32 state; // 4
    /** The ring buffer (NULL if the message pipe has no buffer). */
    u8 *buf; // 8
    /** The position where the next data is written to the buffer. */
    u8 *writePtr; // 12
    /** The position where the next data is read from the buffer. */
    u8 *readPtr; // 16
    /** The size of the buffer. */
    SceSize bufSize; // 20
    /** The number of bytes stored in the buffer. */
    SceSize used; // 24
    /** The memory block of the buffer. */
    SceUID blockId; // 28
} SceThreadmanMsgPipe; // size: 32

/** The data part of a "Vpl" UID object. */
typedef struct {
    /** The option parameter given at creation. */
    SceKernelVplOptParam *option; // 0
    /** The system memory heap of the pool. */
    SceUID heapId; // 4
    /** The free size of the heap at creation (the largest size which can be allocated). */
    SceSize poolSize; // 8
} SceThreadmanVpl; // size: 12

/** The free list link of a block of a fixed-length memory pool. */
typedef struct SceThreadmanFplLink {
    /** The next free block (circular list), or NULL if the block is allocated. */
    struct SceThreadmanFplLink *next; // 0
} SceThreadmanFplLink;

/** The data part of a "Fpl" UID object. */
typedef struct {
    /** The option parameter given at creation. */
    SceKernelFplOptParam *option; // 0
    /** The address of the first block. */
    u8 *base; // 4
    /** The last block of the circular free list (NULL if no block is free). */
    SceThreadmanFplLink *lastFree; // 8
    /** The links of the blocks (inlineLinks, or allocated from the threadman heap for more than two blocks). */
    SceThreadmanFplLink *links; // 12
    /** The number of free blocks. */
    s32 numFreeBlocks; // 16
    /** The block size given at creation. */
    SceSize size; // 20
    /** The number of blocks. */
    s32 numBlocks; // 24
    /** The aligned size of a block. */
    SceSize blockSize; // 28
    /** The partition memory block of the pool. */
    SceUID blockId; // 32
    /** The links of the blocks of a pool of at most two blocks. */
    SceThreadmanFplLink inlineLinks[2]; // 36
} SceThreadmanFpl; // size: 44

/** The data part of a "Tlspl" UID object (same layout as SceThreadmanFpl). */
typedef struct {
    /** The option parameter given at creation. */
    SceKernelTlsplOptParam *option; // 0
    /** The address of the first block. */
    u8 *base; // 4
    /** The last block of the circular free list (NULL if no block is free). */
    SceThreadmanFplLink *lastFree; // 8
    /** The links of the blocks (inlineLinks, or allocated from the threadman heap for more than two blocks). */
    SceThreadmanFplLink *links; // 12
    /** The number of free blocks. */
    s32 numFreeBlocks; // 16
    /** The block size given at creation. */
    SceSize size; // 20
    /** The number of blocks. */
    s32 numBlocks; // 24
    /** The aligned size of a block. */
    SceSize blockSize; // 28
    /** The partition memory block of the pool. */
    SceUID blockId; // 32
    /** The links of the blocks of a pool of at most two blocks. */
    SceThreadmanFplLink inlineLinks[2]; // 36
} SceThreadmanTlspl; // size: 44

/** The maximum number of free delay timers kept for reuse. */
#define SCE_THREADMAN_MAX_FREE_DELAYS   (32)

/** Get the 64-bit value of the system clock clock. */
#define THREADMAN_SYSCLOCK_GET(clock) (((u64)(clock).hi << 32) | (clock).low)
/** Set the system clock clock to the 64-bit value value. */
#define THREADMAN_SYSCLOCK_SET(clock, value) \
    do { \
        (clock).low = (u32)(value); \
        (clock).hi = (u32)((u64)(value) >> 32); \
    } while (0)

/*
 * Pointer-based accessors for a system clock. Most code uses these; _sceKernelInsertTimer and
 * _sceKernelUpdateSysTimerCompare read and write the fields directly (the macros above).
 */

/** Get the 64-bit value of the system clock clock. */
static inline u64 threadmanSysClockGet(SceKernelSysClock *clock)
{
    return ((u64)clock->hi << 32) | clock->low;
}

/** Set the system clock clock to the 64-bit value value. */
/*
 * PARITY-Q007: clock setter stores hi then low through a volatile pointer (the original's store order); pointer-
 * taking accessors
 */
static inline void threadmanSysClockSet(volatile SceKernelSysClock *clock, u64 value)
{
    clock->hi = (u32)(value >> 32);
    clock->low = (u32)value;
}

/*
 * PARITY-Q161: A separate 32-bit setter (hi stored as 0, low from a u32) for the 32-bit constant sets. Through the
 *     u64 threadmanSysClockSet() the 64-bit constant SYSTIMER_OVF_PERIOD leaves a dead `li $v1, 0` (its high word)
 *     in _sceKernelSysTimerInit, which the original does not have. Used for every constant set (SysTimerInit x2,
 *     the two zero clears in timer.c); the zero clears are unchanged.
 */
/** Set a SceKernelSysClock to a 32-bit value. */
static inline void threadmanSysClockSet32(volatile SceKernelSysClock *clock, u32 value)
{
    clock->hi = 0;
    clock->low = value;
}

/** The data of a "Timer" UID object, the base type of the delay timers, the alarms and the virtual timers. */
typedef struct {
    /** The link in the list of active timers (g_threadman.timers) or of free delays (g_threadman.freeDelays). */
    SceThreadmanListEntry link; // 0
    /** The system time at which the timer expires. */
    SceKernelSysClock schedule; // 8
    /** Set when the deletion of the timer has been deferred. */
    s32 deletePending; // 16
} SceThreadmanTimer; // size: 20

/** The handler of a delay timer (its return value is ignored, unlike the one of an alarm). */
typedef void (*SceThreadmanTimerHandler)(void *common);

/** The data of a "Delay" UID object (an internal one-shot timer, used by timeouts and thread delays). */
typedef struct {
    /** The handler called on expiration. */
    SceThreadmanTimerHandler handler; // 0
    /** The argument passed to the handler. */
    void *common; // 4
} SceThreadmanDelay; // size: 8

/** The data of an "Alarm" UID object. */
typedef struct {
    /** Non-zero if the handler is called in user mode. */
    s32 isUser; // 0
    /** The handler called on expiration. */
    SceKernelAlarmHandler handler; // 4
    /** The argument passed to the handler. */
    void *common; // 8
    /** The $gp register value of the handler's module. */
    u32 gp; // 12
} SceThreadmanAlarm; // size: 16

/** The data of a "VTimer" UID object. */
typedef struct {
    /** Non-zero while the virtual timer is running. */
    s32 active; // 0
    /** The system time at which the timer was (virtually) started if active, its time otherwise. */
    SceKernelSysClock base; // 4
    /** The virtual time at which the handler is called next. */
    SceKernelSysClock schedule; // 12
    /** Non-zero if the handler is called in user mode. */
    s32 isUser; // 20
    /** The handler (NULL if none). */
    SceKernelVTimerHandler handler; // 24
    /** The argument passed to the handler. */
    void *common; // 28
    /** The $gp register value of the handler's module. */
    u32 gp; // 32
} SceThreadmanVTimer; // size: 36

/** The maximum number of TLS pools. */
#define SCE_THREADMAN_NUM_TLSPL         (16)

/** A kernel thread local storage allocation (each thread gets its own block of it on request). */
typedef struct {
    /** Non-zero when the allocation is in use. */
    s32 used; // 0
    /** The number of threads which have a block of the allocation. */
    s32 numThreads; // 4
    /** The size of the per-thread block. */
    s32 size; // 8
    /** Called with the block of a thread when the thread is deleted (may be NULL). */
    void (*destructor)(void *ktls, void *arg); // 12
    /** The user argument passed to destructor. */
    void *arg; // 16
    /** The $gp register value of the destructor's module. */
    u32 gp; // 20
} SceThreadmanKtls; // size: 24

/* The slot of a TLS pool is stored in bits 3-6 of its UID. */
#define THREADMAN_TLSPL_SLOT(uid)       (((uid) >> 3) & 0xF)

/** Mutex attribute: the owner of the mutex runs at least at the ceiling priority of the mutex (kernel only). */
#define SCE_THREADMAN_MUTEX_ATTR_PRIORITY_CEILING   (0x400)
/** Mutex attribute: the mutex stays locked when its owner exits (it is unlocked otherwise). */
#define SCE_THREADMAN_MUTEX_ATTR_KEEP_ON_EXIT       (0x800)

/** The owner of a mutex whose owner exited without unlocking it. */
#define SCE_THREADMAN_MUTEX_OWNER_EXITED            ((SceThreadmanThread *)-1)

/** The data part of a "ThreadEventHandler" UID object. */
typedef struct {
    /** Link in g_threadman.threadEventHandlers. */
    SceThreadmanListEntry link; // 0
    /** The UID of the handler. */
    SceUID uid; // 8
    /** The number of running calls of the handler. */
    s32 runCount; // 12
    /** Set once the handler is being deleted. */
    s32 deleted; // 16
    /** The monitored thread, or one of SCE_KERNEL_THREAD_EVENT_ID_ALL/_KERN/_USER. */
    SceThreadmanThread *thread; // 20
    /** Set when the handler was registered from user mode (it is called in user mode). */
    s32 userMode; // 24
    /** The handled events (SCE_KERNEL_THREAD_EVENT_*). */
    u32 mask; // 28
    /** The handler. */
    SceKernelThreadEventHandler handler; // 32
    /** The common pointer passed to the handler. */
    void *common; // 36
    /** The $gp register value of the module of the handler. */
    u32 gp; // 40
} SceThreadmanThreadEventHandler; // size: 44

/** The data part of a "Callback" UID object. */
typedef struct {
    /** Link in thread->callbacks. */
    SceThreadmanListEntry link; // 0
    /** The UID of the callback. */
    SceUID uid; // 8
    /** The number of running calls of the callback. */
    s32 runCount; // 12
    /** Set once the callback is being deleted. */
    s32 deleted; // 16
    /** The thread the callback belongs to (its creator). */
    SceThreadmanThread *thread; // 20
    /** The number of notifications since the last call. */
    s32 notifyCount; // 24
    /** The argument of the last notification. */
    s32 notifyArg; // 28
    /** Set when the callback was created from user mode (it is called in user mode). */
    s32 userMode; // 32
    /** The callback function. */
    SceKernelCallbackFunction func; // 36
    /** The common pointer passed to the callback function. */
    void *common; // 40
    /** The $gp register value of the module of the callback function. */
    u32 gp; // 44
} SceThreadmanCallback; // size: 48

/* Permissions of a user mode caller on a UID object (SceSysmemUidCB.attr) */
#define SCE_THREADMAN_UID_ATTR_REFER    (0x01)
#define SCE_THREADMAN_UID_ATTR_SIGNAL   (0x02)
#define SCE_THREADMAN_UID_ATTR_WAIT     (0x04)
#define SCE_THREADMAN_UID_ATTR_CANCEL   (0x08)
#define SCE_THREADMAN_UID_ATTR_DELETE   (0x10)

/*
 * The wait functions and their callback variants (sceKernelWaitSema() and sceKernelWaitSemaCB(), ...) are
 * tail jumps to a common function, passing withCallbacks in $t3, the 8th argument register, whatever the
 * number of arguments: "j impl; addiu $t3, $zero, withCallbacks". GCC cannot emit this (no naked functions
 * on MIPS, and it would set the unused argument registers), so the thunk is written as top-level assembly.
 * The common function takes only its real arguments and reads $t3 with threadmanGetWithCallbacks().
 */
/*
 * PARITY-Q003: the Wait and WaitCB entry points are `j impl; addiu $t3,$zero,N` thunks in the original (top-level
 * asm); all uses grep THREADMAN_WITH_CALLBACKS_THUNK
 */
/*
 * PARITY-Q186: each thunk is put in its own `.text.<name>` section (pushsection/popsection) instead of `.text`. The
 * matching build compiles with -ffunction-sections (one gas fixup chain per function, which is what reproduces the
 * HI16/LO16 order of the original relocation stream); a thunk left in `.text` would be linked before all the function
 * sections of its object, moving it away from its place between the C functions.
 */
#define THREADMAN_WITH_CALLBACKS_THUNK(name, impl, withCallbacks) \
    asm(".pushsection .text." #name ", \"ax\", @progbits\n" \
        ".align 2\n" \
        ".globl " #name "\n" \
        ".type " #name ", @function\n" \
        ".ent " #name "\n" \
        #name ":\n" \
        ".set push\n" \
        ".set noreorder\n" \
        "j " #impl "\n" \
        "addiu $t3, $zero, " #withCallbacks "\n" \
        ".set pop\n" \
        ".end " #name "\n" \
        ".size " #name ", . - " #name "\n" \
        ".popsection\n")

/*
 * Switch $gp to gp and return the previous value. The handler calls in 6.60 keep `move rX,$gp; move $gp,rY` together,
 * after the load of the new value and before everything that follows (no interblock motion), which is what volatile
 * asm gives (registers.h pspSetGp() is non-volatile, so GCC hoists the $gp read into an earlier block).
 */
/* PARITY-Q005: volatile $gp save+switch kept together after the new value's load, as in the original handler calls */
/*
 * PARITY-Q190 (trick): memory clobber on the $gp switch: the compiler may not keep memory values in registers
 *     across it, so _sceKernelFreeThreadKTLS reloads the destructor after the switch (lw $v1,0xC($s2) after
 *     move $gp,$v0) as the original does. The alarm/vtimer handler calls are unchanged.
 */
static inline u32 threadmanSwapGp(u32 gp)
{
    u32 oldGp;

    asm volatile("move %0, $gp" : "=r" (oldGp));
    asm volatile("move $gp, %0" : : "r" (gp) : "memory"); /* PARITY-Q190: memory clobber */
    return oldGp;
}

/*
 * Read withCallbacks from $t3 (set by THREADMAN_WITH_CALLBACKS_THUNK()), as the first statement of the common
 * function. This is hand-written assembly in the original too: in all 12 common functions of 6.60 the `move rX, $t3`
 * is a full scheduling barrier (every callee save, argument copy and argument spill is before it, every body
 * instruction after it, including the k1/cfc0 entry checks that GCC otherwise moves into the prologue). Only a
 * volatile asm gives that: a C parameter in $t3 is an ordinary argument copy that the scheduler moves freely.
 * See threadman-deviations.md section 4 (assembly usage).
 */
/*
 * PARITY-Q004: volatile read of $t3: in all 12 common wait functions the original's `move rX,$t3` is a full
 * scheduling barrier
 */
static inline s32 threadmanGetWithCallbacks(void)
{
    s32 withCallbacks;

    asm volatile("move %0, $t3" : "=r" (withCallbacks));
    return withCallbacks;
}

/** The break code of the unrecoverable errors (returning from a callback or a stack extension in a wrong context). */
#define THREADMAN_BREAKCODE_FATAL           (0xFFF)

/**
 * Check whether the caller has the permission perm (SCE_THREADMAN_UID_ATTR_*) on the UID object uid: a kernel
 * mode caller always has it.
 */
/** Check whether the caller may do the operations perm (SCE_THREADMAN_UID_ATTR_*) on uid (always in kernel mode). */
/* PARITY-Q033: permission check as an inline function (the original's branch shape) */
static inline u32 threadmanUidPermOk(SceSysmemUidCB *uid, u32 perm)
{
    u32 ok = uid->attr & perm;

    if (((u32)pspGetK1() >> 31) == 0)
        ok = perm;
    return ok;
}

#define THREADMAN_UID_PERM_OK(uid, perm) threadmanUidPermOk((SceSysmemUidCB *)(uid), (perm))

/* THREADMAN_UID_PERM_OK with the caller mode (pspK1IsUserMode()) already read. PARITY-Q076 */
static inline u32 threadmanUidPermOkMode(SceSysmemUidCB *uid, u32 perm, u32 userMode)
{
    u32 ok = uid->attr & perm;

    if (userMode == 0)
        ok = perm;
    return ok;
}

/* Copy a status structure to the caller's buffer, truncated to the smaller of both sizes. */
/*
 * PARITY-Q018: src passed as an address expression so the size is read through a constant address, as the
 * original; uses grep THREADMAN_COPY_STATUS
 */
#define THREADMAN_COPY_STATUS(dst, src) \
    memcpy((dst), (src), ((src)->size < (dst)->size) ? (src)->size : (dst)->size)

/* UID object functions implemented by the WaitQ-derived types */
#define SCE_THREADMAN_UID_FUNC_WAITQ_CANCEL         (0x86D94883)
#define SCE_THREADMAN_UID_FUNC_WAITQ_WAKEUP_WAITERS (0x85FE2B4C)
/** Called with the thread (va_arg) whose wait on the object ends (wakeup, timeout, cancel, ...). */
#define SCE_THREADMAN_UID_FUNC_WAITQ_END_WAIT       (0x5BD49380)

/* Event IDs passed to the debug log handler (g_threadman.logHandler) */
#define SCE_THREADMAN_LOG_CALL_CALLBACK                     (0x07)
#define SCE_THREADMAN_LOG_CALL_THREAD_EVENT_HANDLER         (0x08)
#define SCE_THREADMAN_LOG_CALL_ALARM_HANDLER                (0x05)
#define SCE_THREADMAN_LOG_CALL_VTIMER_HANDLER               (0x06)
#define SCE_THREADMAN_LOG_CREATE_THREAD                     (0x10)
#define SCE_THREADMAN_LOG_DELETE_THREAD                     (0x11)
#define SCE_THREADMAN_LOG_START_THREAD                      (0x12)
#define SCE_THREADMAN_LOG_EXIT_THREAD                       (0x13)
#define SCE_THREADMAN_LOG_EXIT_DELETE_THREAD                (0x14)
#define SCE_THREADMAN_LOG_TERMINATE_THREAD                  (0x15)
#define SCE_THREADMAN_LOG_TERMINATE_DELETE_THREAD           (0x16)
#define SCE_THREADMAN_LOG_SUSPEND_DISPATCH_THREAD           (0x17)
#define SCE_THREADMAN_LOG_RESUME_DISPATCH_THREAD            (0x18)
#define SCE_THREADMAN_LOG_CHANGE_CURRENT_THREAD_ATTR        (0x19)
#define SCE_THREADMAN_LOG_CHANGE_THREAD_PRIORITY            (0x1A)
#define SCE_THREADMAN_LOG_ROTATE_THREAD_READY_QUEUE         (0x1B)
#define SCE_THREADMAN_LOG_RELEASE_WAIT_THREAD               (0x1C)
#define SCE_THREADMAN_LOG_GET_THREAD_ID                     (0x1D)
#define SCE_THREADMAN_LOG_GET_THREAD_CURRENT_PRIORITY       (0x1E)
#define SCE_THREADMAN_LOG_GET_THREAD_EXIT_STATUS            (0x1F)
#define SCE_THREADMAN_LOG_CHECK_THREAD_STACK                (0x20)
#define SCE_THREADMAN_LOG_GET_THREAD_STACK_FREE_SIZE        (0x21)
#define SCE_THREADMAN_LOG_REFER_THREAD_STATUS               (0x22)
#define SCE_THREADMAN_LOG_REFER_THREAD_RUN_STATUS           (0x23)
#define SCE_THREADMAN_LOG_REFER_THREAD_DEBUG_STATUS         (0x24)
#define SCE_THREADMAN_LOG_GET_USER_LEVEL                    (0x25)
#define SCE_THREADMAN_LOG_IS_USER_MODE_THREAD               (0x26)
#define SCE_THREADMAN_LOG_CHANGE_THREAD_STACK               (0x28)
#define SCE_THREADMAN_LOG_EXTEND_THREAD_STACK               (0x29)
#define SCE_THREADMAN_LOG_EXTEND_KERNEL_STACK               (0x2A)
#define SCE_THREADMAN_LOG_CHECK_THREAD_KERNEL_STACK         (0x2B)
#define SCE_THREADMAN_LOG_GET_THREAD_KERNEL_STACK_FREE_SIZE (0x2C)
#define SCE_THREADMAN_LOG_REGISTER_THREAD_EVENT_HANDLER     (0x2D)
#define SCE_THREADMAN_LOG_RELEASE_THREAD_EVENT_HANDLER      (0x2E)
#define SCE_THREADMAN_LOG_REFER_THREAD_EVENT_HANDLER_STATUS (0x2F)
#define SCE_THREADMAN_LOG_SLEEP_THREAD          (0x30)
#define SCE_THREADMAN_LOG_SLEEP_THREAD_CB       (0x31)
#define SCE_THREADMAN_LOG_WAKEUP_THREAD         (0x32)
#define SCE_THREADMAN_LOG_CANCEL_WAKEUP_THREAD  (0x33)
#define SCE_THREADMAN_LOG_DONATE_WAKEUP_THREAD  (0x34)
#define SCE_THREADMAN_LOG_SUSPEND_THREAD        (0x35)
#define SCE_THREADMAN_LOG_RESUME_THREAD         (0x36)
#define SCE_THREADMAN_LOG_WAIT_THREAD_END       (0x37)
#define SCE_THREADMAN_LOG_WAIT_THREAD_END_CB    (0x38)
#define SCE_THREADMAN_LOG_CREATE_SEMA       (0x39)
#define SCE_THREADMAN_LOG_DELETE_SEMA       (0x3A)
#define SCE_THREADMAN_LOG_SIGNAL_SEMA       (0x3B)
#define SCE_THREADMAN_LOG_WAIT_SEMA         (0x3C)
#define SCE_THREADMAN_LOG_WAIT_SEMA_CB      (0x3D)
#define SCE_THREADMAN_LOG_POLL_SEMA         (0x3E)
#define SCE_THREADMAN_LOG_CANCEL_SEMA       (0x3F)
#define SCE_THREADMAN_LOG_REFER_SEMA_STATUS (0x40)
#define SCE_THREADMAN_LOG_CREATE_EVENT_FLAG         (0x41)
#define SCE_THREADMAN_LOG_DELETE_EVENT_FLAG         (0x42)
#define SCE_THREADMAN_LOG_SET_EVENT_FLAG            (0x43)
#define SCE_THREADMAN_LOG_CLEAR_EVENT_FLAG          (0x44)
#define SCE_THREADMAN_LOG_WAIT_EVENT_FLAG           (0x45)
#define SCE_THREADMAN_LOG_WAIT_EVENT_FLAG_CB        (0x46)
#define SCE_THREADMAN_LOG_POLL_EVENT_FLAG           (0x47)
#define SCE_THREADMAN_LOG_CANCEL_EVENT_FLAG         (0x48)
#define SCE_THREADMAN_LOG_REFER_EVENT_FLAG_STATUS   (0x49)
#define SCE_THREADMAN_LOG_CREATE_MUTEX              (0x4A)
#define SCE_THREADMAN_LOG_DELETE_MUTEX              (0x4B)
#define SCE_THREADMAN_LOG_UNLOCK_MUTEX              (0x4C)
#define SCE_THREADMAN_LOG_LOCK_MUTEX                (0x4D)
#define SCE_THREADMAN_LOG_LOCK_MUTEX_CB             (0x4E)
#define SCE_THREADMAN_LOG_TRY_LOCK_MUTEX            (0x4F)
#define SCE_THREADMAN_LOG_CANCEL_MUTEX              (0x50)
#define SCE_THREADMAN_LOG_REFER_MUTEX_STATUS        (0x51)
#define SCE_THREADMAN_LOG_CREATE_LW_MUTEX           (0x52)
#define SCE_THREADMAN_LOG_DELETE_LW_MUTEX           (0x53)
#define SCE_THREADMAN_LOG_UNLOCK_LW_MUTEX           (0x54)
#define SCE_THREADMAN_LOG_LOCK_LW_MUTEX             (0x55)
#define SCE_THREADMAN_LOG_LOCK_LW_MUTEX_CB          (0x56)
#define SCE_THREADMAN_LOG_TRY_LOCK_LW_MUTEX         (0x57)
#define SCE_THREADMAN_LOG_REFER_LW_MUTEX_STATUS_BY_ID   (0x59)
#define SCE_THREADMAN_LOG_CREATE_MBX                    (0x5A)
#define SCE_THREADMAN_LOG_DELETE_MBX                    (0x5B)
#define SCE_THREADMAN_LOG_SEND_MBX                      (0x5C)
#define SCE_THREADMAN_LOG_RECEIVE_MBX                   (0x5D)
#define SCE_THREADMAN_LOG_RECEIVE_MBX_CB                (0x5E)
#define SCE_THREADMAN_LOG_POLL_MBX                      (0x5F)
#define SCE_THREADMAN_LOG_CANCEL_RECEIVE_MBX            (0x60)
#define SCE_THREADMAN_LOG_REFER_MBX_STATUS              (0x61)
#define SCE_THREADMAN_LOG_CREATE_MSG_PIPE               (0x62)
#define SCE_THREADMAN_LOG_DELETE_MSG_PIPE               (0x63)
#define SCE_THREADMAN_LOG_SEND_MSG_PIPE                 (0x64)
#define SCE_THREADMAN_LOG_SEND_MSG_PIPE_CB              (0x65)
#define SCE_THREADMAN_LOG_TRY_SEND_MSG_PIPE             (0x66)
#define SCE_THREADMAN_LOG_RECEIVE_MSG_PIPE              (0x67)
#define SCE_THREADMAN_LOG_RECEIVE_MSG_PIPE_CB           (0x68)
#define SCE_THREADMAN_LOG_TRY_RECEIVE_MSG_PIPE          (0x69)
#define SCE_THREADMAN_LOG_CANCEL_MSG_PIPE               (0x6A)
#define SCE_THREADMAN_LOG_REFER_MSG_PIPE_STATUS         (0x6B)
#define SCE_THREADMAN_LOG_CREATE_VPL                    (0x6C)
#define SCE_THREADMAN_LOG_DELETE_VPL                    (0x6D)
#define SCE_THREADMAN_LOG_ALLOCATE_VPL                  (0x6E)
#define SCE_THREADMAN_LOG_ALLOCATE_VPL_CB               (0x6F)
#define SCE_THREADMAN_LOG_TRY_ALLOCATE_VPL              (0x70)
#define SCE_THREADMAN_LOG_FREE_VPL                      (0x71)
#define SCE_THREADMAN_LOG_CANCEL_VPL                    (0x72)
#define SCE_THREADMAN_LOG_REFER_VPL_STATUS              (0x73)
#define SCE_THREADMAN_LOG_CREATE_FPL                    (0x74)
#define SCE_THREADMAN_LOG_DELETE_FPL                    (0x75)
#define SCE_THREADMAN_LOG_ALLOCATE_FPL                  (0x76)
#define SCE_THREADMAN_LOG_ALLOCATE_FPL_CB               (0x77)
#define SCE_THREADMAN_LOG_TRY_ALLOCATE_FPL              (0x78)
#define SCE_THREADMAN_LOG_FREE_FPL                      (0x79)
#define SCE_THREADMAN_LOG_CANCEL_FPL                    (0x7A)
#define SCE_THREADMAN_LOG_REFER_FPL_STATUS              (0x7B)
#define SCE_THREADMAN_LOG_CREATE_TLSPL                  (0x7C)
#define SCE_THREADMAN_LOG_DELETE_TLSPL                  (0x7D)
#define SCE_THREADMAN_LOG_ALLOCATE_TLSPL                (0x7E)
#define SCE_THREADMAN_LOG_FREE_TLSPL                    (0x81)
#define SCE_THREADMAN_LOG_REFER_TLSPL_STATUS            (0x82)
#define SCE_THREADMAN_LOG_GET_SYSTEM_TIME               (0x83)
#define SCE_THREADMAN_LOG_GET_SYSTEM_TIME_WIDE          (0x84)
#define SCE_THREADMAN_LOG_SET_ALARM                     (0x8A)
#define SCE_THREADMAN_LOG_SET_SYS_CLOCK_ALARM           (0x8B)
#define SCE_THREADMAN_LOG_CANCEL_ALARM                  (0x8C)
#define SCE_THREADMAN_LOG_REFER_ALARM_STATUS            (0x8D)
#define SCE_THREADMAN_LOG_CREATE_VTIMER                 (0x8E)
#define SCE_THREADMAN_LOG_DELETE_VTIMER                 (0x8F)
#define SCE_THREADMAN_LOG_GET_VTIMER_BASE               (0x90)
#define SCE_THREADMAN_LOG_GET_VTIMER_TIME               (0x91)
#define SCE_THREADMAN_LOG_SET_VTIMER_TIME               (0x92)
#define SCE_THREADMAN_LOG_START_VTIMER                  (0x93)
#define SCE_THREADMAN_LOG_STOP_VTIMER                   (0x94)
#define SCE_THREADMAN_LOG_SET_VTIMER_HANDLER            (0x95)
#define SCE_THREADMAN_LOG_CANCEL_VTIMER_HANDLER         (0x96)
#define SCE_THREADMAN_LOG_GET_VTIMER_BASE_WIDE          (0x97)
#define SCE_THREADMAN_LOG_GET_VTIMER_TIME_WIDE          (0x98)
#define SCE_THREADMAN_LOG_SET_VTIMER_TIME_WIDE          (0x99)
#define SCE_THREADMAN_LOG_SET_VTIMER_HANDLER_WIDE       (0x9A)
#define SCE_THREADMAN_LOG_REFER_VTIMER_STATUS           (0x9B)
/** A free-form debug message (the first logged argument is a printf format string). */
#define SCE_THREADMAN_LOG_MESSAGE                       (0x00)
/** A thread switch (logged by the dispatcher). */
#define SCE_THREADMAN_LOG_THREAD_SWITCH                 (0x01)
#define SCE_THREADMAN_LOG_DELAY_THREAD                  (0x86)
#define SCE_THREADMAN_LOG_DELAY_THREAD_CB               (0x87)
#define SCE_THREADMAN_LOG_DELAY_SYS_CLOCK_THREAD        (0x88)
#define SCE_THREADMAN_LOG_DELAY_SYS_CLOCK_THREAD_CB     (0x89)
#define SCE_THREADMAN_LOG_CREATE_CALLBACK           (0x9C)
#define SCE_THREADMAN_LOG_DELETE_CALLBACK           (0x9D)
#define SCE_THREADMAN_LOG_NOTIFY_CALLBACK           (0x9E)
#define SCE_THREADMAN_LOG_CANCEL_CALLBACK           (0x9F)
#define SCE_THREADMAN_LOG_GET_CALLBACK_COUNT        (0xA0)
#define SCE_THREADMAN_LOG_GET_CALLBACK_DEPTH        (0xA1)
#define SCE_THREADMAN_LOG_CHECK_CALLBACK            (0xA2)
#define SCE_THREADMAN_LOG_REFER_CALLBACK_STATUS     (0xA3)
#define SCE_THREADMAN_LOG_REFER_SYSTEM_STATUS       (0xA4)
#define SCE_THREADMAN_LOG_GET_THREADMAN_ID_LIST     (0xA5)
#define SCE_THREADMAN_LOG_GET_THREADMAN_ID_TYPE     (0xA6)
#define SCE_THREADMAN_LOG_ALLOCATE_KTLS             (0xA7)
#define SCE_THREADMAN_LOG_FREE_KTLS                 (0xA8)
#define SCE_THREADMAN_LOG_GET_THREAD_KTLS           (0xAA)

/**
 * The debug log handler. Every API function reports its arguments to it when it is set.
 *
 * @param eventId The event (one of SCE_THREADMAN_LOG_*).
 * @param fp The caller's $fp register (not a meaningful value: the compiler uses $fp as a general register).
 * @param numArgs The number of arguments following.
 */
typedef void (*SceThreadmanLogHandler)(s32 eventId, void *fp, s32 numArgs, ...);

/** The number of UIDs held by a SceThreadmanDeadThreads block. */
#define SCE_THREADMAN_NUM_DEAD_THREADS  (8)

/** A block of dead threads waiting to be deleted by the cleaner thread. */
typedef struct SceThreadmanDeadThreads {
    /** The next block (allocated from the threadman heap). */
    struct SceThreadmanDeadThreads *next; // 0
    /** The UIDs of the dead threads. */
    SceUID uids[SCE_THREADMAN_NUM_DEAD_THREADS]; // 4
    /** The number of UIDs in uids. */
    s32 count; // 36
} SceThreadmanDeadThreads; // size: 40

/**
 * The first word of the interrupt manager's state (intInfo, a variable imported from InterruptManagerForKernel).
 * Read by the dispatcher: if not NULL, it receives the $k0 register value of the next thread.
 */
extern u32 *intInfo;
#define THREADMAN_K0_HOOK           (intInfo)

/** The number of thread priorities (and of ready queues). */
#define SCE_THREADMAN_NUM_PRIORITIES    (128)

/** The global state of the thread manager (exported as ThreadManForKernel_921B7ACD). */
typedef struct {
    /** The currently running thread. */
    SceThreadmanThread *curThread; // 0
    /** The thread which will run next (NULL to force a reschedule). */
    SceThreadmanThread *nextThread; // 4
    /** The debug log handler. */
    SceThreadmanLogHandler logHandler; // 8
    /** Non-zero while thread dispatching is suspended. */
    s32 dispatchSuspended; // 12
    /** The number of thread switches. */
    u32 threadSwitchCount; // 16
    /** The number of dispatches which kept the current thread running. */
    u32 noSwitchCount; // 20
    /** The number of VFPU context switches. */
    u32 vfpuSwitchCount; // 24
    /** The VFPU context currently loaded in the VFPU (NULL if none). */
    SceThreadmanVfpuContext *vfpuOwner; // 28
    /** The mode the VFPU access was set up for (1: user mode, 0: kernel mode). */
    u32 vfpuUserMode; // 32
    /** The number of free delay timers in freeDelays. */
    s32 numFreeDelays; // 36
    /** The free delay timers (SceThreadmanTimer.link), kept for reuse (at most SCE_THREADMAN_MAX_FREE_DELAYS). */
    SceThreadmanListEntry freeDelays; // 40
    /** When set, threads with SCE_THREADMAN_THREAD_ATTR_USER or _UNK_08000000 are skipped when waking waiters. */
    s32 skipUserThreads; // 48
    /** Set when the cleaner thread must run to delete the threads of deadThreads. */
    s32 cleanupRequested; // 52
    /** The dead threads waiting to be deleted by the cleaner thread. */
    SceThreadmanDeadThreads deadThreads; // 56
    /** The UID of the alarm or virtual timer whose handler is running (0 if none). */
    SceUID curTimerUid; // 96
    /** The next timer to be processed by the system timer interrupt handler (NULL outside of it). */
    SceThreadmanListEntry *nextTimer; // 100
    /** The active timers (SceThreadmanTimer.link), sorted by schedule. */
    SceThreadmanListEntry timers; // 104
    /** The system time of the last thread switch. */
    SceKernelSysClock lastSwitchClock; // 112
    /** The 64-bit system time at the last system timer overflow (its upper 32 bits). */
    /* PARITY-Q024: volatile: _sceKernelGetSystemTime64 loads it in source order as the original */
    volatile SceKernelSysClock sysTimeBase; // 120 (volatile: updated by the system timer interrupt; read without interrupts disabled in _sceKernelGetSystemTime64)
    /** The value programmed in the compare register of the system timer. */
    /* PARITY-Q025: volatile: the original's lw/sw/lw/sw copy in _sceKernelSysTimerIntr */
    volatile SceKernelSysClock nextCompare; // 128
    /** Bitmap of the non-empty ready queues. */
    u32 readyBitmap[4]; // 136
    /** The ready queues, one per priority. */
    SceThreadmanListEntry readyQueue[SCE_THREADMAN_NUM_PRIORITIES]; // 152
    /** The sleeping threads. */
    SceThreadmanListEntry sleepingThreads; // 1176
    /** The delayed threads. */
    SceThreadmanListEntry delayedThreads; // 1184
    /** The dormant threads (created but not started, or exited). */
    SceThreadmanListEntry dormantThreads; // 1192
    /** The suspended threads (whose wait ended while they were suspended). */
    SceThreadmanListEntry suspendedThreads; // 1200
    /** The threads killed by sub_0001752C() (status SCE_THREADMAN_STATUS_DEAD). */
    SceThreadmanListEntry deadThreadList; // 1208
    /** The registered thread event handlers (SceThreadmanThreadEventHandler.link). */
    SceThreadmanListEntry threadEventHandlers; // 1216
    /** The "WaitQ" UID type. */
    SceSysmemUidCB *waitQType; // 1224
    /** The "Thread" UID type. */
    SceSysmemUidCB *threadType; // 1228
    /** The "Semaphore" UID type. */
    SceSysmemUidCB *semaType; // 1232
    /** The "EventFlag" UID type. */
    SceSysmemUidCB *eventFlagType; // 1236
    /** The "Mutex" UID type. */
    SceSysmemUidCB *mutexType; // 1240
    /** The "LwMutex" UID type. */
    SceSysmemUidCB *lwMutexType; // 1244
    /** The "Mbx" UID type. */
    SceSysmemUidCB *mbxType; // 1248
    /** The "MsgPipe" UID type. */
    SceSysmemUidCB *msgPipeType; // 1252
    /** The "Vpl" UID type. */
    SceSysmemUidCB *vplType; // 1256
    /** The "Fpl" UID type. */
    SceSysmemUidCB *fplType; // 1260
    /** The "Tlspl" UID type. */
    SceSysmemUidCB *tlsplType; // 1264
    /** The "ThreadEventHandler" UID type. */
    SceSysmemUidCB *threadEventHandlerType; // 1268
    /** The "Callback" UID type. */
    SceSysmemUidCB *callbackType; // 1272
    /*
     * PARITY-Q159: Integer field (cast to SceSysmemUidCB * at each use) instead of a pointer: its loads share the s32
     *     alias set, so the scheduler keeps them after int stores to g_threadman (sub_00010FAC: after the
     *     numFreeDelays decrement), as in the original.
     *     USER-APPROVED TYPE-PUN EXCEPTION (19:03 PT 2026-10-27): kept although it is a pointer held in an integer.
     *     Why: under -fstrict-aliasing a pointer-typed field gets its own alias set, and sched2 then hoists the
     *     free-list path's timerType load above the s32 numFreeDelays store; only the s32 alias set gives the original
     *     load order in sub_00010FAC (exact with it, sched 0.988 without it). See threadman-deviations.md
     *     "Approved exceptions".
     */
    /** The "Timer" UID type (the base type of the delay timers, the alarms and the virtual timers). */
    s32 timerType; // 1276
    /** The "Delay" UID type (internal timers, used by timeouts and thread delays). */
    SceSysmemUidCB *delayType; // 1280
    /** The "Alarm" UID type. */
    SceSysmemUidCB *alarmType; // 1284
    /** The "VTimer" UID type. */
    SceSysmemUidCB *vtimerType; // 1288
    /** The idle thread ("SceThreadmanIdle"). */
    SceThreadmanThread *idleThread; // 1292
    /** The thread deleting the dead threads ("SceThreadmanCleaner"). */
    SceThreadmanThread *cleanerThread; // 1296
    /** The kernel thread local storage allocations. */
    SceThreadmanKtls ktls[SCE_THREADMAN_NUM_KTLS]; // 1300
    /** The UIDs of the TLS pools, indexed by their slot. */
    SceUID tlsplUids[SCE_THREADMAN_NUM_TLSPL]; // 1684
    /** Buffer the Refer*Status functions build their result in before copying it to the caller. */
    union {
        SceKernelSemaInfo semaInfo;
        SceKernelEventFlagInfo eventFlagInfo;
        SceKernelMutexOptParam mutexOption;
        SceKernelMutexInfo mutexInfo;
        SceKernelLwMutexOptParam lwMutexOption;
        SceKernelLwMutexInfo lwMutexInfo;
        SceKernelMbxInfo mbxInfo;
        SceKernelMppInfo msgPipeInfo;
        SceKernelVplInfo vplInfo;
        SceKernelFplOptParam fplOption;
        SceKernelFplInfo fplInfo;
        SceKernelTlsplOptParam tlsplOption;
        SceKernelTlsplInfo tlsplInfo;
        SceKernelThreadEventHandlerInfo threadEventHandlerInfo;
        SceKernelCallbackInfo callbackInfo;
        SceKernelAlarmInfo alarmInfo;
        SceKernelVTimerInfo vtimerInfo;
        SceKernelSystemStatus systemStatus;
        struct {
            SceSysmemPartitionInfo partInfo; // 0
            SceSysmemHeapAllocOption allocOpt; // 16
            SceKernelThreadOptParam option; // 24
            char name[32]; // 32
        } createThread;
        SceKernelThreadInfo threadInfo;
        SceKernelThreadRunStatus threadRunStatus;
        SceKernelThreadKInfo threadKInfo;
        /** The name of the memory block of a stack extension. */
        char stackExtName[32];
        u8 data[244];
    } statusBuf; // 1748
    /** The content of the COP0 state register 22 at boot time. */
    s32 cop0State22; // 1992
    /** The interrupt number of the system timer. */
    s32 sysTimerIntr; // 1996
    /** The system status event flag. */
    SceUID systemStatusFlag; // 2000
    /** The syscall code of _sceKernelExitThread(). */
    u32 exitThreadSyscall; // 2004
    /** The syscall code of _sceKernelReturnFromChangeStack(). */
    u32 returnFromChangeStackSyscall; // 2008
    /** The syscall code of _sceKernelReturnFromCallback() (presumably). */
    u32 returnFromCallbackSyscall; // 2012
    /** The internal heap of the threadman. */
    SceUID heapId; // 2016
    /** The CPU profiler mode (SCE_THREADMAN_PROFILER_MODE_*). */
    s32 profilerMode; // 2020
    /** The minimal delay between two runs of a periodic timer (100 microseconds). */
    u32 minTimerDelay; // 2024
    /** The minimal distance between now and the next compare value of the system timer. */
    u32 minCompareDelay; // 2028
    /** Set to 1 by the timer initialization (unknown use). */
    u32 unk2032; // 2032
    /** Set to 1 by the timer initialization (unknown use). */
    u32 unk2036; // 2036
    /** The handler registered by sceKernelRegisterDeadThreadHandler(). */
    SceThreadmanDeadThreadHandler deadThreadHandler; // 2040
    /** sceKernelGetSystemTimeLow(). */
    unsigned int (*getSystemTimeLow)(void); // 2044
    /** Get the 64-bit system time. */
    u64 (*getSystemTime)(void); // 2048
} SceThreadman; // size: 2052

/* PARITY-Q016: see threadman.c: the aligned declaration is for threadman.c only */
#ifdef THREADMAN_C
/* threadman.c defines it on a data cache line (64 bytes); declared here with the same alignment for its inlines. */
extern SceThreadman g_threadman __attribute__((aligned(64)));
#else
extern SceThreadman g_threadman;
#endif

/* Interrupt context check (the threadman reads the COP0 control register directly). */
#define THREADMAN_IS_INTR_CONTEXT() (pspCop0CtrlGet(COP0_CTRL_IS_INTERRUPT) != 0)

/*
 * PARITY-Q143: a static inline rather than a macro (sceKernelCreateMutex, its only user). The inline copies
 * priority into its own pseudo, which changes the allocation ranking: $fp then holds name (a0) and the s-registers
 * follow the original order (equiv -> sched, 156 -> 155 instructions). Residue: two load/shift pairs swapped. Plain C.
 */
/** Check whether prio is a valid thread priority for the caller (8 - 119 in user mode, 1 - 126 in kernel mode). */
static inline s32 THREADMAN_IS_VALID_PRIORITY(s32 prio)
{
    return pspK1IsUserMode() ? ((u32)(prio - 8) < 0x70) : ((u32)(prio - 1) < 0x7E);
}

/**
 * Call the debug log handler, if set. The original lays out every one of these calls out of line (the call is
 * branched to and jumps back). GCC 3.3 predicts this from the plain check already (its RTL heuristics), so no
 * __builtin_expect: with it, the layout is the same but the call blocks get a lower frequency, which changes
 * the register allocation of the values passed to the handler.
 */
/*
 * PARITY-Q029: log hook passes __builtin_frame_address(0) (the original's `move $a1,$fp`); handler check expected
 * to fail, calls out of line
 */
#define THREADMAN_LOG(eventId, numArgs, ...) \
    if (g_threadman.logHandler != NULL) \
        g_threadman.logHandler(eventId, __builtin_frame_address(0), numArgs, ##__VA_ARGS__)


/*
 * PARITY-Q042 (dae0923): Statement/store order taken from the original's store order.
 */
/** Add a thread at the end of the ready queue of its priority (see sub_000127D0()). */
static inline void threadmanReadyQueueAdd(SceThreadmanThread *thread)
{
    s32 prio;

    prio = thread->currentPriority;
    g_threadman.readyBitmap[THREADMAN_READY_INDEX(prio)] |= THREADMAN_READY_BIT(prio);
    threadmanListAddTail(&g_threadman.readyQueue[prio], &thread->link);
}

/**
 * Add a thread at the head of the ready queue of its current priority (a running thread which is
 * preempted keeps its turn): it is inserted before the first entry of the queue.
 */
static inline void threadmanReadyQueueAddHead(SceThreadmanThread *thread)
{
    s32 prio;

    prio = thread->currentPriority;
    g_threadman.readyBitmap[THREADMAN_READY_INDEX(prio)] |= THREADMAN_READY_BIT(prio);
    threadmanListAddTail(g_threadman.readyQueue[prio].next, &thread->link);
}

/** Get the highest priority (the lowest value) of the non-empty ready queues (128 if they are all empty). */
static inline s32 threadmanReadyQueueGetHighestPriority(void)
{
    s32 prio;
    s32 clz;

    /* Find the first bit set of the ready bitmap. */
    prio = pspClz(g_threadman.readyBitmap[3]) + 32;
    clz = pspClz(g_threadman.readyBitmap[2]);
    if (g_threadman.readyBitmap[2] == 0)
        clz = prio;
    prio = clz + 32;
    clz = pspClz(g_threadman.readyBitmap[1]);
    if (g_threadman.readyBitmap[1] == 0)
        clz = prio;
    prio = clz + 32;
    clz = pspClz(g_threadman.readyBitmap[0]);
    if (g_threadman.readyBitmap[0] == 0)
        clz = prio;
    return clz;
}

/** Remove a thread from the ready queue of a priority, updating the ready bitmap (see sub_00016BF4()). */
static inline void threadmanReadyQueueRemove(SceThreadmanThread *thread, s32 priority)
{
    if (thread->link.next == thread->link.prev && thread->link.next != &thread->link) {
        /* The thread is the last one of the queue. */
        g_threadman.readyBitmap[THREADMAN_READY_INDEX(priority)] &= ~THREADMAN_READY_BIT(priority);
    }
    threadmanListRemove(&thread->link);
}

/*
 * PARITY-Q089: threadmanProfilerPause()/threadmanProfilerResume() jump to the profiler code with a goto after the
 * no-profiler return: the original places both profiler blocks of sub_00000FE4 out of line, after the return.
 */
/** Pause the hardware profiler, if one is in use (it must not count the thread switch). */
static inline void threadmanProfilerPause(void)
{
    SceThreadmanProfiler *profiler;

    profiler = (SceThreadmanProfiler *)threadmanCop0CtrlGet(COP0_CTRL_PROFILER_BASE);
    if (profiler != NULL)
        goto pause;
    return;

pause:
    if (++profiler->pauseCount == 1) {
        profiler->savedEnable = *profiler->enable;
        *profiler->enable = 0;
        pspSync();
    }
}

/** Resume the hardware profiler paused by threadmanProfilerPause(). */
static inline void threadmanProfilerResume(void)
{
    SceThreadmanProfiler *profiler;

    profiler = (SceThreadmanProfiler *)threadmanCop0CtrlGet(COP0_CTRL_PROFILER_BASE);
    if (profiler != NULL)
        goto resume;
    return;

resume:
    if (profiler->pauseCount != 0 && --profiler->pauseCount == 0) {
        *profiler->enable = profiler->savedEnable;
        pspSync();
    }
}

/*
 * Check a buffer of dynamic size against $k1. Same test as pspK1DynBufOk() (common/registers.h), with $k1
 * as the first operand of the AND as in the original: all of threadman's dynamic-size buffer checks
 * (sceKernelStartThread, sceKernelGetThreadmanIdList, ThreadManForUser_28BFD974, the MsgPipe senders and
 * receivers) do `and rd, k1, mask`, while the static-size checks do `and rd, mask, k1`.
 */
/* PARITY-Q017: k1 & mask operand order of all 7 dynamic-size buffer checks of the original */
static inline int threadmanK1DynBufOk(const void *ptr, int size)
{
    return ((pspGetK1() & (((int)ptr + size) | (int)ptr | size)) >= 0);
}

/** Check whether the calling application was compiled with an SDK newer than version. */
/* PARITY-Q035: a macro with a signed compare: the original's slt, constant not held across the call */
#define threadmanSdkVersionAbove(version) ((s32)sceKernelGetCompiledSdkVersion() > (s32)(version))

/*
 * InterruptManagerForKernel thread context functions (not declared in interruptman.h, whose
 * own prototypes in the interruptman sources differ).
 */

/**
 * Save the callee-saved registers and the CPU state of the current thread to a thread context.
 *
 * @param context The thread context.
 *
 * @return 0 when the context has been saved, non-zero when execution resumes from the saved context.
 */
s32 SaveThreadContext(void *context);

/**
 * Load a thread context and resume its execution (does not return to the caller).
 *
 * @param context The thread context.
 */
void InterruptManagerForKernel_551097F2(void *context);

/* wait queue */
s32 sub_00000680(SceSysmemUidCB *uid, s32 waitType, u32 waitAttr, SceKernelSysClock *timeoutClock,
    SceUInt *timeout, s32 *waitCount);
void sub_00000858(SceThreadmanListEntry *waitList, s32 waitType);
void sub_00000890(SceThreadmanThread *thread, SceSysmemUidCB *uid);
s32 sub_00000908(SceSysmemUidCB *uid, s32 waitType, u32 waitAttr, SceKernelSysClock *timeoutClock,
    SceUInt *timeout);
s32 sub_00000924(SceThreadmanThread *thread);
void sub_00000A08(SceThreadmanThread *thread, s32 dispatch, s32 intr);
void sub_00000B08(SceSysmemUidCB *uid, s32 waitStatus, s32 dispatch, s32 intr);
s32 sub_00000B48(SceSysmemUidCB *uid, s32 waitStatus);
void sub_00000BC4(SceThreadmanThread *thread);
s32 sub_00000C30(SceSysmemUidCB *uid);
s32 sub_00000C50(void);
void sub_00000FE4(s32 release, s32 intr);
s32 sub_00001100(void);

/* thread event handler */
extern SceSysmemUidLookupFunc g_ThreadEventHandlerFuncs[];
s32 sub_00001580(u32 event, SceThreadmanThread *thread, s32 intr);
void sub_00002570(SceThreadmanThread *thread);

s32 sub_000025FC(u32 event, SceThreadmanThread *thread);

/* callback */
s32 sub_00002170(s32 intr);
void sub_000026CC(SceThreadmanThread *thread);
void sub_000027B4(SceThreadmanThread *thread);
s32 _sceKernelCallbackInit(void);
s32 sub_000028C0(void);
s32 sub_00002944(s32 userMode, void *func, u32 arg0, u32 arg1, u32 arg2, u32 arg3, u32 gp);
void sub_00002DBC(void);

/* semaphore */
s32 _sceKernelSemaInit(void);

/* event flag */
void _sceKernelEventFlagReleaseOwned(SceThreadmanThread *thread);
s32 _sceKernelEventFlagInit(void);

/* mutex */
void _sceKernelMutexReleaseOwned(SceThreadmanThread *thread);
s32 _sceKernelMutexInit(void);

/* lightweight mutex */
s32 _sceKernelLwMutexInit(void);

/* message boxes */
s32 _sceKernelMbxInit(void);

/* message pipes */
s32 _sceKernelMsgPipeInit(void);
SceUID _sceKernelGetMsgPipeByBlockId(SceUID blockId);

/* variable-length memory pools */
s32 _sceKernelVplInit(void);
SceUID _sceKernelGetVplByHeapId(SceUID heapId);

/* fixed-length memory pools */
s32 _sceKernelFplInit(void);
SceUID _sceKernelGetFplByBlockId(SceUID blockId);

/* TLS pools */
s32 _sceKernelTlsplFreeBlock(SceThreadmanTlspl *tlspl, s32 index);
s32 _sceKernelTlsplAllocBlock(SceThreadmanTlspl *tlspl);
s32 _sceKernelReleaseThreadTls(SceThreadmanThread *thread);
SceUID _sceKernelGetTlsplByBlockId(SceUID blockId);
s32 _sceKernelTlsplInit(void);

/* timers (timer.c and timer_handler.c) */
/** The counter of the system timer (the lower 32 bits of the system time). */
#define SYSTIMER_COUNTER    (0xBC600000)
/** The compare register of the system timer (an interrupt is raised when the counter reaches it). */
#define SYSTIMER_COMPARE    (0xBC600004)
#define SYSTIMER_UNK8       (0xBC600008)
#define SYSTIMER_UNKC       (0xBC60000C)
#define SYSTIMER_UNK10      (0xBC600010)

/** The period of the system timer overflow timer, in microseconds. */
#define SYSTIMER_OVF_PERIOD (0xF0000000)

/** The syscall / suspend / resume handler slot of the system timer. */
#define SYSTIMER_HANDLER_ID (0x18)

/*
 * PARITY-Q054 (4f442e8): Comparison operand order as the original's branch operands.
 */
/** Remove the active timer timer from the timer list (keeping the interrupt handler's iterator valid). */
static inline void _sceKernelRemoveTimer(SceThreadmanTimer *timer)
{
    if (&timer->link == g_threadman.nextTimer)
        g_threadman.nextTimer = timer->link.next;
    threadmanListRemove(&timer->link);
    timer->link.next = &timer->link;
    timer->link.prev = &timer->link;
}

void _sceKernelInsertTimer(SceThreadmanTimer *timer);
void _sceKernelUpdateSysTimerCompare(void);
s32 _sceKernelSysTimerSuspend(s32 unk, void *param);
s32 _sceKernelSysTimerResume(s32 unk, void *param);
void _sceKernelSysClockDivide(SceKernelSysClock *quot, SceKernelSysClock *clock, u32 div, unsigned int *rem);
SceUInt _sceKernelCallAlarmHandler(SceSysmemUidCB *uid, s32 intr);
SceUInt _sceKernelCallVTimerHandler(SceSysmemUidCB *uid, s32 intr);
SceUID _sceKernelSetAlarm(u64 clock, SceKernelAlarmHandler handler, void *common);
s32 _sceKernelDelayInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelDelayDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelAlarmInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelAlarmDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelVTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelVTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelTimerInitialize(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);
s32 _sceKernelTimerDelete(SceSysmemUidCB *uid, SceSysmemUidCB *uidWithFunc, s32 funcId, va_list ap);

/* alarm / system time */
void _sceKernelSysTimerInit(void);
s32 _sceKernelTimerInit(void);
void sub_00010D34(SceKernelSysClock *clock, u32 usec);
void sub_00010D8C(SceKernelSysClock *clock, SceKernelSysClock *delay);
SceUID sub_00010FAC(SceKernelSysClock *clock, SceThreadmanTimerHandler handler, void *common);
s32 sub_00011104(SceUID alarmId, SceUInt *remaining);

/* thread */
void sub_00012C04(SceThreadmanThread *thread);
s32 sub_000126AC(SceUID uid);
s32 _sceKernelFreeThreadKTLS(SceThreadmanThread *thread);
void sub_000134A8(SceThreadmanContext *context);
s32 sub_00014124(s32 resume, s32 isIntr, s32 intr);
void sub_000127D0(SceThreadmanThread *thread);
SceThreadmanContext *sub_00012828(SceThreadmanContext *context, s32 arg);
s32 sub_00014598(SceThreadmanThread *thread, s32 priority, s32 intr);
void sub_00016BF4(SceThreadmanThread *thread, s32 priority);
void sub_00016C48(SceThreadmanThread *thread, s32 intr);
s32 sub_00016FA4(void);
s32 ThreadManForKernel_D366D35A(s32 disable);
void sub_0001752C(SceSysmemUidCB *uid);
void sub_000177C4(SceThreadmanThread *thread, s32 kernelStack, u32 sp);
void sub_000180AC(SceSysmemUidCB *cb, SceKernelThreadKInfo *info, s32 isIntr);
s32 sub_00017EB4(SceUID blockId, void *stackLow, SceSize stackSize, s32 (*func)(void *), void *argp);
void sub_00017844(SceThreadmanThread *thread);

/*
 * PARITY-Q065 (35fb533): Guard and arm shape taken from the original's layout (wait arm first, positive guards, addr
 *     != NULL block).
 */
/**
 * Make a thread ready, switching to it if it has a higher priority than the current thread (the body
 * of sub_00016C48(), which sceKernelStartThread() has inlined).
 */
/* PARITY-Q032: inline ready/preempt logic: sceKernelStartThread has sub_00016C48's body inlined in the original */
static inline void threadmanReadyThread(SceThreadmanThread *thread, s32 intr)
{
    s32 prio;
    SceThreadmanThread *curThread;

    curThread = g_threadman.curThread;
    if (thread->currentPriority < curThread->currentPriority && g_threadman.dispatchSuspended == 0) {
        prio = curThread->currentPriority;
        if (intr == 0) {
            thread->status = SCE_THREADMAN_STATUS_READY;
            sub_000127D0(thread);
            g_threadman.nextThread = NULL;
        } else {
            /* Preempt the current thread: put it back at the head of its ready queue. */
            threadmanListAddTail(g_threadman.readyQueue[prio].next, &curThread->link);
            g_threadman.readyBitmap[THREADMAN_READY_INDEX(prio)] |= THREADMAN_READY_BIT(prio);
            g_threadman.nextThread = thread;
            curThread->status = SCE_THREADMAN_STATUS_READY;
            thread->status = SCE_THREADMAN_STATUS_RUNNING;
        }
        sub_00000FE4(0, intr);
        return;
    }
    thread->status = SCE_THREADMAN_STATUS_READY;
    sub_000127D0(thread);
}
void sub_000183EC(SceKernelSystemStatus *status, s32 intr);
s32 sub_00016B40(void);
void sub_00000338(void);
void sub_000002C8(void);
void sub_00000080(SceThreadmanVfpuContext *vfpuContext);
void sub_000001B8(SceThreadmanVfpuContext *vfpuContext);
void sub_000005A4(SceThreadmanContext *context);
s32 _sceKernelThreadmanIntrHandler(void);
s32 _sceKernelCleanerThreadEntry(SceSize args, void *argp);
void sub_000155B8(SceThreadmanThread *thread, s32 intr);
void sub_000172F8(void);
void sub_00013E80(void);
s32 sub_000172A8(void *data, s32 arg, void *opt);
void sub_000172DC(void);
void sub_00000000(void);
void sub_000172C8(void);
s32 _sceKernelExitThread(void);
s32 _sceKernelReturnFromChangeStack(void);
s32 _sceKernelReturnFromCallback(void);
u64 _sceKernelGetSystemTime64(void);
void sub_000156BC(SceThreadmanThread *thread);
s32 sub_00017A14(SceSysmemUidCB *uid, s32 deleteThread, s32 intr);
void sub_000178C0(s32 deleteThread, s32 intr, s32 exitStatus) __attribute__((noreturn));

#endif /* THREADMAN_INT_H */
