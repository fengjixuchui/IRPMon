
#include <ntifs.h>
#include "preprocessor.h"
#include "allocator.h"
#include "utils.h"
#include "request.h"
#include "process-events.h"
#include "driver-settings.h"
#include "req-queue.h"


/************************************************************************/
/*                            GLOBAL VARIABLES                          */
/************************************************************************/

static KSPIN_LOCK _requestListLock;
static LIST_ENTRY _requestListHead;
static IO_REMOVE_LOCK _removeLock;
static ERESOURCE _connectLock;
static PIRPMNDRV_SETTINGS _driverSettings = NULL;

/************************************************************************/
/*                             HELPER FUNCTIONS                         */
/************************************************************************/




/************************************************************************/
/*                            PUBLIC ROUTINES                           */
/************************************************************************/


NTSTATUS RequestQueueConnect()
{
	PREQUEST_HEADER old = NULL;
	PREQUEST_HEADER psRequest = NULL;
	LIST_ENTRY psRequests;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION_NO_ARGS();
	DEBUG_IRQL_LESS_OR_EQUAL(PASSIVE_LEVEL);

	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(&_connectLock, TRUE);
	if (!_driverSettings->ReqQueueConnected) {
		IoInitializeRemoveLock(&_removeLock, 0, 0, 0x7fffffff);
		status = IoAcquireRemoveLock(&_removeLock, NULL);
		if (NT_SUCCESS(status)) {
			InitializeListHead(&psRequests);
			if (_driverSettings->ProcessEmulateOnConnect)
				status = ListProcessesByEvents(&psRequests);
			
			if (NT_SUCCESS(status) && _driverSettings->DriverSnapshotOnConnect)
				status = ListDriversAndDevicesByEvents(&psRequests);

			if (NT_SUCCESS(status)) {
				psRequest = CONTAINING_RECORD(psRequests.Blink, REQUEST_HEADER, Entry);
				while (&psRequest->Entry != &psRequests) {
					old = psRequest;
					psRequest = CONTAINING_RECORD(psRequest->Entry.Blink, REQUEST_HEADER, Entry);
					RemoveEntryList(&old->Entry);
					ExInterlockedInsertHeadList(&_requestListHead, &old->Entry, &_requestListLock);
					InterlockedIncrement(&_driverSettings->ReqQueueLength);
				}

				_driverSettings->ReqQueueConnected = TRUE;
			}

			if (!NT_SUCCESS(status)) {
				psRequest = CONTAINING_RECORD(psRequests.Blink, REQUEST_HEADER, Entry);
				while (&psRequest->Entry != &psRequests) {
					old = psRequest;
					psRequest = CONTAINING_RECORD(psRequest->Entry.Blink, REQUEST_HEADER, Entry);
					RemoveEntryList(&old->Entry);
					HeapMemoryFree(old);
				}

				IoReleaseRemoveLock(&_removeLock, NULL);
			}
		}
	} else status = STATUS_ALREADY_REGISTERED;

	ExReleaseResourceLite(&_connectLock);
	KeLeaveCriticalRegion();

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


VOID RequestQueueDisconnect(VOID)
{
	DEBUG_ENTER_FUNCTION_NO_ARGS();
	DEBUG_IRQL_LESS_OR_EQUAL(APC_LEVEL);

	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(&_connectLock, TRUE);
	if (_driverSettings->ReqQueueConnected) {
		IoReleaseRemoveLockAndWait(&_removeLock, NULL);
		_driverSettings->ReqQueueConnected = FALSE;
		if (_driverSettings->ReqQueueClearOnDisconnect)
			RequestQueueClear();
	}

	ExReleaseResourceLite(&_connectLock);
	KeLeaveCriticalRegion();

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


VOID RequestQueueInsert(PREQUEST_HEADER Header)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("Header=0x%p", Header);
	DEBUG_IRQL_LESS_OR_EQUAL(DISPATCH_LEVEL);

	if (_driverSettings->ReqQueueConnected ||
		_driverSettings->ReqQueueCollectWhenDisconnected) {
		status = IoAcquireRemoveLock(&_removeLock, NULL);
		if (NT_SUCCESS(status)) {
			ExInterlockedInsertTailList(&_requestListHead, &Header->Entry, &_requestListLock);
			InterlockedIncrement(&_driverSettings->ReqQueueLength);
			IoReleaseRemoveLock(&_removeLock, NULL);
		}
	} else status = STATUS_CONNECTION_DISCONNECTED;
	
	if (!NT_SUCCESS(status))
		HeapMemoryFree(Header);

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


ULONG RequestIdReserve(void)
{
	return InterlockedIncrement(&_driverSettings->ReqQueueLastRequestId);
}


VOID RequestHeaderInit(PREQUEST_HEADER Header, PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT DeviceObject, ERequesttype RequestType)
{
	RequestHeaderInitNoId(Header, DriverObject, DeviceObject, RequestType);
	Header->Id = RequestIdReserve();

	return;
}


VOID RequestHeaderInitNoId(PREQUEST_HEADER Header, PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT DeviceObject, ERequesttype RequestType)
{
	RtlSecureZeroMemory(Header, sizeof(REQUEST_HEADER));
	InitializeListHead(&Header->Entry);
	KeQuerySystemTime(&Header->Time);
	Header->Device = DeviceObject;
	Header->Driver = DriverObject;
	Header->Type = RequestType;
	Header->ResultType = rrtUndefined;
	Header->Result.Other = NULL;
	Header->ProcessId = PsGetCurrentProcessId();
	Header->ThreadId = PsGetCurrentThreadId();
	Header->Irql = KeGetCurrentIrql();

	return;
}


NTSTATUS RequestXXXDetectedCreate(ERequesttype Type, PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT DeviceObject, PREQUEST_HEADER *Header)
{
	SIZE_T requestSize = 0;
	PREQUEST_HEADER tmpHeader = NULL;
	PREQUEST_DRIVER_DETECTED drr = NULL;
	PREQUEST_DEVICE_DETECTED der = NULL;
	UNICODE_STRING uObjectName;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("Type=%u; DriverObject=0x%p; DeviceObject=0x%p; Header=0x%p", DriverObject, DeviceObject, Type, Header);

	status = (Type == ertDeviceDetected) ?
		_GetObjectName(DeviceObject, &uObjectName) :
		_GetObjectName(DriverObject, &uObjectName);

	if (NT_SUCCESS(status)) {
		switch (Type) {
			case ertDriverDetected:
				requestSize = sizeof(REQUEST_DRIVER_DETECTED) + uObjectName.Length;
				drr = (PREQUEST_DRIVER_DETECTED)HeapMemoryAllocNonPaged(requestSize);
				if (drr != NULL) {
					drr->DriverNameLength = uObjectName.Length;
					memcpy(drr + 1, uObjectName.Buffer, drr->DriverNameLength);
					tmpHeader = &drr->Header;
				} else status = STATUS_INSUFFICIENT_RESOURCES;
				break;
			case ertDeviceDetected:
				requestSize = sizeof(REQUEST_DEVICE_DETECTED) + uObjectName.Length;
				der = (PREQUEST_DEVICE_DETECTED)HeapMemoryAllocNonPaged(requestSize);
				if (der != NULL) {
					der->DeviceNameLength = uObjectName.Length;
					memcpy(der + 1, uObjectName.Buffer, der->DeviceNameLength);
					tmpHeader = &der->Header;
				} else status = STATUS_INSUFFICIENT_RESOURCES;
				break;
			default:
				status = STATUS_INVALID_PARAMETER_1;
				break;
		}

		if (NT_SUCCESS(status)) {
			RequestHeaderInit(tmpHeader, DriverObject, DeviceObject, Type);
			*Header = tmpHeader;
		}

		HeapMemoryFree(uObjectName.Buffer);
	}

	DEBUG_EXIT_FUNCTION("0x%x, *Header=0x%p", status, *Header);
	return status;
}


NTSTATUS RequestQueueGet(PREQUEST_HEADER *Buffer, PSIZE_T Length)
{
	SIZE_T reqSize = 0;
	PREQUEST_HEADER h = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("Buffer=0x%p; Length=0x%p", Buffer, Length);
	DEBUG_IRQL_LESS_OR_EQUAL(APC_LEVEL);

	if (_driverSettings->ReqQueueConnected) {
		status = IoAcquireRemoveLock(&_removeLock, NULL);
		if (NT_SUCCESS(status)) {
			PLIST_ENTRY l = NULL;
			
			l = ExInterlockedRemoveHeadList(&_requestListHead, &_requestListLock);
			if (l != NULL) {
				h = CONTAINING_RECORD(l, REQUEST_HEADER, Entry);
				reqSize = RequestGetSize(h);
				if (reqSize <= *Length) {
					InterlockedDecrement(&_driverSettings->ReqQueueLength);
					if (!IsListEmpty(&_requestListHead))
						h->Flags |= REQUEST_FLAG_NEXT_AVAILABLE;

					*Buffer = h;
					status = STATUS_SUCCESS;
				} else {
					ExInterlockedInsertHeadList(&_requestListHead, l, &_requestListLock);
					status = STATUS_BUFFER_TOO_SMALL;
				}
			} else status = STATUS_NO_MORE_ENTRIES;

			*Length = reqSize;
			IoReleaseRemoveLock(&_removeLock, NULL);
		}
	} else status = STATUS_CONNECTION_DISCONNECTED;

	DEBUG_EXIT_FUNCTION("0x%x, *Length=%u", status, *Length);
	return status;
}


NTSTATUS ListDriversAndDevicesByEvents(PLIST_ENTRY ListHead)
{
	const wchar_t *dirNames[2] = {
		L"\\Driver",
		L"\\FileSystem"
	};
	PDRIVER_OBJECT *driverArray = NULL;
	SIZE_T driverArrayLength;
	PDEVICE_OBJECT *deviceArray = NULL;
	ULONG deviceArrayLength = 0;
	UNICODE_STRING tmpName;
	PREQUEST_HEADER tmpRequest = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("ListHead=0x%p", ListHead);

	for (size_t i = 0; i < sizeof(dirNames) / sizeof(dirNames[0]); ++i) {
		RtlInitUnicodeString(&tmpName, dirNames[i]);
		status = _GetDriversInDirectory(&tmpName, &driverArray, &driverArrayLength);
		if (NT_SUCCESS(status)) {
			for (size_t j = 0; j < driverArrayLength; ++j) {
				status = RequestXXXDetectedCreate(ertDriverDetected, driverArray[j], NULL, &tmpRequest);
				if (NT_SUCCESS(status)) {
					tmpRequest->Flags |= REQUEST_FLAG_EMULATED;
					InsertTailList(ListHead, &tmpRequest->Entry);
					status = _EnumDriverDevices(driverArray[j], &deviceArray, &deviceArrayLength);
					if (NT_SUCCESS(status)) {
						for (size_t k = 0; k < deviceArrayLength; ++k) {
							status = RequestXXXDetectedCreate(ertDeviceDetected, driverArray[j], deviceArray[k], &tmpRequest);
							if (NT_SUCCESS(status)) {
								tmpRequest->Flags |= REQUEST_FLAG_EMULATED;
								InsertTailList(ListHead, &tmpRequest->Entry);
							}
						}

						_ReleaseDeviceArray(deviceArray, deviceArrayLength);
					}
				}
			}

			_ReleaseDriverArray(driverArray, driverArrayLength);
		}
	}

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}


void RequestQueueClear(void)
{
	KIRQL irql;
	LIST_ENTRY tmpList;
	PREQUEST_HEADER req = NULL;
	PREQUEST_HEADER old = NULL;
	DEBUG_ENTER_FUNCTION_NO_ARGS();

	InitializeListHead(&tmpList);
	KeAcquireSpinLock(&_requestListLock, &irql);
	if (!IsListEmpty(&_requestListHead)) {
		tmpList = _requestListHead;
		InitializeListHead(&_requestListHead);
		InterlockedExchange(&_driverSettings->ReqQueueLength, 0);
	}

	KeReleaseSpinLock(&_requestListLock, irql);
	tmpList.Flink->Blink = &tmpList;
	tmpList.Blink->Flink = &tmpList;
	req = CONTAINING_RECORD(tmpList.Flink, REQUEST_HEADER, Entry);
	while (&req->Entry != &tmpList) {
		old = req;
		req = CONTAINING_RECORD(req->Entry.Flink, REQUEST_HEADER, Entry);
		HeapMemoryFree(old);
	}

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}


/************************************************************************/
/*                     INITIALIZATION AND FINALIZATION                  */
/************************************************************************/

NTSTATUS RequestQueueModuleInit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	DEBUG_ENTER_FUNCTION("DriverObject=0x%p; RegistryPath=\"%wZ\"; Context=0x%p", DriverObject, RegistryPath, Context);

	UNREFERENCED_PARAMETER(DriverObject);
	UNREFERENCED_PARAMETER(RegistryPath);
	UNREFERENCED_PARAMETER(Context);
	
	_driverSettings = DriverSettingsGet();
	InitializeListHead(&_requestListHead);
	KeInitializeSpinLock(&_requestListLock);
	IoInitializeRemoveLock(&_removeLock, 0, 0, 0x7fffffff);
	status = ExInitializeResourceLite(&_connectLock);

	DEBUG_EXIT_FUNCTION("0x%x", status);
	return status;
}

VOID RequestQueueModuleFinit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID Context)
{
	DEBUG_ENTER_FUNCTION("DriverObject=0x%p; RegistryPath=\"%wZ\"; Context=0x%p", DriverObject, RegistryPath, Context);

	UNREFERENCED_PARAMETER(DriverObject);
	UNREFERENCED_PARAMETER(RegistryPath);
	UNREFERENCED_PARAMETER(Context);

	RequestQueueClear();
	ExDeleteResourceLite(&_connectLock);
	_driverSettings = NULL;

	DEBUG_EXIT_FUNCTION_VOID();
	return;
}
