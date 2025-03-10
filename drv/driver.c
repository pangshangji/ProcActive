

#include <ntddk.h>
#include <wdm.h>

#include "list.h"

#ifndef  DRIVER
#define DRIVER
#include "..\common\data.h"
#endif // ! DRIVER

#define PROC_ACTIVE_POOL_TAG	(ULONG)'itcA'

static LIST Proc_Lists;
#define LISTS_MAX_COUNT 1000

typedef struct _Proc_Action {

	LIST_ELEM list_elem;
	ULONG64 pid;
	DWORD session_id;	
	BOOLEAN createorexit;	
	LARGE_INTEGER current_time;
	ULONG len; //  including NULL  bytes	
	WCHAR* name;
}Proc_Action;

#define PROCESS_QUERY_INFORMATION (0x0400)

void Process_GetProcessName(ULONG_PTR idProcess,
	void** out_buf, ULONG* out_len, WCHAR** out_ptr);
NTSTATUS dispatch_read(PDEVICE_OBJECT DeviceObject, PIRP irp);
BOOLEAN AddDataToBuffer(HANDLE ProcessId, BOOLEAN createorexit);
VOID irp_read_canceled(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS send_irp_sres(PIRP Irp, KIRQL oldirql);
NTSTATUS dispatch(struct _DEVICE_OBJECT* DeviceObject, struct _IRP* irp);
VOID ClearBuffer();

#define NTOS_API(type)  NTSYSAPI type NTAPI
#define NTOS_NTSTATUS   NTOS_API(NTSTATUS)

NTOS_NTSTATUS   ZwQueryInformationProcess(
	IN HANDLE           ProcessHandle,
	IN PROCESSINFOCLASS ProcessInformationClass,
	OUT PVOID           ProcessInformation,
	IN ULONG            ProcessInformationLength,
	OUT PULONG          ReturnLength OPTIONAL);

KSPIN_LOCK proc_spinlock;
LIST_ENTRY proc_listhead = { 0 };
PDEVICE_OBJECT device = NULL;

typedef struct _ExtData
{
	LONG console_is_exist;
	PIRP pendingirp;
}ExtData, *PExtData;


void Process_GetProcessName(ULONG_PTR idProcess,
	void** out_buf, ULONG* out_len, WCHAR** out_ptr)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;;
	OBJECT_ATTRIBUTES objattrs = {0};
	CLIENT_ID cid = {0};
	HANDLE handle = NULL;
	ULONG len = 0;;

	*out_buf = NULL;
	*out_len = 0;
	*out_ptr = NULL;

	if (!idProcess)
		return;

	InitializeObjectAttributes(&objattrs,
		NULL, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	cid.UniqueProcess = (HANDLE)idProcess;
	cid.UniqueThread = 0;
	
	status = ZwOpenProcess(
		&handle, PROCESS_QUERY_INFORMATION, &objattrs, &cid);

	if (!NT_SUCCESS(status))
		return;

	status = ZwQueryInformationProcess(
		handle, ProcessImageFileName, NULL, 0, &len);

	if (status == STATUS_INFO_LENGTH_MISMATCH) {

		ULONG uni_len = len + 8 + 8;
		UNICODE_STRING* uni = ExAllocatePoolWithTag(NonPagedPool, uni_len, PROC_ACTIVE_POOL_TAG);
		if (uni) {

			memset(uni, 0, uni_len);

			status = ZwQueryInformationProcess(
				handle, ProcessImageFileName, uni, len + 8, &len);

			if (NT_SUCCESS(status) && uni->Buffer) {

				WCHAR* ptr;
				uni->Buffer[uni->Length / sizeof(WCHAR)] = L'\0';
				if (!uni->Buffer[0]) {
					uni->Buffer[0] = L'?';
					uni->Buffer[1] = L'\0';
				}
				ptr = wcsrchr(uni->Buffer, L'\\');
				if (ptr) {
					++ptr;
					if (!*ptr)
						ptr = uni->Buffer;
				}
				else
					ptr = uni->Buffer;
				*out_buf = uni;
				*out_len = uni_len;
				*out_ptr = ptr;

			}
			else
				ExFreePoolWithTag(uni, PROC_ACTIVE_POOL_TAG);				
		}
	}

	ZwClose(handle);
}

BOOLEAN AddDataToBuffer(HANDLE ProcessId, BOOLEAN createorexit)
{	
	PIRP irp = NULL;
	LARGE_INTEGER system_time;
	LARGE_INTEGER local_time;
	KeQuerySystemTime(&system_time);
	ExSystemTimeToLocalTime(&system_time, &local_time);
	
	if (NULL == device || NULL == device->DeviceExtension)
	{
		return FALSE;
	}	

	void* nbuf = NULL;
	ULONG nlen = 0;
	WCHAR* nptr = NULL;
	Process_GetProcessName((ULONG_PTR)ProcessId, &nbuf, &nlen, &nptr);
	if (NULL == nbuf) {
		return FALSE;
	}

	Proc_Action* data = (Proc_Action*)ExAllocatePoolWithTag(NonPagedPool, sizeof(Proc_Action), PROC_ACTIVE_POOL_TAG);
	if (NULL == data)
	{
		ExFreePoolWithTag(nbuf, PROC_ACTIVE_POOL_TAG);
		return FALSE;
	}
	memset(data, 0, sizeof(Proc_Action));
	data->pid = (ULONG64)ProcessId;
	data->current_time = local_time;
	data->createorexit = createorexit;

	WCHAR* image_path = ((UNICODE_STRING*)nbuf)->Buffer;
	ULONG image_path_len = ((UNICODE_STRING*)nbuf)->Length;
	data->len = image_path_len + sizeof(WCHAR);

	data->name = ExAllocatePoolWithTag(NonPagedPool, data->len, PROC_ACTIVE_POOL_TAG);
	if (NULL == data->name)
	{
		ExFreePoolWithTag(nbuf, PROC_ACTIVE_POOL_TAG);
		ExFreePoolWithTag(data, PROC_ACTIVE_POOL_TAG);
		data = NULL;
		return FALSE;
	}
	memcpy(data->name, image_path, image_path_len);
	data->name[image_path_len / 2] = L'\0';

	ExFreePoolWithTag(nbuf, PROC_ACTIVE_POOL_TAG);
	nbuf = NULL;
	image_path = NULL;

	PExtData extdata = (PExtData)(device->DeviceExtension);
	KIRQL oldIrql;
	KeAcquireSpinLock(&proc_spinlock, &oldIrql);	
	if (List_Count(&Proc_Lists) >= LISTS_MAX_COUNT)
	{
		Proc_Action* head = (Proc_Action*)List_Head(&Proc_Lists);
		List_Remove(&Proc_Lists, head);
		ExFreePoolWithTag(head->name, PROC_ACTIVE_POOL_TAG);
		ExFreePoolWithTag(head, PROC_ACTIVE_POOL_TAG);
	}
	List_Insert_After(&Proc_Lists, NULL, data);

	irp = extdata->pendingirp;
	if (NULL == irp)
	{		
		KeReleaseSpinLock(&proc_spinlock, oldIrql);
		return TRUE;		
	}
	extdata->pendingirp = NULL;
	send_irp_sres(irp, oldIrql);
	return TRUE;
	
}

NTSTATUS send_irp_sres(PIRP irp, KIRQL oldirql)
{
	KIRQL cancel_irql;	

	IoAcquireCancelSpinLock(&cancel_irql);
	PDRIVER_CANCEL previousCancelRoutine = IoSetCancelRoutine(irp, NULL);
	if (previousCancelRoutine == NULL) {
		if (irp->Cancel) {
			IoReleaseCancelSpinLock(cancel_irql);
			KeReleaseSpinLock(&proc_spinlock, oldirql);
			return STATUS_CANCELLED;
		}		
	}
	IoReleaseCancelSpinLock(cancel_irql);	

	PProcData data = (PProcData)irp->AssociatedIrp.SystemBuffer;
	Proc_Action* head = (Proc_Action*)List_Head(&Proc_Lists);
		
	ASSERT(NULL != head);
	NTSTATUS status = STATUS_SUCCESS;
	if (head->len > data->max_name_len)
	{
		status = STATUS_BUFFER_TOO_SMALL;			
		irp->IoStatus.Information = 0;
	}
	else
	{
		data->pid = head->pid;
		data->createorexit = head->createorexit;
		data->current_time = head->current_time;		

		memcpy(GetName(data), head->name, head->len);
		irp->IoStatus.Information = sizeof(ProcData) + head->len;
		List_Remove(&Proc_Lists, head);
		ExFreePoolWithTag(head->name, PROC_ACTIVE_POOL_TAG);
		ExFreePoolWithTag(head, PROC_ACTIVE_POOL_TAG);
	}
	irp->IoStatus.Status = status;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	KeReleaseSpinLock(&proc_spinlock, oldirql);
	return status;
}

VOID Process_NotifyProcessEx(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo)
{
	UNREFERENCED_PARAMETER(Process);
	UNREFERENCED_PARAMETER(ProcessId);
	UNREFERENCED_PARAMETER(CreateInfo);
	if (NULL != device && TRUE == (BOOLEAN)((PExtData)(device->DeviceExtension))->console_is_exist)
	{		
		// add process to ringbuff
		AddDataToBuffer(ProcessId, NULL != CreateInfo);
	}
}

VOID ClearBuffer()
{
	KIRQL oldIrql;
	KeAcquireSpinLock(&proc_spinlock, &oldIrql);	
	while (0 != List_Count(&Proc_Lists))
	{
		Proc_Action* head = (Proc_Action*)List_Head(&Proc_Lists);
		List_Remove(&Proc_Lists, head);
		ExFreePoolWithTag(head->name, PROC_ACTIVE_POOL_TAG);
		ExFreePoolWithTag(head, PROC_ACTIVE_POOL_TAG);
	}

	KeReleaseSpinLock(&proc_spinlock, oldIrql);
}

VOID Driver_Unload(_In_ struct _DRIVER_OBJECT* DriverObject)
{	
	UNREFERENCED_PARAMETER(DriverObject);
	ClearBuffer();

	UNICODE_STRING uni_symbolic;
	RtlInitUnicodeString(&uni_symbolic, SYMBOLIC_PROCACTIVE);
	IoDeleteSymbolicLink(&uni_symbolic);
	IoDeleteDevice(device);
	device = NULL;
	PsSetCreateProcessNotifyRoutineEx(Process_NotifyProcessEx, TRUE);
	return;
}

NTSTATUS dispatch_read(PDEVICE_OBJECT DeviceObject, PIRP irp)
{
	ASSERT(NULL != DeviceObject);
	ASSERT(NULL != DeviceObject->DeviceExtension);

	IO_STACK_LOCATION* irpstack = IoGetCurrentIrpStackLocation(irp);
	ULONG length = irpstack->Parameters.Read.Length;
	if (length < sizeof(ProcData))
	{
		irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return irp->IoStatus.Status;
	}

	PProcData data = (PProcData)irp->AssociatedIrp.SystemBuffer;	
	ULONG64 name_len = length - sizeof(ProcData);
	if (name_len > 4096 || name_len < 64)
	{
		irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return irp->IoStatus.Status;
	}
	data->max_name_len = (ULONG)name_len;	

	KIRQL oldIrql;
	KeAcquireSpinLock(&proc_spinlock, &oldIrql);

	PExtData extdata = (PExtData)(device->DeviceExtension);
	if (NULL != extdata->pendingirp)
	{
		KeReleaseSpinLock(&proc_spinlock, oldIrql);
		irp->IoStatus.Status = STATUS_DEVICE_BUSY;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return irp->IoStatus.Status;
	}
	
	if (0 == List_Count(&Proc_Lists))
	{
		extdata->pendingirp = irp;
		IoSetCancelRoutine(irp, irp_read_canceled);
		IoMarkIrpPending(irp);
		KeReleaseSpinLock(&proc_spinlock, oldIrql);
		return STATUS_PENDING;
	}
	return send_irp_sres(irp, oldIrql);

}

VOID irp_read_canceled(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	ASSERT(device);
	ASSERT(device->DeviceExtension);

	KIRQL oldIrql;
	KeAcquireSpinLock(&proc_spinlock, &oldIrql);
	PExtData extdata = (PExtData)(device->DeviceExtension);
	ASSERT(Irp == extdata->pendingirp);
	extdata->pendingirp = NULL;
	KeReleaseSpinLock(&proc_spinlock, oldIrql);
	
	IoReleaseCancelSpinLock(Irp->CancelIrql);
	Irp->IoStatus.Status = STATUS_CANCELLED;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

NTSTATUS dispatch(struct _DEVICE_OBJECT* DeviceObject, struct _IRP* irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	IO_STACK_LOCATION* irpstack = IoGetCurrentIrpStackLocation(irp);
	PExtData extdata = (PExtData)(device->DeviceExtension);	
	switch (irpstack->MajorFunction)
	{
	case IRP_MJ_CREATE:
	{
		LONG old = InterlockedCompareExchange(&extdata->console_is_exist, 1, 0);
		if (1 == old)
		{
			status = STATUS_DEVICE_BUSY;
		}
		break;
	}
		
	case IRP_MJ_CLEANUP:
		InterlockedCompareExchange(&extdata->console_is_exist, 0, 1);
		ClearBuffer();
		break;
	case IRP_MJ_READ:		
		return dispatch_read(DeviceObject, irp);
	default:
		break;
	}
	irp->IoStatus.Status = status;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return irp->IoStatus.Status;	
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING register_path)
{
	UNREFERENCED_PARAMETER(register_path);
	
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	BOOLEAN has_symbolic = FALSE;
	BOOLEAN process_callback = FALSE;
	__try
	{
		if (NULL == driver)
		{
			__leave;			
		}
		driver->DriverUnload = Driver_Unload;
		for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
			driver->MajorFunction[i] = dispatch;
		}		

		UNICODE_STRING uni_device;
		RtlInitUnicodeString(&uni_device, PROCACTIVE_DEVICE_NAME);
		status = IoCreateDevice(driver, sizeof(ExtData), &uni_device, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, TRUE, &device);
		if (!NT_SUCCESS(status))
		{
			__leave;
		}
		device->Flags &= ~DO_DEVICE_INITIALIZING;
		device->Flags |= DO_BUFFERED_IO;

		UNICODE_STRING uni_symbolic;
		RtlInitUnicodeString(&uni_symbolic, SYMBOLIC_PROCACTIVE);
		status = IoCreateSymbolicLink(&uni_symbolic, &uni_device);
		if (!NT_SUCCESS(status))
		{
			__leave;
		}

		has_symbolic = TRUE;

		status = PsSetCreateProcessNotifyRoutineEx(Process_NotifyProcessEx, FALSE);
		if (!NT_SUCCESS(status))
		{
			__leave;
		}
		process_callback = TRUE;
		List_Init(&Proc_Lists);
		KeInitializeSpinLock(&proc_spinlock);						

		status = STATUS_SUCCESS;
	}
	__finally
	{
		if (!NT_SUCCESS(status))
		{
			if (has_symbolic)
			{
				UNICODE_STRING uni_symbolic;
				RtlInitUnicodeString(&uni_symbolic, SYMBOLIC_PROCACTIVE);
				IoDeleteSymbolicLink(&uni_symbolic);
				
			}
			if (NULL != device)
			{
				IoDeleteDevice(device);
				device = NULL;
			}
			if (process_callback)
			{
				PsSetCreateProcessNotifyRoutineEx(Process_NotifyProcessEx, TRUE);
			}
		}
	}
	return status;
}