/* Copyright (C) 2011 - 2015 The uOFW team
   See the file COPYING for copying permission.
*/

#ifndef THREADMAN_KERNEL_H
#define	THREADMAN_KERNEL_H

#include "common_header.h"
#include "threadman_user.h"

/* Threads */

typedef s32 (*SceKernelThreadEntry)(SceSize args, void *argp);

typedef s32 (*SceKernelRebootKernelThreadEntry)(s32 arg1, u32 arg2, s32 arg3, s32 arg4);

typedef struct {
    SceSize     size;
    SceUID      stackMpid;
} SceKernelThreadOptParam;

#define SCE_KERNEL_THREAD_ID_SELF               (0) /* UID representing calling thread. */

/** 
 * thread priority - lower numbers mean higher priority 
 */
#define SCE_KERNEL_INVALID_PRIORITY             (0)
#define SCE_KERNEL_HIGHEST_PRIORITY_KERNEL      (1)
#define SCE_KERNEL_HIGHEST_PRIORITY_USER        (16)
#define SCE_KERNEL_MODULE_INIT_PRIORITY         (32)
#define SCE_KERNEL_LOWEST_PRIORITY_USER         (111)
#define SCE_KERNEL_LOWEST_PRIORITY_KERNEL       (126)

/* thread size */
#define SCE_KERNEL_TH_KERNEL_DEFAULT_STACKSIZE  (4 * 1024) /* 4 KB */
#define SCE_KERNEL_TH_USER_DEFAULT_STACKSIZE    (256 * 1024) /* 256 KB */

/* thread attributes */
#define SCE_KERNEL_TH_VSH_MODE                  (0xC0000000) /* Thread runs in VSH mode. */
#define SCE_KERNEL_TH_APP_MODE                  (0xB0000000) /* Thread runs in Application mode. */
#define SCE_KERNEL_TH_USB_WLAN_MODE             (0xA0000000) /* Thread runs in USB_WLAN mode. */
#define SCE_KERNEL_TH_MS_MODE                   (0x90000000) /* Thread runs in MS mode. */
#define SCE_KERNEL_TH_USER_MODE                 (0x80000000) /* Thread runs in User mode. */
#define SCE_KERNEL_TH_NO_FILLSTACK              (0x00100000)
#define SCE_KERNEL_TH_CLEAR_STACK               (0x00200000) /* Specifies that thread memory area should be cleared to 0 when deleted. */
#define SCE_KERNEL_TH_LOW_STACK                 (0x00400000) /* Specifies that the stack area is allocated from the lower addresses in memory, not the higher ones. */
#define SCE_KERNEL_TH_UNK_800000                (0x00800000)
#define SCE_KERNEL_TH_USE_VFPU                  (0x00004000) /* Specifies that the VFPU is available. */
#define SCE_KERNEL_TH_NEVERUSE_FPU              (0x00002000)

#define SCE_KERNEL_TH_DEFAULT_ATTR              (0)

#define SCE_KERNEL_AT_THFIFO                    (0x00000000) /* Waiting threads are queued on a FIFO basis. */
#define SCE_KERNEL_AT_THPRI                     (0x00000100) /* Waiting threads are queued based on priority. */

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, s32 initPriority,
                             SceSize stackSize, SceUInt attr, SceKernelThreadOptParam *option);
s32 sceKernelDeleteThread(SceUID thid);
s32 sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelSuspendThread(SceUID thid);
s32 sceKernelExitThread(s32 status);
s32 sceKernelExitDeleteThread(s32 exitStatus);
s32 sceKernelTerminateDeleteThread(SceUID thid);
int sceKernelDelayThread(SceUInt delay);
s32 sceKernelChangeThreadPriority(SceUID thid, s32 priority);
s32 sceKernelGetThreadCurrentPriority(void);
s32 sceKernelGetThreadId(void);
s32 sceKernelIsUserModeThread(void);
int sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout);
int sceKernelWaitThreadEndCB(SceUID thid, SceUInt *timeout);
s32 sceKernelReleaseWaitThread(SceUID thid);
int sceKernelSuspendAllUserThreads(void);

s32 sceKernelExtendKernelStack(SceSize size, s32 (*func)(void *), void *arg);

unsigned int sceKernelGetSystemTimeLow(void);

enum SceUserLevel {
    SCE_USER_LEVEL_MS       = 1,
    SCE_USER_LEVEL_USBWLAN  = 2,
    SCE_USER_LEVEL_APP      = 3,
    SCE_USER_LEVEL_VSH      = 4,
};
s32 sceKernelGetUserLevel(void);

typedef enum {
    SCE_KERNEL_TMID_Thread = 1,
    SCE_KERNEL_TMID_Semaphore = 2,
    SCE_KERNEL_TMID_EventFlag = 3,
    SCE_KERNEL_TMID_Mbox = 4,
    SCE_KERNEL_TMID_Vpl = 5,
    SCE_KERNEL_TMID_Fpl = 6,
    SCE_KERNEL_TMID_Mpipe = 7,
    SCE_KERNEL_TMID_Callback = 8,
    SCE_KERNEL_TMID_ThreadEventHandler = 9,
    SCE_KERNEL_TMID_Alarm = 10,
    SCE_KERNEL_TMID_VTimer = 11,
    SCE_KERNEL_TMID_Mutex = 12,
    SCE_KERNEL_TMID_LwMutex = 13,
    SCE_KERNEL_TMID_Tlspl = 14,
    SCE_KERNEL_TMID_SleepThread = 64,
    SCE_KERNEL_TMID_DelayThread = 65,
    SCE_KERNEL_TMID_SuspendThread = 66,
    SCE_KERNEL_TMID_DormantThread = 67,
} SceKernelIdListType;

SceKernelIdListType sceKernelGetThreadmanIdType(SceUID uid);
s32 sceKernelGetThreadmanIdList(SceKernelIdListType type, SceUID *readbuf, s32 readbufsize, s32 *idcount);
SceUID sceKernelGetThreadmanIdOwner(SceUID blockId, SceKernelIdListType *type);

/* Thread event handlers */

/* Thread events (the mask passed to sceKernelRegisterThreadEventHandler()) */
#define SCE_KERNEL_THREAD_EVENT_CREATE  (0x1)
#define SCE_KERNEL_THREAD_EVENT_START   (0x2)
#define SCE_KERNEL_THREAD_EVENT_EXIT    (0x4)
#define SCE_KERNEL_THREAD_EVENT_DELETE  (0x8)
#define SCE_KERNEL_THREAD_EVENT_MASK    (0xF)

/* Special thread IDs which can be passed to sceKernelRegisterThreadEventHandler() */
/** The current thread (only for the SCE_KERNEL_THREAD_EVENT_EXIT event). */
#define SCE_KERNEL_THREAD_EVENT_ID_CURRENT  (0)
/** All the threads. */
#define SCE_KERNEL_THREAD_EVENT_ID_ALL      (-1)
/** All the kernel threads. */
#define SCE_KERNEL_THREAD_EVENT_ID_KERN     (-8)
/** All the user threads. */
#define SCE_KERNEL_THREAD_EVENT_ID_USER     (-16)

/**
 * A thread event handler.
 *
 * @param mask The event (one of SCE_KERNEL_THREAD_EVENT_*).
 * @param thid The thread the event occurred on.
 * @param common The common pointer passed to sceKernelRegisterThreadEventHandler().
 *
 * @return Non-zero to release the handler, 0 to keep it.
 */
typedef s32 (*SceKernelThreadEventHandler)(s32 mask, SceUID thid, void *common);

typedef struct {
    SceSize     size;
    char        name[SCE_UID_NAME_LEN + 1];
    SceUID      threadId;
    s32         mask;
    SceKernelThreadEventHandler handler;
    void        *common;
} SceKernelThreadEventHandlerInfo;

SceUID sceKernelRegisterThreadEventHandler(const char *name, SceUID threadID, s32 mask,
    SceKernelThreadEventHandler handler, void *common);
s32 sceKernelReleaseThreadEventHandler(SceUID uid);
s32 sceKernelReferThreadEventHandlerStatus(SceUID uid, SceKernelThreadEventHandlerInfo *info);

/* Mutexes */

typedef struct {
    SceSize     size;
    /** The ceiling priority (0 for the priority of the creating thread), used with attribute 0x400. */
    s32         ceilingPriority;
} SceKernelMutexOptParam;

typedef struct {
    SceSize     size;
    char        name[SCE_UID_NAME_LEN + 1];
    SceUInt     attr;
    s32         initCount;
    s32         currentCount;
    SceUID      currentOwner;
    s32         numWaitThreads;
} SceKernelMutexInfo;

/* Mutex attributes */
#define SCE_KERNEL_MUTEX_ATTR_TH_FIFO       (SCE_KERNEL_AT_THFIFO)
#define SCE_KERNEL_MUTEX_ATTR_TH_PRI        (SCE_KERNEL_AT_THPRI)
#define SCE_KERNEL_MUTEX_ATTR_RECURSIVE     (0x0200) /*Allow recursive locks by threads that own the mutex. */

s32 sceKernelCreateMutex(char *name, s32 attr, s32 initCount, const SceKernelMutexOptParam *pOption);
s32 sceKernelDeleteMutex(SceUID mutexId);
s32 sceKernelLockMutex(SceUID mutexId, s32 lockCount, u32 *pTimeout);
s32 sceKernelLockMutexCB(SceUID mutexId, s32 lockCount, u32 *pTimeout);
s32 sceKernelTryLockMutex(SceUID mutexId, s32 lockCount);
s32 sceKernelUnlockMutex(SceUID mutexId, s32 unlockCount);
s32 sceKernelCancelMutex(SceUID mutexId, s32 newLockCount, s32 *pNumWaitThreads);
s32 sceKernelReferMutexStatus(SceUID mutexId, SceKernelMutexInfo *pInfo);

/* Event flags */

typedef struct {
    SceSize     size;
    char        name[SCE_UID_NAME_LEN + 1];
    SceUInt     attr;
    SceUInt     initPattern;
    SceUInt     currentPattern;
    int         numWaitThreads;
} SceKernelEventFlagInfo;

typedef struct {
    SceSize     size;
} SceKernelEventFlagOptParam;

/* Event flag attributes. */
#define SCE_KERNEL_EA_SINGLE            (0x0000)    /** Multiple thread waits are prohibited. */ 
#define SCE_KERNEL_EA_MULTI             (0x0200)    /** Multiple thread waits are permitted. */

/* Event flag wait modes. */
#define SCE_KERNEL_EW_AND               (0x00)      /** Wait for all bits in the bit pattern to be set. */
#define SCE_KERNEL_EW_OR                (0x01)      /** Wait for one or more bits in the bit pattern to be set. */
#define SCE_KERNEL_EW_CLEAR_ALL         (0x10)      /** Clear all bits after wait condition is satisfied. */
#define SCE_KERNEL_EW_CLEAR_PAT         (0x20)      /** Clear bits specified by bit pattern after wait condition is satisfied. */
#define SCE_KERNEL_EW_CLEAR             SCE_KERNEL_EW_CLEAR_ALL

SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits, SceKernelEventFlagOptParam *opt);
int sceKernelSetEventFlag(SceUID evid, u32 bits);
int sceKernelClearEventFlag(SceUID evid, u32 bits);
int sceKernelPollEventFlag(int evid, u32 bits, u32 wait, u32 *outBits);
int sceKernelWaitEventFlag(int evid, u32 bits, u32 wait, u32 *outBits, SceUInt *timeout);
int sceKernelWaitEventFlagCB(int evid, u32 bits, u32 wait, u32 *outBits, SceUInt *timeout);
int sceKernelCancelEventFlag(SceUID evid, SceUInt setpattern, s32 *numWaitThreads);
int sceKernelDeleteEventFlag(int evid);
int sceKernelReferEventFlagStatus(SceUID event, SceKernelEventFlagInfo *status);

/* MsgPipe */

typedef struct {
    SceSize size;
} SceKernelMppOptParam;

typedef struct {
    SceSize size;
    char    name[SCE_UID_NAME_LEN + 1];
    SceUInt attr;
    int     bufSize;
    int     freeSize;
    int     numSendWaitThreads;
    int     numReceiveWaitThreads;
} SceKernelMppInfo;

/** Message pipe attributes. */
#define SCE_KERNEL_MPA_THFIFO_S     (0x0000)    /** Sending threads wait in FIFO order. */
#define SCE_KERNEL_MPA_THPRI_S      (0x0100)    /** Sending threads wait in priority order. */
#define SCE_KERNEL_MPA_THFIFO_R     (0x0000)    /** Receiving threads wait in FIFO order. */
#define SCE_KERNEL_MPA_THPRI_R      (0x1000)    /** Receiving threads wait in priority order. */
#define SCE_KERNEL_MPA_HIGHMEM      (0x4000)    /** Allocate the buffer from the top of the partition. */

/** Message pipe wait modes. */
#define SCE_KERNEL_MPW_FULL         (0)         /** Wait until all the data is transferred. */
#define SCE_KERNEL_MPW_ASAP         (1)         /** Wait until a part of the data is transferred. */

SceUID sceKernelCreateMsgPipe(const char *name, int part, int attr, SceSize bufSize, SceKernelMppOptParam *option);
int sceKernelDeleteMsgPipe(SceUID uid);
int sceKernelSendMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout);
int sceKernelSendMsgPipeCB(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout);
int sceKernelTrySendMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result);
int sceKernelReceiveMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout);
int sceKernelReceiveMsgPipeCB(SceUID uid, void *message, SceSize size, int waitMode, int *result, SceUInt *timeout);
int sceKernelTryReceiveMsgPipe(SceUID uid, void *message, SceSize size, int waitMode, int *result);
int sceKernelCancelMsgPipe(SceUID uid, int *numSendWaitThreads, int *numReceiveWaitThreads);
int sceKernelReferMsgPipeStatus(SceUID uid, SceKernelMppInfo *info);

/* Semaphores */

typedef struct {
    SceSize     size;
} SceKernelSemaOptParam;

typedef struct {
    SceSize     size;
    char        name[SCE_UID_NAME_LEN + 1];
    SceUInt     attr;
    int         initCount;
    int         currentCount;
    int         maxCount;
    int         numWaitThreads;
} SceKernelSemaInfo;

#define SCE_KERNEL_SA_THFIFO    (0x0000)            /* A FIFO queue is used for the waiting thread */
#define SCE_KERNEL_SA_THPRI     (0x0100)            /* The waiting thread is queued by its thread priority */

SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, SceKernelSemaOptParam *option);
int sceKernelDeleteSema(SceUID semaid);
int sceKernelSignalSema(SceUID semaid, int signal);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout);
int sceKernelWaitSemaCB(SceUID semaid, int signal, SceUInt *timeout);
int sceKernelPollSema(SceUID semaid, int signal);
int sceKernelCancelSema(SceUID semaid, int setCount, int *numWaitThreads);
int sceKernelReferSemaStatus(SceUID semaid, SceKernelSemaInfo *info);

/* Message boxes. */

/** The header of a message sent to a message box (the message data follows it). */
typedef struct SceKernelMsgPacket {
    /** The next message in the message box (managed by the kernel). */
    struct SceKernelMsgPacket *next;
    /** The message priority (with SCE_KERNEL_MBA_MSPRI, lower values are received first). */
    SceUChar msgPriority;
    SceUChar dummy[3];
} SceKernelMsgPacket;

typedef struct {
    SceSize     size;
} SceKernelMbxOptParam;

typedef struct {
    SceSize     size;
    char        name[SCE_UID_NAME_LEN + 1];
    SceUInt     attr;
    int         numWaitThreads;
    int         numMessages;
    void        *firstMessage;
} SceKernelMbxInfo;

#define SCE_KERNEL_MBA_THFIFO   (0x0000)    /* A FIFO queue is used for the waiting threads */
#define SCE_KERNEL_MBA_THPRI    (0x0100)    /* The waiting threads are queued by their thread priority */
#define SCE_KERNEL_MBA_MSFIFO   (0x0000)    /* Messages are received in the order they were sent */
#define SCE_KERNEL_MBA_MSPRI    (0x0400)    /* Messages are received by their priority */

SceUID sceKernelCreateMbx(const char *name, SceUInt attr, SceKernelMbxOptParam *option);
int sceKernelDeleteMbx(SceUID mbxid);
int sceKernelSendMbx(SceUID mbxid, void *message);
int sceKernelReceiveMbx(SceUID mbxid, void **pmessage, SceUInt *timeout);
int sceKernelReceiveMbxCB(SceUID mbxid, void **pmessage, SceUInt *timeout);
int sceKernelPollMbx(SceUID mbxid, void **pmessage);
int sceKernelCancelReceiveMbx(SceUID mbxid, int *pnum);
int sceKernelReferMbxStatus(SceUID mbxid, SceKernelMbxInfo *info);

/* KTLS */
s32 sceKernelAllocateKTLS(s32 size, void (*destructor)(void *ktls, void *arg), void *arg);
s32 sceKernelFreeKTLS(s32 id);
void *sceKernelGetKTLS(s32 id);
void *sceKernelGetThreadKTLS(s32 id, SceUID thid, s32 alloc);

/* Alarms. */

typedef SceUInt (*SceKernelAlarmHandler)(void *common);

typedef struct {
	SceUInt32   low;
	SceUInt32   hi;
} SceKernelSysClock;

/* Thread status. */

/** The status of a thread (sceKernelReferThreadStatus()). */
typedef struct SceKernelThreadInfo {
    /** The size of the structure (0x68 for applications compiled with an SDK version <= 2.60). */
    SceSize     size; // 0
    char        name[SCE_UID_NAME_LEN + 1]; // 4
    SceUInt     attr; // 36
    /** The thread status. */
    s32         status; // 40
    SceKernelThreadEntry entry; // 44
    /** The lowest address of the stack (the user stack for a user mode thread). */
    void        *stack; // 48
    s32         stackSize; // 52
    /** The $gp register value of the thread. */
    void        *gpReg; // 56
    s32         initPriority; // 60
    s32         currentPriority; // 64
    /** The wait type (+0x100 if the thread waits with the callbacks enabled); only set when waiting. */
    s32         waitType; // 68
    /** The UID of the object waited on; only set when waiting. */
    SceUID      waitId; // 72
    s32         wakeupCount; // 76
    s32         exitStatus; // 80
    SceKernelSysClock runClocks; // 84
    SceUInt     intrPreemptCount; // 92
    SceUInt     threadPreemptCount; // 96
    SceUInt     releaseCount; // 100
    /** Whether a callback of the thread was notified. */
    s32         notifyCallback; // 104
} SceKernelThreadInfo; // size: 108

/** The runtime status of a thread (sceKernelReferThreadRunStatus()). */
typedef struct SceKernelThreadRunStatus {
    SceSize     size; // 0
    s32         status; // 4
    s32         currentPriority; // 8
    s32         waitType; // 12
    SceUID      waitId; // 16
    s32         wakeupCount; // 20
    SceKernelSysClock runClocks; // 24
    SceUInt     intrPreemptCount; // 32
    SceUInt     threadPreemptCount; // 36
    SceUInt     releaseCount; // 40
    s32         notifyCallback; // 44
} SceKernelThreadRunStatus; // size: 48

/** The debug status of a thread (sceKernelReferThreadDebugStatus()). */
typedef struct SceKernelThreadKInfo {
    SceSize     size; // 0
    char        name[SCE_UID_NAME_LEN + 1]; // 4
    SceUInt     attr; // 36
    s32         status; // 40
    void        *entry; // 44
    void        *stack; // 48
    s32         stackSize; // 52
    /** The kernel stack of a user mode thread. */
    void        *kStack; // 56
    s32         kStackSize; // 60
    void        *gpReg; // 64
    SceSize     args; // 68
    void        *argp; // 72
    s32         initPriority; // 76
    s32         currentPriority; // 80
    s32         waitType; // 84
    SceUID      waitId; // 88
    s32         wakeupCount; // 92
    SceKernelSysClock runClocks; // 96
    SceUInt     intrPreemptCount; // 104
    SceUInt     threadPreemptCount; // 108
    SceUInt     releaseCount; // 112
    s32         notifyCallback; // 116
    /** The saved context of the thread. */
    void        *context; // 120
    /** The saved VFPU context of the thread (NULL if it is still in the VFPU). */
    void        *vfpuContext; // 124
    /** The return address of the current syscall of the thread (0 if none). */
    u32         syscallRa; // 128
    /** The profiler registers of the thread (NULL if none). */
    void        *profilerRegs; // 132
    /** The syscall frame of the current syscall of the thread (NULL if none). */
    void        *syscallFrame; // 136
    /** The timeout of the current wait. */
    SceUInt     timeout; // 140
    /** Wait type specific data. */
    u32         waitData[8]; // 144
    /** The $k0 area of the thread. */
    void        *k0; // 176
    /** The kernel thread local storage blocks. */
    void        *ktls[16]; // 180
} SceKernelThreadKInfo; // size: 244

s32 sceKernelReferThreadStatus(SceUID thid, SceKernelThreadInfo *info);
s32 sceKernelReferThreadRunStatus(SceUID thid, SceKernelThreadRunStatus *status);
s32 sceKernelReferThreadDebugStatus(SceUID thid, SceKernelThreadKInfo *info);

/** The system status (see sceKernelReferSystemStatus()). */
typedef struct SceKernelSystemStatus {
    /** The size of the structure. */
    SceSize size; // 0
    /** The current state (0: normal, 1: dispatch suspended, 3: interrupts disabled, 4: interrupt context). */
    u32 status; // 4
    /** The time the idle thread has been running. */
    SceKernelSysClock idleClocks; // 8
    /** The number of times the idle thread was preempted by an interrupt. */
    u32 comesOutOfIdleCount; // 16
    /** The number of thread switches. */
    u32 threadSwitchCount; // 20
    /** The number of VFPU context switches. */
    u32 vfpuSwitchCount; // 24
} SceKernelSystemStatus; // size: 28

s32 sceKernelReferSystemStatus(SceKernelSystemStatus *status);

typedef struct {
	SceSize size;
	SceKernelSysClock schedule;
	SceKernelAlarmHandler handler;
	void *common;
} SceKernelAlarmInfo;

SceUID sceKernelSetAlarm(SceUInt clock, SceKernelAlarmHandler handler, void *common);
SceUID sceKernelSetSysClockAlarm(SceKernelSysClock *clock, SceKernelAlarmHandler handler, void *common);
int sceKernelCancelAlarm(SceUID alarmid);
int sceKernelReferAlarmStatus(SceUID alarmid, SceKernelAlarmInfo *info);

/* Virtual timers. */

typedef SceUInt (*SceKernelVTimerHandler)(SceUID uid, SceKernelSysClock *schedule, SceKernelSysClock *actual,
    void *common);
typedef SceUInt (*SceKernelVTimerHandlerWide)(SceUID uid, SceInt64 schedule, SceInt64 actual, void *common);

typedef struct SceKernelVTimerOptParam {
    SceSize size;
} SceKernelVTimerOptParam;

typedef struct SceKernelVTimerInfo {
    SceSize size;
    char name[SCE_UID_NAME_LEN + 1];
    s32 active;
    SceKernelSysClock base;
    SceKernelSysClock current;
    SceKernelSysClock schedule;
    SceKernelVTimerHandler handler;
    void *common;
} SceKernelVTimerInfo;

SceUID sceKernelCreateVTimer(const char *name, SceKernelVTimerOptParam *opt);
int sceKernelDeleteVTimer(SceUID uid);
int sceKernelGetVTimerBase(SceUID uid, SceKernelSysClock *base);
SceInt64 sceKernelGetVTimerBaseWide(SceUID uid);
int sceKernelGetVTimerTime(SceUID uid, SceKernelSysClock *time);
SceInt64 sceKernelGetVTimerTimeWide(SceUID uid);
int sceKernelSetVTimerTime(SceUID uid, SceKernelSysClock *time);
SceInt64 sceKernelSetVTimerTimeWide(SceUID uid, SceInt64 time);
int sceKernelStartVTimer(SceUID uid);
int sceKernelStopVTimer(SceUID uid);
int sceKernelSetVTimerHandler(SceUID uid, SceKernelSysClock *time, SceKernelVTimerHandler handler, void *common);
int sceKernelSetVTimerHandlerWide(SceUID uid, SceInt64 time, SceKernelVTimerHandlerWide handler, void *common);
int sceKernelCancelVTimerHandler(SceUID uid);
int sceKernelReferVTimerStatus(SceUID uid, SceKernelVTimerInfo *info);

/* System time. */

int sceKernelUSec2SysClock(unsigned int usec, SceKernelSysClock *clock);
SceInt64 sceKernelUSec2SysClockWide(unsigned int usec);
int sceKernelSysClock2USec(SceKernelSysClock *clock, unsigned int *low, unsigned int *high);
int sceKernelSysClock2USecWide(SceInt64 clock, unsigned int *low, unsigned int *high);
int sceKernelGetSystemTime(SceKernelSysClock *time);

/* Callbacks */
typedef s32 (*SceKernelCallbackFunction)(s32 arg1, s32 arg2, void *arg);

typedef struct {
    SceSize size;
    char name[SCE_UID_NAME_LEN + 1];
    SceUID threadId;
    SceKernelCallbackFunction callback;
    void *common;
    s32 notifyCount;
    s32 notifyArg;
} SceKernelCallbackInfo;

SceUID sceKernelCreateCallback(const char *name, SceKernelCallbackFunction func, void *common);
s32 sceKernelDeleteCallback(SceUID uid);
s32 sceKernelNotifyCallback(SceUID uid, s32 arg);
s32 sceKernelCancelCallback(SceUID uid);
s32 sceKernelGetCallbackCount(SceUID uid);
s32 sceKernelCheckCallback(void);
s32 sceKernelReferCallbackStatus(SceUID uid, SceKernelCallbackInfo *info);

/* VPL Functions */

typedef struct {
    SceSize 	size;
} SceKernelVplOptParam;

/** The waiting threads are queued in FIFO order. */
#define SCE_KERNEL_VPA_THFIFO   (0x0000)
/** The waiting threads are queued by priority. */
#define SCE_KERNEL_VPA_THPRI    (0x0100)
/** A waiting thread whose request can be satisfied may be served before the threads queued before it. */
#define SCE_KERNEL_VPA_PASS     (0x0200)
/** The pool is allocated from the top of the memory partition. */
#define SCE_KERNEL_VPA_HIGHMEM  (0x4000)


SceUID sceKernelCreateVpl(const char *name, int part, int attr, unsigned int size, SceKernelVplOptParam *opt);

int sceKernelDeleteVpl(SceUID uid);
int sceKernelAllocateVpl(SceUID uid, unsigned int size, void **data, unsigned int *timeout);
int sceKernelAllocateVplCB(SceUID uid, unsigned int size, void **data, unsigned int *timeout);
int sceKernelTryAllocateVpl(SceUID uid, unsigned int size, void **data);
int sceKernelFreeVpl(SceUID uid, void *data);
int sceKernelCancelVpl(SceUID uid, int *pnum);

typedef struct {
	SceSize 	size;
    char 	name[SCE_UID_NAME_LEN + 1];
	SceUInt 	attr;
	int 	poolSize;
	int 	freeSize;
	int 	numWaitThreads;
} SceKernelVplInfo;

int sceKernelReferVplStatus(SceUID uid, SceKernelVplInfo *info);

/* FPL Functions */

typedef struct {
    SceSize 	size;
    /** The alignment of the blocks (a power of two, 0 for the default alignment of 4). */
    SceSize 	alignment;
} SceKernelFplOptParam;

/** The waiting threads are queued in FIFO order. */
#define SCE_KERNEL_FPA_THFIFO   (0x0000)
/** The waiting threads are queued by priority. */
#define SCE_KERNEL_FPA_THPRI    (0x0100)
/** The pool is allocated from the top of the memory partition. */
#define SCE_KERNEL_FPA_HIGHMEM  (0x4000)

int sceKernelCreateFpl(const char *name, int part, int attr, unsigned int size, unsigned int blocks, SceKernelFplOptParam *opt);
int sceKernelDeleteFpl(SceUID uid);
int sceKernelAllocateFpl(SceUID uid, void **data, unsigned int *timeout);
int sceKernelAllocateFplCB(SceUID uid, void **data, unsigned int *timeout);
int sceKernelTryAllocateFpl(SceUID uid, void **data);
int sceKernelFreeFpl(SceUID uid, void *data);
int sceKernelCancelFpl(SceUID uid, int *pnum);

typedef struct {
    SceSize 	size;
    char 	name[SCE_UID_NAME_LEN + 1];
    SceUInt 	attr;
    int 	blockSize;
    int 	numBlocks;
    int 	freeBlocks;
    int 	numWaitThreads;
} SceKernelFplInfo;

int sceKernelReferFplStatus(SceUID uid, SceKernelFplInfo *info);

/* TLS pool Functions */

typedef struct {
    SceSize 	size;
    /** The alignment of the blocks (a power of two, 0 for the default alignment of 4). */
    SceSize 	alignment;
} SceKernelTlsplOptParam;

/** The waiting threads are queued in FIFO order. */
#define SCE_KERNEL_TLSPA_THFIFO     (0x0000)
/** The waiting threads are queued by priority. */
#define SCE_KERNEL_TLSPA_THPRI      (0x0100)
/** The pool is allocated from the top of the memory partition. */
#define SCE_KERNEL_TLSPA_HIGHMEM    (0x4000)

SceUID sceKernelCreateTlspl(const char *name, int part, int attr, unsigned int size, unsigned int blocks,
    SceKernelTlsplOptParam *opt);
int sceKernelDeleteTlspl(SceUID uid);
int _sceKernelAllocateTlspl(SceUID uid, void **data, unsigned int *timeout);
int sceKernelFreeTlspl(SceUID uid);

typedef struct {
    SceSize 	size;
    char 	name[SCE_UID_NAME_LEN + 1];
    SceUInt 	attr;
    /** The slot of the pool. */
    int 	index;
    int 	blockSize;
    int 	totalBlocks;
    int 	freeBlocks;
    int 	numWaitThreads;
} SceKernelTlsplInfo;

int sceKernelReferTlsplStatus(SceUID uid, SceKernelTlsplInfo *info);

s64 sceKernelGetSystemTimeWide(void);

#endif /* THREADMAN_KERNEL_H */

