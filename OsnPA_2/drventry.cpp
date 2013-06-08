
#include "OSNddk.h"
#include "ntdddisk.h"
#include "queue.h"

#include "OSNEvent.h"

#include "OSNTcpIp.h"
#include "..\common\Queue.h"
#include "..\..\Applications\common\OSNDefs.h"
#include "..\..\Applications\common\CfgCommon.h"
#include "..\..\Applications\common\OSNCommon.h"
#include "..\..\Applications\common\lun.h"
#include "..\..\Applications\common\OsnWwn.h"

#include "DDOsnPA.h"

#include "DiskDevice.h"
#include "DiskCacheVolume.h"
#include "ActiveIrp.h"
#include "OsnPADevice.h"

#include "global.h"

#include "OsnPAmsg.h"

extern "C" NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    );

VOID		Unload(PDRIVER_OBJECT);

CGlobal* g_pGlobal=NULL;

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
	//
    // Set up the device driver entry points.
    //
	KdPrint(("OSNPA - Enter driverentry \n"));
	ULONG               ulIndex;
    PDRIVER_DISPATCH  * dispatch;

    //
    // Create dispatch points
    //
    for (ulIndex = 0, dispatch = DriverObject->MajorFunction;
         ulIndex <= IRP_MJ_MAXIMUM_FUNCTION;
         ulIndex++, dispatch++) 
	{

			 *dispatch = COSNBaseDevice::DefaultDispatch;
    }

	DriverObject->MajorFunction[IRP_MJ_CREATE]                  = COSNBaseDevice::DispatchCreate;
	DriverObject->MajorFunction[IRP_MJ_CLOSE]                   = COSNBaseDevice::DispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = COSNBaseDevice::DispatchIoctl;
	DriverObject->MajorFunction[IRP_MJ_READ]                    = COSNBaseDevice::DispatchReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE]                   = COSNBaseDevice::DispatchReadWrite;
	DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = COSNBaseDevice::DispatchInternalIoctl;
	
	DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = COSNBaseDevice::DispatchWmi; 

	DriverObject->DriverExtension->AddDevice                    = (PDRIVER_ADD_DEVICE) AddDevice;

    DriverObject->MajorFunction[IRP_MJ_PNP]                     = COSNBaseDevice::DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = COSNBaseDevice::DispatchPower;

	DriverObject->DriverUnload                                  = Unload;

	UNICODE_STRING driverName;
	RtlInitUnicodeString(&driverName, L"\\Device\\OsnPA");
	//_asm int 3;

	g_pGlobal=new CGlobal(DriverObject);

	if(!g_pGlobal)
	{
		COSNEventLog::OSNLogEvent(DriverObject,OSNPA_FAIL_TO_LOAD, STATUS_INSUFFICIENT_RESOURCES);

		return STATUS_INSUFFICIENT_RESOURCES;
	}

	NTSTATUS status=g_pGlobal->CreateDeviceStack(
								DriverObject,
								&driverName,
								FILE_DEVICE_UNKNOWN,
								FILE_DEVICE_SECURE_OPEN,
								NULL,
								0);
	if(!NT_SUCCESS(status))
	{
		delete g_pGlobal;

		g_pGlobal=NULL;

		COSNEventLog::OSNLogEvent(DriverObject,OSNPA_FAIL_TO_LOAD, status);

		return STATUS_INSUFFICIENT_RESOURCES;
	}

	status = g_pGlobal->Initialize(RegistryPath);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - Failed to initialize global ,status = %x\n",status));
	}


	UNICODE_STRING win32Name;
	RtlInitUnicodeString(&win32Name, L"\\??\\OsnPA");

	IoDeleteSymbolicLink(&win32Name);
	
	IoCreateSymbolicLink(&win32Name,&driverName);

    return(STATUS_SUCCESS);

} // DriverEntry


NTSTATUS AddDevice(IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT pPhysicalDeviceObject)
{
	UNICODE_STRING driverName;
	VOLUME_TYPE    volumetype;

	PDRIVER_OBJECT pPhysicalDriver=pPhysicalDeviceObject->DriverObject;

	UNICODE_STRING vmDriverName;
	RtlInitUnicodeString(&vmDriverName, L"\\Driver\\OsnVm");

	UNICODE_STRING dmDriverName;
	RtlInitUnicodeString(&dmDriverName, L"\\Driver\\OsnDm");

	UNICODE_STRING tvmDriverName;
	RtlInitUnicodeString(&tvmDriverName, L"\\Driver\\OsnTvm");


	if(RtlCompareUnicodeString(&pPhysicalDriver->DriverName, &vmDriverName, false) == 0)
	{
		volumetype = VOLUME_TYPE_VM;
	}
	else if(RtlCompareUnicodeString(&pPhysicalDriver->DriverName, &dmDriverName, false) == 0)
	{
		volumetype = VOLUME_TYPE_DM;
	}
	else if(RtlCompareUnicodeString(&pPhysicalDriver->DriverName, &tvmDriverName, false) == 0)
	{
		volumetype = VOLUME_TYPE_TVM;
	}
	else
	{
		volumetype = VOLUME_TYPE_UNDEFINED;
		return STATUS_NO_SUCH_DEVICE;
	}
	
	COsnPADevice *pOsnPADevice = new COsnPADevice(volumetype);

	if(!pOsnPADevice)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}


	//reated new device will have this pDiskDevice as its DeviceExtension 
	NTSTATUS status = pOsnPADevice->CreateDeviceStack(
		DriverObject,
		NULL,//no name,
		FILE_DEVICE_DISK,
		FILE_DEVICE_SECURE_OPEN,
		pPhysicalDeviceObject,
		0);

	if(!NT_SUCCESS(status))
	{
		delete pOsnPADevice;

		return status;
	}
	g_pGlobal->InsertDeviceToList(pOsnPADevice);
	
	return status;
}

//in win2k and after
//normally you don't need to do anything in unload routine
//
VOID Unload( IN PDRIVER_OBJECT DriverObject)
{
	if(g_pGlobal)
	{
		delete g_pGlobal;
		g_pGlobal=NULL;
	}
}

