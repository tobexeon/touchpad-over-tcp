#include "Driver.h"

static NTSTATUS SubmitIoctlFrame(PDEVICE_CONTEXT context, const MTP_IOCTL_FRAME* input) {
    MTP_PTP_REPORT report;
    HID_XFER_PACKET packet;
    UCHAR index;
    UCHAR reportContacts;
    BOOLEAN surfaceEnabled;
    BOOLEAN buttonEnabled;

    if (input->abi_version != MTP_IOCTL_ABI_VERSION ||
        (input->flags & ~(MTP_IOCTL_FRAME_RESET | MTP_IOCTL_FRAME_BUTTON |
                          MTP_IOCTL_FRAME_BUTTON2 | MTP_IOCTL_FRAME_HAS_SCAN_TIME)) != 0 ||
        input->active_contact_count > MTP_HID_MAX_CONTACTS ||
        input->report_contact_count > MTP_HID_MAX_CONTACTS) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&report, sizeof(report));
    report.ReportId = MTP_REPORT_ID_INPUT;

    if ((input->flags & MTP_IOCTL_FRAME_HAS_SCAN_TIME) != 0) {
        report.ScanTime = input->scan_time_100us;
        context->LastScanTime = report.ScanTime;
    } else {
        report.ScanTime = input->scan_time_100us != 0
                        ? input->scan_time_100us
                        : ++context->LastScanTime;
        context->LastScanTime = report.ScanTime;
    }

    surfaceEnabled = (context->FunctionSwitch & 0x01) != 0;
    buttonEnabled = (context->FunctionSwitch & 0x02) != 0;

    /*
     * PTP spec: Contact Count is the number of contacts reported in this frame
     * regardless of their Tip Switch state. It therefore has to be
     * report_contact_count, not active_contact_count. Filling it with the active
     * count made every lift frame advertise zero contacts, so Windows skipped
     * the whole Contacts[] array and never observed Tip=0 -- the pan gesture was
     * then cancelled by timeout instead of ending cleanly, which is why the
     * native scroll inertia never engaged.
     */
    reportContacts = surfaceEnabled ? input->report_contact_count : 0;
    report.ContactCount = reportContacts;

    /* Bit 0 = Button 1 (clickpad), bit 2 = Button 3 (external secondary). */
    if (buttonEnabled) {
        report.Button = (UCHAR)(((input->flags & MTP_IOCTL_FRAME_BUTTON) != 0 ? 1 : 0) |
                                ((input->flags & MTP_IOCTL_FRAME_BUTTON2) != 0 ? 4 : 0));
    }

    for (index = 0; index < reportContacts; ++index) {
        const MTP_IOCTL_CONTACT* source = &input->contacts[index];
        MTP_PTP_CONTACT* target;
        UCHAR confidence;
        UCHAR tip;
        if (source->slot >= MTP_HID_MAX_CONTACTS ||
            (source->flags & ~(MTP_IOCTL_CONTACT_IN_RANGE | MTP_IOCTL_CONTACT_TIP |
                               MTP_IOCTL_CONTACT_CONFIDENCE)) != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        /*
         * Pack densely by report order. Indexing with source->slot left a
         * zero-filled Contacts[0] behind whenever a lower finger lifted, and
         * with ContactCount = 1 Windows read that empty slot as the only
         * contact -- the surviving finger simply vanished.
         */
        target = &report.Contacts[index];
        confidence = (source->flags & MTP_IOCTL_CONTACT_CONFIDENCE) != 0 ? 1 : 0;
        tip = (source->flags & MTP_IOCTL_CONTACT_TIP) != 0 ? 1 : 0;
        target->Flags = (UCHAR)(confidence | (tip << 1));
        target->ContactId = source->contact_id;
        target->X = source->x > MTP_COORDINATE_LOGICAL_MAX ? MTP_COORDINATE_LOGICAL_MAX : source->x;
        target->Y = source->y > MTP_COORDINATE_LOGICAL_MAX ? MTP_COORDINATE_LOGICAL_MAX : source->y;
    }

    packet.reportBuffer = (PUCHAR)&report;
    packet.reportBufferLen = sizeof(report);
    packet.reportId = MTP_REPORT_ID_INPUT;
    context->LastReportSize = (USHORT)sizeof(report);
    context->LastActiveContactCount = input->active_contact_count;
    context->LastSubmitStatus = VhfReadReportSubmit(context->VhfHandle, &packet);
    InterlockedIncrement(&context->SubmitCount);
    return context->LastSubmitStatus;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, MtpEvtDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
}

NTSTATUS MtpEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit) {
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PDEVICE_CONTEXT context;
    WDF_IO_QUEUE_CONFIG queueConfig;
    VHF_CONFIG vhfConfig;
    UNICODE_STRING symbolicLink;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);

    WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, TRUE);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = MtpEvtDeviceCleanup;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    RtlInitUnicodeString(&symbolicLink, L"\\DosDevices\\MtpVhfTouchpad");
    status = WdfDeviceCreateSymbolicLink(device, &symbolicLink);
    if (!NT_SUCCESS(status)) return status;

    context = DeviceGetContext(device);
    context->VhfHandle = NULL;
    context->LastScanTime = 0;
    context->LastSubmitStatus = STATUS_NOT_FOUND;
    context->InputMode = 3;
    /* Surface Switch (bit 0) and Button Switch (bit 1) both enabled. A zero
       value here would discard every contact below. */
    context->FunctionSwitch = 3;
    context->LatencyMode = 0;
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &context->SubmitLock);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = MtpEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    VHF_CONFIG_INIT(&vhfConfig, WdfDeviceWdmGetDeviceObject(device),
                    g_MtpReportDescriptorLength, g_MtpReportDescriptor);
    vhfConfig.VhfClientContext = context;
    vhfConfig.VendorID = 0x1209;
    vhfConfig.ProductID = 0x3987;
    vhfConfig.VersionNumber = 1;
    vhfConfig.EvtVhfAsyncOperationGetFeature = MtpEvtVhfGetFeature;
    vhfConfig.EvtVhfAsyncOperationSetFeature = MtpEvtVhfSetFeature;
    status = VhfCreate(&vhfConfig, &context->VhfHandle);
    if (!NT_SUCCESS(status)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    status = VhfStart(context->VhfHandle);
    if (!NT_SUCCESS(status)) {
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

VOID MtpEvtDeviceCleanup(WDFOBJECT object) {
    PDEVICE_CONTEXT context = DeviceGetContext((WDFDEVICE)object);
    if (context->VhfHandle != NULL) {
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
    }
}

VOID MtpEvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputBufferLength,
                           size_t inputBufferLength, ULONG ioControlCode) {
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    MTP_IOCTL_FRAME* input = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(outputBufferLength);

    if (ioControlCode == IOCTL_MTP_QUERY_STATUS) {
        MTP_IOCTL_STATUS* output = NULL;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(MTP_IOCTL_STATUS),
                                                (PVOID*)&output, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(output, sizeof(*output));
            output->abi_version = MTP_IOCTL_ABI_VERSION;
            output->submit_count = (ULONG)context->SubmitCount;
            output->last_submit_status = context->LastSubmitStatus;
            output->last_report_size = context->LastReportSize;
            output->last_active_contact_count = context->LastActiveContactCount;
            output->input_mode = context->InputMode;
            output->function_switch = context->FunctionSwitch;
            output->get_feature_count = (ULONG)context->GetFeatureCount;
            output->set_feature_count = (ULONG)context->SetFeatureCount;
            WdfRequestSetInformation(request, sizeof(*output));
        }
        WdfRequestComplete(request, status);
        return;
    }
    if (ioControlCode != IOCTL_MTP_SUBMIT_FRAME || inputBufferLength != sizeof(MTP_IOCTL_FRAME)) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    status = WdfRequestRetrieveInputBuffer(request, sizeof(MTP_IOCTL_FRAME), (PVOID*)&input, NULL);
    if (NT_SUCCESS(status)) {
        WdfWaitLockAcquire(context->SubmitLock, NULL);
        status = SubmitIoctlFrame(context, input);
        WdfWaitLockRelease(context->SubmitLock);
    }
    WdfRequestComplete(request, status);
}

VOID MtpEvtVhfGetFeature(PVOID clientContext, VHFOPERATIONHANDLE operationHandle,
                         PVOID operationContext, PHID_XFER_PACKET packet) {
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    PDEVICE_CONTEXT context = (PDEVICE_CONTEXT)clientContext;
    UNREFERENCED_PARAMETER(operationContext);
    InterlockedIncrement(&context->GetFeatureCount);
    if (packet != NULL && packet->reportBuffer != NULL) {
        if (packet->reportId == MTP_REPORT_ID_CAPS && packet->reportBufferLen >= 3) {
            packet->reportBuffer[0] = MTP_REPORT_ID_CAPS;
            packet->reportBuffer[1] = MTP_HID_MAX_CONTACTS;
            packet->reportBuffer[2] = 0; /* Click pad. */
            status = STATUS_SUCCESS;
        } else if (packet->reportId == MTP_REPORT_ID_CERTIFICATION &&
                   packet->reportBufferLen >= 257) {
            packet->reportBuffer[0] = MTP_REPORT_ID_CERTIFICATION;
            RtlCopyMemory(packet->reportBuffer + 1, g_MtpCertificationBlob, 256);
            status = STATUS_SUCCESS;
        } else if (packet->reportId == MTP_REPORT_ID_MODE && packet->reportBufferLen >= 2) {
            packet->reportBuffer[0] = MTP_REPORT_ID_MODE;
            packet->reportBuffer[1] = context->InputMode;
            status = STATUS_SUCCESS;
        } else if (packet->reportId == MTP_REPORT_ID_FUNCTION && packet->reportBufferLen >= 2) {
            packet->reportBuffer[0] = MTP_REPORT_ID_FUNCTION;
            packet->reportBuffer[1] = context->FunctionSwitch;
            status = STATUS_SUCCESS;
        } else if (packet->reportId == MTP_REPORT_ID_LATENCY && packet->reportBufferLen >= 2) {
            packet->reportBuffer[0] = MTP_REPORT_ID_LATENCY;
            packet->reportBuffer[1] = context->LatencyMode;
            status = STATUS_SUCCESS;
        }
    }
    VhfAsyncOperationComplete(operationHandle, status);
}

VOID MtpEvtVhfSetFeature(PVOID clientContext, VHFOPERATIONHANDLE operationHandle,
                         PVOID operationContext, PHID_XFER_PACKET packet) {
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    PDEVICE_CONTEXT context = (PDEVICE_CONTEXT)clientContext;
    UNREFERENCED_PARAMETER(operationContext);
    if (packet != NULL && packet->reportBuffer != NULL && packet->reportBufferLen >= 2) {
        if (packet->reportId == MTP_REPORT_ID_MODE) {
            context->InputMode = packet->reportBuffer[1];
            InterlockedIncrement(&context->SetFeatureCount);
            status = STATUS_SUCCESS;
        } else if (packet->reportId == MTP_REPORT_ID_FUNCTION) {
            context->FunctionSwitch = packet->reportBuffer[1];
            InterlockedIncrement(&context->SetFeatureCount);
            status = STATUS_SUCCESS;
        } else if (packet->reportId == MTP_REPORT_ID_LATENCY) {
            context->LatencyMode = (UCHAR)(packet->reportBuffer[1] & 0x01);
            InterlockedIncrement(&context->SetFeatureCount);
            status = STATUS_SUCCESS;
        }
    }
    VhfAsyncOperationComplete(operationHandle, status);
}
