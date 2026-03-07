/*
 * Mvisor vGPU WDDM display-only miniport skeleton.
 *
 * This is a bring-up scaffold for a Display-class driver path (KMDOD).
 * PresentDisplayOnly/SystemDisplayWrite currently stage frames into a
 * guarded shadow framebuffer; later phases will wire host scanout.
 */

#include <ntddk.h>
#include <dispmprt.h>

#define MVISOR_WDDM_TAG 'WvMM'
#define MVISOR_WDDM_MAX_VIEWS 1
#define MVISOR_WDDM_MAX_CHILDREN 1
#define MVISOR_WDDM_VENDOR_ID 0x1AF4
#define MVISOR_WDDM_DEVICE_ID 0x105B
#define MVISOR_WDDM_DEFAULT_HEIGHT 768
#define MVISOR_WDDM_MAX_SHADOW_BYTES (64 * 1024 * 1024)

#ifndef DXGKDDI_WDDMv1_3
#define DXGKDDI_WDDMv1_3 DXGKDDI_WDDMv1_2
#endif

#define MVISOR_WDDM_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, "mvisor_wddm: " fmt "\n", __VA_ARGS__)

typedef struct _MVISOR_WDDM_DEVICE_CONTEXT {
    DEVICE_OBJECT* PhysicalDeviceObject;
    DXGKRNL_INTERFACE DxgkInterface;
    USHORT VendorId;
    USHORT DeviceId;
    ULONG Width;
    ULONG Height;
    ULONG Pitch;
    ULONG BytesPerPixel;
    ULONG64 PresentCount;
    ULONG64 ScanoutSeq;
    PVOID ShadowFrameBuffer;
    SIZE_T ShadowFrameBufferSize;
    KSPIN_LOCK PresentLock;
    BOOLEAN Started;
} MVISOR_WDDM_DEVICE_CONTEXT, *PMVISOR_WDDM_DEVICE_CONTEXT;

static PMVISOR_WDDM_DEVICE_CONTEXT
MvisorWddmContextFromAdapterHandle(_In_ CONST HANDLE hAdapter)
{
    return (PMVISOR_WDDM_DEVICE_CONTEXT)hAdapter;
}

static NTSTATUS
MvisorWddmCheckHardware(_Inout_ PMVISOR_WDDM_DEVICE_CONTEXT context)
{
    NTSTATUS status;
    ULONG bytesRead;
    ULONG configId;

    /*
     * PCI config offset 0x00 layout:
     * bits  0..15: Vendor ID
     * bits 16..31: Device ID
     */
    status = context->DxgkInterface.DxgkCbReadDeviceSpace(
        context->DxgkInterface.DeviceHandle,
        DXGK_WHICHSPACE_CONFIG,
        &configId,
        0,
        sizeof(configId),
        &bytesRead);
    if (!NT_SUCCESS(status)) {
        MVISOR_WDDM_LOG("DxgkCbReadDeviceSpace failed status=0x%08x", status);
        return status;
    }
    if (bytesRead < sizeof(configId)) {
        MVISOR_WDDM_LOG("DxgkCbReadDeviceSpace short read bytes=%lu", bytesRead);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    context->VendorId = (USHORT)(configId & 0xFFFF);
    context->DeviceId = (USHORT)((configId >> 16) & 0xFFFF);

    MVISOR_WDDM_LOG("PCI id vendor=0x%04x device=0x%04x", context->VendorId, context->DeviceId);

    if (context->VendorId != MVISOR_WDDM_VENDOR_ID || context->DeviceId != MVISOR_WDDM_DEVICE_ID) {
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }

    return STATUS_SUCCESS;
}

static ULONG
MvisorWddmEstimateHeight(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY* present)
{
    ULONG i;
    ULONG maxBottom = 0;

    if (present->pDirtyRect != NULL && present->NumDirtyRects > 0) {
        for (i = 0; i < present->NumDirtyRects; i++) {
            LONG bottom = present->pDirtyRect[i].bottom;
            if (bottom > 0 && (ULONG)bottom > maxBottom) {
                maxBottom = (ULONG)bottom;
            }
        }
    }

    if (maxBottom > 0) {
        return maxBottom;
    }
    return MVISOR_WDDM_DEFAULT_HEIGHT;
}

static NTSTATUS
MvisorWddmEnsureShadowBuffer(
    _Inout_ PMVISOR_WDDM_DEVICE_CONTEXT context,
    _In_ SIZE_T requiredBytes)
{
    KIRQL oldIrql;
    PVOID oldBuffer;
    PVOID newBuffer;

    if (requiredBytes == 0 || requiredBytes > MVISOR_WDDM_MAX_SHADOW_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&context->PresentLock, &oldIrql);
    if (context->ShadowFrameBuffer != NULL && context->ShadowFrameBufferSize >= requiredBytes) {
        KeReleaseSpinLock(&context->PresentLock, oldIrql);
        return STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&context->PresentLock, oldIrql);

    newBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, requiredBytes, MVISOR_WDDM_TAG);
    if (newBuffer == NULL) {
        return STATUS_NO_MEMORY;
    }

    KeAcquireSpinLock(&context->PresentLock, &oldIrql);
    oldBuffer = context->ShadowFrameBuffer;
    context->ShadowFrameBuffer = newBuffer;
    context->ShadowFrameBufferSize = requiredBytes;
    KeReleaseSpinLock(&context->PresentLock, oldIrql);

    if (oldBuffer != NULL) {
        ExFreePoolWithTag(oldBuffer, MVISOR_WDDM_TAG);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
MvisorWddmSafeSizeMultiply(
    _In_ SIZE_T left,
    _In_ SIZE_T right,
    _Out_ SIZE_T* result)
{
    if (result == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (left != 0 && right > (MAXULONG_PTR / left)) {
        return STATUS_INTEGER_OVERFLOW;
    }

    *result = left * right;
    return STATUS_SUCCESS;
}

static ULONG
MvisorWddmClampRectCoord(_In_ LONG value, _In_ ULONG maxExclusive)
{
    if (value <= 0) {
        return 0;
    }

    if ((ULONG)value >= maxExclusive) {
        return maxExclusive;
    }

    return (ULONG)value;
}

static VOID
MvisorWddmUpdateFrameStats(
    _Inout_ PMVISOR_WDDM_DEVICE_CONTEXT context,
    _In_ ULONG width,
    _In_ ULONG height,
    _In_ ULONG pitch,
    _In_ ULONG bytesPerPixel)
{
    context->Pitch = pitch;
    context->BytesPerPixel = bytesPerPixel;
    context->Height = height;
    context->Width = width;
    context->PresentCount++;
    context->ScanoutSeq++;
}

static NTSTATUS
MvisorWddmCopyFullFrame(
    _In_ CONST DXGKARG_PRESENT_DISPLAYONLY* present,
    _In_ ULONG height,
    _In_ PUCHAR src,
    _Inout_ PUCHAR dst)
{
    ULONG y;

    for (y = 0; y < height; y++) {
        SIZE_T rowOffset;
        NTSTATUS status = MvisorWddmSafeSizeMultiply((SIZE_T)y, (SIZE_T)present->Pitch, &rowOffset);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        RtlCopyMemory(dst + rowOffset, src + rowOffset, present->Pitch);
    }

    return STATUS_SUCCESS;
}

static BOOLEAN
MvisorWddmTryCopyDirtyRect(
    _In_ CONST DXGKARG_PRESENT_DISPLAYONLY* present,
    _In_ ULONG width,
    _In_ ULONG height,
    _In_ PUCHAR src,
    _Inout_ PUCHAR dst,
    _In_ CONST RECT* rect)
{
    ULONG left;
    ULONG right;
    ULONG top;
    ULONG bottom;
    SIZE_T leftOffset;
    SIZE_T rowBytes;
    ULONG y;

    if (rect == NULL) {
        return FALSE;
    }

    left = MvisorWddmClampRectCoord(rect->left, width);
    right = MvisorWddmClampRectCoord(rect->right, width);
    top = MvisorWddmClampRectCoord(rect->top, height);
    bottom = MvisorWddmClampRectCoord(rect->bottom, height);

    if (right <= left || bottom <= top) {
        return FALSE;
    }

    if (!NT_SUCCESS(MvisorWddmSafeSizeMultiply((SIZE_T)left, (SIZE_T)present->BytesPerPixel, &leftOffset))) {
        return FALSE;
    }
    if (!NT_SUCCESS(MvisorWddmSafeSizeMultiply((SIZE_T)(right - left), (SIZE_T)present->BytesPerPixel, &rowBytes))) {
        return FALSE;
    }

    for (y = top; y < bottom; y++) {
        SIZE_T rowOffset;
        if (!NT_SUCCESS(MvisorWddmSafeSizeMultiply((SIZE_T)y, (SIZE_T)present->Pitch, &rowOffset))) {
            return FALSE;
        }
        RtlCopyMemory(dst + rowOffset + leftOffset, src + rowOffset + leftOffset, rowBytes);
    }

    return TRUE;
}

static NTSTATUS
MvisorWddmStageSystemDisplayFrame(
    _Inout_ PMVISOR_WDDM_DEVICE_CONTEXT context,
    _In_ CONST VOID* source,
    _In_ UINT sourceWidth,
    _In_ UINT sourceHeight,
    _In_ UINT sourceStride)
{
    NTSTATUS status;
    SIZE_T requiredBytes;
    UINT y;
    CONST PUCHAR src;
    PUCHAR dst;
    KIRQL oldIrql;

    if (source == NULL || sourceWidth == 0 || sourceHeight == 0 || sourceStride == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    status = MvisorWddmSafeSizeMultiply((SIZE_T)sourceStride, (SIZE_T)sourceHeight, &requiredBytes);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (requiredBytes == 0 || requiredBytes > MVISOR_WDDM_MAX_SHADOW_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    status = MvisorWddmEnsureShadowBuffer(context, requiredBytes);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    src = (CONST PUCHAR)source;
    KeAcquireSpinLock(&context->PresentLock, &oldIrql);
    dst = (PUCHAR)context->ShadowFrameBuffer;
    if (dst == NULL || context->ShadowFrameBufferSize < requiredBytes) {
        KeReleaseSpinLock(&context->PresentLock, oldIrql);
        return STATUS_UNSUCCESSFUL;
    }

    for (y = 0; y < sourceHeight; y++) {
        SIZE_T rowOffset;
        status = MvisorWddmSafeSizeMultiply((SIZE_T)y, (SIZE_T)sourceStride, &rowOffset);
        if (!NT_SUCCESS(status)) {
            KeReleaseSpinLock(&context->PresentLock, oldIrql);
            return status;
        }
        RtlCopyMemory(dst + rowOffset, src + rowOffset, sourceStride);
    }

    MvisorWddmUpdateFrameStats(context, sourceWidth, sourceHeight, sourceStride, 4);
    KeReleaseSpinLock(&context->PresentLock, oldIrql);
    return STATUS_SUCCESS;
}

static NTSTATUS
MvisorWddmStagePresentFrame(
    _Inout_ PMVISOR_WDDM_DEVICE_CONTEXT context,
    _In_ CONST DXGKARG_PRESENT_DISPLAYONLY* present)
{
    NTSTATUS status;
    SIZE_T requiredBytes;
    ULONG width;
    ULONG height;
    ULONG i;
    ULONG dirtyCopied;
    PUCHAR src;
    PUCHAR dst;
    KIRQL oldIrql;

    if (present->pSource == NULL || present->Pitch == 0 || present->BytesPerPixel == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((present->Pitch % present->BytesPerPixel) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    width = present->Pitch / present->BytesPerPixel;
    if (width == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    height = MvisorWddmEstimateHeight(present);
    status = MvisorWddmSafeSizeMultiply((SIZE_T)present->Pitch, (SIZE_T)height, &requiredBytes);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (requiredBytes == 0 || requiredBytes > MVISOR_WDDM_MAX_SHADOW_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    status = MvisorWddmEnsureShadowBuffer(context, requiredBytes);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    src = (PUCHAR)present->pSource;

    KeAcquireSpinLock(&context->PresentLock, &oldIrql);
    dst = (PUCHAR)context->ShadowFrameBuffer;
    if (dst == NULL || context->ShadowFrameBufferSize < requiredBytes) {
        KeReleaseSpinLock(&context->PresentLock, oldIrql);
        return STATUS_UNSUCCESSFUL;
    }

    dirtyCopied = 0;
    if (present->pDirtyRect != NULL && present->NumDirtyRects > 0) {
        for (i = 0; i < present->NumDirtyRects; i++) {
            if (MvisorWddmTryCopyDirtyRect(
                    present,
                    width,
                    height,
                    src,
                    dst,
                    &present->pDirtyRect[i])) {
                dirtyCopied++;
            }
        }
    }

    if (dirtyCopied == 0) {
        status = MvisorWddmCopyFullFrame(present, height, src, dst);
        if (!NT_SUCCESS(status)) {
            KeReleaseSpinLock(&context->PresentLock, oldIrql);
            return status;
        }
    }

    MvisorWddmUpdateFrameStats(context, width, height, present->Pitch, present->BytesPerPixel);

    if ((context->PresentCount % 120) == 0) {
        MVISOR_WDDM_LOG("present-staged frames=%llu size=%lux%lu pitch=%lu dirty=%lu/%u",
            context->PresentCount,
            context->Width,
            context->Height,
            context->Pitch,
            dirtyCopied,
            present->NumDirtyRects);
    }

    KeReleaseSpinLock(&context->PresentLock, oldIrql);
    return STATUS_SUCCESS;
}

VOID
MvisorWddmUnload(VOID)
{
    PAGED_CODE();
}

NTSTATUS
MvisorWddmAddDevice(
    _In_ DEVICE_OBJECT* PhysicalDeviceObject,
    _Outptr_ PVOID* MiniportDeviceContext)
{
    PMVISOR_WDDM_DEVICE_CONTEXT context;

    PAGED_CODE();

    if (PhysicalDeviceObject == NULL || MiniportDeviceContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *MiniportDeviceContext = NULL;

    context = (PMVISOR_WDDM_DEVICE_CONTEXT)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(*context),
        MVISOR_WDDM_TAG);
    if (context == NULL) {
        return STATUS_NO_MEMORY;
    }

    RtlZeroMemory(context, sizeof(*context));
    context->PhysicalDeviceObject = PhysicalDeviceObject;
    KeInitializeSpinLock(&context->PresentLock);
    context->Started = FALSE;
    *MiniportDeviceContext = context;

    return STATUS_SUCCESS;
}

NTSTATUS
MvisorWddmRemoveDevice(_In_ VOID* MiniportDeviceContext)
{
    PMVISOR_WDDM_DEVICE_CONTEXT context;

    PAGED_CODE();

    context = (PMVISOR_WDDM_DEVICE_CONTEXT)MiniportDeviceContext;
    if (context != NULL) {
        if (context->ShadowFrameBuffer != NULL) {
            ExFreePoolWithTag(context->ShadowFrameBuffer, MVISOR_WDDM_TAG);
            context->ShadowFrameBuffer = NULL;
            context->ShadowFrameBufferSize = 0;
        }
        ExFreePoolWithTag(context, MVISOR_WDDM_TAG);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
MvisorWddmStartDevice(
    _In_ VOID* MiniportDeviceContext,
    _In_ DXGK_START_INFO* DxgkStartInfo,
    _In_ DXGKRNL_INTERFACE* DxgkInterface,
    _Out_ ULONG* NumberOfViews,
    _Out_ ULONG* NumberOfChildren)
{
    NTSTATUS status;
    PMVISOR_WDDM_DEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(DxgkStartInfo);
    PAGED_CODE();

    if (MiniportDeviceContext == NULL || DxgkInterface == NULL ||
        NumberOfViews == NULL || NumberOfChildren == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    context = (PMVISOR_WDDM_DEVICE_CONTEXT)MiniportDeviceContext;
    RtlCopyMemory(&context->DxgkInterface, DxgkInterface, sizeof(*DxgkInterface));

    status = MvisorWddmCheckHardware(context);
    if (!NT_SUCCESS(status)) {
        MVISOR_WDDM_LOG("hardware check failed status=0x%08x", status);
        return status;
    }

    *NumberOfViews = MVISOR_WDDM_MAX_VIEWS;
    *NumberOfChildren = MVISOR_WDDM_MAX_CHILDREN;

    context->Started = TRUE;
    MVISOR_WDDM_LOG("StartDevice views=%lu children=%lu", *NumberOfViews, *NumberOfChildren);

    return STATUS_SUCCESS;
}

NTSTATUS
MvisorWddmStopDevice(_In_ VOID* MiniportDeviceContext)
{
    PMVISOR_WDDM_DEVICE_CONTEXT context;

    PAGED_CODE();

    if (MiniportDeviceContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    context = (PMVISOR_WDDM_DEVICE_CONTEXT)MiniportDeviceContext;
    context->Started = FALSE;
    context->Width = 0;
    context->Height = 0;
    context->Pitch = 0;
    context->BytesPerPixel = 0;

    return STATUS_SUCCESS;
}

NTSTATUS
MvisorWddmDispatchIoRequest(
    _In_ VOID* MiniportDeviceContext,
    _In_ ULONG VidPnSourceId,
    _In_ VIDEO_REQUEST_PACKET* VideoRequestPacket)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    PAGED_CODE();

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
MvisorWddmSetPowerState(
    _In_ VOID* MiniportDeviceContext,
    _In_ ULONG HardwareUid,
    _In_ DEVICE_POWER_STATE DevicePowerState,
    _In_ POWER_ACTION ActionType)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(HardwareUid);
    UNREFERENCED_PARAMETER(DevicePowerState);
    UNREFERENCED_PARAMETER(ActionType);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
MvisorWddmQueryChildRelations(
    _In_ VOID* MiniportDeviceContext,
    _Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR* ChildRelations,
    _In_ ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    PAGED_CODE();

    if (ChildRelations == NULL || ChildRelationsSize < sizeof(DXGK_CHILD_DESCRIPTOR)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(ChildRelations, ChildRelationsSize);
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_OTHER;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = 0;

    return STATUS_SUCCESS;
}

NTSTATUS
MvisorWddmQueryChildStatus(
    _In_ VOID* MiniportDeviceContext,
    _Inout_ DXGK_CHILD_STATUS* ChildStatus,
    _In_ BOOLEAN NonDestructiveOnly)
{
    PMVISOR_WDDM_DEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    PAGED_CODE();

    if (MiniportDeviceContext == NULL || ChildStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    context = (PMVISOR_WDDM_DEVICE_CONTEXT)MiniportDeviceContext;

    switch (ChildStatus->Type) {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = context->Started ? TRUE : FALSE;
        return STATUS_SUCCESS;
    case StatusRotation:
        return STATUS_INVALID_PARAMETER;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS
MvisorWddmQueryDeviceDescriptor(
    _In_ VOID* MiniportDeviceContext,
    _In_ ULONG ChildUid,
    _Inout_ DXGK_DEVICE_DESCRIPTOR* DeviceDescriptor)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(DeviceDescriptor);
    PAGED_CODE();

    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

BOOLEAN
MvisorWddmInterruptRoutine(
    _In_ VOID* MiniportDeviceContext,
    _In_ ULONG MessageNumber)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(MessageNumber);

    return FALSE;
}

VOID
MvisorWddmDpcRoutine(_In_ VOID* MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

VOID
MvisorWddmResetDevice(_In_ VOID* MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

NTSTATUS
APIENTRY
MvisorWddmQueryAdapterInfo(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo)
{
    if (hAdapter == NULL || QueryAdapterInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (QueryAdapterInfo->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
    {
        DXGK_DRIVERCAPS* driverCaps;

        if (QueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS) ||
            QueryAdapterInfo->pOutputData == NULL) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        driverCaps = (DXGK_DRIVERCAPS*)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(driverCaps, sizeof(*driverCaps));
        driverCaps->WDDMVersion = DXGKDDI_WDDMv1_3;
        driverCaps->HighestAcceptableAddress.QuadPart = -1;
        driverCaps->SupportNonVGA = TRUE;
        driverCaps->SupportSmoothRotation = TRUE;
        return STATUS_SUCCESS;
    }

#ifdef DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
    {
        DXGK_DISPLAY_DRIVERCAPS_EXTENSION* displayCaps;

        if (QueryAdapterInfo->pOutputData == NULL || QueryAdapterInfo->OutputDataSize < sizeof(*displayCaps)) {
            return STATUS_INVALID_PARAMETER;
        }

        displayCaps = (DXGK_DISPLAY_DRIVERCAPS_EXTENSION*)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(displayCaps, QueryAdapterInfo->OutputDataSize);
        displayCaps->VirtualModeSupport = 1;
        return STATUS_SUCCESS;
    }
#endif

    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS
APIENTRY
MvisorWddmSetPointerPosition(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETPOINTERPOSITION* SetPointerPosition)
{
    UNREFERENCED_PARAMETER(hAdapter);

    PAGED_CODE();

    if (SetPointerPosition == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!SetPointerPosition->Flags.Visible) {
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
MvisorWddmSetPointerShape(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETPOINTERSHAPE* SetPointerShape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(SetPointerShape);
    PAGED_CODE();

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
APIENTRY
MvisorWddmPresentDisplayOnly(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_PRESENT_DISPLAYONLY* PresentDisplayOnly)
{
    PMVISOR_WDDM_DEVICE_CONTEXT context;

    if (hAdapter == NULL || PresentDisplayOnly == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    context = MvisorWddmContextFromAdapterHandle(hAdapter);
    if (!context->Started) {
        return STATUS_UNSUCCESSFUL;
    }

    if (PresentDisplayOnly->BytesPerPixel < 4) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Phase-4 step: ingest and stage presents in a shadow buffer with
     * geometry/dirty-rect tracking. Host scanout transport wiring is next.
     */
    return MvisorWddmStagePresentFrame(context, PresentDisplayOnly);
}

NTSTATUS
APIENTRY
MvisorWddmStopDeviceAndReleasePostDisplayOwnership(
    _In_ VOID* MiniportDeviceContext,
    _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _Out_ DXGK_DISPLAY_INFORMATION* DisplayInfo)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(TargetId);

    if (DisplayInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(DisplayInfo, sizeof(*DisplayInfo));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmIsSupportedVidPn(
    _In_ CONST HANDLE hAdapter,
    _Inout_ DXGKARG_ISSUPPORTEDVIDPN* IsSupportedVidPn)
{
    UNREFERENCED_PARAMETER(hAdapter);

    PAGED_CODE();

    if (IsSupportedVidPn == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    IsSupportedVidPn->IsVidPnSupported = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmRecommendFunctionalVidPn(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN* CONST RecommendFunctionalVidPn)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(RecommendFunctionalVidPn);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmEnumVidPnCofuncModality(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY* CONST EnumCofuncModality)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(EnumCofuncModality);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmSetVidPnSourceVisibility(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY* SetVidPnSourceVisibility)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(SetVidPnSourceVisibility);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmCommitVidPn(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_COMMITVIDPN* CONST CommitVidPn)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(CommitVidPn);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmUpdateActiveVidPnPresentPath(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* CONST UpdateActiveVidPnPresentPath)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(UpdateActiveVidPnPresentPath);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmRecommendMonitorModes(
    _In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST RecommendMonitorModes)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(RecommendMonitorModes);
    PAGED_CODE();

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmQueryVidPnHWCapability(
    _In_ CONST HANDLE hAdapter,
    _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* VidPnHWCaps)
{
    UNREFERENCED_PARAMETER(hAdapter);

    PAGED_CODE();

    if (VidPnHWCaps == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(VidPnHWCaps, sizeof(*VidPnHWCaps));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
MvisorWddmSystemDisplayEnable(
    _In_ VOID* MiniportDeviceContext,
    _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
    _Out_ UINT* Width,
    _Out_ UINT* Height,
    _Out_ D3DDDIFORMAT* ColorFormat)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);

    if (Width == NULL || Height == NULL || ColorFormat == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Width = 1024;
    *Height = 768;
    *ColorFormat = D3DDDIFMT_X8R8G8B8;

    return STATUS_SUCCESS;
}

VOID
APIENTRY
MvisorWddmSystemDisplayWrite(
    _In_ VOID* MiniportDeviceContext,
    _In_ VOID* Source,
    _In_ UINT SourceWidth,
    _In_ UINT SourceHeight,
    _In_ UINT SourceStride,
    _In_ UINT PositionX,
    _In_ UINT PositionY)
{
    PMVISOR_WDDM_DEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PositionX);
    UNREFERENCED_PARAMETER(PositionY);

    if (MiniportDeviceContext == NULL || Source == NULL) {
        return;
    }

    context = (PMVISOR_WDDM_DEVICE_CONTEXT)MiniportDeviceContext;
    status = MvisorWddmStageSystemDisplayFrame(
        context,
        Source,
        SourceWidth,
        SourceHeight,
        SourceStride);
    if (!NT_SUCCESS(status)) {
        MVISOR_WDDM_LOG("SystemDisplayWrite stage failed status=0x%08x", status);
    }
}

NTSTATUS
DriverEntry(
    _In_ DRIVER_OBJECT* DriverObject,
    _In_ UNICODE_STRING* RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA initData;

    PAGED_CODE();

    RtlZeroMemory(&initData, sizeof(initData));

    initData.Version = DXGKDDI_INTERFACE_VERSION;

    initData.DxgkDdiAddDevice = MvisorWddmAddDevice;
    initData.DxgkDdiStartDevice = MvisorWddmStartDevice;
    initData.DxgkDdiStopDevice = MvisorWddmStopDevice;
    initData.DxgkDdiResetDevice = MvisorWddmResetDevice;
    initData.DxgkDdiRemoveDevice = MvisorWddmRemoveDevice;
    initData.DxgkDdiDispatchIoRequest = MvisorWddmDispatchIoRequest;
    initData.DxgkDdiInterruptRoutine = MvisorWddmInterruptRoutine;
    initData.DxgkDdiDpcRoutine = MvisorWddmDpcRoutine;
    initData.DxgkDdiQueryChildRelations = MvisorWddmQueryChildRelations;
    initData.DxgkDdiQueryChildStatus = MvisorWddmQueryChildStatus;
    initData.DxgkDdiQueryDeviceDescriptor = MvisorWddmQueryDeviceDescriptor;
    initData.DxgkDdiSetPowerState = MvisorWddmSetPowerState;
    initData.DxgkDdiUnload = MvisorWddmUnload;

    initData.DxgkDdiQueryAdapterInfo = MvisorWddmQueryAdapterInfo;
    initData.DxgkDdiSetPointerPosition = MvisorWddmSetPointerPosition;
    initData.DxgkDdiSetPointerShape = MvisorWddmSetPointerShape;
    initData.DxgkDdiPresentDisplayOnly = MvisorWddmPresentDisplayOnly;
    initData.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = MvisorWddmStopDeviceAndReleasePostDisplayOwnership;
    initData.DxgkDdiIsSupportedVidPn = MvisorWddmIsSupportedVidPn;
    initData.DxgkDdiRecommendFunctionalVidPn = MvisorWddmRecommendFunctionalVidPn;
    initData.DxgkDdiEnumVidPnCofuncModality = MvisorWddmEnumVidPnCofuncModality;
    initData.DxgkDdiSetVidPnSourceVisibility = MvisorWddmSetVidPnSourceVisibility;
    initData.DxgkDdiCommitVidPn = MvisorWddmCommitVidPn;
    initData.DxgkDdiUpdateActiveVidPnPresentPath = MvisorWddmUpdateActiveVidPnPresentPath;
    initData.DxgkDdiRecommendMonitorModes = MvisorWddmRecommendMonitorModes;
    initData.DxgkDdiQueryVidPnHWCapability = MvisorWddmQueryVidPnHWCapability;
    initData.DxgkDdiSystemDisplayEnable = MvisorWddmSystemDisplayEnable;
    initData.DxgkDdiSystemDisplayWrite = MvisorWddmSystemDisplayWrite;

    return DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &initData);
}
