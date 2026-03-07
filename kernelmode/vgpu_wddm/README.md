# Mvisor vGPU WDDM Miniport Skeleton (KMDOD)

## Purpose

This directory contains the first guest-side skeleton for a Display-class WDDM miniport path.
It is a transition step from the current System-class render-only `vgpu.sys`.

Current status:
- Skeleton only (bring-up stage)
- Uses KMDOD registration (`DxgkInitializeDisplayOnlyDriver`)
- `PresentDisplayOnly` ingests frames into a shadow buffer for bring-up telemetry
- Not yet wired to virtio-vgpu queues or real host present/scanout path

## Why this exists

The current Mvisor Windows vGPU driver is installed as `Class=System`, which allows OpenGL render transport but does not become the primary WDDM display adapter.
This skeleton starts the Display-class driver track needed for VMware-like behavior.

## Files

- `mvisor_wddm.inf`: Display-class INF (PCI `VEN_1AF4&DEV_105B`)
- `mvisor_wddm.c`: KMDOD callback skeleton and DriverEntry

## Build Notes

1. Use Visual Studio + WDK10.
2. Open `mvisor_wddm.sln` in this directory.
3. Build `Release | x64`.
4. Keep test-signing mode enabled in the guest for unsigned/test certificates.

CLI build (Developer Command Prompt):

```bat
cd kernelmode\vgpu_wddm
msbuild mvisor_wddm.sln /t:Build /p:Configuration=Release /p:Platform=x64
```

Shortcut:

```bat
cd kernelmode\vgpu_wddm
build-wddm.bat
```

Expected output artifacts (under project `x64\Release\` intermediate/output path):
- `mvisor_wddm.sys`
- `mvisor_wddm.inf`
- `mvisor_wddm.cat` (if signing/catalog step is enabled in your WDK setup)

## Next Integration Tasks

1. Implement real adapter capability reporting in `DxgkDdiQueryAdapterInfo`.
2. Replace no-op `DxgkDdiPresentDisplayOnly` with virtio-vgpu present transport.
3. Connect mode-setting callbacks (`CommitVidPn`, `SetVidPnSourceVisibility`) to host scanout protocol.
4. Add interrupt/fence synchronization for DWM-friendly present timing.
