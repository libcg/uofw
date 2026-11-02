# Copyright (C) 2016 The uOFW team
# See the file COPYING for copying permission.

TARGET = threadman
OBJS = threadman.o waitq.o threadevent.o callback.o callback_ret.o threadwait.o semaphore.o eventflag.o mutex.o lwmutex.o mbx.o msgpipe.o vpl.o fpl.o tlspl.o timer.o timer_syscall.o ktls.o threadman_asm.o

#DEBUG = 1

LIBS = 	-lInterruptManagerForKernel -lExceptionManagerForKernel -lSysMemForKernel -lKDebugForKernel \
		-lsceSuspendForKernel -lSysclibForKernel -lLoadCoreForKernel

include $(ROOT_DIR)/lib/build.mak