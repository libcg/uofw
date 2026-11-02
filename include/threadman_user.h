/* Copyright (C) 2011, 2012, 2013 The uOFW team
   See the file COPYING for copying permission.
*/

#ifndef THREADMAN_USER_H
#define THREADMAN_USER_H

#include "common_header.h"

/* LwMutex */

enum SceKernelLwMutexTypes {
    SCE_KERNEL_LWMUTEX_RECURSIVE = 0x200
};

typedef struct {
	s32 lockCount; // 0
	s32 thid; // 4
	u32 flags; // 8
	/* The number of threads waiting on the mutex (PPSSPP NativeLwMutexWorkarea.numWaitThreads; the user unlock
	   only does the unlock syscall when it is not zero). */
	s32 numWaitThreads; // 12
	s32 id; // 16
	/* Unused (PPSSPP NativeLwMutexWorkarea.pad). */
	u32 pad[3]; // 20
} SceLwMutex; // size: 32

typedef struct {
    SceSize     size;
} SceKernelLwMutexOptParam;

typedef struct {
    SceSize     size;
    char        name[SCE_UID_NAME_LEN + 1];
    SceUInt     attr;
    SceUID      uid;
    SceLwMutex  *workarea;
    s32         initCount;
    s32         currentCount;
    SceUID      currentOwner;
    s32         numWaitThreads;
} SceKernelLwMutexInfo;

s32 sceKernelCreateLwMutex(SceLwMutex *mutex, const char *name, u32 attr, s32 initCount,
    SceKernelLwMutexOptParam *option);
s32 sceKernelDeleteLwMutex(SceLwMutex *mutex);
s32 _sceKernelLockLwMutexCB(SceLwMutex *mutex, s32 count, SceUInt *timeout);
s32 _sceKernelLockLwMutex(SceLwMutex *mutex, s32 count, SceUInt *timeout);
s32 _sceKernelTryLockLwMutex(SceLwMutex *mutex, s32 count);
s32 _sceKernelUnlockLwMutex(SceLwMutex *mutex, s32 count);
s32 sceKernelReferLwMutexStatusByID(SceUID id, SceKernelLwMutexInfo *info);
int _sceKernelAllocateTlspl(SceUID uid, void **data, unsigned int *timeout);

#endif /* THREADMAN_USER_H */

