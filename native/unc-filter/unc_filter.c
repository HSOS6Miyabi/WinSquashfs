/* SPDX-License-Identifier: GPL-3.0-only */
#pragma warning(push, 0)
#include <fltKernel.h>
#pragma warning(pop)
#define ARCHIVEFS_KERNEL 1
#include "alias_path.h"

static PFLT_FILTER ArchiveFsFilter;
static BOOLEAN IncludeUnqualified;

DRIVER_INITIALIZE DriverEntry;
static FLT_PREOP_CALLBACK_STATUS FLTAPI ArchiveFsCreate(
    PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS Objects, PVOID *CompletionContext);
static FLT_PREOP_CALLBACK_STATUS FLTAPI ArchiveFsNetworkQueryOpen(
    PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS Objects, PVOID *CompletionContext);
static NTSTATUS FLTAPI ArchiveFsSetup(
    PCFLT_RELATED_OBJECTS Objects, FLT_INSTANCE_SETUP_FLAGS Flags,
    DEVICE_TYPE VolumeDeviceType, FLT_FILESYSTEM_TYPE VolumeFilesystemType);
static NTSTATUS FLTAPI ArchiveFsUnload(FLT_FILTER_UNLOAD_FLAGS Flags);
static NTSTATUS FLTAPI ArchiveFsTeardown(PCFLT_RELATED_OBJECTS Objects, FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags);

static const FLT_OPERATION_REGISTRATION Operations[] = {
    { IRP_MJ_CREATE, 0, ArchiveFsCreate, NULL },
    { IRP_MJ_NETWORK_QUERY_OPEN, 0, ArchiveFsNetworkQueryOpen, NULL },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION Registration = {
    sizeof(FLT_REGISTRATION), FLT_REGISTRATION_VERSION, 0,
    NULL, Operations, ArchiveFsUnload, ArchiveFsSetup, ArchiveFsTeardown
};

static NTSTATUS FLTAPI ArchiveFsSetup(
    PCFLT_RELATED_OBJECTS Objects, FLT_INSTANCE_SETUP_FLAGS Flags,
    DEVICE_TYPE VolumeDeviceType, FLT_FILESYSTEM_TYPE VolumeFilesystemType)
{
    WCHAR buffer[128];
    UNICODE_STRING name = {0, sizeof(buffer), buffer};
    UNICODE_STRING mup = RTL_CONSTANT_STRING(L"\\Device\\Mup");
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeFilesystemType);
    if (VolumeDeviceType != FILE_DEVICE_NETWORK_FILE_SYSTEM)
        return STATUS_FLT_DO_NOT_ATTACH;
    if (!NT_SUCCESS(FltGetVolumeName(Objects->Volume, &name, NULL)) ||
        !RtlEqualUnicodeString(&name, &mup, TRUE))
        return STATUS_FLT_DO_NOT_ATTACH;
    return STATUS_SUCCESS;
}

static NTSTATUS FLTAPI ArchiveFsTeardown(
    PCFLT_RELATED_OBJECTS Objects, FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Objects);
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;
}

static NTSTATUS FLTAPI ArchiveFsUnload(FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    FltUnregisterFilter(ArchiveFsFilter);
    return STATUS_SUCCESS;
}

static ArchiveFsAliasResult ArchiveFsMatch(PFILE_OBJECT FileObject, ArchiveFsAlias *Alias)
{
    if (!FileObject || FileObject->RelatedFileObject || !FileObject->FileName.Buffer ||
        (FileObject->FileName.Length % sizeof(WCHAR)) != 0)
        return ArchiveFsAliasUnrelated;
    return ArchiveFsParseAlias((const ArchiveFsChar *)FileObject->FileName.Buffer,
        FileObject->FileName.Length / sizeof(WCHAR), IncludeUnqualified != FALSE, Alias);
}

static FLT_PREOP_CALLBACK_STATUS Complete(PFLT_CALLBACK_DATA Data, NTSTATUS Status, ULONG_PTR Information)
{
    Data->IoStatus.Status = Status;
    Data->IoStatus.Information = Information;
    return FLT_PREOP_COMPLETE;
}

static FLT_PREOP_CALLBACK_STATUS FLTAPI ArchiveFsCreate(
    PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS Objects, PVOID *CompletionContext)
{
    PFILE_OBJECT file;
    ArchiveFsAlias alias;
    ArchiveFsAliasResult matched;
    PWSTR target;
    USHORT bytes;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Objects);
    UNREFERENCED_PARAMETER(CompletionContext);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !FLT_IS_IRP_OPERATION(Data) ||
        FlagOn(Data->Iopb->OperationFlags, SL_OPEN_PAGING_FILE) ||
        FlagOn(Data->Iopb->Parameters.Create.Options, FILE_OPEN_BY_FILE_ID))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    file = Data->Iopb->TargetFileObject;
    matched = ArchiveFsMatch(file, &alias);
    if (matched == ArchiveFsAliasUnrelated) return FLT_PREOP_SUCCESS_NO_CALLBACK;
    if (matched == ArchiveFsAliasInvalid) return Complete(Data, STATUS_BAD_NETWORK_NAME, 0);

    bytes = (USHORT)(alias.target_length * sizeof(WCHAR));
    /* PagedPool is sufficient at PASSIVE_LEVEL. Keep Windows 10 1809
       compatibility rather than importing ExAllocatePool2 (2004+). */
#pragma warning(suppress:4996)
    target = ExAllocatePoolWithTag(PagedPool, bytes, 'fAsW');
    if (!target) return Complete(Data, STATUS_INSUFFICIENT_RESOURCES, 0);
    ArchiveFsMakeAlias((const ArchiveFsChar *)file->FileName.Buffer,
        file->FileName.Length / sizeof(WCHAR), &alias, (ArchiveFsChar *)target);
    status = IoReplaceFileObjectName(file, target, bytes);
    ExFreePoolWithTag(target, 'fAsW');
    if (!NT_SUCCESS(status)) return Complete(Data, status, 0);
    /* No new file is opened here. The I/O manager retries the original
       request with the original security context and access parameters. */
    return Complete(Data, STATUS_REPARSE, IO_REPARSE);
}

static FLT_PREOP_CALLBACK_STATUS FLTAPI ArchiveFsNetworkQueryOpen(
    PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS Objects, PVOID *CompletionContext)
{
    ArchiveFsAlias alias;
    UNREFERENCED_PARAMETER(Objects);
    UNREFERENCED_PARAMETER(CompletionContext);
    if (KeGetCurrentIrql() > APC_LEVEL || !FLT_IS_FASTIO_OPERATION(Data))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    if (ArchiveFsMatch(Data->Iopb->TargetFileObject, &alias) == ArchiveFsAliasUnrelated)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    /* Fast I/O cannot return STATUS_REPARSE. Force the regular create path. */
    return FLT_PREOP_DISALLOW_FASTIO;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    HANDLE parameters;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(RegistryPath);
    IncludeUnqualified = FALSE;
    if (NT_SUCCESS(IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters,
        KEY_QUERY_VALUE, 0, &parameters))) {
        UNICODE_STRING name = RTL_CONSTANT_STRING(L"IncludeUnqualified");
        ULONG length;
        struct {
            ULONG TitleIndex, Type, DataLength, Value;
        } value;
        if (NT_SUCCESS(ZwQueryValueKey(parameters, &name, KeyValuePartialInformation,
            &value, sizeof(value), &length)) && value.Type == REG_DWORD &&
            value.DataLength == sizeof(ULONG) && value.Value == 1)
            IncludeUnqualified = TRUE;
        ZwClose(parameters);
    }
    status = FltRegisterFilter(DriverObject, &Registration, &ArchiveFsFilter);
    if (!NT_SUCCESS(status)) return status;
    status = FltStartFiltering(ArchiveFsFilter);
    if (!NT_SUCCESS(status)) FltUnregisterFilter(ArchiveFsFilter);
    return status;
}
