# Copyright (C) 2026 The uOFW team
# See the file COPYING for copying permission.

# The imports of the module in the layout of the original (MATCHING=1 build, used instead of the stub
# libraries): the stub library entries in the link order of the libraries, 5 words long for the
# libraries which export no variable, the flags are the attributes of the exporting library, then the
# function and variable stubs sorted by library name and by NID.

#include "common/pspimport.s"

    IMPORT_START "InterruptManagerForKernel",0x00090011
    IMPORT_START "ExceptionManagerForKernel",0x00010011
    IMPORT_START "SysMemForKernel",0x00010011,5
    IMPORT_START "KDebugForKernel",0x00010011,5
    IMPORT_START "sceSuspendForKernel",0x00010011,5
    IMPORT_START "SysclibForKernel",0x00010011,5
    IMPORT_START "LoadCoreForKernel",0x00010011,5

    IMPORT_FUNC "ExceptionManagerForKernel",0x54B0CAD2,sceKernelReleaseExceptionHandler
    IMPORT_FUNC "ExceptionManagerForKernel",0x96D47F7E,ExceptionManagerForKernel_96D47F7E
    IMPORT_FUNC "ExceptionManagerForKernel",0xFBC2353A,sceKernelRegisterExceptionHandler
    IMPORT_FUNC "InterruptManagerForKernel",0x02314986,sceKernelCpuEnableIntr
    IMPORT_FUNC "InterruptManagerForKernel",0x092968F4,sceKernelCpuSuspendIntr
    IMPORT_FUNC "InterruptManagerForKernel",0x3B84732D,sceKernelCpuResumeIntrWithSync
    IMPORT_FUNC "InterruptManagerForKernel",0x4D6E7305,sceKernelEnableIntr
    IMPORT_FUNC "InterruptManagerForKernel",0x551097F2,InterruptManagerForKernel_551097F2
    IMPORT_FUNC "InterruptManagerForKernel",0x55D18836,sceKernelSetPrimarySyscallHandler
    IMPORT_FUNC "InterruptManagerForKernel",0x58DD8978,sceKernelRegisterIntrHandler
    IMPORT_FUNC "InterruptManagerForKernel",0x5F10D406,sceKernelCpuResumeIntr
    IMPORT_FUNC "InterruptManagerForKernel",0x6042AEE0,SaveThreadContext
    IMPORT_FUNC "InterruptManagerForKernel",0xAF799F96,sceKernelCallUserIntrHandler
    IMPORT_FUNC "InterruptManagerForKernel",0xB941600E,sceKernelSetIntrLevel
    IMPORT_FUNC "InterruptManagerForKernel",0xCD36EB65,sceKernelGetUserIntrStack
    IMPORT_FUNC "InterruptManagerForKernel",0xD774BA45,sceKernelDisableIntr
    IMPORT_FUNC "InterruptManagerForKernel",0xE790EAED,InterruptManagerForKernel_E790EAED
    IMPORT_FUNC "InterruptManagerForKernel",0xF153B371,sceKernelQuerySystemCall
    IMPORT_FUNC "InterruptManagerForKernel",0xF987B1F0,sceKernelReleaseIntrHandler
    IMPORT_FUNC "KDebugForKernel",0x84F370BC,Kprintf
    IMPORT_FUNC "KDebugForKernel",0x86010FCB,sceKernelDipsw
    IMPORT_FUNC "KDebugForKernel",0xACF427DC,sceKernelIsDevelopmentToolMode
    IMPORT_FUNC "LoadCoreForKernel",0x410084F9,sceKernelGetModuleGPByAddressForKernel
    IMPORT_FUNC "LoadCoreForKernel",0xF976EF41,sceKernelSetBootCallbackLevel
    IMPORT_FUNC "SysMemForKernel",0x01810023,sceKernelHeapTotalFreeSize
    IMPORT_FUNC "SysMemForKernel",0x034129FB,sceKernelCreateUIDtype
    IMPORT_FUNC "SysMemForKernel",0x0A34C078,sceKernelCreateUID
    IMPORT_FUNC "SysMemForKernel",0x22A114DC,sceKernelMemset32
    IMPORT_FUNC "SysMemForKernel",0x235C2646,sceKernelCallUIDObjCommonFunction
    IMPORT_FUNC "SysMemForKernel",0x23D81675,sceKernelAllocHeapMemory
    IMPORT_FUNC "SysMemForKernel",0x361F0F88,sceKernelDeleteUID
    IMPORT_FUNC "SysMemForKernel",0x44BDF332,sceKernelGetUIDcontrolBlockWithType
    IMPORT_FUNC "SysMemForKernel",0x58148F07,sceKernelCreateHeap
    IMPORT_FUNC "SysMemForKernel",0x7158CE7E,sceKernelAllocPartitionMemory
    IMPORT_FUNC "SysMemForKernel",0x7D50CE82,sceKernelCallUIDObjFunction
    IMPORT_FUNC "SysMemForKernel",0x87C2AB85,sceKernelFreeHeapMemory
    IMPORT_FUNC "SysMemForKernel",0xB4F00CB5,sceKernelGetCompiledSdkVersion
    IMPORT_FUNC "SysMemForKernel",0xC1A26C6F,sceKernelFreePartitionMemory
    IMPORT_FUNC "SysMemForKernel",0xC4EEAF20,sceKernelQueryMemoryPartitionInfo
    IMPORT_FUNC "SysMemForKernel",0xC90B0992,sceKernelGetUIDcontrolBlock
    IMPORT_FUNC "SysMemForKernel",0xD222DAA7,sceKernelCreateUIDtypeInherit
    IMPORT_FUNC "SysMemForKernel",0xDD6512D0,sceKernelDeleteHeap
    IMPORT_FUNC "SysMemForKernel",0xE860BE8F,sceKernelQueryMemoryBlockInfo
    IMPORT_FUNC "SysMemForKernel",0xF12A62F7,sceKernelGetBlockHeadAddr
    IMPORT_FUNC "SysMemForKernel",0xF2284ECC,sceKernelAllocHeapMemoryWithOption
    IMPORT_FUNC "SysMemForKernel",0xFAF29F34,sceKernelQueryMemoryInfo
    IMPORT_FUNC "SysMemForKernel",0xFFC63884,sceKernelIsKindOf
    IMPORT_FUNC "SysclibForKernel",0x10F3BB61,memset
    IMPORT_FUNC "SysclibForKernel",0xAB7592FF,memcpy
    IMPORT_FUNC "SysclibForKernel",0xB49A7697,strncpy
    IMPORT_FUNC "SysclibForKernel",0xC2145E80,snprintf
    IMPORT_FUNC "sceSuspendForKernel",0x91A77137,sceKernelRegisterSuspendHandler
    IMPORT_FUNC "sceSuspendForKernel",0xB43D1A8C,sceKernelRegisterResumeHandler

    IMPORT_VAR "ExceptionManagerForKernel",0x6437D22A,ExcepManCB
    IMPORT_VAR "InterruptManagerForKernel",0x201A1A19,intInfo
